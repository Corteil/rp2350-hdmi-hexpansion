// Phase 0 A2 diagnostic #2: reads SPI0's own SSPSR/CR0/CR1 registers
// directly instead of calling spi_write_read_blocking(). Built because
// the wiring is now independently confirmed correct (src/gpio_probe.c
// showed clean CS/SCK/MOSI activity matching the badge's transfer) but
// src/main.c still never unblocks -- this checks whether the SPI0
// peripheral itself ever sees a valid framed transaction (BSY/RNE bits)
// even if spi_write_read_blocking()'s higher-level polling loop doesn't
// return.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define SPI_PORT spi0
#define PIN_MOSI 20
#define PIN_CS   21
#define PIN_SCK  22
#define PIN_MISO 23

int main(void) {
    stdio_init_all();

    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        sleep_ms(100);
    }

    spi_init(SPI_PORT, 1000 * 1000);
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);  // mode 3, matching main.c/badge_test.py's current attempt
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    uint32_t cr0 = spi_get_hw(SPI_PORT)->cr0;
    uint32_t cr1 = spi_get_hw(SPI_PORT)->cr1;
    printf("\nspi_status_probe: SPI0 slave, polling SSPSR directly (bypassing spi_write_read_blocking)\n");
    printf("FUNCSEL readback: MOSI=%d CS=%d SCK=%d MISO=%d (GPIO_FUNC_SPI=%d -- should all match)\n",
           gpio_get_function(PIN_MOSI), gpio_get_function(PIN_CS),
           gpio_get_function(PIN_SCK), gpio_get_function(PIN_MISO),
           (int)GPIO_FUNC_SPI);
    printf("cr0=0x%08lx cr1=0x%08lx  SSE=%d MS=%d DSS=%lu SPO=%d SPH=%d FRF=%lu\n",
           (unsigned long)cr0, (unsigned long)cr1,
           (int)((cr1 & SPI_SSPCR1_SSE_BITS) != 0),
           (int)((cr1 & SPI_SSPCR1_MS_BITS) != 0),
           (unsigned long)(cr0 & SPI_SSPCR0_DSS_BITS),
           (int)((cr0 & SPI_SSPCR0_SPO_BITS) != 0),
           (int)((cr0 & SPI_SSPCR0_SPH_BITS) != 0),
           (unsigned long)((cr0 & SPI_SSPCR0_FRF_BITS) >> SPI_SSPCR0_FRF_LSB));
    printf("Run badge_test.py now. Reporting every SSPSR change plus cumulative RX byte count.\n\n");

    uint32_t last_sr = 0xffffffff;
    uint32_t rx_bytes = 0;

    while (true) {
        uint32_t sr = spi_get_hw(SPI_PORT)->sr;
        if (sr & SPI_SSPSR_RNE_BITS) {
            (void)spi_get_hw(SPI_PORT)->dr;  // drain so RNE can clear
            rx_bytes++;
        }
        if (sr != last_sr) {
            printf("SR=0x%02lx  BSY=%d RFF=%d RNE=%d TNF=%d TFE=%d   rx_bytes=%lu\n",
                   (unsigned long)sr,
                   (int)((sr & SPI_SSPSR_BSY_BITS) != 0),
                   (int)((sr & SPI_SSPSR_RFF_BITS) != 0),
                   (int)((sr & SPI_SSPSR_RNE_BITS) != 0),
                   (int)((sr & SPI_SSPSR_TNF_BITS) != 0),
                   (int)((sr & SPI_SSPSR_TFE_BITS) != 0),
                   (unsigned long)rx_bytes);
            last_sr = sr;
        }
    }
}
