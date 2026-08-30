// Phase 0 C3, second half: push one real 115,200-byte frame (the exact
// size of the badge's mirrored RGB565 framebuffer, main README section
// 3.1) over SPI0-slave and time it end to end. Same pin roles and mode 3
// / CS-continuous protocol as src/main.c (see that file's header for the
// mode-3 rationale and this project's own HS_x role choice).
//
// One small throwaway transfer first, unmeasured: the speed sweep
// (spi_speed_sweep.py) found that the very first transfer after the
// badge constructs a fresh machine.SPI object is reliably corrupted in a
// fixed, deterministic way (an ESP32-S3 master-side peripheral-init
// artifact -- every subsequent transfer on the same object is clean),
// so the real timed frame needs to not be that first transfer.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define SPI_PORT    spi0
#define PIN_MOSI    20
#define PIN_CS      21
#define PIN_SCK     22
#define PIN_MISO    23

#define WARMUP_LEN  16
#define FRAME_LEN   (240 * 240 * 2)  // 115200 -- badge's mirrored RGB565 framebuffer, README section 3.1

static uint8_t tx_buf[FRAME_LEN];
static uint8_t rx_buf[FRAME_LEN];

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

    spi_init(SPI_PORT, 1000 * 1000);  // nominal only -- slave derives timing from the badge's SCK
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);  // mode 3, see main.c
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    printf("\nspi_frame_test: SPI0 slave, one %d-byte warmup then one %d-byte timed frame\n",
           WARMUP_LEN, FRAME_LEN);
    printf("  (MOSI=GPIO%d CS=GPIO%d SCK=GPIO%d MISO=GPIO%d)\n", PIN_MOSI, PIN_CS, PIN_SCK, PIN_MISO);
    printf("  Waiting for the badge -- nothing prints until it runs its script.\n\n");

    // Warmup: content doesn't matter, not verified, not timed.
    uint8_t warm_tx[WARMUP_LEN] = {0};
    uint8_t warm_rx[WARMUP_LEN];
    spi_write_read_blocking(SPI_PORT, warm_tx, warm_rx, WARMUP_LEN);
    printf("warmup done (discarded, expected to be the corrupted first transfer)\n");

    // Real frame: fixed pattern (i & 0xff), so the badge can verify what it
    // received, and this firmware can verify what it received matches the
    // badge's own known pattern.
    for (uint32_t i = 0; i < FRAME_LEN; i++) {
        tx_buf[i] = (uint8_t)(i & 0xff);
    }

    absolute_time_t start = get_absolute_time();
    int got = spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, FRAME_LEN);
    int64_t us = absolute_time_diff_us(start, get_absolute_time());

    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < (uint32_t)got; i++) {
        if (rx_buf[i] != (uint8_t)(i & 0xff)) {
            mismatches++;
        }
    }

    double mbps = (got > 0 && us > 0) ? ((double)got / (double)us) : 0.0;  // bytes/us == MB/s

    printf("frame: got %d/%d bytes in %lld us  (%.2f MB/s)  mismatches=%lu\n",
           got, FRAME_LEN, (long long)us, mbps, (unsigned long)mismatches);
    if (mismatches == 0 && got == FRAME_LEN) {
        printf("RESULT: CLEAN -- full frame received correctly\n");
    } else {
        printf("RESULT: CORRUPTED -- %lu/%d bytes wrong\n", (unsigned long)mismatches, FRAME_LEN);
    }

    while (true) {
        sleep_ms(1000);
    }
}
