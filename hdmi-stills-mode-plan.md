# Plan: high-resolution stills mode and second-screen mode

Status: **proposal, nothing built yet.** Written while building the
[Tildagon-gallery](https://github.com/Corteil/Tildagon-gallery) app, which would
be the first user. The two ideas turned out to be one mode on the wire, so they
are planned together.

## Why

Today the hexpansion only mirrors: the badge pushes its own 240×240 framebuffer
(`display.attach_mirror()`), and the RP2350 doubles it to 480×480 and masks it
round. That is right for "every app gets HDMI", but:

- **Stills look soft.** A photo shown by a badge app is already limited to 240 px,
  then pixel-doubled. A monitor could show four times the detail.
- **No second screen.** The TV can only ever show what the badge screen shows, so
  an app can't put a picture on the TV and its controls on the badge.

## What limits us now (from `firmware/pio-testcard/src/main.c`)

- One frame type: 4-byte `TDHD` header, then 240 rows × 480 bytes = 115,200 bytes
  of RGB565, byte-swapped (the badge's `CTX_FORMAT_RGB565_BYTESWAPPED`).
- `framebuf[2][SRC_ROWS=240][DBL_WORDS=240]` of `uint32_t` = **460,800 bytes**,
  i.e. nearly all of the RP2350's 520 KB SRAM: two 240-row buffers of 480
  pre-doubled pixels, with rows doubled vertically at scan-out.
- The output is masked round (`row_half_width`) with a grey-to-black vignette.
- The PSRAM on the PCB is footprint-only (DNP), so don't assume it.

## Proposal: one new frame type, `TDHS` ("stills")

The host sends a complete picture that is **not** a copy of the badge's own screen.
It is the same mechanism for both ideas: a high-resolution still is just a
second-screen frame that happens to be a photo, and a second screen is just a
stills frame the app draws itself. The difference is only who makes the frame.

### Wire format (v1)

- Same SPI link as today: mode 0, MSB first, one continuous CS-low frame.
- Header: ASCII `TDHS` (0x54 0x44 0x48 0x53), found by the same marker scan
  as `TDHD`, then one flags byte (bit 0 = keep the round mask, otherwise square),
  then the pixels.
- Payload: **480 × 480 RGB565, byte-swapped as for `TDHD`**, 460,800 bytes,
  row by row. No pixel doubling: these are real pixels.
- 480×480 is the biggest square that fits the screen height, and it is exactly the
  RAM the two mirror buffers use today, so no new memory is needed.

### RP2350 behaviour

- `TDHD` keeps working exactly as now (mirror mode).
- On `TDHS`, switch to **stills mode**: scan out one 480×480 buffer, pillarboxed
  into 640×480 (80 px bars), unmasked by default. The mirror's two buffers and the
  stills buffer share the same memory (a union), so the modes can't be live at once.
- Stay in stills mode, holding the last picture, until the next `TDHD` arrives (the
  badge re-attached its mirror). Don't time out: a still is meant to stay up.
- There is one buffer, not two, so a new picture wipes in from the top over the
  transfer time. For slides that is fine. If it bothers us: blank to black first,
  or fade out, then draw. Decide on the bench.

### Timing

At 20 MHz, `phase0-a2-spi` measured 47.6 ms for 115,200 bytes (96.7 % efficiency).
A stills frame is four times that: **about 190 ms** at 20 MHz, about 375 ms at
10 MHz. Fine for slides, far too slow for video (about 5 fps), so this is not a
general high-resolution display, and the mirror stays the right choice for
animation.

### Later, if wanted

- 640×480 full-screen: 614,400 bytes of RGB565 doesn't fit in SRAM. It would need
  the PSRAM (fitted and detected at boot) or 8-bit colour (RGB332, 307,200 bytes).
- Partial updates (a rectangle) for a second screen that changes a little at a
  time.
- A second flags bit for "no vignette" when the mask is on.

## Badge side (a MicroPython app, no firmware change)

`attach_mirror()` can only forward the badge's own framebuffer, so an app that wants
stills or a second screen does this instead:

1. `display.detach_mirror(port)`.
2. Drive the port with `machine.SPI` using the pins in `_PORT_OVERRIDE`
   (`spi_frame_timing.py` already does this), mode 0, sending `TDHS`, flags and
   the pixels. The first transfer after creating a new `SPI` object is corrupted
   on the ESP32-S3 (see `phase0-a2-spi/README.md`), so send a throwaway
   16-byte warm-up first.
3. When it quits, call `display.attach_mirror(...)` again; the first `TDHD` frame
   returns the RP2350 to mirror mode.

Building the 480×480 frame on the badge is the gallery's job: its native
`imgtool` module already resamples to RGB565 rows, so it needs one more output
mode, "raw RGB565 into a buffer" (about 450 KB, in PSRAM).

## The hexpansion and an SD hexpansion can't be used together

So the pictures can't come from the SD card in this mode. The gallery already
keeps up to 10 resized copies in the badge's flash for exactly this case. Its
"kept size" setting will become 240 px or 480 px, with 480 being the stills mode's
native size.

## Bench plan

1. RP2350: add `TDHS` handling to `pio-testcard`, test with a Python script on a
   PC-driven badge (`spi_frame_timing.py` style) sending a test chart: gradient,
   480×480 pixel grid, 1-pixel lines to check nothing is doubled or shifted.
2. Check the mode switch both ways (`TDHD` → `TDHS` → `TDHD`) with a real mirror
   running, and that a held still survives for minutes with no frames.
3. Measure the transfer time and any visible wipe on the Pico 2 + DVI Sock bench
   rig and the Metro, at 10 and 20 MHz.
4. Gallery: slideshow to the TV with the controls on the badge screen.

## Open questions

- Is the single-buffer wipe acceptable, or do we blank first?
- Square 480×480 only, or also 640×360 for 16:9 photos (460,800 bytes too, and
  it fits the same buffer)? Photos are mostly 4:3 or 3:2, so 640×360 would mean
  more black bars rather than fewer; probably not worth it.
- Mask on or off by default for stills? I'd say off.
