#include "pico/stdlib.h"
#include "hardware/gpio.h"

// Header GPIOs on a Pico 2 (GP23/24/29 are internal, GP25 is the LED).
#define LED_PIN 25
static const uint8_t watch_pins[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 26, 27, 28,
};
#define N_WATCH (sizeof(watch_pins) / sizeof(watch_pins[0]))

// Read over SWD: nm pin_probe.elf | grep -E ' (toggles|level_mask|seq)$'
// toggles[i] counts transitions seen on watch_pins[i].
volatile uint32_t toggles[N_WATCH];
volatile uint32_t level_mask;   // GPIO_IN snapshot (bit n = GPn)
volatile uint32_t seq;          // increments every loop, proves it's alive

int main(void) {
    for (uint i = 0; i < N_WATCH; i++) {
        gpio_init(watch_pins[i]);
        gpio_set_dir(watch_pins[i], GPIO_IN);
        gpio_disable_pulls(watch_pins[i]);
    }
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    uint32_t prev = gpio_get_all();
    absolute_time_t led_hold = nil_time;
    absolute_time_t next_hb = make_timeout_time_ms(500);
    bool hb = false;

    for (;;) {
        uint32_t now = gpio_get_all();
        uint32_t diff = now ^ prev;
        if (diff) {
            for (uint i = 0; i < N_WATCH; i++) {
                if (diff & (1u << watch_pins[i])) toggles[i]++;
            }
            led_hold = make_timeout_time_ms(300);
        }
        prev = now;
        level_mask = now;
        seq++;

        if (!is_nil_time(led_hold) && !time_reached(led_hold)) {
            gpio_put(LED_PIN, 1);
        } else {
            if (time_reached(next_hb)) {
                hb = !hb;
                next_hb = delayed_by_ms(next_hb, 500);
            }
            gpio_put(LED_PIN, hb);
        }
    }
}
