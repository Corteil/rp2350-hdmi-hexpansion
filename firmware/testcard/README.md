# Testcard — the first end-to-end link demonstration

The first firmware to combine every Phase 0 piece into one image and prove them working
*together*, not just individually: badge inserts a hexpansion → discovers the emulated
identification EEPROM with this project's real assigned identity (**VID `0x1969`, PID
`0x4544`**) → mounts a real LittleFS filesystem living inside that EEPROM → auto-launches the
badge-side app packed into it → that app opens the proven SPI0 link and sends pattern-select
commands → the RP2350 applies them and the connected monitor changes, live, over HDMI.

Every layer here was proven individually in Phase 0. This is the first time they've all run
at once, in one firmware, and it's confirmed working end to end on real hardware.

## Architecture

```
  badge (ESP32-S3)                          RP2350 (Metro bench board)
  ┌─────────────────────┐                   ┌──────────────────────────┐
  │ hexpansion insertion │  I2C0 (GPIO4/5)  │ eeprom_i2c.c              │
  │ -> detect EEPROM     │◄─────────────────►│  VID=0x1969 PID=0x4544   │
  │ -> mount LittleFS    │                   │  real fs_image.h inside │
  │ -> import app.py     │                   └──────────────────────────┘
  │ -> TestcardApp runs  │
  │                       │  SPI0 (GPIO20-23) ┌──────────────────────────┐
  │  L/R button: send    │◄─────────────────►│ SPI0 slave command loop  │
  │  pattern-select cmd  │  mode 3, CS-cont.  │  applies to framebuf[]   │
  └───────────────────────┘                   └────────────┬─────────────┘
                                                            │
                                               HSTX (GPIO12-19, colour-
                                               correct per phase0-dvi-
                                               colorfix) ──► monitor
```

