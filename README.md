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

### Alternative bench board: Adafruit Feather RP2350 + HSTX (builds, not yet tested on hardware)

Build with `-DHEXI_BOARD=feather` (see [Building and flashing](#building-and-flashing)).
The Feather is an RP2350A, the chip variant the real hexpansion targets, and
its 22-pin HSTX FPC connector carries GPIO12–19 in the same lane order the
firmware already uses. So video needs no changes: plug in the same DVI
breakout and FPC cable as on the Metro.

Two things do move. GPIO21 is the Feather's onboard NeoPixel and isn't
broken out, so the Metro's GPIO20–23 SPI block can't be used. The
PIO-based receiver doesn't need hardware-SPI pins, just three consecutive
GPIOs for MOSI/CS/SCK, so it moves to D9–D11:

| Function | RP2350 GPIO | Feather pin |
|---|---|---|
| HSTX video (all 8 lines) | 12–19 | 22-pin HSTX FPC connector |
| SPI MOSI (`HS_F`) | 9 | D9 |
| SPI CS (`HS_H` on port 4) | 10 | D10 |
| SPI SCK (`HS_G` on port 4) | 11 | D11 |
| SPI MISO (`HS_I`, never driven) | 20 | MI |
| EEPROM-emulation I2C0 SDA / SCL | 4 / 5 | GPIO4 (CircuitPython `D12`/`IO4`) / D5 |
| Debug UART0 TX / RX (`_debug` build) | 0 / 1 | TX / RX |
| Status NeoPixel | 21 | onboard |
| `LS_A` → RUN | — | RST |
| GND | — | GND |

The port-4 SCK/CS crossover is already applied above. GPIO4 is labelled
from CircuitPython's pin map, so check the board silkscreen for that pad.
SWD is on the Feather's 3-pin JST-SH connector, so the Debug Probe cable
plugs straight in. UF2 flashing (hold BOOT, tap RESET) also works. Flash
is 8 MB, not 16 MB, which is plenty for this firmware. Use the RESET
button for step 4 of the reinsertion procedure below.

### Alternative bench board: Waveshare RP2350-PiZero (not yet tested)

The [Waveshare RP2350-PiZero](https://www.waveshare.com/wiki/RP2350-PiZero)
can stand in for the Metro, with one big caveat: its onboard mini-HDMI is
wired to **GPIO32–39**, and HSTX only reaches **GPIO12–19**, so the current
firmware can't drive the onboard connector. Instead, wire HSTX from the
40-pin header to an external DVI breakout, exactly as on the Metro.

Its chip (RP2350B), flash (W25Q128, 16 MB) and 12 MHz crystal match the
Metro, and every pin the firmware uses is on the header. The existing
`pio_testcard` build should therefore run unmodified, but that hasn't been
checked on real hardware yet.

**The header is not Raspberry-Pi numbered.** Waveshare's J5 net labels are
RP2350 GPIOs and differ from the silkscreen/BCM names (e.g. header pin 7 is
BCM4 but RP2350 **GPIO14**). Wire by *header pin number* from this table,
taken from Waveshare's schematic (`RP2350-PiZero.pdf`):

| Function | RP2350 GPIO | PiZero header pin |
|---|---|---|
| HSTX D2+ / D2− | 12 / 13 | 21 / 33 |
| HSTX CLK+ / CLK− | 14 / 15 | 7 / 29 |
| HSTX D1+ / D1− | 16 / 17 | 36 / 11 |
| HSTX D0+ / D0− | 18 / 19 | 12 / 35 |
| SPI MOSI (`HS_F`) | 20 | 38 |
| SPI CS (`HS_H` on port 4) | 21 | 40 |
| SPI SCK (`HS_G` on port 4) | 22 | 15 |
| SPI MISO (`HS_I`) | 23 | 16 |
| EEPROM-emulation I2C0 SDA / SCL | 4 / 5 | 8 / 10 |
| Debug UART0 TX / RX (`_debug` build) | 0 / 1 | 27 / 28 |
| GND | — | 6, 9, 14, 20, 25, 30, 34, 39 |
| `LS_A` → RUN | — | not on the header: solder to the RUN side of the reset button (Key2) |

HSTX lane assignment comes from `hstx_dvi_init()` in
`firmware/pio-testcard/src/main.c` (`lane_to_output_bit`), and the port-4
SCK/CS crossover above is already applied. Other differences from the Metro:

- **No addressable RGB LED.** The only LED is a red power LED. The firmware
  still drives its status-LED signal on GPIO25 (header pin 22). That's
  harmless, and you can wire a WS2812 there to get the status colours back.
- **SD card-detect isn't on a GPIO** (the slot's CD pin goes to GND), so
  GPIO22 is free for SCK.
- **Buttons:** Key1 is BOOTSEL and Key2 is reset (RUN). Use Key2 for step 4
  of the reinsertion procedure below.
- **SWD** is on the 3-pin header H1 (SWCLK / GND / SWDIO), so Debug Probe
  flashing works as documented. Holding Key1 while plugging in USB and
  copying `pio_testcard.uf2` also works.
- **Don't switch the build to the Pico SDK's `waveshare_rp2350_pizero` board
  file** without overriding its default UART. It puts UART1 on GPIO4/5,
  which collides with the EEPROM-emulation I2C in the `_debug` build. The
  Metro board file already used here puts UART0 on GPIO0/1.
- **Signal integrity:** the TMDS pairs are spread across the header, so they
  run at 252 Mbit/s over hand-run wires rather than an FPC cable. Keep each
  +/− pair short, equal-length and twisted together. Suspect the wiring
  first if the picture sparkles or drops out.

Using the onboard mini-HDMI would need one of two things. One is a PIO-based
PicoDVI scanout: 252 MHz `clk_sys`, a PIO block set to see GPIO16–47, and a
reworked framebuffer. The other is a board mod: remove the eight 200 Ω
series resistors (R1–R4, R6, R7, R10, R12) and feed GPIO12–19 into their
connector-side pads. Neither has been tried. The first diverges from the
HSTX design the hexpansion PCB will use.

## Reinsertion procedure (2026-09-20, still manual)

Plugging the hexpansion into an already-running badge doesn't reliably start
mirroring on its own yet, even with the auto-attach relay packed into the
fake EEPROM (`firmware/testcard/badge_app/app.py`) and its LS_A-pulse
hardware reset (`firmware/pio-testcard/src/main.c`'s own comments have the
full root-cause trail on why a reset is needed at all). The sequence
confirmed working on the bench tonight, in order:

1. Insert the `HDMI-HEX` hexpansion.
2. In `tildagon-hdmi-manager` (or `display_manager`), **detach** the mirror.
3. **Attach** the mirror again, from the same app.
4. Press the physical **reset button (SW2)** on the Metro RP2350 board.

All four steps were needed together to get a reliably working mirror in
tonight's testing — auto-attach alone was not enough. Not yet root-caused
which of steps 2–4 is actually load-bearing versus just "worked this time";
treat this as the known-good procedure until that's narrowed down further,
not as a minimal one.

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

That builds for the Metro RP2350 (the default). For the Adafruit Feather
RP2350 + HSTX, configure a separate build directory from
`firmware/pio-testcard` instead:

```sh
cmake -B build-feather -DHEXI_BOARD=feather
cmake --build build-feather
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
