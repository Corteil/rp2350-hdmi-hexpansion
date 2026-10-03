# Hexi-GFX — RP2350 HDMI Hexpansion for the EMF Tildagon Badge

A Tildagon hexpansion that mirrors the EMF badge's 240×240 round screen onto a
real HDMI/DVI monitor — an RP2350 drives the video over HSTX, pixel-doubled and
pillarboxed into a standard 640×480@60 signal. Every existing badge app gets
HDMI output with no code changes.

For the full design rationale, bench measurements, BOM, costing, and the
phase-by-phase delivery plan that got here, see [`HISTORY.md`](HISTORY.md).
This file covers what's true *now*.

## Status (03/10/26)

**Real mirroring and stable HSTX video work together, confirmed on real
hardware** — not just clean logs, actual moving content on a monitor. Current
firmware is `firmware/pio-testcard/`, running on an **Adafruit Metro RP2350**
dev board wired to a real badge hexpansion port, and since 03/10/26 also on
a **Raspberry Pi Pico 2 + Adafruit DVI Sock** (`-DHEXI_BOARD=pico2`, see
[its section](#alternative-bench-board-raspberry-pi-pico-2--adafruit-dvi-sock-tested)),
powered straight from the badge's 3V3. The custom hexpansion PCB's
schematic is drafted in `hardware/`; layout hasn't started (see
[Next step](#next-step)).

- Badge side: `display.attach_mirror()`, from a fork of
  [hazanjon/badge-2024-software](https://github.com/hazanjon/badge-2024-software)
  PR #454, with three real bugs found and fixed (SPI acquire-bus timeout, a
  non-idempotent display init that froze the badge's own screen, and an
  SPI-host/GPIO-pin ordering bug). Fixes were merged into hazanjon's
  `feature/hdmi-mirror` branch as
  [hazanjon/badge-2024-software#1](https://github.com/hazanjon/badge-2024-software/pull/1);
  that branch was rebuilt and flashed to the badge on 03/10/26 and the
  mirror was confirmed working with the Pico 2.
- RP2350 side: a from-scratch PIO-based SPI0 slave receiver (the hardware
  SPI0/PL022 peripheral loses byte alignment mid-burst on this chip and never
  recovers), double-buffered HSTX/DVI scanout, and a grey-to-black vignette
  fade on the pillarbox/mask area around the circular content.
- Known, understood, non-blocking: ~12% of frames (Metro) to ~18% (Pico 2,
  one sample) get abandoned when the badge starts a new frame before the
  RP2350 finishes the previous one — benign, fully instrumented, not a bug
  to fix.
- Known, benign: on the Pico 2 each port insertion resets the board twice
  about 2 s apart (likely the badge power-cycling the port); the mirror
  re-attaches by itself.

## Repo layout

| Path | What it is |
|---|---|
| `firmware/pio-testcard/` | **Current firmware.** Real HSTX/DVI video and real SPI mirror reception together — what's actually flashed to the bench Metro RP2350 today. |
| `firmware/testcard/` | Earlier milestone: full hexpansion-identity EEPROM emulation + the first end-to-end SPI link demo. Superseded by `pio-testcard` for the mirror path itself, but still the reference for EEPROM emulation. |
| `firmware/pin-probe/` | Pico 2 bench tool that counts transitions on every header GPIO (read over SWD), used with the badge's Pin Tester app to prove which Pico pin each `HS_F`..`HS_I` wire lands on. |
| `firmware/mirror_debug/` | Bench diagnostic tool used to bring up the PIO-based SPI slave receiver protocol. |
| `firmware/phase0-*/` | Individual bring-up experiments from early de-risking (HSTX/DVI basics, colour-channel fix, SPI speed sweep, EEPROM emulation, ctx rasterisation benchmark, microSD). Each has its own README. |
| `hdmi-mirror-plan.md` | Implementation notes for switching to hazanjon's real `attach_mirror()` protocol (SPI mode, byte order, per-port pin overrides). Completed; kept for reference. |
| `HISTORY.md` | The full design journal — rationale, bench measurements, BOM, costing, risk register, phase plan. Read this for *why*, not *what's true now*. |
| `hardware/` | KiCad 10 project for the hexpansion PCB (schematic v0.1 done, layout not started), `lib/jlc.*` (JLCPCB/LCSC symbols, footprints and 3D models), and `hexpansion_bom.xlsx`, the JLCPCB assembly BOM tracker. |
| `tildagon-base/` | Tildagon KiCad symbol and footprint library (hexpansion edge connector etc.), used by `hardware/`. |
| `bom.csv` | Machine-readable bill of materials, generated from the schematic (LCSC numbers, JLC Basic/Extended, prices). |

## Hardware today

Bench rig: an Adafruit Metro RP2350 (SPI0 slave on GPIO20–23, HSTX on
GPIO12–19), wired to a real hexpansion port on an EMF Tildagon badge.

**HS wiring gotcha (all ports):** hazanjon's badge-side `PORT_PINS` table
puts SCK on `HS_G` and CS on `HS_H`, the opposite of this project's original
`HS_G`=CS / `HS_H`=SCK convention. For a real mirror test, wire
`HS_F`→GPIO20, `HS_G`→GPIO22, `HS_H`→GPIO21, `HS_I`→GPIO23 at the RP2350 end.
First found on port 4, since tested on all six ports; the PCB is wired this
way. See `HISTORY.md` §4.1 for the full derivation and why the two sides
disagree.

VID/PID assigned for the eventual EEPROM-emulating hexpansion: `0x1969` /
`0x4544`.

### Alternative bench board: Adafruit Feather RP2350 + HSTX (builds, not yet tested on hardware)

Build with `-DHEXI_BOARD=feather` (see [Building and flashing](#building-and-flashing)).
The Feather is an RP2350A: the same die as the RP2354A the real hexpansion
uses (the RP2354A just adds 2 MB of flash in the package), and
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
button for step 3 of the reinsertion procedure below.

### Alternative bench board: Raspberry Pi Pico 2 + Adafruit DVI Sock (tested)

Build with `-DHEXI_BOARD=pico2`. **Confirmed working end to end on 03/10/26**:
stable mirror picture on the official Raspberry Pi monitor, status LED at
5 Hz while frames arrive. The Pico 2 is an RP2350A, so HSTX reaches the same
GPIO12–19 block the [Adafruit DVI Sock for Pico](https://www.adafruit.com/product/5957)
is wired to. The Sock is a passive connector plus 220 Ω series resistors, and
its product page describes it for the RP2040. It works fine with HSTX, but its
**lane order differs from the Metro/Feather wiring**, so this variant uses
`lane_to_output_bit = {0, 6, 4}` (D0 = GP12/13, D1 = GP18/19, D2 = GP16/17,
CK = GP14/15, "+" on the even GPIO).

| Function | RP2350 GPIO | Notes |
|---|---|---|
| HSTX video | 12–19 | DVI Sock, see lane order above |
| SPI MOSI (`HS_F`) | 0 | |
| SPI CS (`HS_H` on port 4) | 1 | |
| SPI SCK (`HS_G` on port 4) | 2 | port-4 SCK/CS crossover already applied |
| SPI MISO (`HS_I`, never driven) | 3 | |
| EEPROM-emulation I2C0 SDA / SCL | 4 / 5 | unchanged |
| Debug UART1 TX / RX (`_debug` build) | 8 / 9 | moved off GP0/1, which are now SPI inputs |
| Status LED (plain GPIO) | 25 | 1 Hz = running, no frames; 5 Hz = frames arriving |
| Status NeoPixel (external WS2812) | 22 | same colours as the other boards |
| `LS_A` → `RUN` | — | lets the badge reset the Pico 2; a reset switch from `RUN` to GND is also fitted |
| Badge 3V3 → VSYS (pin 39) | — | powers the board, with no USB attached |
| GND | — | shared with the badge: any GND pin on the port breakout hexpansion |

A step-by-step hardware build and flashing guide for this variant is in
[`docs/index.md`](docs/index.md) (published at
<https://corteil.github.io/rp2350-hdmi-hexpansion/> once GitHub Pages is on).

Notes from bring-up:

- **Power:** it runs fine from the badge's 3V3 with no USB. Never connect USB
  and an external 3V3 into VSYS together: VBUS feeds VSYS through a diode, so
  the external source gets back-fed. Use a series Schottky if both are needed.
- **5 V on the Sock's 5 V pad** is monitor-dependent. Some monitors only show
  a signal when it is present; the official Raspberry Pi monitor and a video
  capture dongle don't need it. With no USB attached there is no VBUS, so for
  a monitor that needs it, use a boost converter: input from VSYS and GND,
  5 V output to the Sock's 5 V pad, GND common with the port breakout's GND.
- **Each port insertion resets the Pico 2 twice, about 2 s apart** (probably
  the badge power-cycling the port; not scoped). It is harmless: the mirror
  re-attaches and the picture returns by itself.
- **Wire-continuity check:** `firmware/pin-probe` plus Pin Tester's `blink`
  (via `mpremote exec`, store-installed path `/apps/Corteil_Pin_Tester`) maps
  each badge pin to a Pico GPIO. A bad `HS_F` wire here looked like CS/SCK
  arriving but a stuck-high or stuck-low data line and no `TDHD` match.

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
- **Buttons:** Key1 is BOOTSEL and Key2 is reset (RUN). Use Key2 for step 3
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

## Reinsertion procedure (corrected 03/10/26, still manual)

Plugging the hexpansion into an already-running badge doesn't reliably start
mirroring on its own yet, even with the auto-attach relay packed into the
fake EEPROM (`firmware/testcard/badge_app/app.py`) and its LS_A-pulse
hardware reset (`firmware/pio-testcard/src/main.c`'s own comments have the
full root-cause trail on why a reset is needed at all). The sequence
confirmed working on the bench, in order:

1. Insert the `HDMI-HEX` hexpansion.
2. In `tildagon-hdmi-manager` (or `display_manager`), **detach** the mirror.
3. Press the physical **reset button** on the RP2350 board (SW2 on the Metro).
4. **Attach** the mirror again, from the same app.

The order matters: the reset goes between detach and attach, not after.
Auto-attach alone wasn't enough.

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

For the Raspberry Pi Pico 2 + Adafruit DVI Sock, use `-DHEXI_BOARD=pico2`
the same way (`cmake -B build-pico2 -DHEXI_BOARD=pico2`).

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

**Schematic v0.1 is done** (`hardware/rp2350-hdmi-hexpansion.kicad_sch`,
28/09/26). The PCB layout hasn't started: open the project and run
*Tools → Update PCB from Schematic*. Every part has a JLCPCB/LCSC number,
and its symbol, footprint and 3D model are in the project library
`hardware/lib/jlc.*`, pulled with
[easyeda2kicad](https://github.com/uPesy/easyeda2kicad.py). KiCad's ERC is
clean apart from one expected warning (HEXP_DET tied to GND is how the
badge detects a hexpansion), and the exported netlist was checked
pin-for-pin against the design. `bom.csv` and `hardware/hexpansion_bom.xlsx`
are generated from the schematic.

Changes from the `HISTORY.md` plan:

- **MCU is the RP2354A**, not the RP2350A: the same die and QFN-60 pinout,
  with 2 MB of flash in the package on QSPI CS0. There's no external flash
  chip (the W25Q128 is gone). 2 MB is plenty: the largest firmware build is
  about 260 KB. Build with `PICO_FLASH_SIZE_BYTES` set to 2 MB.
- **The PSRAM is footprint-only (DNP)**, for optional hand soldering. The
  part is the **APS6404L-3SQR-SN** (SOP-8, 3.3 V); the `-ZR` that the old
  BOM listed is USON-8. It stays on QSPI CS1 = GPIO0, with a 10k pull-up on
  CS that is fitted even when the PSRAM isn't, so firmware must detect it
  at boot rather than assume it.
- **Full-size HDMI Type A** socket instead of mini-HDMI. It's wider, so check
  the fit on the outer flat.
- **One Qwiic socket** instead of two, to save edge space.
- **TLV62569** 2 A buck instead of the TPS62203 (300 mA, poor stock).
- **No HDMI DDC/EDID.** The output is always 640×480@60, which every
  monitor must accept, so there's nothing to read. The PCA9306 and its six
  passives are gone. Hot-plug detect stays (GPIO2).
- **No microSD.** That frees the second side flat and GPIO5 and 8–11.
  Spare GPIOs are now **3, 4, 5, 8, 9, 10, 11**, all left no-connect.
- **Trimmed to the Raspberry Pi minimal design.** TMDS lines go straight
  from GPIO12–19 to the ESD chips and socket, with no 0R insurance resistors,
  as the bench has always run. There are 7 × 100 nF on 3.3 V (pins 44/45 and
  53/54 share, as in Raspberry Pi's reference), no BOOTSEL pull-up (QSPI_SS
  has one internally) and no RUN debounce cap. The ferrite beads are gone.
- Kept from the design guide: the 1k crystal series resistor, 33R
  VREG_AVDD filter, 27R USB series resistors and a 10k RUN pull-up (because
  of the badge reset). Also added: an HDMI +5V PTC fuse and test pads.
- Result: **80 JLC-placed parts, about $5.11 a board** at the 10-board price
  tier (25 capacitors, 32 resistors).

**Pin map changes from `HISTORY.md` §4.1.** It had no free GPIOs for three
signals, so `LS_C`/`LS_D`/`LS_E` are left unconnected and their GPIOs reused:

| GPIO | Now | Was |
|---:|---|---|
| 27 | SK6805 RGB LED data | `LS_C` (bootloader-entry idea) |
| 28 | Badge-rail sense (ADC2, 100k/100k from `3V3_BADGE`) | `LS_D` spare |
| 29 | HDMI +5V boost enable (100k pull-down) | `LS_E` spare |

On the board, `HS_G`→GPIO22 (SCK) and `HS_H`→GPIO21 (CS). That's the
crossing the badge's mirror driver needs, **tested on all six ports**.

**Firmware changes the PCB needs** (from `pio-testcard`): EEPROM emulation
moves from GPIO4/5 to **GPIO24/25**, with its internal pull-ups *off*
(they'd back-power an unpowered badge). The status NeoPixel moves to
**GPIO27**. Set the flash size to 2 MB.

**Badge reset without backfeed.** `LS_A` drives RUN through **U11, an
SN74LVC1G07 open-drain buffer powered from `3V3_BADGE`**. R4 (10k to
`3V3_BADGE`) holds `LS_A` high, so it idles at "not reset". When the badge
is off, U11 is unpowered. Its partial-power-down (I_off) spec, ≤10 µA and
typically far less, stops the RUN pull-up leaking into the badge, and the
RP2354A keeps running from USB. A plain diode can't do this: the reset
current and the leak flow the same way.

## Acknowledgements

This project depends on the work of [hazanjon](https://github.com/hazanjon).
The badge-side display mirroring that the hexpansion receives, including
`display.attach_mirror()`, the SPI frame protocol with its `TDHD` header, and
the per-port pin handling in `flow3r_bsp_display_mirror.c`, comes from
his [badge-2024-software](https://github.com/hazanjon/badge-2024-software)
work (PR #454 against `emfcamp/badge-2024-software`). His
[display_manager](https://github.com/hazanjon/display_manager) app provides
the badge-side driver definitions and attach controls used to drive it.
Without that groundwork there would be nothing for this hexpansion to mirror,
and the fixes in this repo's badge-side PR were built on top of it. Thank you.
