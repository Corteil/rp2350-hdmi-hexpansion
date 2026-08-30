// Phase 0 A2 diagnostic: raw GPIO edge counter for the SPI0 bench pins,
// bypassing the SPI peripheral entirely. Built because spi_write_read_blocking()
// in src/main.c never unblocked despite the badge's SPI master script (badge_test.py)
// completing all 3 rounds without error -- this isolates "is the wiring live at all"
// from "is the SPI peripheral configured/framing correctly".
//
// Watches MOSI/CS/SCK (GPIO20/21/22) as plain SIO inputs with edge IRQs, and
// separately polls MISO (GPIO23) as a level -- not IRQ, since GPIO23 doubles as
// this board's onboard LED (see main.c's PIN_MISO vs the board header's
// PICO_DEFAULT_LED_PIN, both 23; irrelevant here since this probe never claims
// GPIO_FUNC_SPI on it).
//
// Not part of the normal A2 test flow -- see main README's A2 section for how
// this diagnostic result gets folded back into the design doc once resolved.

#include <stdio.h>
#include "pico/stdlib.h"

#define PIN_MOSI 20
#define PIN_CS   21
#define PIN_SCK  22
#define PIN_MISO 23

static volatile uint32_t edge_count[30];

static void gpio_probe_callback(uint gpio, uint32_t events) {
    (void)events;
    if (gpio < 30) {
        edge_count[gpio]++;
    }
}

int main(void) {
    stdio_init_all();

    absolute_time_t wait_until = make_timeout_time_ms(10000);
    while (!stdio_usb_connected() && !time_reached(wait_until)) {
        sleep_ms(100);
    }

    gpio_init(PIN_MOSI);
    gpio_set_dir(PIN_MOSI, GPIO_IN);
    gpio_disable_pulls(PIN_MOSI);

    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_IN);
    gpio_disable_pulls(PIN_CS);

    gpio_init(PIN_SCK);
    gpio_set_dir(PIN_SCK, GPIO_IN);
    gpio_disable_pulls(PIN_SCK);

    gpio_init(PIN_MISO);
    gpio_set_dir(PIN_MISO, GPIO_IN);
    gpio_disable_pulls(PIN_MISO);

    gpio_set_irq_enabled_with_callback(PIN_MOSI, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_probe_callback);
    gpio_set_irq_enabled(PIN_CS, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(PIN_SCK, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(PIN_MISO, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    printf("\ngpio_probe: watching MOSI=GPIO%d CS=GPIO%d SCK=GPIO%d MISO=GPIO%d as plain inputs\n",
           PIN_MOSI, PIN_CS, PIN_SCK, PIN_MISO);
    printf("  (no pulls, no SPI peripheral -- pure edge counting)\n");
    printf("  Run badge_test.py now. Printing levels + cumulative edge counts every 500ms.\n\n");

    while (true) {
        printf("MOSI=%d(edges=%lu)  CS=%d(edges=%lu)  SCK=%d(edges=%lu)  MISO=%d(edges=%lu)\n",
               gpio_get(PIN_MOSI), (unsigned long)edge_count[PIN_MOSI],
               gpio_get(PIN_CS),   (unsigned long)edge_count[PIN_CS],
               gpio_get(PIN_SCK),  (unsigned long)edge_count[PIN_SCK],
               gpio_get(PIN_MISO), (unsigned long)edge_count[PIN_MISO]);
        sleep_ms(500);
    }
}
