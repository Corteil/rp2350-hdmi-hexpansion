# Phase 0, Part A — HSTX DVI bring-up (Stage 1)

Bench firmware for the Feather in the design's [Phase 0
plan](../../README.md#phase-0--de-risk-on-the-bench-23-weekends), Part A —
specifically **A1** (scale-during-scanout) and **A3** (640×480@60 DVI
output on a real monitor).

Board: **Adafruit Feather RP2350 with HSTX**, plus the Adafruit RP2350
22-pin FPC HSTX-to-DVI adapter and a monitor.

## What this is (Stage 1, not the full A1 yet)

The real A1 target is a 240×240 source, pillarboxed and circular-masked,
scaled 2× into 640×480 — see main README §3.1/§3.4. That's a substantial
per-scanline command table (the mask boundary varies every line). Rather
than write and test all of that blind, this stage proves the more basic
plumbing first:

- **320×240 source doubled to fill 640×480 exactly** — no pillarbox, no
  circular mask needed, since 320×2=640 and 240×2=480 land exactly on the
  output size. Once this works, the harder pillarboxed/masked 240×240
  case is an extension, not a from-scratch rewrite.
- **Horizontal 2× done by pre-expanding into 640-wide rows in SRAM at
  init**, not via HSTX/DMA's pixel-duplication register trick. Sidesteps
  a fiddly bit-packing detail for this first attempt; vertical 2× is done
  the simple way regardless (re-reading each source row twice).
- **Two-DMA-channel ping-pong + one IRQ per scanline** (from the official
  [`raspberrypi/pico-examples`](https://github.com/raspberrypi/pico-examples)
  `hstx/dvi_out_hstx_encoder` example), not the more CPU-efficient
  one-IRQ-per-*frame* ring-buffer mechanism. The ring-buffer approach
  (used by Adafruit's own PicoDVI driver — see below) is real and worth
  adopting once this simpler, fully-traced version is confirmed correct on
  hardware — CPU load is what A1 actually needs to measure, just not
  from this stage.

Two references, both credited in `src/main.c`'s file header:
- **[`raspberrypi/pico-examples`](https://github.com/raspberrypi/pico-examples)**
  `hstx/dvi_out_hstx_encoder` (BSD-3) — the DMA ping-pong mechanism, the
  standard 640×480@60 timing constants (`V_BACK_PORCH=33`, CEA-861 VIC 1),
  and the overall command-list structure this firmware is built from.
- **[`adafruit/circuitpython`](https://github.com/adafruit/circuitpython)**
  `ports/raspberrypi/common-hal/picodvi/Framebuffer_RP2350.c` (MIT) — the
  RGB565 `expand_tmds`/`expand_shift` register values (proven on real
  HSTX+DVI hardware), and confirmation of the BSWAP-during-DMA idea for
  RGB565. **Not used for timing** — that file's own `MODE_640_V_BACK_PORCH`
  is 133, not the standard 33, for reasons unclear; the spec-correct
  pico-examples numbers are used instead, matching main README §3.3.

## Feather-specific pin mapping

Confirmed from [Adafruit's Feather RP2350 pinout
page](https://learn.adafruit.com/adafruit-feather-rp2350/pinouts), **not**
assumed from the Pico DVI Sock pinout pico-examples' own comment
describes (different board, different mapping):

| GPIO | Signal |
|------|--------|
| 12/13 | TMDS lane 2 |
| 14/15 | Clock |
| 16/17 | TMDS lane 1 |
| 18/19 | TMDS lane 0 |

Color-channel-to-lane assignment (which of R/G/B is "lane 0" and so on)
is inferred from Adafruit's driver's own internal naming (`red` feeds
`expand_tmds`'s L0 field in their code) matched against their pinout
page's own "Lane0/1/2" terminology — consistent, but not seen stated
explicitly as "GPIO18/19 carries red" in so many words. If the test
pattern's colours come out channel-swapped, that's a one-line fix
(swap two entries in `lane_to_output_bit[]` in `src/main.c`) — a cheap,
easily visually diagnosed thing to get wrong, not a hardware risk.

## Test pattern

8 vertical colour bars, 80px each: white / yellow / cyan / green /
magenta / red / blue / black — the classic order. Lets one glance confirm
both geometry (bars should be sharp, evenly spaced, no skew) and colour
wiring (each bar is a known, named colour) at once.

## Build

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

## Flash

Hold BOOT, tap RESET (or plug in while holding BOOT), copy
`build/bringup.uf2` onto the `RPI-RP2` drive. No USB CDC console output
from this firmware (it's pure video output, no `stdio` use in the render
path) — the only thing to check is the monitor.

## What to check

1. **Does a picture appear at all** — the fundamental TMDS/HSTX/DMA/timing
   chain working, independent of anything else.
2. **Is it stable** — no flickering, rolling, or loss of sync.
3. **Are there exactly 8 sharp, evenly-spaced vertical bars filling the
   whole 640×480 area**, in the order listed above — confirms the
   doubling math and DMA read addressing are right.
4. **Are the colours right** (white/yellow/cyan/green/magenta/red/blue/
   black, in that order) — confirms the RGB565 TMDS config and the
   lane/pin mapping. If channel-swapped (e.g. red and blue traded), see
   the note above — cheap, expected-possible, easy fix.

## Results (2026-08-30, Adafruit Feather RP2350 with HSTX)

**Working, after three real bugs found and fixed on hardware:**

1. **Ribbon cable inserted backwards** — showed as "no signal" on the
   monitor. Diagnosed by flashing CircuitPython and running Adafruit's own
   `picodvi` example (same resolution, same adapter) — it also showed
   nothing until the cable was reseated correctly, which isolated the
   problem to the physical connection rather than any code, ours or
   Adafruit's.
2. **Wrong pixel clock** — `clk_hstx` follows `clk_sys` undivided by
   default (confirmed via [RP2350 datasheet](https://pip.raspberrypi.com/documents/RP-008373-DS-rp2350-datasheet.pdf)
   §12.11.4), and the real pixel clock is `clk_hstx`/5 (fixed by the
   `N_SHIFTS=5` DDR scheme) — not adjustable via `CSR.CLKDIV`, which is
   only the *clock generator* output's own period and must equal
   `N_SHIFTS` to keep clock and data aligned. An initial attempt to "fix"
   the pixel clock by changing `CLKDIV` (5→6) was wrong and made things
   worse (regressed from "out of range" to "no signal" — see git history).
   The actual fix: `set_sys_clock_khz(126000, true)` at the top of
   `main()`, confirmed against
   [`Panda381/DispHSTX`](https://github.com/Panda381/DispHSTX)'s own
   tested video mode table, which explicitly uses "system clock 126 MHz"
   for 640×480@60.
3. **Colour channels rotated** — see the comment above `bar_colours[]` in
   `src/main.c`. White and black round-tripped correctly (colour-order
   invariant), every other colour came out as a fixed, consistent
   substitute — a clean 3-way rotation, not a scramble. Fixed by
   empirically inverting the observed substitution in the test pattern's
   colour table. **This is a test-pattern-level fix, not a root-cause
   one** — real badge pixel data can't be pre-rotated like this for free,
   so the actual `expand_tmds`/lane-mapping bug this is compensating for
   still needs finding before Stage 2.

**Fully confirmed on hardware, including the colour fix**: 8 sharp,
evenly-spaced, correctly-coloured vertical bars (white/yellow/cyan/green/
magenta/red/blue/black, left to right), stable (no flicker/rolling/sync
loss). Black bars either side on a widescreen monitor are expected —
640×480 is a 4:3 signal, shown at native size on a 16:9 panel rather than
stretched. **Stage 1 is done.**

## Next: the real A1

Once this is confirmed, extend to the actual A1 target: 240×240 source,
2× scaled, pillarboxed (80px black bars each side, since 640−480=160),
circular-masked (matching what the badge's own round display would show).
That needs a per-scanline command table where the "real pixel data" width
varies with a circle equation, computed once at mode-set — see main
README §3.4. Also worth then measuring actual CPU load against §3.4's
2–5% estimate, and (per B3/A2) comparing the ring-buffer one-IRQ-per-frame
mechanism against this stage's simpler one-IRQ-per-scanline approach.
