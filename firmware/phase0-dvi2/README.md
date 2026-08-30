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
  mechanism.** Each active row now takes 4 DMA-completion IRQs (command
  list / left black fill / pixel data / right black fill) instead of
  Stage 1's 2, on top of Stage 1's own already-simpler-than-the-real-design
  starting point (two-channel per-scanline IRQ, not the CPU-efficient
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

## Command-list structure — one HSTX command per row, not three

**First hardware attempt at this stage used three separate
`HSTX_CMD_TMDS`/`TMDS_REPEAT` *commands* per active row (left fill,
pixel-data-priming, right fill) and got "no signal" on the monitor** —
the board stayed alive (LED heartbeat, CPU-load measurement both
completed fine), but no valid picture, worse than Stage 1's "out of
range." Root cause: RP2350 datasheet §12.11.5 — "the command expander
cannot output data on the cycle where it pops a command from the FIFO,
so the expansion shift register is empty for at least one cycle."
Stage 1's proven active line has exactly **one** command word
(`HSTX_CMD_TMDS|640`); the three-command version added two extra
command-boundary stalls per active row that blanking lines don't have,
almost certainly breaking the constant per-line duration a monitor needs
to lock horizontal sync to.

**Fixed by restructuring to match Stage 1's command shape exactly**: one
`HSTX_CMD_TMDS|640` command per active row (`vactive_line`, byte-for-byte
Stage 1's version), consuming its declared 320-word budget across up to
three separate DMA *data* transfers with no command words in between:

1. **Command phase** — `vactive_line`'s single `TMDS|640` command
   (unchanged from Stage 1).
2. **Left fill** — `left_black/2` words of pure black, read from
   `zero_buf` (a static all-zero SRAM buffer, sized for the worst case —
   see its comment for why it's deliberately *not* `const`/flash-resident).
3. **Pixel data** — `half_width` words read directly from `framebuf`, at
   a per-row column offset so the visible span is horizontally centred.
4. **Right fill** — `right_black/2` words, also from `zero_buf`.

`left_black` and `right_black` are always equal by construction (the mask
is centred), and — since `PILLARBOX` alone is 80px — always nonzero, so
phases 2 and 4 always run for every active row; only phase 3 (real pixel
data) can be zero-length, on fully-masked rows near the very top/bottom
of the circle (a zero-length DMA transfer completes immediately and is
harmless, no special-casing needed). The word budget always sums to
exactly 320 (`left_black/2 + half_width + right_black/2` — the pixel-level
identity `left_black + 2·half_width + right_black = 640` divided by 2),
matching `vactive_line`'s declared consumption precisely.

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

## Results (2026-08-30, Adafruit Feather RP2350 with HSTX)

**Working, after one real bug found and fixed on hardware** (see the
"Command-list structure" section above for the full story — first
attempt used three separate HSTX commands per active row and got "no
signal"; fixed by restructuring to Stage 1's proven single-command shape).

**Confirmed on the monitor**: a circle of 8 coloured bars, correctly
pillarboxed and circular-masked, centred in the 640×480 frame. The
circle's outline shows some pixel-level stepping — expected, from the 2px
horizontal quantisation needed for word-aligned DMA reads (documented
above, not a bug) — geometry and colours otherwise correct, stable (no
flicker/rolling/sync loss).

**Colour path since changed (2026-08-30):** the above run used Stage 1's
empirically-compensated `bar_colours[]`/`expand_tmds` (correct-looking
output, root cause not yet found). Both are now replaced with the actual
root-cause fix — see `expand_tmds`'s comment in `src/main.c` and
[`../phase0-dvi-colorfix/README.md`](../phase0-dvi-colorfix/README.md).
Builds cleanly with the new values; not yet re-flashed on the physical
Feather to re-confirm visually (the fix is board-independent register
math, already hardware-confirmed on a Metro RP2350, so this is a
formality rather than an open question — but genuinely not yet done).

**CPU load measurement**:

```
CPU load baseline (DVI not yet running): 1399971 busy-loop iterations / 200ms
CPU load with DVI running: 1304061 busy-loop iterations / 200ms
-> approx 6.9% of one core spent servicing the DVI scanout IRQ
```

**6.9% is close to the design's own 2–5% estimate** (main README §3.4),
despite this stage deliberately using a less efficient mechanism than the
real design calls for (4 DMA-completion IRQs per active row instead of
the target one-IRQ-per-*frame* ring-buffer approach, plus USB CDC's own
interrupt overhead present in this measurement but not in a console-free
build). A more optimised implementation should land inside the original
estimate. **This is a strong validation of the whole mirroring
architecture's core CPU-budget assumption** — the thing A1 exists to
check.
