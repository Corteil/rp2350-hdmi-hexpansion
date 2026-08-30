# Phase 0, Part B — Adafruit Metro RP2350 bring-up

Bench firmware for the Metro RP2350 bring-up in the design's [Phase 0
plan](../../README.md#phase-0--de-risk-on-the-bench-23-weekends), Part B
(the PSRAM + microSD bench, RP2350B).

Board: **Adafruit Metro RP2350 with PSRAM** (RP2350B, 8 MB PSRAM, 16 MB
flash — the PSRAM variant, not the plain Metro RP2350).

## Status

- [x] Toolchain + custom board header bring-up: blink the onboard red LED
      (GPIO23), heartbeat over USB CDC.
- [x] **PSRAM bring-up** (GPIO47, QMI CS1) via pico-sdk's official
      `hardware_psram` library. Auto-detects size and self-tests by
      writing/reading back a pattern across the whole chip, timed.
- [x] **B1 — ctx rasterisation benchmark at 640×480** (the highest-value
      Phase 0 measurement — see README §8). `ctx` vendored under
      `third_party/ctx/` (single-header C, LGPL-3.0-or-later — see
      `third_party/ctx/VENDORED.md`). **Measured on hardware: 2.9 fps
      typical scene, 0.7 fps worst case — too slow for animation.**
      Decomposed to find out why: see below. Main README §3.2 and risk
      17 updated with these numbers.
- [x] **B2, microSD half** — SPI0 (GPIO34/35/36 SCK/MOSI/MISO, CS GPIO39)
      via `carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` (Apache-2.0) +
      elm-chan's FatFs, vendored as a submodule under `third_party/`.
      **Measured on hardware: mount, capacity report, and write/read-back
      all PASS** (FAT32, ~7.5 GB card). Card-detect (GPIO40) deliberately
      unused — see below.
- [x] **B2, PSRAM-backed scanout half** — DMA-driven bulk reads out of
      the PSRAM framebuffer (`src/psram_dma_bench.c`), simulating what
      real HSTX scanout would do (§3.3's "needs measurement" mode, target
      ~37 MB/s). B1's numbers only covered CPU-driven writes *into*
      PSRAM; this is the read side, DMA case — a materially different
      access pattern. Builds clean; not yet run on hardware (see below).
- [ ] B3 — run the same firmware on the RP2350A Feather (Part A) to compare.

### PSRAM: using pico-sdk's official driver, not a hand-rolled one

The RP2350 PSRAM QMI dance (direct-mode ID read, then switching XIP CS1
into quad mode with M1_TIMING/M1_RFMT/M1_RCMD/M1_WFMT/M1_WCMD) isn't
something we wrote ourselves. It turns out **pico-sdk 2.3.0 (released
2026-07-03) added an official `hardware_psram` library** — BSD-3, written
by Raspberry Pi, with auto-detection of both the CS pin and chip size. That
landed after this repo started, so it wasn't in the pico-sdk 2.2.0 this
project began on.

This avoided a real alternative: the most complete third-party reference we
found (fanpico's `psram.c`) is GPL-3.0-or-later, which would have been a
heavier license obligation than intended — this design already carries one
LGPL dependency (ctx, for B1), and picking up a second, stronger copyleft
one for something as core as PSRAM deserved a deliberate decision, not an
accident. The official driver made that decision moot.

Board header just sets `PICO_PSRAM_CS_PIN 47` and
`PICO_AUTO_DETECT_PSRAM_SIZE 1` — the same idiom Raspberry Pi's own
`adafruit_feather_rp2350.h` uses, for the same reason (Adafruit sells this
board both with and without PSRAM fitted). PSRAM then initializes itself
before `main()` runs; `src/main.c` just calls `psram_is_available()` /
`psram_get_size()` and runs a write/read-back self-test.

**Upgrading to SDK 2.3.0 required a matching `picotool` 2.3.0** — the SDK's
CMake enforces picotool/SDK version parity. No prebuilt Windows binary
ships in picotool's own GitHub releases (source tarball only), but
`raspberrypi/pico-sdk-tools` release `v2.3.0-1` has
`picotool-2.3.0-x64-win.zip`, installed at `~/.pico-sdk/picotool/2.3.0/`
alongside the existing 2.2.0-a4 (kept, in case other projects still target
2.2.0).

### B1: ctx benchmark

`src/ctx_bench.c` renders two hand-built scenes into a 640×480
RGB565_BYTESWAPPED framebuffer in PSRAM, 10 iterations each, timed with
min/avg/max and derived fps:

- **raw write** — not a ctx scene: a plain sequential scalar write of the
  same byte count straight to the PSRAM framebuffer pointer. Reference
  upper bound for "solid fill" below.
- **solid fill** — the cheapest possible ctx draw call: one opaque,
  axis-aligned, full-canvas rectangle. Isolates "cost of one full-canvas
  pass through ctx's rasteriser" from gradient/shape cost.
- **typical** — a handful of rounded-rect cards with icon circles and a
  header gradient, roughly the complexity of a simple Tildagon app screen.
- **worst case** — ~160 overlapping translucent circles/rects plus a
  full-screen radial gradient, to stress alpha-compositing and AA hard.

The first measurement run used ctx's own default `CTX_RASTERIZER_AA`
(15) instead of the badge's actual setting (5); fixed, but the fix only
changed timing by ~7% — nowhere near the 3x expected — which is why
**raw write** and **solid fill** were added: to find out what's actually
dominating (PSRAM write bandwidth vs. ctx's own rasterisation cost)
instead of guessing.

These are **not** a drawlist captured from a real badge app — that needs
the badge itself plus the `display.get_fb()` upstream patch (Phase 0 Part
C, not started). They're a reasonable stand-in for "how fast can this
chip rasterise ctx content" until a real captured drawlist exists; see
`third_party/ctx/VENDORED.md` for the caveats. `ctx_bench_run()` skips
itself (prints why) if PSRAM isn't available or is too small for the
framebuffer.

#### Measured results (Metro RP2350 with PSRAM, 2026-08-30)

```
raw write   min   69.69 ms  avg   70.63 ms ( 14.2 fps)   [~8.70 MB/s]
solid fill  min  169.29 ms  avg  169.38 ms (  5.9 fps)
typical     min  343.10 ms  avg  343.21 ms (  2.9 fps)
worst case  min 1426.38 ms  avg 1426.57 ms (  0.7 fps)
```

Reading the decomposition: **raw write** (70.6 ms) is a hard floor —
even a hypothetically free rasteriser can't beat it, because that's just
what it costs to push 614 KB into PSRAM one scalar store at a time. ctx's
**solid fill** (169 ms, the cheapest possible draw call) is ~2.4× that
floor, meaning roughly 40% of even the cheapest frame's cost is PSRAM
write bandwidth rather than ctx's own rasterisation work. **typical**
(343 ms) is ~2× solid-fill; **worst case** (1426 ms) is ~8.4× solid-fill,
reflecting the full-screen radial gradient (expensive per-pixel
evaluation) plus 160 shapes with alpha blending.

DMA wasn't tried and wouldn't help here regardless: ctx's rasteriser
fills spans with plain C stores, not DMA, no matter where the target
buffer lives. DMA only helps the *scanout* (HSTX) side, which this
benchmark doesn't touch.

**Conclusion:** full-frame ctx drawlist forwarding can't support
animation on this chip (2.9 fps best case, hand-built scene). It may
still work for occasional full redraws of static screens (e.g. a
settings menu redrawn once on navigation) where a few hundred ms of
latency is acceptable. See the main README §3.2 and risk 17 for what
this means for the design.

ctx was configured stripped down for this target: no text/fonts, no
XML/parser/formatter/events (desktop-oriented features), and only the one
pixel format actually used — see the `#define`s at the top of
`ctx_bench.c`. Adds ~110 KB to the flash image (`arm-none-eabi-size`:
~169 KB text total vs. ~59 KB before ctx was added) — a non-issue against
16 MB of flash.

### B2: PSRAM-backed scanout (DMA reads)

`src/psram_dma_bench.c` measures the opposite direction from B1: DMA
engine reads *out of* the PSRAM framebuffer, rather than ctx's CPU-driven
writes into it. This is what real HSTX scanout would actually be doing —
§3.3 rates this mode "Medium — needs measurement", target ~37 MB/s for
640×480@60. Two shapes, mirroring B1's bulk-vs-decomposed approach:

- **bulk** — one 256 KB DMA transfer, PSRAM into a static SRAM buffer.
  The achievable ceiling, no per-transaction setup cost.
- **per-line** — 480 separate DMA transfers of one 1280-byte
  (640×RGB565) scanline each, back to back, totalling one frame's worth
  of bytes. Closer to how real chained per-line HSTX descriptors would
  actually drive this (§3.4) — captures per-transaction setup overhead
  the bulk number doesn't.

Reads via the PSRAM **uncached** XIP alias (`XIP_NOCACHE_NOALLOC_BASE`,
not the `XIP_BASE`-relative one main.c/ctx_bench.c use) deliberately: real
scanout reads different framebuffer content every frame, so it should
never hit in the small on-chip XIP cache. Using the cached alias here
could make repeated timing iterations look faster than a real streaming
scanout would be — same spirit as B1's raw-write/solid-fill decomposition,
measure the thing that's actually representative rather than whatever's
convenient.

DMA channel is claimed with `dma_claim_unused_channel`, so this coexists
fine with anything else on the board that also uses DMA (SD card SPI
DMA, USB). Skips itself if PSRAM isn't available or no DMA channel is
free.

#### Measured results (Metro RP2350 with PSRAM, 2026-08-30)

```
bulk     256 KB in one transfer   min   9.21 ms  avg   9.22 ms  max   9.27 ms  [~27.13 MB/s]
per-line 480 x 1280 B (one frame)   min  21.64 ms  avg  21.65 ms  max  21.68 ms  [~27.07 MB/s, 46.2 fps]
```

DMA is a real win over B1's CPU-scalar path — ~3.3× B1's raw-write
number (27.1 vs 8.3 MB/s) — and bulk vs. per-line being nearly identical
confirms per-transaction DMA setup overhead isn't the bottleneck; this
is close to the actual sustained hardware ceiling for whatever clock
PSRAM is running at.

**But 27 MB/s falls short of the ~37 MB/s continuous 640×480@60 scanout
actually needs** (not a target to merely beat — that's the literal
sustained byte rate a fixed 60 Hz signal demands: 640×480×2 bytes ×
60 fps ≈ 35.2 MB/s). A ~27% shortfall. Added a diagnostic to `main.c`'s
PSRAM section (`clk_sys` frequency + the QMI M1 CLKDIV register value)
to find out whether this is simply a conservative default clock divisor
(fixable) before concluding anything stronger — not yet run against
hardware.

### B2: microSD

`src/sd_bench.c` + `src/hw_config.c`, on top of a vendored (submodule)
copy of `carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` under
`third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/` (Apache-2.0, wraps
elm-chan's FatFs — its own permissive BSD-1-Clause-style license). Chose
this over hand-rolling the SD-over-SPI protocol (CMD0/CMD8/ACMD41 init
sequence, R1/R7 response parsing, CRC, the 400 kHz-then-fast-clock dance)
for the same reason as pico-sdk's `hardware_psram`: a well-tested existing
implementation beats a hand-written one for something this fiddly to get
right at the protocol level.

**File-API only, deliberately never raw sectors.** This runs against
whatever card is actually inserted on the bench, which may have other
data on it — a distinctly-named test file (`phase0test.txt`,
`phase0bw.bin`, deleted after use) can't clobber that, a raw sector write
could.

`hw_config.c` wires the library to this board's socket: SPI0 on GPIO34
(SCK) / 35 (MOSI) / 36 (MISO), CS on GPIO39, matching
`boards/adafruit_metro_rp2350.h`. **Card-detect (GPIO40) is deliberately
not wired up** — its active-high/low polarity for this specific socket
isn't confirmed against Adafruit's schematic, and a wrong guess would
make the code misreport "no card" rather than just letting `f_mount()`'s
own success/failure say so.

`sd_bench_run()` mounts, prints capacity/free space, runs the
write/read-back correctness test, then (only if that passed) times
writing a 256 KB file in 4 KB chunks for a rough file-I/O bandwidth
number. Prints a clear "skipped" message and returns cleanly — never
panics — if there's no card or the mount fails for any reason.

**Needed a prebuilt `pioasm`, not just a matching SDK/picotool.** The
library's CMake unconditionally builds its SDIO backend too (even though
we only use SPI), which needs `pioasm` to compile a `.pio` file. Building
`pioasm` from source needs a host C++ compiler, which isn't installed on
this machine (see the PSRAM section above re: the same gap blocking a
from-source `picotool` build). Fix: `raspberrypi/pico-sdk-tools` release
`v2.3.0-1`'s `pico-sdk-tools-2.3.0-x64-win.zip` bundle has a prebuilt
`pioasm.exe`, installed at `~/.pico-sdk/tools/2.3.0/pioasm/`. `CMakeLists.txt`
points at it by default (`pioasm_DIR`) so this is transparent on rebuild.

#### Measured results (Metro RP2350 with PSRAM, ~7.5 GB FAT32 card, 2026-08-30)

```
mounted: 7503.9 MB total, 7503.9 MB free, FAT type 3
write/read-back test (phase0test.txt): PASS
write bandwidth: 262144 bytes in 287.75 ms (~0.87 MB/s)
```

Mount, capacity reporting, and the write/read-back correctness test all
passed cleanly — confirms the SPI wiring, chip select, and FatFs stack
all work on real hardware. FAT type 3 = FAT32, as expected for a card
this size.

**0.87 MB/s is a deliberately conservative number, not a driver
limitation.** `hw_config.c` set `baud_rate` to 12.5 MHz for first
bring-up (reliability over speed); at that clock, 1.5 MB/s is the
single-bit-SPI theoretical ceiling, so ~0.87 MB/s (58% of that) is in
the range expected once per-block command/response overhead is
accounted for. Raising the SPI clock (common SD-over-SPI cards handle
20-25 MHz) is the obvious next tuning step if this number ends up
mattering, but wasn't necessary to prove B2's actual question — does
this stack work at all on this board — which it does.

## Board header

`boards/adafruit_metro_rp2350.h` is a hand-written board header — Adafruit
doesn't ship one for bare pico-sdk (only Arduino-Pico and CircuitPython).
Pin numbers came from Adafruit's own pinout page
(learn.adafruit.com/adafruit-metro-rp2350/pinouts); the PSRAM CS pin
(GPIO47) is the RP2350B's fixed QMI CS1 alternate-function pin, not a
board-specific choice, confirmed against Pimoroni's Pico Plus 2 RP2350 and
Raspberry Pi's own Adafruit Feather RP2350 board headers in pico-sdk (same
pin, same PSRAM idiom).

## Build

This repo has a submodule (the SD/FatFs library); clone or update it first:

```bash
git submodule update --init third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
```

Uses the pico-sdk toolchain already cached by the Raspberry Pi Pico VS Code
extension at `~/.pico-sdk` (SDK 2.3.0, matching picotool 2.3.0, and a
prebuilt `pioasm` were added there for this project — see above). From
this directory:

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

Or open this folder in VS Code with the Raspberry Pi Pico extension — point
its SDK version setting at 2.3.0.

## Flash

Hold BOOT, tap RESET (or plug in while holding BOOT) to drop the Metro into
UF2 bootloader mode, then copy `build/bringup.uf2` onto the `RPI-RP2` drive
that appears. Or, with the board already in USB-CDC-running mode:

```bash
picotool load -f build/bringup.uf2
```

## Expected result

On boot, the USB CDC serial port prints PSRAM detection (size, CS pin),
the self-test result (PASS/FAIL, time, throughput), the B1 ctx
benchmark's scene timings, the B2 PSRAM-scanout DMA read timings, then
the B2 SD card result (capacity, write/read-back PASS/FAIL, bandwidth —
or a clear "skipped" line if no card is inserted), once. Then the red
LED (next to BOOT/RESET) blinks at 1 Hz with a `phase0-metro alive: tick
N` heartbeat every 500 ms.

An SD card is optional for this firmware to run — if none is inserted
(or FatFs can't mount it), B2 just prints why and moves on to the
heartbeat loop like everything else did.
