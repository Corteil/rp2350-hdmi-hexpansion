// Phase 0, Part A / B3 bring-up: Adafruit Feather RP2350 (+ HSTX).
//
// RP2350A -- the design-representative chip (see ../../README.md
// section 4.1, "Why the RP2350A and not the B"). Part A's real job is
// pin-map validation and HSTX DVI bring-up (A1-A5), which need the
// badge/protoboard hexpansion and DVI adapter this firmware doesn't
// touch. This is B3: reusing the Metro's PSRAM/ctx benchmarks
// (board-agnostic -- no board-specific pins) to get RP2350A vs RP2350B
// comparison numbers against firmware/phase0-metro's results.
//
// The Feather ships with its PSRAM footprint unpopulated (DNP) unless
// someone's soldered an APS6404L onto it -- see README section 8, Phase
// 0 rig table. Both benchmarks below already skip themselves cleanly if
// PSRAM isn't detected, so this runs safely either way; if you see them
// skip, that itself is the answer to "is PSRAM populated on this board."
//
// No SD card bench here -- the Feather has no microSD slot.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/psram.h"
#include "hardware/flash.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "ctx_bench.h"
#include "psram_dma_bench.h"

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

    // Same rationale as phase0-metro: wait for the terminal to actually
    // connect rather than gambling on a fixed sleep_ms().
    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(100);
    }

    printf("\nphase0-feather: PSRAM bring-up\n");
    if (!psram_is_available()) {
        printf("PSRAM: not detected (CS GPIO %d) - expected if this board's\n"
               "PSRAM footprint hasn't been populated (it ships DNP).\n",
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

        uint32_t clk_sys_hz = clock_get_hz(clk_sys);
        uint32_t divisor = qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS;
        printf("  clk_sys %.1f MHz, PSRAM QMI CLKDIV %u -> PSRAM clock ~%.1f MHz "
               "(quad SPI, theoretical read ceiling ~%.1f MB/s)\n",
               clk_sys_hz / 1e6, divisor, clk_sys_hz / (double)divisor / 1e6,
               (clk_sys_hz / (double)divisor) * 4.0 / 8.0 / (1024 * 1024));
    }

    ctx_bench_run();
    psram_dma_bench_run();

    uint32_t frame = 0;
    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, frame & 1);
        printf("phase0-feather alive: tick %lu\n", (unsigned long)frame);
        frame++;
        sleep_ms(500);
    }
}
