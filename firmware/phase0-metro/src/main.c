// Phase 0, Part B bring-up: Adafruit Metro RP2350.
//
// Milestone 1 (done): toolchain + board header sanity check — blink +
// USB CDC heartbeat.
// Milestone 2 (this file): PSRAM bring-up via pico-sdk's official
// hardware_psram library (added in SDK 2.3.0), auto-detected on
// PICO_PSRAM_CS_PIN=47 per the board header. Write/read-back test across
// the whole detected chip. Next: B1, the ctx rasterisation benchmark
// (see ../../README.md section 8, "Phase 0 - Part B").

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "hardware/flash.h"
#include "ctx_bench.h"
#include "sd_bench.h"

static bool psram_self_test(volatile uint32_t *psram, size_t words) {
    for (size_t i = 0; i < words; i++) {
        psram[i] = (uint32_t)i ^ 0xA5A5A5A5u;
    }
    for (size_t i = 0; i < words; i++) {
        uint32_t expected = (uint32_t)i ^ 0xA5A5A5A5u;
        uint32_t got = psram[i];
        if (got != expected) {
            printf("PSRAM mismatch at word %u: wrote 0x%08lx, read 0x%08lx\n",
                   (unsigned)i, (unsigned long)expected, (unsigned long)got);
            return false;
        }
    }
    return true;
}

int main(void) {
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Wait for a terminal to actually open the USB CDC port before printing
    // anything: bytes sent before that are simply dropped, not buffered, so
    // no terminal app can recover them after the fact -- a fixed sleep_ms()
    // just gambles on how fast the terminal gets opened. Blink fast while
    // waiting for visible feedback, but give up after 10s so the board
    // still runs standalone (e.g. on USB power with nothing attached).
    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(100);
    }

    printf("\nphase0-metro: PSRAM bring-up\n");
    if (!psram_is_available()) {
        printf("PSRAM: not detected (CS GPIO %d) - check the board is the\n"
               "'with PSRAM' Metro RP2350 variant and the chip is fitted.\n",
               PICO_PSRAM_CS_PIN);
    } else {
        size_t size = psram_get_size();
        printf("PSRAM: detected, %u bytes (%u MiB) on CS GPIO %d\n",
               (unsigned)size, (unsigned)(size / (1024 * 1024)), PICO_PSRAM_CS_PIN);

        volatile uint32_t *psram_base =
            (volatile uint32_t *)(XIP_BASE + flash_devinfo_size_to_bytes(FLASH_DEVINFO_SIZE_MAX));
        size_t words = size / sizeof(uint32_t);

        absolute_time_t start = get_absolute_time();
        bool ok = psram_self_test(psram_base, words);
        int64_t us = absolute_time_diff_us(start, get_absolute_time());

        printf("PSRAM self-test (write %u words, read back, verify): %s\n",
               (unsigned)words, ok ? "PASS" : "FAIL");
        printf("  %.2f ms total, ~%.2f MB/s (write+read combined)\n",
               us / 1000.0, (2.0 * size) / (us / 1e6) / (1024 * 1024));
    }

    ctx_bench_run();
    sd_bench_run();

    uint32_t frame = 0;
    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, frame & 1);
        printf("phase0-metro alive: tick %lu\n", (unsigned long)frame);
        frame++;
        sleep_ms(500);
    }
}
