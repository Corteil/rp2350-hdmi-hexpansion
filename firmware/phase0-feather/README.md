# Phase 0, Part A / B3 — Adafruit Feather RP2350 (+ HSTX) bring-up

Bench firmware for the Feather in the design's [Phase 0
plan](../../README.md#phase-0--de-risk-on-the-bench-23-weekends) —
RP2350A, the chip variant the actual hexpansion design targets (see
README §4.1, "Why the RP2350A and not the B").

Board: **Adafruit Feather RP2350 (+ HSTX)**, RP2350A, no PSRAM populated
by default (DNP footprint) unless someone's soldered an APS6404L onto it.

## What this is (and isn't)

Part A's real job — HSTX DVI mirroring (A1), pin-map validation against
the badge's hexpansion edge connector (A2), DVI output on a real monitor
(A3), current draw (A4) — needs the badge, a protoboard hexpansion, an
HSTX-to-DVI adapter, and a monitor. None of that is here yet.

**This is B3**: reusing `firmware/phase0-metro`'s PSRAM and ctx
benchmarks — both board-agnostic, no board-specific pins involved — to
get RP2350A vs. RP2350B numbers to compare against the Metro's results.
`src/ctx_bench.c`, `src/psram_dma_bench.c` and `third_party/ctx/` are
copies of the Metro's, not shared library code — see the note in
`firmware/phase0-metro/README.md` (and the design's own general
"duplication over premature abstraction" instinct) for why: this is
disposable Phase 0 bench code, not the eventual product firmware, so two
small independent copies beat coupling two separate bench projects
together.

**No SD card bench** — the Feather has no microSD slot, unlike the Metro.

**No custom board header needed** — unlike the Metro, Adafruit's Feather
RP2350 already has an official pico-sdk board header
(`adafruit_feather_rp2350.h`, RP2350A, LED GPIO7, PSRAM CS GPIO8 with
`PICO_AUTO_DETECT_PSRAM_SIZE` since Adafruit sells this board both with
and without PSRAM). `CMakeLists.txt` just sets `PICO_BOARD
adafruit_feather_rp2350`.

## PSRAM: populated or not, this runs either way

Both benchmarks already check `psram_is_available()` and skip themselves
cleanly (printing why) if PSRAM isn't detected. If you see them skip,
that itself answers "is PSRAM populated on this specific board" — no
separate check needed.

## Build

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

## Flash

Hold BOOT, tap RESET (or plug in while holding BOOT) to drop the Feather
into UF2 bootloader mode, then copy `build/bringup.uf2` onto the
`RPI-RP2` drive that appears. Or, with the board already running:

```bash
picotool load -f build/bringup.uf2
```

## Expected result

Same shape as the Metro's: USB CDC prints PSRAM detection (or a clear
"not populated" line), the self-test, the `clk_sys`/PSRAM-clock
diagnostic, the B1 ctx benchmark, then the B2 PSRAM-scanout DMA
benchmark — once. Then the LED (GPIO7) blinks at 1 Hz with a
`phase0-feather alive: tick N` heartbeat every 500 ms.

## Results

Not yet run on hardware.
