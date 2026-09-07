cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD adafruit_metro_rp2350)

include(pico_sdk_import.cmake)

project(testcard C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(bringup
    src/main.c
    src/eeprom_i2c.c
)

target_link_libraries(bringup
    pico_stdlib
    pico_unique_id
    pico_multicore
    hardware_dma
    hardware_irq
    hardware_i2c
    hardware_spi
    hardware_pio
    hardware_vreg
)

# Heartbeat LED via the Metro's onboard NeoPixel (GPIO25) -- reuses
# pico-sdk's own official ws2812.pio program (BSD-3, Raspberry Pi) rather
# than a hand-rolled/copied one. See src/main.c's heartbeat comment for
# why GPIO25/NeoPixel instead of PICO_DEFAULT_LED_PIN (GPIO23 is already
# claimed as SPI0's MISO in this firmware).
pico_generate_pio_header(bringup ${PICO_SDK_PATH}/src/rp2_common/pico_status_led/ws2812.pio)

# No USB CDC on purpose: this firmware's job is to be indistinguishable
# from a real hexpansion (I2C0 target up in the first few hundred
# microseconds, per README section 5's mitigation #1) -- stdio_usb setup
# and TinyUSB init cost time on that budget for no benefit here, unlike
# the diagnostic-only Phase 0 bench firmwares.
pico_enable_stdio_usb(bringup 0)
pico_enable_stdio_uart(bringup 0)

pico_add_extra_outputs(bringup)

# Diagnostic twin of bringup, with USB CDC + printf() instrumentation
# (src/main.c's #ifdef DEBUG_SERIAL blocks) enabled -- built to find why
# framebuf stopped visibly updating despite the badge reporting every SPI0
# row transfer as successful (2026-08-30). Not part of the delivered
# firmware; keep or remove once root-caused.
add_executable(bringup_debug
    src/main.c
    src/eeprom_i2c.c
)
target_link_libraries(bringup_debug
    pico_stdlib
    pico_unique_id
    pico_multicore
    hardware_dma
    hardware_irq
    hardware_i2c
    hardware_spi
    hardware_pio
    hardware_vreg
)
target_compile_definitions(bringup_debug PRIVATE DEBUG_SERIAL=1)
pico_generate_pio_header(bringup_debug ${PICO_SDK_PATH}/src/rp2_common/pico_status_led/ws2812.pio)
pico_enable_stdio_usb(bringup_debug 1)
pico_enable_stdio_uart(bringup_debug 0)
pico_add_extra_outputs(bringup_debug)
