# Plan: switch rp2350-hdmi-hexpansion to hazanjon's `attach_mirror` sink

## Goal

Two repos, two sets of changes, then a build+flash:

1. **`Corteil/rp2350-hdmi-hexpansion`** (this repo) — patch the RP2350 receive
   firmware and the badge-side app so they use hazanjon's push-based
   `display.attach_mirror()` sink instead of the current `display.get_fb()`
   polling loop.
2. **`hazanjon/badge-2024-software`**, branch **`feature/hdmi-mirror`**
   (upstream PR emfcamp/badge-2024-software#454 — NOT `main`, which only has
   an earlier, simpler version of the mirror sink) — build as-is, no patches
   needed there. Its `attach_mirror(port, driver, sck, mosi, cs, dc)` already
   accepts explicit pin overrides.

Do not attempt to patch hazanjon's C code — the plan below works entirely
by passing keyword overrides from Python.

---

## Part A — Patch `firmware/testcard/src/main.c`

**A1. SPI mode 0, not mode 3.**

hazanjon's mirror sink hardcodes `spi_device_interface_config_t.mode = 0`
(CPOL=0, CPHA=0) in `flow3r_bsp_display_mirror.c`, with no way to configure
it from Python. The current RP2350 firmware is built for mode 3.

Find:
```c
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
```
Replace with:
```c
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
```

**A2. Undo the byte-swap on this side instead of the badge side.**

`display.attach_mirror()` forwards the badge's raw `tildagon_fb` straight
from C (`flow3r_bsp_display_dispatch_sinks(tildagon_fb, ...)`), which is
`CTX_FORMAT_RGB565_BYTESWAPPED`. There is no per-frame Python callback left
to un-swap it (that's what the old `MirrorApp._swap_bytepairs()` used to
do) — so the swap has to happen when this firmware unpacks each pixel.

Find:
```c
            uint16_t *src16 = (uint16_t *)(void *)row_rx;
            uint32_t *dst32 = framebuf[recv_buf][row_index];
            for (int i = 0; i < DBL_WORDS; i++) {
                uint32_t px = src16[i];
                dst32[i] = px | (px << 16);
            }
```
Replace with:
```c
            uint32_t *dst32 = framebuf[recv_buf][row_index];
            for (int i = 0; i < DBL_WORDS; i++) {
                // hazanjon's attach_mirror sink forwards tildagon_fb raw
                // (CTX_FORMAT_RGB565_BYTESWAPPED) -- undo that here since
                // there's no Python-side _swap_bytepairs() step any more.
                uint32_t px = (row_rx[2 * i] << 8) | row_rx[2 * i + 1];
                dst32[i] = px | (px << 16);
            }
```

Leave everything else in `main.c` (DMA ring receive, `FRAME_MARKER`
handling, row-vs-frame framing) untouched — it's already
chunking-/CS-framing-agnostic and doesn't care that hazanjon's sink sends
in 4096-byte DMA bursts under one continuous CS-low frame.

---

## Part B — Patch `firmware/testcard/badge_app/app.py`

Replace the entire `MirrorApp` class (from `class MirrorApp(app.App):` down
to, but not including, the trailing `__app_export__ = MirrorApp` line) with:

