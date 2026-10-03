# Build an HDMI hexpansion for the Tildagon badge (Pico 2 + DVI Sock)

This guide builds a working HDMI output for the EMF Tildagon badge from two
cheap boards: a **Raspberry Pi Pico 2** and an **Adafruit DVI Sock**. The badge
mirrors its round screen to any HDMI monitor. If you don't want to compile
the firmware, there is a binary already compiled.

*Tested on 03/10/26 with the official Raspberry Pi monitor on badge port 4.*

## What you need

| Part | Notes |
|---|---|
| Raspberry Pi **Pico 2** | The plain Pico 2, **not** the Pico 2 W (its LED is on the wireless chip, so the status LED won't work). |
| [Adafruit DVI Sock for Pico](https://www.adafruit.com/product/5957) | Passive HDMI connector board, no active parts. |
| Port breakout hexpansion | The [port breakout hexpansion](https://github.com/emfcamp/badge-2024-hardware/tree/main/hexpansion) from the EMF badge hardware repo. It brings the port's pads out to pins you can wire to. |
| Reset push button | The Pico 2 has no reset button of its own, so add one between its `RUN` pin and GND. |
| HDMI cable and monitor | Some monitors need 5 V on the Sock's 5 V pad to show a picture, which needs a boost converter (see Troubleshooting). |
| 5 V boost converter (only if your monitor needs it) | Small 3.3 V to 5 V module. |
| Jumper wires | 8 wires, or 9 with the spare SPI pin: 3 SPI, 2 I2C, `LS_A` to `RUN`, VSYS power and GND (see below). |
| USB cable | Only for flashing. |
| Tildagon badge | Running badge firmware with display mirroring, see [Badge side](#badge-side). |

## 1. Attach the DVI Sock to the Pico 2

The Sock connects to **GP12 to GP19** on the Pico 2 and carries the HDMI
signals. Fit and solder it as Adafruit's product page describes, then check
that the connections match:

| Pico 2 pin | Sock signal |
|---|---|
| GP12 / GP13 | D0+ / D0− |
| GP14 / GP15 | CK+ / CK− |
| GP16 / GP17 | D2+ / D2− |
| GP18 / GP19 | D1+ / D1− |
| GND | GND |

The firmware is built for exactly this pin order, so don't swap the pairs.

## 2. Wire the Pico 2 to the badge port

Plug the port breakout hexpansion into the badge, and wire its pins to the
Pico 2 as below (the guide was tested on port 4):

| Badge signal | Pico 2 pin | Direction | Purpose |
|---|---|---|---|
| `HS_F` | GP0 | badge → Pico | SPI data (MOSI) |
| `HS_H` | GP1 | badge → Pico | SPI chip select (CS) |
| `HS_G` | GP2 | badge → Pico | SPI clock (SCK) |
| `HS_I` | GP3 | not used | spare |
| I2C SDA | GP4 | both | lets the badge identify the board |
| I2C SCL | GP5 | both | |
| `LS_A` | `RUN` | badge → Pico | lets the badge reset the Pico 2 |
| GND | GND | | **must be shared**: connect to any GND pin on the port breakout |
| 3V3 | VSYS (pin 39) | badge → Pico | power (see below) |

**Note the swap:** `HS_H` (the badge's chip select) goes to **GP1** and `HS_G`
(its clock) goes to **GP2**. The badge driver puts clock and chip select on
those two pins the opposite way round to what you might expect, and the
firmware needs them on GP1 and GP2 as shown.

The Pico 2 has no reset button, so also fit a **reset push button** between
its `RUN` pin and GND, so you can reset it by hand.

### Power

The Pico 2 runs fine from the badge's 3V3, wired to its **VSYS pin** (pin 39),
with no USB cable attached.
**Never connect USB and the badge's 3V3 together.** USB feeds the Pico's power
rail through a diode, so the badge's 3V3 would be pushed back into. Unplug USB
after flashing, before connecting the badge power.

## 3. Flash the firmware

1. Download [`pio_testcard_pico2.uf2`](https://github.com/Corteil/rp2350-hdmi-hexpansion/releases/latest/download/pio_testcard_pico2.uf2)
   from the latest release. Check it against the `.sha256` file on the same
   page (`sha256sum -c pio_testcard_pico2.uf2.sha256`).
2. Hold the **BOOTSEL** button on the Pico 2 and plug it into your computer
   by USB. Let go once a drive called **RP2350** appears.
3. Drag `pio_testcard_pico2.uf2` onto that drive. It reboots by itself when
   the copy finishes.
4. Unplug the USB cable.

The current release was built from commit `0a6ea73` and has the checksum
`381a32b4677d10293090b2ff0aa552e232379617f3af6e2996a23775f8e03e4d`.

## 4. First run

1. Connect the HDMI cable and power the Pico 2 from the badge's 3V3.
2. The monitor should show a test card, and the onboard LED should blink
   **once a second**. That means the board is running with no frames arriving.
3. When the badge starts sending its screen, the LED blinks **five times a
   second** and the monitor shows the badge screen.

### Badge side

The badge needs firmware with display mirroring and the `display_manager`
app (both by [hazanjon](https://github.com/hazanjon), see the
[README](https://github.com/Corteil/rp2350-hdmi-hexpansion#acknowledgements)).
On the badge:

1. Insert the board in the port (the Pico identifies itself as `HDMI-HEX`).
2. In `tildagon-hdmi-manager` (or `display_manager`), **detach** the mirror.
3. Press the **reset button** on the Pico 2.
4. **Attach** the mirror again, using the **HDMI** driver for that port.

The order matters: the reset goes between detach and attach.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Monitor shows nothing at all | Some monitors only show a picture if the Sock's 5 V pad gets 5 V. The official Raspberry Pi monitor and a video capture dongle didn't need it. Because the Pico 2 runs from the badge's 3V3, there is no USB 5 V to use, so add a boost converter: input from the Pico 2's VSYS pin and GND, output 5 V to the 5 V pad on the Sock. Its GND must also be common with the port breakout's GND. |
| Test card shows, LED stays at 1 Hz after attaching | The badge frames aren't arriving. Check the SPI wires, above all `HS_F` to GP0, and that the mirror was attached with the **HDMI** driver. A broken data wire looks exactly like this: clock and chip select work but no frames decode. |
| Picture drops out, then returns, when you plug the badge in | Normal. The Pico 2 resets itself twice, about 2 s apart, when the port is inserted, then recovers. |
| Some frames missing | Normal. Around 18% of frames are dropped because the badge starts a new frame before the last finishes. It is not visible in practice. |

If a wire is suspect, the repo's `firmware/pin-probe` tool and the Pin Tester
badge app can show which Pico pin each badge pin actually reaches.

## Source and build

Everything, including how to build the firmware yourself, is in the
[repository README](https://github.com/Corteil/rp2350-hdmi-hexpansion#alternative-bench-board-raspberry-pi-pico-2--adafruit-dvi-sock-tested).
