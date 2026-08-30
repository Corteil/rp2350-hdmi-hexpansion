#ifndef EEPROM_I2C_H
#define EEPROM_I2C_H

#include <stdint.h>
#include <stdbool.h>

// End-to-end link test: emulates the badge's hexpansion identification
// EEPROM over I2C0 (target/slave mode) -- see ../../../README.md
// section 1.4 (the EEPROM contract) and section 5 (Faking the EEPROM).
// Adapted from firmware/phase0-a2-eeprom (I2C0-target mechanics already
// confirmed working on real hardware there), with two real differences:
//
//   1. Real assigned VID/PID (0x1969/0x4544), not placeholders.
//   2. A real LittleFS filesystem image (fs_image.h, built from
//      badge_app/ by tools/build_fs_image.py) copied into the EEPROM's
//      filesystem region at init, instead of leaving it empty -- so the
//      badge doesn't just enumerate this hexpansion, it actually mounts
//      a filesystem and auto-launches the packed app.py.
//
// Still RAM-only (no flash-backed persistence) -- this is a link-proof
// bench build, not the product's eventual field-update-via-EEPROM-write
// feature.

// Call as the ABSOLUTE FIRST thing in main(), before anything else --
// section 5's mitigation #1: "bring the I2C target up in the first few
// hundred microseconds of main(), before PSRAM init, HSTX, or anything
// else. Target < 20 ms from power-good."
void eeprom_i2c_start(void);

// Diagnostics: how many read/write byte events the ISR has serviced,
// and whether the header (byte 0) has ever been read.
uint32_t eeprom_i2c_read_count(void);
uint32_t eeprom_i2c_write_count(void);
bool eeprom_i2c_header_was_read(void);

#endif
