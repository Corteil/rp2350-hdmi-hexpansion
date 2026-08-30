# Phase 0, A1 — root-causing the colour channel rotation

Bench firmware for the **Metro RP2350**, not the Feather — see the main README's Phase 0
Part A ([A1](../../README.md#phase-0--de-risk-on-the-bench-23-weekends)) "Still open" note.
`firmware/phase0-dvi/` (Feather, Stage 1) found a clean 3-way colour rotation on real
hardware and worked around it with an empirically-inverted test-pattern colour table — a
test-pattern-level fix, not usable for real badge pixel data. This firmware is the actual
root-cause fix, derived from first principles and confirmed on real hardware, then backported
unchanged into `phase0-dvi/` and `phase0-dvi2/`.

## Why the Metro, not the Feather

The board available to test with at the time. Not a problem: HSTX is silicon-fixed to
GPIO12–19 regardless of RP2350A/RP2350B package (HSTX bit index `n` is always GPIO `12+n`),
and Adafruit's own Metro RP2350 pinout page confirms its dedicated 22-pin HSTX connector
breaks out exactly GPIO12–19 as D0P/D0N/D1P/D1N/D2P/D2N/CKP/CKN — the identical mapping
`phase0-dvi` already uses on the Feather. **The main README previously claimed the Metro has
"no HSTX peripheral broken out" — that was wrong**, corrected alongside this firmware. A fix
that's purely register-level TMDS math (this one) transfers between boards with zero risk;
DMA/HSTX/timing mechanics (already proven separately on the Feather in `phase0-dvi`/
`phase0-dvi2`) are untouched here.

## The root cause

Not a simple "swap two lanes" bug. Two separate mistakes compounded:

1. **Wrong lane order.** `phase0-dvi` copied Adafruit's PicoDVI driver's `expand_tmds`
   ROT/NBITS values under the assumption L0=Red, L1=Green, L2=Blue. The real DVI/HDMI TMDS
   channel convention is **L0=Blue, L1=Green, L2=Red** — not alphabetical, a well-known trap.
2. **Wrong bit-field math even for the assumed lane order.** Per pico-sdk's
   `hardware/regs/hstx_ctrl.h` (straight from the SVD): each lane's `ROT` is a **right-rotate**
   applied to the current 32-bit shifter word before that lane's encoder, and `NBITS` is the
   valid-bit count "starting from bit 7 of the rotated data" — so a channel's own MSB must
   land exactly on bit 7 after rotation. Adafruit's specific ROT values don't decode to a
   clean single-channel window under this formula against this project's own RGB565 packing
   (two pixels/word, earlier pixel in the low 16 bits, standard R\[15:11\]/G\[10:5\]/B\[4:0\]
   layout) — they must assume a different internal convention Adafruit's own driver uses
   internally, not stated anywhere obvious in their source.

**Validated the formula against a known-good reference before trusting it**: fetched
`raspberrypi/pico-examples`' own `hstx/dvi_out_hstx_encoder` RGB332 sample (L0 NBITS=1/ROT=26,
L1 NBITS=2/ROT=29, L2 NBITS=2/ROT=0) and decoded it by hand —

> `original_bit_index = (output_bit + ROT) mod 32`, window = rotated bits `[7 : 7-count+1]`

— which resolves EXACTLY to L0=Blue, L1=Green, L2=Red, matching RGB332's known bit layout
bit-for-bit. That confirmed both the rotate-direction/window formula and the true lane order,
independent of anything Adafruit-specific.

**Recomputed from scratch for this project's own RGB565 layout** using
`ROT = (channel_MSB_bit_index - 7) mod 32`:

| Channel | Lane | MSB bit | ROT | NBITS field | Bits |
|---|---|---:|---:|---:|---:|
| Red | L2 | 15 | 8 | 4 | 5 |
| Green | L1 | 10 | 3 | 5 | 6 |
| Blue | L0 | 4 | 29 | 4 | 5 |

`expand_shift`'s `ENC_SHIFT=16` right-rotates the whole 32-bit word by 16 between the two
packed pixels, bringing the second pixel down into the same low-16-bit position the first
started in — so these same fixed ROT values correctly decode both pixels per word, the same
way pico-examples' single fixed set works across all 4 sub-pixels of its RGB332 case.

## Test pattern

Same 8-bar pattern as `phase0-dvi` Stage 1 (white/yellow/cyan/green/magenta/red/blue/black,
80px each), but using the **true, uncompensated** RGB565 values directly — no software
inversion table. If the fix is right, colours come out correctly ordered with no further
massaging.

## Build

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

## Flash

`picotool load -f build/bringup.uf2` (forces a reboot over USB, no BOOTSEL button needed if
the board is already running any pico-sdk firmware with a live USB connection), or hold BOOT
+ tap RESET and copy `build/bringup.uf2` onto the `RPI-RP2` drive. No USB CDC console output
(pure video, same as `phase0-dvi` Stage 1) — check the monitor, connected via the Metro's
22-pin HSTX port and the Adafruit HSTX-to-DVI adapter.

## Results (2026-08-30, Metro RP2350, real monitor)

**Confirmed correct on the first attempt.** 8 sharp, evenly-spaced, **correctly-coloured**
vertical bars — white/yellow/cyan/green/magenta/red/blue/black, left to right, stable, no
compensation table involved. The derivation above is the real fix, not a guess that happened
to work.

**Backported unchanged into `firmware/phase0-dvi/` and `firmware/phase0-dvi2/`** (both
rebuild cleanly) — the register math is board-independent, so the same fix applies there
without re-deriving anything. Not re-flashed on the physical Feather in this session (only
the Metro was connected), but there is no board-specific mechanism left in this fix to
re-verify: same silicon, same GPIO/lane mapping, same RGB565 packing convention, same
`expand_shift` config, all already true on both boards. Re-flashing the Feather to visually
re-confirm is cheap whenever it's next connected, but not expected to reveal anything new.

Settles the main README's A1 "still open" item: the colour channel rotation is now fixed at
the source, not compensated in the test pattern.
