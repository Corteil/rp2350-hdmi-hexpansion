# Phase 0, Part A — HSTX DVI bring-up (Stage 2: the real A1 target)

Builds on [`../phase0-dvi/`](../phase0-dvi/) (Stage 1, confirmed working
on real hardware — three bugs found and fixed there: flipped ribbon
cable, wrong pixel clock, rotated colour channels; see its README). This
is the actual **A1** target from the design's [Phase 0
plan](../../README.md#phase-0--de-risk-on-the-bench-23-weekends): a
240×240 source, scaled 2×, pillarboxed and circular-masked into 640×480 —
main README §3.1/§3.4.

Board: **Adafruit Feather RP2350 with HSTX**, plus the HSTX-to-DVI
adapter and a monitor.

## What's new vs. Stage 1

- **240×240 source** (not 320×240), pre-doubled horizontally in software
  into 480-wide rows in SRAM — same technique as Stage 1.
- **Pillarbox**: 80px of black on each side of the 480-wide doubled
  content (640−480=160, ÷2=80) — always present.
- **Circular mask**: `row_half_width[]`, one value per output row
  (0–479), computed once at init from the circle equation (diameter
  480, centred on the 480×480 square) — "the per-line descriptor table
  for the circular mask" main README §3.4 describes. Rows near the
  vertical centre show close to the full width; rows near the top/bottom
  show progressively less, down to nothing outside the circle's radius.
- **CPU load measurement**: a busy-loop counter, timed for a fixed window
  before HSTX/DMA starts and again after — printed over USB CDC. The
  design's own estimate (main README §3.4) is 2–5% of one core.

## Known limitations of this measurement (read before trusting the number)

- **Per-row IRQ overhead is higher here than the design's target
  mechanism.** Each active row now takes 3 DMA-completion IRQs (left
  black fill / pixel data / right black fill) instead of Stage 1's 2, on
  top of Stage 1's own already-simpler-than-the-real-design starting
  point (two-channel per-scanline IRQ, not the CPU-efficient
  one-IRQ-per-*frame* ring-buffer mechanism the actual product firmware
  should use — see Stage 1's README). **Treat the measured CPU load as an
  upper bound**, not the number the real design will achieve.
- **USB CDC adds its own interrupt overhead**, present in this build's
  "loaded" measurement but absent from a hypothetical console-free build.
  Stage 1 had no USB CDC at all (pure video, no diagnostics beyond the
  LED) for exactly this reason; Stage 2 trades a small amount of measurement
  purity for actually being able to read the CPU-load number out.
- The busy-loop technique itself is coarse (counts loop iterations in a
  fixed wall-clock window, compares to a no-DVI baseline) — good enough
  to sanity-check "is this in the same ballpark as 2-5%, or way off,"
  not a precise profiler.

## Command-list structure (why 3 phases, not 2)

Stage 1's active rows had 2 DMA transfers: one static command-list buffer,
then one straight read of a full 640-wide pre-doubled row. Here the
*visible* width varies per row (from `row_half_width[]`), so each active
row's command list is now:

1. **Left fill** — front porch / hsync / back porch preamble (unchanged
   from Stage 1) + `HSTX_CMD_TMDS_REPEAT` for `left_black` pixels of pure
   black + `HSTX_CMD_TMDS` priming the expander for the pixel data that
   follows in phase 2. (RP2350 datasheet §12.11.5 confirms the command
   expander treats the FIFO as one continuous stream, so a command word
   arriving via this transfer correctly primes it for the *next* DMA
   transfer's raw pixel words — no command word needed in phase 2 itself.)
2. **Pixel data** — `half_width` words read directly from `framebuf`, at
   a per-row column offset so the visible span is horizontally centred.
3. **Right fill** — `HSTX_CMD_TMDS_REPEAT` for `right_black` pixels.

`left_black` and `right_black` are always equal by construction — the
mask is centred, so whatever's cut from the visible span splits evenly.
Fully-masked rows (`half_width == 0`, near the very top/bottom) skip
phases 2 and 3 entirely — one `TMDS_REPEAT` command covers the whole
640px line.

Per-channel scratch buffers (`active_left[2][]`, `active_right[2][]`,
indexed by which of the two ping-pong DMA channels is being armed) avoid
a race: the IRQ handler only ever writes the buffer belonging to the
channel that just finished (guaranteed idle by the ping-pong discipline
itself), never a buffer the other channel might still be mid-read from.

## Test pattern

Same 8-bar pattern and colour table as Stage 1 (see its README for how
those colour values were empirically derived), scaled to the new content
width: 60px/bar (480/8) instead of 80px/bar (640/8).

## Build

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

## Flash

Hold BOOT, tap RESET, copy `build/bringup.uf2` onto `RPI-RP2`. Unlike
Stage 1, this build **does** have USB CDC — open a serial terminal on its
port to see the CPU-load measurement print once at boot.

## What to check

1. **On the monitor**: a circle (not a square) of 8 coloured bars,
   pillarboxed and letterboxed by black, roughly filling the 480×480
   region centred in the 640×480 frame. Top and bottom of the circle
   should curve smoothly, not step raggedly (the mask is computed
   per-pixel-row, so it should look genuinely round, not blocky) —
   though at 480px diameter some visible stepping at extreme angles is
   normal for a pixel-quantised circle, not a bug.
2. **Stability** — no flicker, rolling, or sync loss (same as Stage 1).
3. **On the USB serial console**: the CPU load percentage. Sanity-check
   against the caveats above rather than the raw design target.

## Results

Not yet run on hardware.
