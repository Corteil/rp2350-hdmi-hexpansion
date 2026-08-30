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
    hardware_dma
    hardware_irq
    hardware_i2c
    hardware_spi
)

# No USB CDC on purpose: this firmware's job is to be indistinguishable
# from a real hexpansion (I2C0 target up in the first few hundred
# microseconds, per README section 5's mitigation #1) -- stdio_usb setup
# and TinyUSB init cost time on that budget for no benefit here, unlike
# the diagnostic-only Phase 0 bench firmwares.
pico_enable_stdio_usb(bringup 0)
pico_enable_stdio_uart(bringup 0)

pico_add_extra_outputs(bringup)
