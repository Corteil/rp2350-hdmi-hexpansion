// Phase 0 A2, SPI0-slave half: bring up RP2350's SPI0 in slave mode on
// the badge-facing pins, on the Metro RP2350 -- see the main README's
// A2 section (bench board choice, wiring, chosen HS_x role mapping) and
// section 4.1.
//
// Unlike the I2C0-target half (firmware/phase0-a2-eeprom, confirmed
// working -- the badge scans I2C on its own), SPI0 needs something on
// the BADGE side actively driving it as SPI master; nothing happens
// here until a badge-side MicroPython test script runs. See this
// project's README for that script.
//
// Register-level technique (spi_init + spi_set_slave + gpio_set_function
// on all 4 pins, CSn included -- hardware-handled automatically, no
// manual GPIO toggling needed) is the same pattern as the official
// raspberrypi/pico-examples spi/spi_master_slave/spi_slave example
// (BSD-3), not copied verbatim (different pins, different test
// protocol, added USB CDC reporting/LED heartbeat matching this
// project's other Phase 0 firmware).

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

// Bench-test wiring: GPIO20=RX(badge's MOSI, HS_F), GPIO21=CSn(HS_G),
// GPIO22=SCK(HS_H), GPIO23=TX(badge's MISO, HS_I) -- see main README
// section 4.1's "resolved" note on how this role assignment was chosen.
#define SPI_PORT   spi0
#define PIN_MOSI   20  // badge -> RP2350 (RP2350's RX/data-in)
#define PIN_CS     21
#define PIN_SCK    22
#define PIN_MISO   23  // RP2350 -> badge (RP2350's TX/data-out)

#define XFER_LEN   16

int main(void) {
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

    // Baud rate here is nominal -- in slave mode the PL022 derives its
    // bit timing from whatever SCK the master (badge) actually drives,
    // not from this value. Passed anyway since spi_init() requires it.
    spi_init(SPI_PORT, 1000 * 1000);
    spi_set_slave(SPI_PORT, true);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    printf("\nphase0-a2-spi: SPI0 slave running (MOSI=GPIO%d CS=GPIO%d SCK=GPIO%d MISO=GPIO%d)\n",
           PIN_MOSI, PIN_CS, PIN_SCK, PIN_MISO);
    printf("  Waiting for the badge to drive a %d-byte transfer. This call blocks --\n", XFER_LEN);
    printf("  nothing will print until the badge's SPI master script actually runs.\n\n");

    // Known pattern transmitted to the badge every round, so the badge
    // can verify what it received matches what this firmware sent
    // (round 0's shown here; content doesn't change between rounds).
    uint8_t tx_buf[XFER_LEN];
    for (int i = 0; i < XFER_LEN; i++) {
        tx_buf[i] = (uint8_t)(0xA0 + i);  // 0xA0, 0xA1, ... 0xAF
    }

    for (uint32_t round = 0; ; round++) {
        uint8_t rx_buf[XFER_LEN];
        int got = spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, XFER_LEN);

        gpio_put(PICO_DEFAULT_LED_PIN, round & 1);

        printf("round %lu: transferred %d bytes. Received from badge (MOSI): ",
               (unsigned long)round, got);
        for (int i = 0; i < got; i++) {
            printf("%02x ", rx_buf[i]);
        }
        printf("\n");
    }
}
