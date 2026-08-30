#ifndef EEPROM_I2C_H
#define EEPROM_I2C_H

#include <stdint.h>
#include <stdbool.h>

// C4 / A2: emulates the badge's hexpansion identification EEPROM over
// I2C0 (target/slave mode) -- see ../../README.md section 1.4 (the
// EEPROM contract) and section 5 (Faking the EEPROM).
//
// RAM-only for this bench test (no flash-backed persistence) -- proving
// reliable enumeration is the goal here, not the eventual product's
// field-update-via-EEPROM-write feature (section 5's "nice second-order
// benefit").

// Call as the ABSOLUTE FIRST thing in main(), before anything else --
// section 5's mitigation #1: "bring the I2C target up in the first few
// hundred microseconds of main(), before PSRAM init, HSTX, or anything
// else. Target < 20 ms from power-good."
void eeprom_i2c_start(void);

// Diagnostics: how many read/write byte events the ISR has serviced,
// and whether the header (byte 0) has ever been read -- lets main()
// report "badge has scanned us" over USB CDC without touching the ISR's
// own state.
uint32_t eeprom_i2c_read_count(void);
uint32_t eeprom_i2c_write_count(void);
bool eeprom_i2c_header_was_read(void);

#endif
