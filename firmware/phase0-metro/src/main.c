// Phase 0, Part B bring-up: Adafruit Metro RP2350.
//
// First rung of the ladder before B1 (ctx rasterisation benchmark) and B2
// (microSD + PSRAM-backed scanout) — see ../../README.md section 8, "Phase
// 0 - Part B". This just proves the toolchain, board header and a basic
// GPIO + USB CDC path all work on real silicon.

#include <stdio.h>
#include "pico/stdlib.h"

int main(void) {
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    uint32_t frame = 0;
    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, frame & 1);
        printf("phase0-metro alive: tick %lu\n", (unsigned long)frame);
        frame++;
        sleep_ms(500);
    }
}
