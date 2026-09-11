# Testcard — badge-generated mirror streaming

The first firmware to combine every Phase 0 piece into one image and prove them working
*together*, not just individually: badge inserts a hexpansion → discovers the emulated
identification EEPROM with this project's real assigned identity (**VID `0x1969`, PID
`0x4544`**) → mounts a real LittleFS filesystem living inside that EEPROM → auto-launches the
badge-side app packed into it → that app **builds a 240×240 frame in Python and streams it
continuously over the proven SPI0 link, one burst per row** → the RP2350 receives, buffers,
and displays it live over HDMI, "as if mirrored from the badge" (not a literal mirror of
this badge's own rendered `ctx` screen — that needed `display.get_fb()`, Phase 0 C2's patch;
**merged and proven working end-to-end as of 2026-09-08, see the bottom of this file**).

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
cycled with UP/DOWN (L/R still controls the bars' rotation direction): `checker`, `bunny`,
`spider7`, `spider8`, `bat`, `duck`.

* **`checker`** — 20×20px black/white squares, generated the same way the bars are (raw
  pixel math in Python), no font/ctx involved.
* **`bunny`/`spider7`/`spider8`/`bat`/`duck`** — five unusual codepoints (`U+41E9 U+71E9
  U+81E9 U+BA7A` and `U+21E9`) found in `EMFCampFont.h`'s own glyph-index comment that don't
  belong to any normal alphabet/symbol block — the font's license header credits bundled
  "Solder Party logo" and "Keebdeck icons" as custom additions, and most of these are the
  leftover oddities (`U+21E9`/duck is the one exception — see below). Rendered via an
  **off-screen `ctx.Context(buffer=..., format=ctx.RGB565, ...)` surface backed by a
  bytearray this app allocates and controls itself** — the same underlying
  `ctx_new_for_framebuffer()` the real display driver uses, just pointed at our own buffer
  instead of the badge's screen. This is what makes `ctx`'s real vector font rendering
  available to a hexpansion app *without* needing `display.get_fb()` (Phase 0 C2's still-
  unmerged patch) at all: rendering into a buffer we own and read back ourselves sidesteps
  that blocker entirely, for this one narrow case (rendering static content), even though it
  doesn't solve C2's original goal (reading back the badge's *own live* screen).

**Identified on real hardware, all confirmed 2026-08-31:**

* **`spider7`** (`U+71E9`) and **`spider8`** (`U+81E9`) both genuinely look spider-like (7-
  and 8-leg respectively) — two similar glyphs, not one glyph miscounted via an earlier
  centering bug (re-confirmed after fixing that bug).
* **`bat`** (`U+BA7A`) looks bat-like — not independently confirmed against any outside
  source the way spider/duck were, so this name is a best-guess label, not a certainty.
* **`bunny`** (`U+41E9`) is a genuine, intentionally multi-coloured Easter-egg — its own
  glyph data sets its own fill colours internally (e.g. an embedded `0xFF0000FF` = opaque
  red), overriding whatever colour this app sets beforehand.
* **`duck`** (`U+21E9`) — a separately-sourced claim that this codepoint is a repurposed
  "duck" icon looked implausible purely from the font source (it sits mid-sequence among
  ordinary directional arrows, not among the four genuine oddities above, with an x-advance
  matching its arrow neighbours), and a fetch of a badge docs page repeating the claim wasn't
  trusted either (the fetch tool summarizes through a model regardless of prompt — two
  fetches of the same page gave inconsistent detail). **Tested directly on real hardware
  instead: `U+21E9` genuinely renders as a rubber duck**, not a plain arrow. The positional
  inference was a reasonable hypothesis that turned out wrong — direct hardware testing
  settled it where source-reading and an unreliable web fetch both fell short. The EMF Camp
  logo's own codepoint is still unidentified.

Per-icon colours (requested 2026-08-31, `ICON_COLOURS` in `badge_app/app.py`): `spider7`
white-on-dark-red, `spider8` white-on-dark-green, `bat` pale-lavender-on-dark-purple, `duck`
yellow-on-blue. `bunny` is deliberately excluded — its foreground is set by its own glyph
data regardless of what this app requests.

**A real bug this surfaced**: the `'g'` byte at the start of the bunny glyph's definition was
initially misread here as "this glyph is empty" (its own font-tool-generated comment reads
`/* Nothing to see here */`, which reads as confirmation of that at a glance). It's actually
`CTX_SAVE` (a state push) — checked against `ctx.h`'s opcode table — and real path data
follows immediately after. Worth remembering: a font/asset generator's own comments can be
generic placeholders for a *class* of command, not a claim about the specific glyph.

Screen-switching itself made the known no-frame-sync gap (see fix 1's "Remaining accepted
gap" in "Bugs found and fixed", above) more visible than continuous single-content streaming
had — each switch is a content discontinuity, and confirmed on real hardware, the bars
screen can show transient tearing "mostly after switching" to it from another screen,
self-correcting shortly after. After the per-icon colours above were added, a second
confirming observation on real hardware: switching between differently-coloured icon screens
can briefly show colours mixed between the old and new screen. Same underlying,
already-deferred gap both times, not two separate bugs — the decision to fix it properly (a
real frame-sync marker in `spi0_receive_row()`) is deferred to the PCB-prototype stage
alongside the frame-rate work above.

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

**2026-09-07, real double-buffered framebuf:** the tearing described just above ("top-down
sweep during live updates") turned out not to be fully gone -- still visible as a one-time
shear on every landed frame once badge_app streamed real, continuously-changing content
over an extended session. Root cause: `framebuf` was never actually double-buffered (see
this firmware's own earlier comment on that), only per-row write-gated, which stops
corruption but not a frame showing a mix of old/new rows during the ~240-row copy.
Fixed by shrinking `HEX_EEPROM_SIZE` 64 KiB -> 8 KiB (freeing ~57 KiB of SRAM) and giving
`framebuf` a real second copy, swapped by `dma_irq_handler` only at the start of vblank.
`badge_app/` is no longer packed into the emulated EEPROM's LittleFS (now built empty) --
side-loaded onto the badge directly instead:

```
mpremote fs mkdir :/sideload_test
mpremote fs cp badge_app/app.py :/sideload_test/app.py
mpremote reset   # clean module state -- see below
mpremote resume exec "
import sideload_test.app as sideload_app
from system.hexpansion.config import HexpansionConfig
from system.scheduler.events import RequestStartAppEvent
from system.eventbus import eventbus
from system.scheduler import scheduler
config = HexpansionConfig(2)  # whichever port the hexpansion is in
a = sideload_app.__app_export__(config=config)
eventbus.emit(RequestStartAppEvent(a))
scheduler.run_forever()
"
```

Two real gotchas hit while working this out, worth recording:
* A plain `import app` after `sys.path.insert(0, '/sideload_test')` shadows the framework's
  own fundamental `app.py` (the base `App` class nearly everything imports) with the
  side-loaded file of the same name, breaking unrelated modules elsewhere with confusing
  `ImportError`s. Use a qualified import (`import sideload_test.app`) instead, matching how
  the real launcher does it (`__import__(f"{mount}.app")`) -- never touches the bare `app`
  name.
* `mpremote exec`/`resume exec`'s Ctrl-C doesn't just pause `main.py`'s top-level
  `scheduler.run_forever()`, it unwinds out of it entirely (a real `KeyboardInterrupt`
  through that call). Registering an app with `RequestStartAppEvent` alone does nothing
  further until something calls `scheduler.run_forever()` again -- do that as the last line
  of the exec'd script (run in the background; it never returns) rather than expecting the
  badge to keep ticking on its own afterward.

Confirmed on real hardware, side-loaded via the above: bars streaming continuously, monitor
showing **clean, tear-free** frame swaps -- no visible shear at all, unlike the same test
before this fix.

## 2026-09-08 — the real `display.get_fb()` mirror, working end to end

**The actual product feature this whole firmware was standing in for.** With
`display.get_fb()` merged into `badge-2024-software` (commit `28e1906`), `badge_app/app.py`
gained `MirrorApp`: reads the badge's own live framebuffer and streams it over the same
SPI0 link/protocol `TestcardApp` already proved, instead of generating synthetic content in
Python. Runs entirely in `background_update()`, never requests foreground -- unlike
`TestcardApp`, it must show whatever app the user actually has open, not steal the screen
for itself (`_launch_hexpansion_app` already starts hexpansion apps in the background;
`background_update()` runs for every registered app every ~50ms regardless of foreground
state, while `update()`/`draw()` are foreground-only -- see `system/app.py`'s `App.run()`
docstring).

Getting a real, correct image out of this took three more real, hardware-confirmed fixes
on top of everything above -- each one looked like a plausible complete fix on its own, and
each was wrong or incomplete in a way only real hardware revealed:

1. **Wrong colours.** `undo_madctl_bgr()` (ported from `spaceagon-display-tap`'s
   `phase3-merged`, where it belongs) turned out to be a mistaken port: that fix compensates
   for the *real GC9A01 panel hardware* reinterpreting Red/Blue when `MADCTL_BGR` is set --
   relevant only when physically sniffing the wire between the badge and that real panel.
   This project reads `display.get_fb()` directly in software, never touching that panel's
   hardware at all, so there was nothing to undo. Confirmed on real hardware: applying it
   turned pure red (`0xF800`) into pure blue (`0x001F`), matching exactly the "blue instead
   of red/orange" symptom seen testing the real mirror. Removed.
2. **Image torn into duplicated-edge bands, changing every reflash.** Isolated by writing a
   known 4-quadrant test pattern directly into `framebuf` from C, bypassing SPI/the badge
   entirely -- the corruption persisted even then, ruling out the data pipeline and pointing
   at the DMA/HSTX scanout path itself. Root cause: `spi0_receive_row()` busy-polled the
   SPI0 peripheral's registers in a tight loop continuously, even with nothing connected --
   confirmed by disabling the loop entirely, which produced a clean image. That much
   register traffic from core 0 was enough bus contention to glitch core 1's HSTX/DMA
   scanout (zero timing margin). Fixed by replacing the busy-poll with an interrupt-driven
   receiver (`spi0_rx_irq_handler()` + `spi0_take_row()`, standard PL022 RXIM+RTIM pattern)
   -- core 0 now generates zero SPI-related bus traffic while idle.
3. **Occasional vertical misalignment, self-correcting over a few seconds.** The
   already-documented no-frame-sync-marker limitation, now actually hit in practice: one
   dropped byte anywhere left the badge's and RP2350's row counts out of phase until pure
   chance realigned them. Fixed with `FRAME_MARKER`, an 8-byte pattern sent once before row
   0 of every frame; the RP2350 checks a rolling window against it on every incoming byte
   (deliberately before the row-storage gate, so a marker already waiting for the main loop
   to consume the previous row is never missed) and force-resyncs row counting on a match --
   bounding any drift to at most one frame instead of persisting indefinitely.

**Confirmed on real hardware, badge foreground app left running normally (not a bench
script):** a 4-quadrant test pattern mirrors through with correct colours and no tearing or
drift after all three fixes; then the badge's own **real launcher menu**, and separately its
**GPS clock app**, both mirror through correctly onto the external monitor -- indistinguishable
from the badge's own screen, "as if mirrored from the badge" finally literally true. Measured
frame rate with the GPS app in the foreground: **~0.5 fps**, consistent with `MirrorApp`
sending a whole 240-row frame per `background_update()` call at this link's proven-stable
1 MHz (~940ms/frame of SPI time alone, per `TestcardApp`'s own frame-rate history above) --
well under the main README §3.1 C1 measurement's ~2-14 fps range for real apps, the expected
cost of *not* chunking the send (see `MirrorApp.ROWS_PER_TICK`'s own comment for why
chunking was tried and reverted, and remains open to revisit against the now-interrupt-driven
receiver).

## Pushing towards 15fps (2026-09-08) -- real gains, a real hardware limit, and where to look next

Chasing the main README §3.1 C1 target (~15fps, matching real apps' worst-case render rate)
surfaced two more genuine RP2350-side bugs, fixed properly, plus a hard limit on the badge
side that stopped short of 15fps and needs different tools than a live badge to actually
diagnose.

**RP2350-side, both fixed:** the interrupt-driven receiver (previous section) couldn't keep
up with the raw byte rate once the badge's SPI clock went past ~1MHz -- confirmed on real
hardware as diagonal streaking at 20MHz, traced to per-byte interrupt-entry/register-read
overhead, not a logic bug. Replaced with a DMA-driven receiver (`spi_rx_ring`, a
ring-addressed buffer; `spi0_process_ring()`, called from `main()`'s own loop instead of an
ISR) -- confirmed via a debug-build capture to genuinely decode correct frames at 20MHz.
Getting there also surfaced (and fixed) a real DMA channel conflict (`hstx_dvi_init()`'s
hardcoded `DMACH_PING`/`PONG` were never formally claimed via `dma_channel_claim()`, so the
new SPI0 RX channel's own `dma_claim_unused_channel()` call could -- and once did -- grab
one of them) and added explicit high priority on HSTX's own DMA channels (a second DMA
channel now genuinely competes with HSTX for bus cycles at higher SPI rates; the existing
`bus_ctrl_hw->priority` only ever arbitrated DMA-vs-CPU, never DMA-vs-DMA).

**Badge-side, not resolved:** with the RP2350 side proven correct at 20MHz, the badge's own
ESP32-S3 SPI master turned out to be the actual ceiling. 20MHz crashed it hard -- USB
dropped off the bus entirely, needing a physical power-cycle to recover -- twice, on
otherwise-clean attempts with no debug-probe interference either time. 10MHz produced a
different failure, a silent hang (RP2350 confirmed receiving zero bytes for 6+ seconds while
the badge's own mpremote connection stayed superficially alive, consistent with one blocking
`spi.write()` call wedged forever rather than a full crash). **5MHz is the highest rate
confirmed stable**: no crash or hang across repeated real-hardware tests, badge's own
launcher mirrors through correctly, real frames landing at ~1fps -- 2x the 1MHz baseline,
but still well short of 15fps, and clearly not scaling linearly with clock rate (2MHz gave
about the same ~0.5fps as 1MHz; only 5MHz showed a real jump). That non-linearity is itself
a clue -- see below.

**Two concrete next steps, not yet attempted, both bigger than a config tweak:**

1. **Move `MirrorApp`'s send loop into C.** The non-linear scaling above points at
   Python-level per-row overhead -- a 240-iteration loop, a fresh `bytearray` slice
   allocated every row (`self._frame[r*row_bytes:(r+1)*row_bytes]`, 480 bytes copied each
   time), two `self.cs.value()` GPIO calls per row -- competing with, or exceeding, raw SPI
   transfer time even before considering the ESP32-S3 SPI master's own apparent instability
   above 5MHz. A small custom C user-module (matching how `display`/`ctx` are already
   implemented in `badge-2024-software`'s `drivers/gc9a01/`) doing the byteswap-and-send as
   one tight C loop -- no per-row Python call, no repeated slice allocation -- would remove
   that overhead entirely. Needs a new MicroPython binding exposed from badge firmware, not
   just a badge_app/app.py change.
2. **Move the badge's own SPI transfer onto DMA**, the same idea this session's RP2350-side
   fix already proved out in the other direction: let the ESP32-S3 stream `tildagon_fb`
   directly via hardware DMA instead of a blocking, CPU-driven write per row. Two possible
   wins at once -- removes the remaining per-row CPU cost C alone wouldn't (DMA hands the
   transfer to hardware entirely), and plausibly more stable than the current blocking-write
   approach at higher clock rates, though that's a hypothesis, not confirmed. Needs checking
   whether MicroPython's `machine.SPI` already uses ESP-IDF's own DMA path internally for
   large transfers before assuming a from-scratch driver is required.

Both need real C-level firmware work (a new ESP-IDF component, a MicroPython binding) rather
than badge_app/app.py tweaks, and the 10-20MHz instability specifically needs hardware-level
tools (logic analyzer/scope on the SPI lines) to actually root-cause -- more live
trial-and-error against a real badge already cost two hard crashes needing a physical
power-cycle each time. Diagnose before reattempting higher clock rates blind.

## 2026-09-10/11 — switching to hazanjon's `attach_mirror` sink, and everything that broke on the way there

Migrated off the `display.get_fb()` polling approach above entirely, onto hazanjon's
push-based `display.attach_mirror()` sink (`badge-2024-software` branch `feature/hdmi-mirror`,
upstream PR emfcamp/badge-2024-software#454) -- real ESP32-S3 hardware SPI2 + DMA on the badge
side instead of a Python-driven `machine.SPI` loop, following the plan in
`hdmi-mirror-plan.md`. Four distinct, real bugs surfaced and got fixed, in order, each one
only visible once the previous was actually resolved. All confirmed on real hardware.

**Bug 1 -- `dc` pin driving straight into this side's own MISO line.** hazanjon's driver
always drives a `dc` pin HIGH on attach (`mirror_init_pins()`), defaulting per-port to
`HS_I` -- this repo's own MISO line (README §4.1). Left at that default, the badge hard-reset
in a fast panic-reboot loop the instant the hexpansion was plugged in: genuine electrical
contention between the badge driving that pin and the RP2350's own SPI0 peripheral driving
its TX output, not a software failure. Fixed by redirecting `dc` to GPIO6 in
`badge_app/app.py` (`_DC_OVERRIDE_PIN`) -- port 6's own DC line in hazanjon's PORT_PINS
table, genuinely unconnected as long as nothing is plugged into port 6.

**Bug 2 -- GPIO21/22 have hardware-fixed SPI0 roles; they aren't software-reassignable.**
With bug 1 fixed, `attach_mirror()` returned success and the badge kept transmitting
continuously and successfully -- yet the RP2350 side received exactly **zero bytes**, on
every port tried (1 and 2), at every baudrate tried (1MHz through 20MHz), confirmed via a
raw SSPSR-register poll showing the shift register occasionally going BSY but never once
completing a byte into the RX FIFO (RNE never set). Root cause: this repo's own README §4.1
already documents (from real-silicon verification against `RP2350.svd`) that GPIO21 is
hardware-fixed as SPI0's `spi0_ss_n` (CS) input and GPIO22 as its `spi0_sclk` (SCK) input --
`gpio_set_function(pin, GPIO_FUNC_SPI)` only selects *which peripheral* claims a pin, not
*which signal within it* that pin becomes. hazanjon's own PORT_PINS table assigns the
opposite role to the two middle HS lines (HS_G=SCK, HS_H=CS) relative to this repo's
independently-resolved assignment (HS_G=CS, HS_H=SCK) -- so hazanjon's real SCK signal was
landing on this peripheral's hardware CS input (which only toggles a handful of times per
frame) and his real CS signal was landing on the hardware SCK input (which then only ever
saw a few stray edges, never a real clock). Fixed two ways together: reverted
`src/main.c`'s `PIN_CS`/`PIN_SCK` back to the silicon-correct values (21/22), and physically
rewired the bench connection so `HS_G`/`HS_H` land on RP2350 GPIO22/GPIO21 respectively,
matching hazanjon's PORT_PINS defaults directly -- letting `badge_app/app.py` drop its
`sck`/`mosi`/`cs` overrides to `attach_mirror()` entirely and trust hazanjon's own per-port
defaults (only `dc` still needs overriding, for bug 1).

**Bug 3 -- CPHA needed to be 1, not 0, despite hazanjon's ESP-IDF code hardcoding `.mode = 0`.**
With bug 2 fixed, real bytes finally arrived (`total_bytes_seen` advancing), but the 8-byte
FRAME_MARKER decoded as a stuck `a5a5a5a5a5a5a5a5` instead of alternating `a5`/`5a` (0xA5's
exact bit-complement) -- consistent with a one-half-clock-cycle sampling-edge mismatch, not
a wiring problem. Textbook SPI mode 0 is CPOL0/CPHA0 on both sides and should have matched;
empirically, CPOL0/CPHA1 on the RP2350 side is what actually decodes correctly against this
specific ESP32-S3 hardware-SPI-master + RP2350-hardware-SPI-slave pairing. Root cause not
pinned down further (a GPIO-matrix routing delay specific to non-IOMUX pins, at a guess);
fixed empirically and confirmed repeatedly via real-hardware capture (dozens of frames
landing cleanly, marker matching exactly) at both 1MHz and 5MHz once applied.

**Bug 4 -- the frame-resync marker fought the very reception it was meant to protect.**
With bugs 1-3 fixed, reception looked perfect from the SPI0/DMA layer (`frame_synced=1`
sustained, tens of millions of bytes received) -- but the actual video display stayed stuck
on the fallback test card forever. Root cause, confirmed via a `dma_irq_handler` swap
counter: the front/back buffer swap happened exactly **once**, then never again, despite
15M+ bytes and roughly 130 frames' worth of continuous traffic afterward. The frame-sync
marker (`spi0_row0_pending`, added 2026-09-08 to recover from a dropped byte) forced
`row_index` back to 0 on *every* marker match, unconditionally -- fine when markers were
rare relative to how long a frame took (the old 1MHz per-row-burst badge this was designed
against), but the badge now sends full frames continuously back-to-back, so a marker arrives
roughly once per frame *regardless* of whether the RP2350 has actually finished consuming
the previous frame's 240 rows yet. Every new marker was interrupting an already-in-progress,
otherwise-healthy frame before it could complete, so `row_index` essentially never wrapped
239→0 via its own natural increment. Fixed with `bytes_since_marker`/
`MIN_BYTES_BETWEEN_MARKERS`: a marker match is now only honored once ~239 rows' worth of
bytes have actually been seen since the last accepted one (once synced), so an early/false
match falls through and gets treated as ordinary row data instead of resetting anything.
Confirmed on real hardware: swap count climbed continuously in lockstep with "frame landed"
events (40+ in one capture) instead of sticking at 1, and real, correctly-interactive badge
UI content (menu text, correct white/highlighted-colour selection state) became visible on
the external monitor for the first time this project has ever shown it.

**Fast iteration setup: sideloading `badge_app/app.py` onto the badge's own flash.** Debugging
bugs 1-4 needed many Python-only iterations; rebuilding+repacking the RP2350's own EEPROM
image for each one (§ "Real EEPROM-hosted app restored", above) was the slow part of the
loop. `modules/system/hexpansion/app.py`'s own `_launch_hexpansion_app()` already has a
built-in fallback for exactly this case: if the hexpansion's mounted EEPROM filesystem has no
importable `app` module, it tries `/drivers/hex_{vid:04x}_{pid:04x}/app` on the badge's own
flash instead -- a real, existing mechanism, not something patched in. `tools/build_fs_image.py`
now supports `EMPTY_FS=1` to leave the packed filesystem region empty (no `app.mpy`), and
`badge_app/app.py` gets copied straight onto the badge's flash at `/drivers/hex_1969_4544/`
(with a blank `__init__.py`) via `mpremote fs cp` -- physical hexpansion insertion still
triggers everything for real, but a Python-only edit only needs one `mpremote fs cp`, no
RP2350 rebuild/reflash at all. Revert to packing `app.mpy` into the real EEPROM image (drop
`EMPTY_FS`, matching the 2026-09-08 "fix it properly" decision) once satisfied with the
Python side; the sideload path was always meant as a debugging convenience, not the shipped
end state.

**Debugging tool note: `DEBUG_SERIAL`'s USB-CDC output actively broke video.** `bringup_debug`
used to route `printf()` over `stdio_usb` (TinyUSB). This shares clock infrastructure with
`clk_sys`, which `clk_hstx` (video) follows undivided at the 126MHz this firmware sets for
640x480@60 -- confirmed on real hardware to disrupt HSTX's own zero-timing-margin video
output (production `bringup`, with no `DEBUG_SERIAL` code at all, never showed this; enabling
USB debug output on `bringup_debug` reliably did). Fixed by switching `bringup_debug` to
UART0 on GPIO0/1 instead (the Adafruit Metro RP2350's own labelled TX/RX pins, confirmed
unused elsewhere in this firmware, its own board header's own stdio default) -- a genuinely
separate clock domain. Read via the Raspberry Pi Debug Probe's own UART bridge (its second,
separate 3-pin JST-SH connector, not the SWD one) rather than a second on-target USB port;
needed a Debug Probe firmware update (1.0.1 → 2.3.1, from
`raspberrypi/debugprobe`'s own GitHub releases) before that bridge actually passed data
through. **Anything using `bringup_debug` from here on should still be treated as
disturbing video** -- use it to check the SPI0/DMA receive state (which doesn't care about
video timing at all), not to judge whether the picture looks right; use plain `bringup` for
that.

**Known, real, remaining limitations -- not further fixable without changes outside this
repo's own scope:**

1. **The whole badge UI blocks for the duration of every mirror transmission.**
   `mirror_sink_send_frame()` runs synchronously inside `display.end_frame()`, called
   directly from whichever app is currently rendering (typically the Launcher) --
   MicroPython here is single-threaded and cooperative, so there is no way to background
   this call from the Python side. ~184ms per 115,200-byte frame at 5MHz (~0.92s at 1MHz).
   The only lever available without patching hazanjon's C driver is baudrate -- 5MHz is the
   highest confirmed clean and self-resyncing on this bench dupont-wire setup (20MHz failed
   to sync entirely: `frame_synced` stuck at 0, corrupted marker values, despite 1M+ bytes
   arriving -- consistent with this project's own repeated conclusion elsewhere that higher
   rates need a real PCB, not dupont wire, to be reliable).

   This is a genuine, unavoidable architectural conflict, not specific to this bench setup
   -- confirmed by reading `badge-2024-software`'s own scheduler source. The Launcher's own
   `update()` (`modules/system/launcher/app.py`) never returns `False`, and `App.run()`
   (`modules/app.py`) triggers a re-render on every single iteration unless `update()`
   explicitly returns `False` -- so the Launcher redraws continuously at roughly 20fps (a
   50ms pacing built into `mark_update_finished()`) purely to keep its own animated
   background (`bg.update(delta)`) smooth, regardless of whether the menu or mirrored
   content has actually changed. Since that's faster than one mirror transmission can
   complete at any baudrate confirmed clean on this bench setup, `attach_mirror()` will
   always be "behind" while the Launcher is foregrounded, queuing the next block before the
   previous one even finishes. hazanjon's own PR demo photos showing both displays working
   don't contradict this -- a still photo can't show whether the badge felt sluggish while
   it was taken, and if that demo was also shot from the Launcher, it would have hit the
   exact same blocking. A foreground app that explicitly returns `False` from `update()`
   when genuinely idle (no animated background, redrawing only on real state changes) would
   reduce how often this blocking actually triggers, without needing any change outside
   this repo -- worth trying if perceived responsiveness matters more than always mirroring
   from the Launcher specifically.

   **A real Doom-on-both-screens demo video seen separately** (external monitor genuinely
   mirroring the badge's own display, smooth and full-speed) doesn't necessarily contradict
   this either, on reflection -- `display.c` does expose a second, independent `Screen` type
   with its own `end_frame()` (`mp_display_screen_end_frame`, separate from the module-level
   `display.end_frame()` `attach_mirror()` hooks into), but that theory doesn't fit once the
   mirrored content is confirmed to genuinely track the primary screen. More likely: a Doom
   engine on this hardware probably already renders at a fairly modest native framerate
   (raycasting on a microcontroller, not a GPU) -- if that native rate is already in the same
   ballpark as one mirror transmission's own duration (~184ms/frame, ~5.4fps, at 5MHz), the
   *added* latency from mirroring is comparatively small and easy not to notice, especially
   in a moving 3D scene where a human eye is far less sensitive to an extra frame or two of
   lag than to an absolute framerate number. The Launcher is the opposite case: its own
   *native* target is a smooth ~20fps (the animated background alone demands it), so the
   exact same absolute mirroring cost is a much larger *relative* drop -- 20fps to ~5fps is
   immediately, jarringly obvious, where a already-modest Doom framerate dropping by a
   similar absolute amount may not be. Not independently confirmed against the actual Doom
   build's own frame timing -- offered as the most plausible reconciliation, not a proven
   fact.
2. **An occasional one-byte transmission drift**, independent of clock speed. Confirmed via
   a raw row-byte dump (`ROWDUMP`) at both 5MHz and 1MHz: two captures of the same row, 30
   frames apart, showed the identical repeating byte pattern shifted by exactly one byte
   (`4a 00 4a 00...` vs `00 4a 00 4a...`) -- the same artifact at both rates rules out signal
   integrity/clock rate as the cause. Since a uniform background colour decodes to two
   different RGB565 values depending on which byte-phase is active, this is what produces
   the visible alternating green/blue background bars (text is naturally more forgiving of a
   one-byte shift than a smooth background fill is). Most likely an occasional interrupt/
   task-switch hiccup on the badge's own ESP32-S3 corrupting a byte mid-transfer. This
   repo's own resync mechanism can only correct at full-frame boundaries by design (the
   protocol has no denser sync signal than one marker per frame) -- catching a genuine
   mid-frame single-byte glitch would need a change inside hazanjon's C driver, which this
   project's own plan (`hdmi-mirror-plan.md`) explicitly avoids patching.

**Important scope correction, written the same day: the practical, reliable end-to-end
result has NOT actually been achieved yet -- most attempts show no signal on the monitor
at all.** Everything above this line is real and independently verified: the SPI0/DMA
receive layer is proven correct via direct instrumentation (UART debug capture showing
`frame_synced=1` sustained, dozens of "frame landed" events, `swap_count` advancing in
lockstep) -- that part of the pipeline genuinely works, in isolation, when checked directly.
But that verification happened over a debug channel, with the badge's own screen and the
external monitor not necessarily being watched at the same moment, and the one time
recognizable menu content with correct selection-highlight colouring was actually seen on
the monitor, it wasn't cleanly reproducible on demand afterward -- most later attempts (on
plain `bringup`, no debug interference) showed no signal at all rather than either the
fallback test card or real content. The exact cause of that unreliability is still open;
it is NOT the same bug as bugs 1-4 above (those are confirmed fixed at the protocol level)
and is NOT fully explained by the USB-CDC/HSTX clock-sharing issue either (plain `bringup`
never touches USB stdio at all). Treat every "confirmed on real hardware" claim above as
scoped to exactly what it verified (the byte-level protocol), not as a claim that the
monitor reliably shows a clean picture end to end -- it has not, so far.

**A separate, real, permanent hang was also found and is believed fixed, but needs
re-confirmation.** Independent of the video-signal unreliability above: repeated
insert/remove cycles could leave the badge in a state that never recovers even after 60+
seconds -- not the ~184ms-per-frame slowness in limitation 1 above, a genuine deadlock.
`mirror_sink_send_frame()` calls `spi_device_acquire_bus(mp->spi, portMAX_DELAY)` (an
*unbounded* wait) and only reaches the matching `spi_device_release_bus()` at the very end
of the function, with no early-return path and no error-path cleanup in between -- if
anything goes wrong during the header or payload transmit, the bus is left permanently held
by a context that can never release it, and every future call blocks forever waiting for a
release that will never come. A test with `attach_mirror()` deliberately skipped (see
`_ATTACH_DELAY_MS`/`background_update()` in `badge_app/app.py`) confirmed the app
load/registration sequence alone is completely clean and the badge stays fully responsive
with mirroring never started -- isolating the hang specifically to `attach_mirror()`/the
send loop, not to anything about hexpansion detection or EEPROM loading (both independently
confirmed solid on both stock and hazanjon firmware). Deferring the first `attach_mirror()`
call by 1.5s past insertion (letting insertion-time housekeeping settle first) appeared to
prevent the permanent hang across a 40-second continuous real-hardware capture (100
successful `send_frame` cycles, no interruption) -- but this result needs treating with
real caution: the first attempt at confirming it accidentally ran against a badge that
turned out to still have stock firmware flashed (`AttributeError: 'module' object has no
attribute 'attach_mirror'`, caught harmlessly by this app's own try/except -- of course
there's no hang if the function being called doesn't exist), giving a false-positive "no
hang" reading with two physical badges in play during testing. Re-flashing hazanjon's
firmware fresh and re-running the same test did show `attach_mirror` succeeding
repeatedly with no hang across another full capture -- but given how easily this specific
test can silently pass for the wrong reason, do not treat the 1.5s delay as a confirmed fix
without independently re-verifying `attach_mirror returned OK` (not a caught
`AttributeError`) appears in the very same test run.

**Also open: the badge's own native screen can go completely static while mirroring runs,
independent of both issues above.** Confirmed even where the SPI/mirror send loop is
verifiably still succeeding continuously (100 real `send_frame complete` cycles logged) --
the badge's own round display showed zero perceptible change whatsoever over an extended
period, not merely slow. Since `flow3r_bsp_display_send_fb()` (the real native-panel write)
runs unconditionally before `dispatch_sinks()` in the same `tildagon_blit_fb()` call the
mirror hooks into, a successfully-completing mirror send implies the native write path was
reached too -- so this isn't simply "the render loop is stuck." The most likely
explanation, not yet confirmed: the Launcher's own background-animation code
(`bg.update(delta)`, see limitation 1 above) computes its animation step from `delta`
(elapsed wall-clock time since the previous frame); at the ~184ms per frame this now takes
(versus the ~50ms it was designed and tested against), an unusually large `delta` could
alias to the exact same modular animation position on every call, producing genuinely
identical rendered output frame after frame rather than a stuck render loop. Unconfirmed;
would need reading `badge-2024-software`'s own background-pattern code directly, not yet
done.
