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

## Results (2026-08-30, Metro RP2350, real badge)

**First attempt: RP2350 never unblocked.** Badge's `badge_test.py` completed all 3 rounds
without error, but received all zeros back (not the expected `a0..af`) — and the Metro's
console never printed past the startup/waiting banner, meaning `spi_write_read_blocking()`
never saw a complete 16-byte transfer. Since MicroPython's master-mode SPI has no handshake
with the slave, the badge finishing cleanly does **not** prove the RP2350 received anything —
a mode/bit-order mismatch would still let bytes clock in (just garbled), so a full stall
points at CS or SCK not reaching the RP2350 at all (see the datasheet-sourced note above).

Also found in passing (not yet confirmed as the cause, but a real conflict): this board's
`PICO_DEFAULT_LED_PIN` is GPIO23 — the same pin `PIN_MISO` uses. `gpio_set_function()` on
GPIO23 overrides the LED's SIO function to SPI TX, so this is survivable for the SPI signal
itself (the LED is just a few-mA load), but it's a real, board-specific pin conflict worth
fixing before this becomes anything beyond a bench test.

Built `src/gpio_probe.c` (second target, `build/gpio_probe.uf2`) to isolate wiring from SPI
peripheral config: watches MOSI/CS/SCK/MISO as plain SIO inputs with edge-count IRQs, no SPI
peripheral involved at all. First run (against the then-current mode-0, per-byte-CS-toggle
script) showed clean CS/SCK/MOSI activity matching the badge's transfer — wiring judged good
at that point.

**Second attempt (v3: mode 3, CS held low for the whole burst) — also failed, differently.**
Badge received all `0xff` this time (not all-zero like v1), and the Metro's console again
never printed anything — `spi_write_read_blocking()` still never unblocked. Root-caused with
three more probes, all flashed and run against real hardware in this session (via
`picotool load -f` / `picotool reboot -f` — no BOOTSEL button needed, the running app's own
USB connection is enough for picotool to force a reboot):

1. **`src/spi_loopback_test.c`** (`spi_lbm.uf2`) — SPI0 master mode with the PL022's internal
   loopback bit (LBM) set, entirely bypassing external pins. **Result: MATCH** — sent
   `10..1f`, received `10..1f` back, 154 µs. **The SPI0 peripheral itself is functional.**
2. **`src/spi_status_probe.c`** (`spi_status_probe.uf2`) — polls `SSPSR` directly instead of
   calling `spi_write_read_blocking()`, so it can show partial/failed activity that the
   blocking call would hide. FUNCSEL readback confirmed all 4 pins correctly muxed to SPI
   (`MOSI=1 CS=1 SCK=1 MISO=1`, matching `GPIO_FUNC_SPI`); `cr0`/`cr1` confirmed slave mode 3
   as configured (`SSE=1 MS=1 DSS=7 SPO=1 SPH=1`). **Result: `SR` never changed at all**
   across a full 3-round badge transfer — `BSY` and `RNE` stayed 0 throughout. Not corrupted
   data, not partial reception — the peripheral never reacted to the transfer at all.
3. **`src/gpio_probe.c`, re-run against the current v3 script** (the first gpio_probe result
   above predates the mode-3/CS-continuous change, so it needed re-confirming). **This is the
   finding that explains everything above it:**

   ```
   MOSI=0(edges=120)  CS=1(edges=0)  SCK=1(edges=768)  MISO=1(edges=63)
   ```

   Over 3 rounds × 16 bytes: **SCK shows exactly 768 edges** (16 bytes × 8 bits × 2 edges ×
   3 rounds — a perfect match) and **MOSI shows 120 edges** of real data activity. **CS shows
   zero edges and never leaves logic 1**, despite `badge_test.py` explicitly calling
   `cs.value(0)` before every round and `cs.value(1)` after. SCK and MOSI are proven to be
   wired correctly and arriving cleanly; **CS (`HS_G`, RP2350 GPIO21) is not** — the badge is
   driving it, but the RP2350 pin never sees it move. This is consistent with every failure
   mode observed so far: per the datasheet note above, the PL022 slave will not sample
   MOSI/SCK at all without CS asserted low, so a CS line that never reaches the RP2350 fully
   explains both the v1 all-zero result and the v3 zero-register-activity result, with no need
   to invoke an SPI-mode or silicon-errata explanation for either. (The CPOL=1/CPHA=1 research
   that produced v3 is still probably correct per
   [pico-sdk#941](https://github.com/raspberrypi/pico-sdk/issues/941) and the
   [Pico 2 SPI-slave forum thread](https://forums.raspberrypi.com/viewtopic.php?t=375430) — it
   just wasn't the blocking problem here, and can't be properly evaluated until CS itself is
   confirmed reaching the chip.)

**Conclusion: this was a physical wiring problem specific to the CS line** — `HS_G` on the
badge side to GPIO21 (`SCL` on the Metro's silkscreen) on the RP2350 side (bench protoboard
wiring, per this project's own free choice of `HS_x` SPI roles in the main README's §4.1).
SCK and MOSI on the same connector, same protoboard, same wiring pass, both worked
perfectly throughout, which pointed at a single bad connection (loose/broken wire, missed
pad, cold joint) rather than a systemic issue.

## Fixed and confirmed working (2026-08-30, same session)

**Rewired the CS line and re-ran the full diagnostic sequence. Working end to end.**

1. `gpio_probe` re-run: `CS` now shows exactly 6 edges over 3 rounds (2 per round — low then
   high), matching `SCK`'s 768 and `MOSI`'s 120 exactly. Confirms the physical fix.
2. `spi_status_probe` re-run: `SR`'s `RNE` bit now toggles continuously through the whole
   transfer, and cumulative `rx_bytes` reaches **exactly 48** (16 bytes × 3 rounds) — the
   PL022 slave is now receiving every byte. (Its all-zero "received from RP2350 TX" in the
   badge's own printout at this step is **not** a transmit-path finding — this probe never
   loads the TX FIFO with real data, so that result is expected and uninformative for MISO.)
3. **`bringup` (the real firmware) re-run — full bidirectional match:**
   - Badge received `a0 a1 a2 ... af` — the RP2350's fixed test pattern, exactly, all 3
     rounds.
   - Metro's console showed `Received from badge (MOSI): 00 01 02 03 04 05 06 07 08 09 0a 0b
     0c 0d 0e 0f` — the badge's ascending pattern, exactly, all 3 rounds.

**A2's SPI0-slave half is done: confirmed working on real hardware**, mode 3 (CPOL=1,
CPHA=1), CS held low for the whole burst, matching A2's I2C0-target half
(`../phase0-a2-eeprom/`, already confirmed working). Both halves of the SPI0 role mapping
chosen in the main README's §4.1 are now hardware-verified, not just planned.
