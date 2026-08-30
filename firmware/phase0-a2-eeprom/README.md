# Phase 0, A2 / C4 — emulated hexpansion EEPROM over I2C0 target

Bench firmware for the Metro RP2350, testing two things at once:

- **A2** ([Phase 0 plan](../../README.md#phase-0--de-risk-on-the-bench-23-weekends)) —
  does RP2350's I2C0 target/slave mode actually work, wired the way §4.1 (as adapted for
  this specific bench board — see the main README's A2 section) says it should?
- **C4** — does the badge actually discover and enumerate this emulated EEPROM reliably?
  (§5's "one genuine risk": the badge scans shortly after power-on; if the RP2350 hasn't
  brought its I2C target up yet, the port enumerates as "no EEPROM" and nothing runs.)

Board: **Adafruit Metro RP2350** — see the main README's A2 section for why this replaced
the Feather (GPIO21/CS wasn't accessible there for the SPI0 test; unrelated to this I2C0
test, but same board now used for both).

## Wiring

I2C0 target uses **GPIO4 (SDA) / GPIO5 (SCL)** — a bench-test-only substitution for the
real design's GPIO24/25 (see the main README's A2 section for why: Metro's own GPIO24
isn't broken out, GPIO25 is its onboard NeoPixel). `HEXP_DET` tied to GND, badge `SDA`/`SCL`
to GPIO4/5, `GND` shared, **badge `+3V3` NOT connected** (Metro is USB-powered; no
isolation circuit exists in this bench setup to safely tie two live 3.3V rails together —
see the main README §4.5 for the real design's protection, not present here).

## What this does

- `eeprom_i2c_start()` runs as the **literal first line of `main()`**, before
  `stdio_init_all()` or anything else — §5's mitigation #1 ("bring the I2C target up in
  the first few hundred microseconds ... target < 20 ms from power-good").
- Emulates a 64 KiB I2C EEPROM at address `0x50`, 16-bit addressing, RAM-only (no flash
  persistence — proving reliable *enumeration* is this test's goal, not the product's
  eventual field-update-via-EEPROM-write feature).
- Header at offset 0 matches §1.4's `<4s4sHHIHHH9s` + checksum layout exactly: magic
  `THEX`, manifest `2026`, `fs_offset`=64, `page_size`=64, `total_size`=65536, placeholder
  `vid`/`pid` (0x0000 — **not yet requested from the badge team ("UHB-IF"), §1.4 flags this
  as a "do this early" item, still outstanding**), `unique_id` from the RP2350's own
  factory-programmed chip ID (low 16 bits, via `pico_get_unique_board_id()`), friendly name
  `Hexi-GFX`.
- I2C slave ISR technique (raw register access, ISR in RAM, 2-byte address state machine)
  adapted from
  [`sammachin/rp2040-hexpansion`](https://github.com/sammachin/rp2040-hexpansion) (MIT) —
  a real working THEX-header EEPROM emulator whose header layout independently
  cross-validates this design's own §1.4. Ported from Arduino/RP2040 to bare pico-sdk C
  targeting RP2350's I2C0; pico-sdk's own `gpio_set_function()`/`i2c_init()` are used for
  setup, only the slave-mode register overrides and the ISR itself need raw access (pico-sdk
  has no built-in slave/target API).
- Prints I2C read/write byte counts and a "header has been read" announcement over USB CDC
  whenever they change, so you can watch the badge's scan happen live.

## Build

```bash
export PICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.3.0"
export PICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1"
export PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1/bin:$HOME/.pico-sdk/cmake/v3.31.5/bin:$HOME/.pico-sdk/ninja/v1.12.1:$HOME/.pico-sdk/picotool/2.3.0/picotool:$PATH"

cmake -G Ninja -B build .
cmake --build build
```

## Flash

Hold BOOT, tap RESET, copy `build/bringup.uf2` onto `RPI-RP2`.

## What to check

1. **USB serial console**: should print the startup banner, then activity lines as I2C
   traffic happens.
2. **Plug the hexpansion into the badge** (or use the badge's Hexpansions app to trigger a
   scan on the port it's in). Watch for the "*** Header (byte 0) has been read ***" line —
   that's the badge's own enumeration scan finding this EEPROM.
3. **Does the badge itself recognise the port as occupied** — check the badge's own UI/menu
   for the hexpansion slot in question. It likely won't run an *app* yet (no LittleFS
   filesystem is present past the header, and `vid`/`pid` are placeholders), but it should
   at minimum detect *something* answering at `0x50`.

## Results (2026-08-30, Metro RP2350, real badge)

**Worked on the first attempt — no bugs found.**

```
phase0-a2-eeprom: I2C0 target 0x50 running (started before stdio/USB init)
  Plug the hexpansion into the badge now, or run the badge's Hexpansions app
  to trigger a scan. Watching for reads/writes below.

  I2C activity: 632 bytes read, 0 bytes written (total)
  *** Header (byte 0) has been read -- badge has scanned this EEPROM ***
  I2C activity: 664 bytes read, 0 bytes written (total)
```

The badge found the EEPROM and read well past the 32-byte header (632, then 664 bytes) —
strong evidence the header **validated correctly** (magic `THEX`, manifest `2026`, checksum
all passed), since a failed check would have made the badge stop at byte 32. The extra reads
are almost certainly the badge attempting to mount a LittleFS filesystem at `fs_offset=64`
(§1.4, 512-byte blocks) and finding nothing there — expected, since no real filesystem image
was provided; that wasn't this test's goal.

**No enumeration race observed** — confirms §5's mitigation #1 (I2C target started as the
literal first line of `main()`) is sufficient in practice, at least for this single test.
Settles main README risk 1 (§9) in principle; the formal 50×-cold-plug reliability/timing
measurement C4 calls for is still outstanding.
