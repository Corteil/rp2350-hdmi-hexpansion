# Testcard — badge-generated mirror streaming

The first firmware to combine every Phase 0 piece into one image and prove them working
*together*, not just individually: badge inserts a hexpansion → discovers the emulated
identification EEPROM with this project's real assigned identity (**VID `0x1969`, PID
`0x4544`**) → mounts a real LittleFS filesystem living inside that EEPROM → auto-launches the
badge-side app packed into it → that app **builds a 240×240 frame in Python and streams it
continuously over the proven SPI0 link, one burst per row** → the RP2350 receives, buffers,
and displays it live over HDMI, "as if mirrored from the badge" (not a literal mirror of
this badge's own rendered `ctx` screen — that needs `display.get_fb()`, Phase 0 C2's patch,
still unmerged; see below).

Every layer here was proven individually in Phase 0. This firmware was originally a
pattern-select demo (RP2350 held a handful of fixed test patterns, badge picked among them)
— rewritten 2026-08-31 so the badge generates and streams real, changing pixel content
instead, exercising the link the way the actual mirroring product will.

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
  │  builds one 480-byte │─────────────────►│ spi0_receive_row(): FIFO  │
  │  row (240 RGB565 px, │  mode 3, CS-      │  byte-stream, no CS-edge  │
  │  colours rotating),  │  framed bursts    │  detection (see "Bugs     │
  │  streams it 240x/    │                   │  found" below) — stages  │
  │  frame continuously  │                   │  into frame_staging[],   │
  └───────────────────────┘                   │  swaps into framebuf[]  │
                                               │  once per full frame    │
                                               └────────────┬─────────────┘
                                                            │
                                               HSTX (GPIO12-19, colour-
                                               correct per phase0-dvi-
                                               colorfix, Stage 2 pillar-
                                               boxed/circular-masked
                                               geometry) ──► monitor

  Metro NeoPixel (GPIO25): RED 1Hz heartbeat (wall-clock, alive even with
  nothing connected) — briefly GREEN whenever a full 240-row frame lands.
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
  Stage 2's pillarboxed/circular-masked 240×240→480×480-in-640×480 geometry, matching the
  badge's own round 240×240 display rather than a generic test card), the SPI0 slave row
  receive loop, a NeoPixel heartbeat/link-activity indicator, and an RP2350-generated
  analog-TV-style static pattern shown at boot and whenever the link goes idle.

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

**SPI0**: mode 3 (CPOL=1, CPHA=1), one CS-framed burst per row, 480 bytes/burst — 240 RGB565
pixels, little-endian per pixel (`badge_app/app.py`'s `_build_row()` packs low byte first,
matching `src/main.c`'s `uint16_t` cast). The badge sends 240 bursts back-to-back per frame,
continuously, for as long as the app runs — there is no command byte and no reply/ack; this
is a one-way stream, not a request/response protocol like the pattern-select demo this
firmware used to be. `spi0_receive_row()` on the RP2350 side reads it as one continuous byte
stream (see "Bugs found and fixed" below for why, and for the one gap that leaves).

**Known pre-existing quirk (Phase 0 C3):** the very first SPI transfer after the badge
constructs a fresh `machine.SPI` object is occasionally corrupted — a repeatable ESP32-S3
master-side peripheral-init artifact, not a link problem (see
`firmware/phase0-a2-spi/README.md`'s C3 section for the original finding). Not worked around
here, since hiding it would misrepresent how the link actually behaves; every transfer after
the first is reliable.

## Bugs found and fixed (2026-08-30/31, all confirmed on real hardware)

**1. `gpio_get()` doesn't reflect this pin's real level while it's configured
`GPIO_FUNC_SPI`.** The original receive loop tried to frame each row by polling
`gpio_get(PIN_CS)` for edges. On this board/SDK combination it read `0` on every single poll
— including multi-second idle stretches with nothing plugged in — so the "wait for CS low"
and "abort if CS goes high mid-burst" logic never actually did anything; the function was
really just "flush the FIFO, then drain up to 480 bytes," running unsynchronized against a
continuous stream. Result: ~5 successful rows out of ~6000 attempts, all by timing luck.
**Fix:** stopped relying on CS at the GPIO level entirely. The badge sends each row as one
unbroken 480-byte burst with no filler bytes between rows, so the RX FIFO is already
naturally row-aligned as long as the receive side never discards or fabricates a byte.
`spi0_receive_row()` now just drains the FIFO into a byte counter that **persists across
calls** (so a row arriving a few bytes at a time, between other main-loop work, still
accumulates correctly) and only resets once a full row completes. Went from ~5/6000 to
continuous, reliable reception.

**Remaining accepted gap from fix 1:** with no CS-level resync, an interrupted mid-row
transfer (e.g. the hexpansion physically removed mid-burst) leaves the byte counter partway
through a row with no way to tell; the next bytes received, from whatever comes next, are
mis-attributed to finishing that stale row — a persistent misalignment until the RP2350 is
reset. Acceptable for a bench demo where the link is a continuous stream once started; a
product firmware would want a frame-sync marker or per-row CRC to detect and recover from
this instead.

**2. Writing `framebuf` from the CPU while HSTX's DMA concurrently scans it out could
disrupt already-marginal per-line timing.** A full unconditional rewrite of the whole 230KB
buffer (the original boot-time "no signal" static fill, called from the idle path) held the
bus long enough to break the display outright — confirmed by direct A/B testing on real
hardware. **Fix:** `framebuf_row_write_is_safe()` checks the DMA's current scan position
(from the shared, now-`volatile` `v_scanline`) before writing any given row, and skips
writing only that one row if the scan position is about to reach it — every other row is
always safe regardless of what the CPU is doing. This let the idle static refresh speed up
from 8 rows/100ms to a full-buffer pass every idle tick, without reintroducing the bus
contention that made the slow throttle necessary in the first place.

**3. Single-buffered `framebuf` produced a visible top-down "sweep"** as each live row landed
mid-scanout. A real second framebuffer would fix this outright but doesn't fit — another
230,400 bytes against ~220KB of free SRAM (512KB total RAM region minus the existing
framebuf, the 64KB emulated-EEPROM RAM copy, and code/other statics). **Fix:**
`frame_staging[]` holds incoming rows at source resolution (240 px, not the pre-doubled 480)
— 115,200 bytes, half the cost — and `framebuf` only gets touched once a full frame has
landed in staging, as one row-safety-gated pass. The display now holds the previous complete
frame steady for the ~1s it takes the badge to stream the next one, then swaps cleanly,
instead of visibly filling in row by row.

## Frame rate: ~5fps, capped by the badge's own scheduler, not this link

Original per-row-burst/1MHz protocol managed ~0.77fps (one new frame roughly every 1.3s).
Moving to one CS-framed burst per frame (all 240 rows, 115,200 bytes) at 20MHz — both
choices matching Phase 0 C3's own proven, real-hardware-measured configuration
(`firmware/phase0-a2-spi/README.md`) — plus precomputing the 8 possible colour-rotated rows
once at startup instead of rebuilding one every frame, got this to **~5fps**, confirmed by
two independent measurements: the badge's own on-screen timing (`avg=203ms/frame`) and the
RP2350's own row-arrival serial timestamps, in close agreement.

**Investigated further (2026-08-31) for a 15fps target and found a hard architectural
ceiling, not a bug to fix here.** `badge-2024-software/modules/app.py`'s `run()` loop awaits
`mark_update_finished()` after every `update()`, which contains an **unconditional
`await asyncio.sleep(0.05)`** — a hardcoded 50ms-per-tick floor in the framework's own
foreground-app update cycle, not something a hexpansion app can bypass. Stacked with the real
SPI wire time (~47ms at 20MHz, already near that baudrate's theoretical minimum) and roughly
100ms of further overhead not yet isolated (asyncio scheduling, the render/draw cycle, other
background tasks sharing the same single-threaded cooperative event loop), 15fps is not
reachable through the framework's normal foreground update() loop as currently written,
independent of how fast the SPI transfer itself is made.

**Accepted as-is; revisit once a real PCB prototype exists**, not on this bench's devkit
connector + dupont-wire leg. Phase 0 C3 already found real bit errors at 30MHz+ on that exact
wiring, so pushing SPI baudrate further wasn't attempted here without first re-validating
reliability on better wiring — a real PCB (no dupont leg, direct short traces) is the most
promising remaining lever and the right place to re-run C3's speed sweep before assuming a
higher rate is safe.

## Multiple test screens

Beyond the rotating colour bars, `badge_app/app.py` now has 6 more selectable screens,
cycled with UP/DOWN (L/R still controls the bars' rotation direction):

* **`checker`** — 20×20px black/white squares, generated the same way the bars are (raw
  pixel math in Python), no font/ctx involved.
* **`icon0`..`icon4`** — five unusual codepoints (`U+41E9 U+71E9 U+81E9 U+BA7A U+BA7B`) found
  in `EMFCampFont.h`'s own glyph-index comment that don't belong to any normal
  alphabet/symbol block — the font's license header credits bundled "Solder Party logo" and
  "Keebdeck icons" as custom additions, and these are the leftover oddities. Rendered via an
  **off-screen `ctx.Context(buffer=..., format=ctx.RGB565, ...)` surface backed by a
  bytearray this app allocates and controls itself** — the same underlying `ctx_new_for_framebuffer()`
  the real display driver uses, just pointed at our own buffer instead of the badge's screen.
  This is what makes `ctx`'s real vector font rendering available to a hexpansion app
  *without* needing `display.get_fb()` (Phase 0 C2's still-unmerged patch) at all: rendering
  into a buffer we own and read back ourselves sidesteps that blocker entirely, for this one
  narrow case (rendering static content), even though it doesn't solve C2's original goal
  (reading back the badge's *own live* screen).

**Identified on real hardware (2026-08-31):** icon1 (`U+71E9`, 7-leg) and icon2 (`U+81E9`,
8-leg) both look genuinely spider-like — two similar glyphs, not one glyph miscounted via an
earlier centering bug (re-confirmed after fixing that bug, below); icons 3/4
(`U+BA7A`/`U+BA7B`) look bat-like; icon0 (`U+41E9`) is a genuine, intentionally
multi-coloured Easter-egg bunny — its own glyph data sets its own fill colours internally
(e.g. an embedded `0xFF0000FF` = opaque red), overriding whatever colour this app sets
beforehand, which is why it renders in red/blue rather than the white every other icon uses.
A separately-sourced claim that `U+21E9` is "duck" does not hold up against the font file
itself (that codepoint sits mid-sequence among ordinary directional arrows, not among the
five oddities above). A badge docs page lists this font's mascot glyphs as "shark, duck,
spider, bats" — consistent with what's confirmed here (spider + plural bats) but doesn't
pin down which codepoint is duck vs shark; that page's own fetched content wasn't consistent
across two fetch attempts (a tooling limitation — content gets summarized through a model
regardless of prompt), so specific per-codepoint claims from it aren't treated as verified,
only the general mascot-category list, which real hardware corroborates. The duck glyph's
codepoint, and the EMF Camp logo's, remain unidentified.

**A real bug this surfaced**: the `'g'` byte at the start of icon0's glyph definition was
initially misread here as "this glyph is empty" (its own font-tool-generated comment reads
`/* Nothing to see here */`, which reads as confirmation of that at a glance). It's actually
`CTX_SAVE` (a state push) — checked against `ctx.h`'s opcode table — and real path data
follows immediately after. Worth remembering: a font/asset generator's own comments can be
generic placeholders for a *class* of command, not a claim about the specific glyph.

Screen-switching itself made the known no-frame-sync gap (see fix 1's "Remaining accepted
gap" in "Bugs found and fixed", above) more visible than continuous single-content streaming
had — each switch is a content discontinuity, and confirmed on real hardware, the bars
screen can show transient tearing "mostly after switching" to it from another screen,
self-correcting shortly after. Same underlying, already-deferred gap, not a new one — the
decision to fix it properly (a real frame-sync marker in `spi0_receive_row()`) is deferred to
the PCB-prototype stage alongside the frame-rate work above.

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

## Results

**2026-08-30, first attempt:** I2C0/EEPROM/LittleFS discovery confirmed end to end against
the badge's own real mount code (`system.hexpansion.util`'s `detect_eeprom_addr`/
`read_hexpansion_header`/`get_hexpansion_block_devices`, the same functions the real
insertion-IRQ handler calls — not a simulation):

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
math above), the packed `app.py` mounts and reads back byte-for-byte correct. (That first
attempt's SPI0 half used the original fixed-pattern-select protocol, since replaced — see
above.)

**2026-08-31, after the badge-generated-streaming rewrite and the three bugs above:**
confirmed on real hardware, badge inserted and app running normally (not a bench script):

* A `bringup_debug` (USB CDC + instrumentation) capture showed **16,560+ successfully
  received rows over a 90-second window** of continuous streaming — up from ~5 out of ~6000
  attempts before fix 1 — with the sampled row content cycling through the same repeating
  colour sequence every 240 rows, confirming full frames were completing, not just
  individual bytes landing.
* On the real `bringup` (no-USB) target: monitor shows the badge's colour bars, confirmed
  **rotating colour** (not scrolling position) in real time.
* NeoPixel confirmed **red 1Hz** with nothing streaming, and **green flashing** once per
  completed 240-row frame while the badge app runs — both observed directly by eye.
* Idle/no-signal static confirmed visibly faster/more chaotic after fix 2 ("static is much
  better").
* The top-down sweep during live updates confirmed **gone** after fix 3 ("sweep is gone,
  clean update now") — frames now appear to swap in one clean update instead of filling in
  row by row.

**The link works, and now carries badge-generated content.** Badge discovery → real
filesystem → real app → badge-built frame data streamed continuously over SPI0 → clean,
tear-free, colour-rotating HDMI output — all confirmed on real hardware, in one firmware
image.
