# Hexi-GFX — RP2350 HDMI Hexpansion for the EMF Tildagon Badge

A Tildagon hexpansion that mirrors the EMF badge's 240×240 round screen onto a
real HDMI/DVI monitor — an RP2350 drives the video over HSTX, pixel-doubled and
pillarboxed into a standard 640×480@60 signal. Every existing badge app gets
HDMI output with no code changes.

For the full design rationale, bench measurements, BOM, costing, and the
phase-by-phase delivery plan that got here, see [`HISTORY.md`](HISTORY.md).
This file covers what's true *now*.

## Status (2026-09-20)

**Real mirroring and stable HSTX video work together, confirmed on real
hardware** — not just clean logs, actual moving content on a monitor. Current
firmware is `firmware/pio-testcard/`, running on an **Adafruit Metro RP2350**
dev board wired to a real badge hexpansion port; the custom hexpansion PCB
itself hasn't been designed yet (see [Next step](#next-step)).

- Badge side: `display.attach_mirror()`, from a fork of
  [hazanjon/badge-2024-software](https://github.com/hazanjon/badge-2024-software)
  PR #454, with three real bugs found and fixed (SPI acquire-bus timeout, a
  non-idempotent display init that froze the badge's own screen, and an
  SPI-host/GPIO-pin ordering bug). Fixes are up as
  [hazanjon/badge-2024-software#1](https://github.com/hazanjon/badge-2024-software/pull/1),
  not yet merged upstream.
- RP2350 side: a from-scratch PIO-based SPI0 slave receiver (the hardware
  SPI0/PL022 peripheral loses byte alignment mid-burst on this chip and never
  recovers), double-buffered HSTX/DVI scanout, and a grey-to-black vignette
  fade on the pillarbox/mask area around the circular content.
- Known, understood, non-blocking: ~12% of frames get abandoned when the badge
  starts a new frame before the RP2350 finishes the previous one — benign,
  fully instrumented, not a bug to fix.

## Repo layout

| Path | What it is |
|---|---|
| `firmware/pio-testcard/` | **Current firmware.** Real HSTX/DVI video and real SPI mirror reception together — what's actually flashed to the bench Metro RP2350 today. |
| `firmware/testcard/` | Earlier milestone: full hexpansion-identity EEPROM emulation + the first end-to-end SPI link demo. Superseded by `pio-testcard` for the mirror path itself, but still the reference for EEPROM emulation. |
| `firmware/mirror_debug/` | Bench diagnostic tool used to bring up the PIO-based SPI slave receiver protocol. |
| `firmware/phase0-*/` | Individual bring-up experiments from early de-risking (HSTX/DVI basics, colour-channel fix, SPI speed sweep, EEPROM emulation, ctx rasterisation benchmark, microSD). Each has its own README. |
| `hdmi-mirror-plan.md` | Implementation notes for switching to hazanjon's real `attach_mirror()` protocol (SPI mode, byte order, per-port pin overrides). Completed; kept for reference. |
| `HISTORY.md` | The full design journal — rationale, bench measurements, BOM, costing, risk register, phase plan. Read this for *why*, not *what's true now*. |
| `bom.csv` | Machine-readable bill of materials for the planned PCB (not yet built). |

## Hardware today

Bench rig: an Adafruit Metro RP2350 (SPI0 slave on GPIO20–23, HSTX on
GPIO12–19), wired to a real hexpansion port on an EMF Tildagon badge.

**Port 4 wiring gotcha:** hazanjon's badge-side `PORT_PINS` table for port 4
puts SCK on `HS_G` and CS on `HS_H` — the opposite of this project's own
`HS_G`=CS / `HS_H`=SCK convention. For a real mirror test on port 4, wire
`HS_F`→GPIO20, `HS_G`→GPIO22, `HS_H`→GPIO21, `HS_I`→GPIO23 at the RP2350 end.
See `HISTORY.md` §4.1 for the full derivation and why the two sides disagree.

VID/PID assigned for the eventual EEPROM-emulating hexpansion: `0x1969` /
`0x4544`.

## Building and flashing

### RP2350 firmware (`pio-testcard`)

Needs the Raspberry Pi Pico SDK (2.x) and an `arm-none-eabi-gcc` toolchain,
with `PICO_SDK_PATH` set.

```sh
cd firmware/pio-testcard
mkdir -p build && cd build
cmake ..
cmake --build .
```

Flash over SWD with a Raspberry Pi Debug Probe:

```sh
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" \
  -c "program pio_testcard.elf verify reset exit"
```

### Badge-side driver

Depends on `display.attach_mirror()` from the fork above. Built via the
project's own Docker image (`ghcr.io/emfcamp/esp_idf:v5.5.1`):

```sh
git clone --recursive --branch pr-454-hazan-mirror https://github.com/Corteil/badge-2024-software.git
cd badge-2024-software
./scripts/firstTime.sh
docker run --rm --env "TARGET=esp32s3" -v "$(pwd)":/firmware -u "$(id -u):$(id -g)" \
  -e HOME=/tmp ghcr.io/emfcamp/esp_idf:v5.5.1
```

Flash to a real badge in bootloader mode (disconnect USB, hold **BAT + BOOP**
for ~20s, reconnect):

```sh
docker run --rm --device /dev/ttyACM0:/dev/ttyUSB0 --group-add <your dialout/plugdev GID> \
  --env "TARGET=esp32s3" -v "$(pwd)":/firmware -u "$(id -u):$(id -g)" -e HOME=/tmp \
  ghcr.io/emfcamp/esp_idf:v5.5.1 deploy
```

The badge stays in bootloader mode after flashing — disconnect/reconnect the
USB cable once more to actually boot the new image.

## Next step

Designing the actual hexpansion PCB (44 mm hexagon, EEPROM emulation, USB-C +
HDMI power isolation) — the whole plan for it, including the pin map, power
architecture, BOM, and costing this bench work was de-risking, is in
[`HISTORY.md`](HISTORY.md).
