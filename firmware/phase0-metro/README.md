# Phase 0, Part B — Adafruit Metro RP2350 bring-up

Bench firmware for the Metro RP2350 bring-up in the design's [Phase 0
plan](../../README.md#phase-0--de-risk-on-the-bench-23-weekends), Part B
(the PSRAM + microSD bench, RP2350B).

Board: **Adafruit Metro RP2350 with PSRAM** (RP2350B, 8 MB PSRAM, 16 MB
flash — the PSRAM variant, not the plain Metro RP2350).

## Status

- [x] Toolchain + custom board header bring-up: blink the onboard red LED
      (GPIO23), heartbeat over USB CDC. Builds clean against pico-sdk 2.2.0.
- [ ] **B1 — ctx rasterisation benchmark at 640×480** (the highest-value
      Phase 0 measurement — see README §8). Needs ctx vendored
      (single-header C, LGPL-3.0-or-later) and a representative drawlist.
- [ ] PSRAM bring-up (GPIO47, QMI CS1). **Deliberately not attempted yet**
      — see note below.
- [ ] B2 — microSD (SPI0, GPIO34–40) + PSRAM-backed 640×480 16bpp scanout.
- [ ] B3 — run the same firmware on the RP2350A Feather (Part A) to compare.

### Why PSRAM init isn't in yet

The RP2350 PSRAM QMI dance (direct-mode ID read, then switching XIP CS1 into
quad mode with M1_TIMING/M1_RFMT/M1_RCMD/M1_WFMT/M1_WCMD) isn't in pico-sdk
2.2.0 as a library — every board that has it vendors their own driver. The
most complete reference implementation found (fanpico's `psram.c`) is
GPL-3.0-or-later, which is a heavier obligation than we want on firmware
this board's design already has one LGPL dependency in (ctx, for B1) — worth
a deliberate call, not picking up by accident. Options before writing B2:
write a clean-room version from the RP2350 datasheet's QMI register
documentation, or find a permissively-licensed reference (Pimoroni's
`pimoroni-pico` repo is one to check).

## Board header

`boards/adafruit_metro_rp2350.h` is a hand-written board header — Adafruit
doesn't ship one for bare pico-sdk (only Arduino-Pico and CircuitPython).
Pin numbers came from Adafruit's own pinout page
(learn.adafruit.com/adafruit-metro-rp2350/pinouts); the PSRAM CS pin
(GPIO47) is the RP2350B's fixed QMI CS1 alternate-function pin, not a
board-specific choice, confirmed against Pimoroni's Pico Plus 2 RP2350
board header in pico-sdk (same pin).

## Build

Uses the pico-sdk toolchain already cached by the Raspberry Pi Pico VS Code
extension at `~/.pico-sdk`. From this directory:

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.2.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.2.0-a4/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

Or just open this folder in VS Code with the Raspberry Pi Pico extension —
it already has the same SDK/toolchain cached.

## Flash

Hold BOOT, tap RESET (or plug in while holding BOOT) to drop the Metro into
UF2 bootloader mode, then copy `build/bringup.uf2` onto the `RPI-RP2` drive
that appears. Or, with the board already in USB-CDC-running mode:

```bash
picotool load -f build/bringup.uf2
```

## Expected result

Red LED (next to BOOT/RESET) blinks at 1 Hz. Opening the board's USB CDC
serial port shows a `phase0-metro alive: tick N` line every 500 ms.
