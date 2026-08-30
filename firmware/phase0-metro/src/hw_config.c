// Hardware config for no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (see
// third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/README.md, "Customizing
// for the hardware configuration") -- wires the library to the Metro
// RP2350's microSD socket: SPI0 on GPIO34 (SCK) / 35 (MOSI) / 36 (MISO),
// CS on GPIO39. Pins per ../boards/adafruit_metro_rp2350.h.
//
// Card detect (GPIO40) deliberately left unused: its active-high/low
// polarity for this specific socket isn't confirmed against Adafruit's
// schematic, and a wrong guess would misreport "no card" rather than
// just leaving it to f_mount()'s own success/failure to say so.

#include "hw_config.h"

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 34,
    .mosi_gpio = 35,
    .miso_gpio = 36,
    // Conservative starting rate for bring-up; the driver handles the
    // <=400kHz SD init sequence itself regardless of this value, which
    // only sets the post-init transfer speed.
    .baud_rate = 12 * 1000 * 1000,
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 39,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
    return (num == 0) ? &sd_card : NULL;
}