All three peripheral pin groups (I2C0 on GPIO4/5, SPI0 on GPIO20–23, HSTX on GPIO12–19) are
disjoint, so one physical Metro RP2350 runs the whole demo — same bench rig as A2/C3/C4
(badge devkit hexpansion wired to the Metro's headers) plus phase0-dvi-colorfix's HSTX port +
Adafruit adapter + monitor, all connected at once.

## Pieces

* **`src/eeprom_i2c.c`/`.h`** — I2C0 target EEPROM emulation, adapted from
  `firmware/phase0-a2-eeprom/` (I2C0-target mechanics already confirmed working on real
  hardware there). Real differences: the real VID/PID instead of placeholders, and a real,
  mountable LittleFS2 image copied into the filesystem region instead of leaving it empty.
* **`src/fs_image.h`** — **generated**, not hand-edited. A LittleFS2 filesystem image built
  from `badge_app/` by `tools/build_fs_image.py`, emitted as a C byte array. Re-run that
  script (and rebuild) after changing `badge_app/`.
* **`badge_app/app.py`** — the actual badge-side source. Not run from a loose checkout: it's
  packed *into* the LittleFS image above, so it ships and updates with the hexpansion
  firmware itself, matching the product's own design (main README §2, item 2: "the RP2350
  owns that image, so the driver updates itself with the hexpansion firmware — no separate
  EEPROM flashing step, ever").
* **`src/main.c`** — combines I2C0 target start (first thing in `main()`, per §5's
  race-condition mitigation), HSTX/DVI setup (colour-correct, from `phase0-dvi-colorfix/`,
  Stage 1's exact-double 320×240→640×480 geometry — no pillarbox/mask, since this is a
  generic test card, not a mirror of the badge's round display), and the SPI0 slave
  pattern-select command loop.

## The LittleFS block-count math (why this matters, why it isn't guessed)

The badge computes its own filesystem partition size independently — `block_size = 512`,
`block_count = (eeprom_total_size − fs_offset) / 512` — from
`badge-2024-software/modules/system/hexpansion/util.py`'s `get_hexpansion_block_devices()`
and `modules/eeprom_partition.py`'s `EEPROMPartition.ioctl(4)`, confirmed by reading that
code directly (a local clone from this project's earlier C2 work), not assumed. With this
project's header values (`fs_offset=64`, `eeprom_total_size=65536`): `(65536−64)/512 =
127.875`, which Python's integer division floors to **127 blocks = 65,024 bytes** — so the
image is built with exactly `block_count=127` to match precisely what the badge will
actually address, leaving the trailing 448 bytes as harmless, unreachable padding.

Also confirmed directly from `micropython/extmod/vfs.c`'s `mp_vfs_autodetect()` (the code
`vfs.mount()` calls when handed a raw block device): a LittleFS2 image is recognised by the
literal ASCII bytes `"littlefs"` at **byte offset 8** of block 0 (or block 1, as a fallback)
— checked against the actual generated image before trusting any of this on hardware.

## Protocol

**I2C0 / EEPROM**: unchanged from `firmware/phase0-a2-eeprom/`, header format per main
README §1.4. Real VID/PID here, not placeholders — `unique_id` (from the RP2350's own factory
chip ID) still keeps distinct boards from colliding even though the VID/PID is now shared.

**SPI0**: mode 3 (CPOL=1, CPHA=1), CS held low for the whole burst, 16-byte transfers — the
exact protocol confirmed working on real hardware in Phase 0 A2/C3
(`firmware/phase0-a2-spi/`). Byte 0 of the badge's transfer is the requested pattern index
(0–3); the RP2350 applies it to `framebuf[]` and replies with `0xC0, <current pattern>, ...`
on the *next* transfer.

**The one-round ACK lag is deliberate, not a bug.** Full-duplex SPI means a single blocking
transfer's reply can't depend on the byte it's still receiving — so the RP2350 always replies
with the ack for whatever was *already* showing before this transfer, then applies the new
request afterward. The badge app's status line reports this plainly rather than pretending
otherwise; the monitor is the real, near-instant (well under one HSTX frame) proof a command
landed, not the ack byte. See `src/main.c`'s command loop and `badge_app/app.py`'s file header
for both sides of this.

**Known pre-existing quirk (Phase 0 C3):** the very first SPI transfer after the badge
constructs a fresh `machine.SPI` object is occasionally corrupted — a repeatable ESP32-S3
master-side peripheral-init artifact, not a link problem (see
`firmware/phase0-a2-spi/README.md`'s C3 section for the original finding). Not worked around
here, since hiding it would misrepresent how the link actually behaves; every transfer after
the first is reliable.

## Build

RP2350 firmware:

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

Regenerating `src/fs_image.h` after changing `badge_app/` (needs `pip install
littlefs-python` once):

```bash
python tools/build_fs_image.py
```

## Flash

No USB CDC on this firmware, on purpose — I2C0 needs to be live within a few hundred
microseconds of power-good (§5's race-condition mitigation), and TinyUSB init competes with
that budget for no benefit here. Flash via BOOTSEL: hold BOOT, tap RESET (or plug in while
holding BOOT), then `picotool load build/bringup.uf2` (or copy the `.uf2` onto the `RPI-RP2`
drive by hand). `picotool load -f`'s "force reboot over a live USB connection" trick won't
work here or after any other stdio-less Phase 0 firmware — there's no USB serial connection
for it to grab.

## Results (2026-08-30, Metro RP2350, real badge, real monitor)

**Confirmed working end to end on the first attempt.**

**I2C0 / EEPROM / LittleFS**, exercised directly against the badge's own real mount code
(`system.hexpansion.util`'s `detect_eeprom_addr`/`read_hexpansion_header`/
`get_hexpansion_block_devices`, the same functions the real insertion-IRQ handler calls —
not a simulation):

```
I2C scan: ['0x50']
HexpansionHeader[ manifest version: 2026, fs offset: 64, eeprom page size: 64,
  eeprom total size: 65536, vendor id: 0x1969, product id: 0x4544,
  unique id: 49111, friendly name: Hexi-GFX ]
VID/PID confirmed: 0x1969 / 0x4544
eeprom block count: 128
partition block count: 127
partition block size: 512
Mounted OK at /hexpansion_test_3
Contents: ['app.py']
app.py length: 5930
has __app_export__: True
```

Every number matches the design exactly — VID/PID, block count (127, precisely the floored
math above), the packed `app.py` mounts and reads back byte-for-byte correct.

**SPI0 pattern link**, exercised with the same protocol `badge_app/app.py` uses (16 rounds,
cycling all 4 patterns 4 times): every reply carried a valid ack (`0xC0`, valid pattern
index), and — confirmed by eye on the connected monitor — **all 4 patterns (colour bars,
solid red, solid green, checkerboard) cycled correctly on the display in step with the
requests**, with the expected one-round ack lag and no other discrepancy.

**The link works.** Badge discovery → real filesystem → real app → SPI0 command → visible
HDMI change, all confirmed on real hardware, in one firmware image.
