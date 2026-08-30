// Phase 0 A2 diagnostic #3: RP2350 SPI0 internal loopback self-test.
// Built because slave mode never sees any peripheral reaction (SSPSR
// pinned at idle) despite wiring and register config both independently
// confirmed correct (src/gpio_probe.c, src/spi_status_probe.c) -- this
// isolates "is SPI0 functional at all in this build/environment" from
// "is it specifically slave-mode-driven-by-an-external-clock that's
// broken". PL022's LBM bit internally shorts TX to RX, entirely bypassing
// external pins -- no badge, no wiring needed for this test.
//
// LBM isn't exposed by pico-sdk's spi.h API, so it's set with a raw
// register read-modify-write, following the same disable/modify/re-enable
// pattern spi_set_slave() itself uses internally (SSE must be off while
// changing CR1 mode bits).

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define SPI_PORT spi0
#define XFER_LEN 16

int main(void) {
    stdio_init_all();

    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        sleep_ms(100);
    }

    spi_init(SPI_PORT, 1000 * 1000);  // master mode (default), not slave

    hw_clear_bits(&spi_get_hw(SPI_PORT)->cr1, SPI_SSPCR1_SSE_BITS);
    hw_set_bits(&spi_get_hw(SPI_PORT)->cr1, SPI_SSPCR1_LBM_BITS);
    hw_set_bits(&spi_get_hw(SPI_PORT)->cr1, SPI_SSPCR1_SSE_BITS);

    printf("\nspi_loopback_test: SPI0 master + LBM (internal loopback, no pins involved)\n");
    printf("cr1=0x%08lx (LBM=%d SSE=%d MS=%d)\n",
           (unsigned long)spi_get_hw(SPI_PORT)->cr1,
           (int)((spi_get_hw(SPI_PORT)->cr1 & SPI_SSPCR1_LBM_BITS) != 0),
           (int)((spi_get_hw(SPI_PORT)->cr1 & SPI_SSPCR1_SSE_BITS) != 0),
           (int)((spi_get_hw(SPI_PORT)->cr1 & SPI_SSPCR1_MS_BITS) != 0));

    uint8_t tx_buf[XFER_LEN], rx_buf[XFER_LEN];
    for (int i = 0; i < XFER_LEN; i++) tx_buf[i] = (uint8_t)(0x10 + i);

    absolute_time_t start = get_absolute_time();
    int got = spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, XFER_LEN);
    int64_t us = absolute_time_diff_us(start, get_absolute_time());

    printf("spi_write_read_blocking returned after %lld us, got=%d\n", (long long)us, got);
    printf("sent:     ");
    for (int i = 0; i < XFER_LEN; i++) printf("%02x ", tx_buf[i]);
    printf("\nreceived: ");
    for (int i = 0; i < got; i++) printf("%02x ", rx_buf[i]);
    printf("\n");

    bool match = (got == XFER_LEN);
    for (int i = 0; match && i < XFER_LEN; i++) {
        if (rx_buf[i] != tx_buf[i]) match = false;
    }
    printf(match ? "RESULT: MATCH -- SPI0 peripheral is functional (loopback worked)\n"
                 : "RESULT: MISMATCH -- SPI0 peripheral itself is not working correctly\n");

    while (true) {
        sleep_ms(1000);
    }
}