```python
# Per-port GPIO overrides. hazanjon's PORT_PINS table
# (badge-2024-software feature/hdmi-mirror,
# components/flow3r_bsp/flow3r_bsp_display_mirror.c) labels these
# sck/mosi/cs/dc, but its physical role assignment doesn't match ours
# (this repo's README §4.1: HS_F=MOSI, HS_G=CS, HS_H=SCK). mosi already
# lines up; sck/cs are swapped relative to hazanjon's defaults. Re-check
# this table if that upstream file's PORT_PINS values ever change.
_PORT_OVERRIDE = {
    # port: (sck, mosi, cs)
    1: (41, 39, 40),
    2: (37, 35, 36),
    3: (47, 34, 33),
    4: (13, 11, 14),
    5: (15, 18, 16),
    6: ( 5,  3,  4),
}


class MirrorApp(app.App):
    def __init__(self, config):
        super().__init__()
        self.config = config
        self.buttons = Buttons(self)
        self.status = "starting..."

        port = getattr(config, "port", None) if config else None
        if port is not None:
            eventbus.emit(HexpansionAppLauncherAddEvent(port, "HDMI Mirror"))
            self._attach(port)

    def _attach(self, port):
        sck, mosi, cs = _PORT_OVERRIDE.get(port, (None, None, None))
        if sck is None:
            self.status = "no pin override for port %d" % port
            print(self.status)
            return
        driver = {
            "baudrate": 20_000_000,  # re-sweep once this is on real hardware
            "init": [], "prefix": [], "postfix": [],
            "header": FRAME_MARKER,  # bytes([0xA5, 0x5A] * 4) -- unchanged
        }
        try:
            display.attach_mirror(port=port, driver=driver, sck=sck, mosi=mosi, cs=cs)
            self.status = "mirroring (attach_mirror)"
        except Exception as e:
            self.status = "attach_mirror failed: {!r}".format(e)
            print(self.status)

    def deinit(self):
        port = getattr(self.config, "port", None) if self.config else None
        if port is not None:
            try:
                display.detach_mirror(port)
            except Exception as e:
                print("detach_mirror failed: {!r}".format(e))

    def update(self, delta):
        if self.buttons.pressed(BUTTON_TYPES["CANCEL"]):
            self.buttons.clear()
            self.minimise()
            return True

    def draw(self, ctx):
        ctx.save()
        clear_background(ctx)
        ctx.rgb(1, 1, 1).move_to(0, -30).text("HDMI Mirror")
        ctx.font_size = small_font_size
        ctx.rgb(1, 1, 0).move_to(0, 10).text(self.status)
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 45).text("CANCEL: back")
        ctx.restore()
```

Also **delete the now-unused `_swap_bytepairs` viper function** (the block
immediately above `class MirrorApp`, from its comment header down through
the `def _swap_bytepairs(...)` body) — nothing else in this file calls it
once `MirrorApp` no longer does its own SPI send loop.

Leave `TestcardApp` and everything above it untouched.

---

## Part C — Build hazanjon's badge firmware on the Pi 5

Docker + this base image are already proven working on this Pi 5 (built the
`get_fb()` version successfully over the weekend and the day after) — this
is the same process, different branch, no patching required.

```bash
git clone --recursive https://github.com/hazanjon/badge-2024-software.git
cd badge-2024-software
git checkout feature/hdmi-mirror
./scripts/firstTime.sh

docker run -it --rm --env "TARGET=esp32s3" \
  -v "$(pwd)":/firmware -u "$UID" -e HOME=/tmp \
  ghcr.io/emfcamp/esp_idf:v5.5.1
```

Flash to a real badge (put it in bootloader mode first: disconnect USB,
hold **BAT + BOOP** for 20s, reconnect):

```bash
docker run -it --rm --device /dev/ttyACM0:/dev/ttyUSB0 \
  --env "TARGET=esp32s3" -v "$(pwd)":/firmware -u "$UID" -e HOME=/tmp \
  ghcr.io/emfcamp/esp_idf:v5.5.1 deploy
```

---

## Part D — Verify before trusting it

1. **`HS_I`/`dc` pin contention** — hazanjon's firmware always drives a `dc`
   pin (falls back to the port's default, `HS_I`, if not overridden — there
   is no "disable" value). This repo's own convention wires `HS_I` as the
   RP2350's SPI0 MISO/TX line. Before connecting both boards live: either
   pass `dc=<some genuinely unused ESP32-S3 GPIO>` in `_attach()` above to
   redirect it away from `HS_I`, or confirm with a meter/scope that the
   RP2350's MISO line tri-states when not actively selected/clocked. Do not
   assume either side of this is safe without checking.
2. **Re-sweep SPI baudrate on real hardware.** The last speed data (20 MHz
   clean, 30 MHz+ bit errors) was measured against this repo's own
   Python-driven `machine.SPI` master (Phase 0 C3), not hazanjon's hardware
   SPI+DMA master. Confirm the same ceiling still holds before leaving
   `driver["baudrate"]` at 20 MHz.
3. Confirm the badge's "HDMI Mirror" launcher entry appears and mirrors
   correctly with the actual RP2350 hexpansion attached, not just that the
   firmware boots.
