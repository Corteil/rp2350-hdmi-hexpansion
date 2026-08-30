# Phase 0, A2 — SPI0-slave half

Bench firmware for the Metro RP2350, testing whether RP2350's SPI0 in slave mode actually
works as wired for the badge link — see the main README's A2 section for the bench board
choice/wiring, and section 4.1's "resolved" note on how the `HS_F/G/H/I` SPI role mapping
was chosen (this project's own free choice, matched by the badge-side script here).

Companion to [`../phase0-a2-eeprom/`](../phase0-a2-eeprom/) (the I2C0-target half, already
confirmed working on real hardware). **Unlike I2C0, nothing happens here until something on
the badge actively drives SPI0 as master** — the badge doesn't touch these pins on its own,
so this test needs both the RP2350 firmware below *and* [`badge_test.py`](badge_test.py)
run against the badge.

## Wiring

Same as `phase0-a2-eeprom` for GND/HEXP_DET, plus the four SPI0 pins:

| Badge signal | RP2350 GPIO | Role |
|---|---:|---|
| `HS_F` | 20 | MOSI (badge → RP2350) |
| `HS_G` | 21 | CS |
| `HS_H` | 22 | SCK |
| `HS_I` | 23 | MISO (RP2350 → badge) |

## What this does

- RP2350 side (`src/main.c`): brings up SPI0 as slave on the four pins above (register-level
  technique — `spi_init` + `spi_set_slave` + `gpio_set_function` on all 4 pins, CS included
  — same pattern as the official
  [`raspberrypi/pico-examples`](https://github.com/raspberrypi/pico-examples)
  `spi/spi_master_slave/spi_slave` example, BSD-3, not copied verbatim). Blocks on
  `spi_write_read_blocking()` waiting for a 16-byte transfer from the badge; transmits a
  fixed known pattern (`0xA0..0xAF`) back on every round, and prints whatever it received
  over USB CDC.
- Badge side (`badge_test.py`): a MicroPython script using `machine.SPI` as master on the
  ESP32-S3's matching `hs_x` pins for **badge port 3 (C)** specifically (`MOSI=GPIO34,
  CS=GPIO33, SCK=GPIO47, MISO=GPIO48` — see the script's own header comment for the port
  table if the hexpansion is in a different port). Sends an ascending byte pattern
  (`0x00..0x0F`), 3 rounds, 500 ms apart, printing what came back.

## Build (RP2350 side)

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

## Running the test

1. Flash `build/bringup.uf2` onto the Metro (hold BOOT, tap RESET, copy the file).
2. Open a serial terminal on the Metro's USB CDC port. It'll print the pin config and then
   block, waiting — nothing else happens until step 3.
3. Run `badge_test.py` against the badge:
   ```bash
   mpremote connect COMx exec "$(cat badge_test.py)"
   ```
   **This interrupts whatever's running on the badge's screen** (any `mpremote` command
   does — see the project's own mpremote-gotcha notes) — expected here, this is a
   deliberate test run, not passively watching an app. The badge will need resetting
   afterward to get back to normal use.
4. Watch both consoles: the badge's should show 3 rounds of "sent ... received ...", and the
   Metro's should show matching "round N: transferred 16 bytes. Received from badge (MOSI):
   00 01 02 ... 0f" lines.

## What success/failure looks like

- **Working**: Metro's console shows the badge's ascending `00..0f` pattern; badge's console
  shows the Metro's fixed `a0..af` pattern. Both sides received exactly what the other sent.
- **No signal at all** (Metro never unblocks): SPI0 isn't seeing clock activity — check the
  SCK wiring (`HS_H`↔GPIO22) first, then CS (without CS asserted low, the PL022 slave
  peripheral won't sample MOSI/SCK at all — confirmed from the RP2350 datasheet's SPI
  chapter, this isn't optional).
- **Metro unblocks but data is garbled**: check SPI mode (`polarity`/`phase`) match between
  both scripts (both use mode 0 here — CPOL=0, CPHA=0), and bit order (both default to
  MSB-first).

## Results

Not yet run on hardware.
