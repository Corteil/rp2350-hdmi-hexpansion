// Phase 0 A2/C4 bring-up: emulated hexpansion identification EEPROM
// over I2C0 target mode, on the Metro RP2350 -- see
// ../../README.md's A2 section (bench board choice, wiring) and
// section 5 (Faking the EEPROM).
//
// eeprom_i2c_start() is called as the literal first line of main(),
// before stdio/USB/anything else -- section 5's mitigation #1: bring
// the I2C target up in the first few hundred microseconds, target
// <20ms from power-good, so the badge's own post-insertion I2C scan
// never finds "no EEPROM".

#include <stdio.h>
#include "pico/stdlib.h"
#include "eeprom_i2c.h"

int main(void) {
    eeprom_i2c_start();

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(100);
    }

    printf("\nphase0-a2-eeprom: I2C0 target 0x50 running (started before stdio/USB init)\n");
    printf("  Plug the hexpansion into the badge now, or run the badge's Hexpansions app\n");
    printf("  to trigger a scan. Watching for reads/writes below.\n\n");

    uint32_t last_reads = 0, last_writes = 0;
    bool announced_header = false;
    uint32_t frame = 0;

    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, frame & 1);
        frame++;

        uint32_t reads = eeprom_i2c_read_count();
        uint32_t writes = eeprom_i2c_write_count();

        if (reads != last_reads || writes != last_writes) {
            printf("  I2C activity: %lu bytes read, %lu bytes written (total)\n",
                   (unsigned long)reads, (unsigned long)writes);
            last_reads = reads;
            last_writes = writes;
        }

        if (!announced_header && eeprom_i2c_header_was_read()) {
            printf("  *** Header (byte 0) has been read -- badge has scanned this EEPROM ***\n");
            announced_header = true;
        }

        sleep_ms(500);
    }
}
