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
      `third_party/ctx/VENDORED.md`). Builds clean; timing numbers not
      yet captured on hardware (see below).
- [ ] B2 — microSD (SPI0, GPIO34–40) + PSRAM-backed 640×480 16bpp scanout.
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

- **typical** — a handful of rounded-rect cards with icon circles and a
  header gradient, roughly the complexity of a simple Tildagon app screen.
- **worst case** — ~160 overlapping translucent circles/rects plus a
  full-screen radial gradient, to stress alpha-compositing and AA hard.

These are **not** a drawlist captured from a real badge app — that needs
the badge itself plus the `display.get_fb()` upstream patch (Phase 0 Part
C, not started). They're a reasonable stand-in for "how fast can this
chip rasterise ctx content" until a real captured drawlist exists; see
`third_party/ctx/VENDORED.md` for the caveats. `ctx_bench_run()` skips
itself (prints why) if PSRAM isn't available or is too small for the
framebuffer.

ctx was configured stripped down for this target: no text/fonts, no
XML/parser/formatter/events (desktop-oriented features), and only the one
pixel format actually used — see the `#define`s at the top of
`ctx_bench.c`. Adds ~110 KB to the flash image (`arm-none-eabi-size`:
~169 KB text total vs. ~59 KB before ctx was added) — a non-issue against
16 MB of flash.

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

Uses the pico-sdk toolchain already cached by the Raspberry Pi Pico VS Code
extension at `~/.pico-sdk` (SDK 2.3.0 and matching picotool 2.3.0 were
added there for this project — see above). From this directory:

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
the self-test result (PASS/FAIL, time, throughput), then the B1 ctx
benchmark's two scene timings, once. Then the red LED (next to
BOOT/RESET) blinks at 1 Hz with a `phase0-metro alive: tick N` heartbeat
every 500 ms.
