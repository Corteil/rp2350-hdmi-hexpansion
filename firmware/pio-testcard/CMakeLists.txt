cmake_minimum_required(VERSION 3.13)

# pio-testcard: firmware/testcard's real HSTX/DVI video pipeline and
# double-buffered framebuffer, combined with firmware/mirror_debug's
# PIO-based SPI0 slave receiver (2026-09-19 fix -- see spi_slave_rx.pio's
# own comment and hexigfx-mirror-status memory for the full evidence
# trail: the RP2350's hardware SPI0/PL022 peripheral loses byte-alignment
# after the first word of a continuous CS-low burst and never recovers,
# proven via a real logic-analyzer capture + a fine-grained SSPSR/DMA
# trace). testcard/src/main.c's own SPI0 receiver was built against the
# OLDER badge_app/app.py synthetic-frame protocol (mode 3, an 8-byte
# 0xA55A marker, no real CS framing, sent by a bespoke Python test
# generator in THIS repo) -- this firmware instead speaks hazanjon's REAL
# `flow3r_bsp_display_mirror.c` protocol (mode 0, a real 4-byte "TDHD"
# header, one continuous CS-low burst per frame), matching what mirror_debug
# proved works on real hardware. Also carries testcard's hexpansion-EEPROM
# emulation (src/eeprom_i2c.c, copied verbatim 2026-09-20 -- see that
# file's own header comment) so this bench setup auto-enumerates and
# auto-launches the packed app.py instead of needing a manual sideload.
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/../testcard/boards)
set(PICO_BOARD adafruit_metro_rp2350)

include(pico_sdk_import.cmake)

project(pio_testcard C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pio_testcard
    src/main.c
    src/eeprom_i2c.c
)

target_link_libraries(pio_testcard
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_dma
    hardware_irq
    hardware_pio
    hardware_vreg
    hardware_i2c
)

pico_generate_pio_header(pio_testcard ${PICO_SDK_PATH}/src/rp2_common/pico_status_led/ws2812.pio)
pico_generate_pio_header(pio_testcard ${CMAKE_CURRENT_LIST_DIR}/src/spi_slave_rx.pio)

# No USB CDC: stdio_usb shares clock infrastructure with clk_sys, which
# clk_hstx follows undivided at the 126MHz this firmware sets for
# 640x480@60 -- confirmed (in testcard's own history) to disrupt HSTX's
# zero-timing-margin video output on real hardware.
pico_enable_stdio_usb(pio_testcard 0)
pico_enable_stdio_uart(pio_testcard 0)

pico_add_extra_outputs(pio_testcard)

# Diagnostic twin, with USB CDC disabled but UART printf() instrumentation
# enabled (src/main.c's #ifdef DEBUG_SERIAL blocks) -- same reasoning and
# same UART wiring (Debug Probe's UART bridge, GPIO0/1) as testcard's own
# bringup_debug.
add_executable(pio_testcard_debug
    src/main.c
    src/eeprom_i2c.c
)
target_link_libraries(pio_testcard_debug
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_dma
    hardware_irq
    hardware_pio
    hardware_vreg
    hardware_i2c
)
target_compile_definitions(pio_testcard_debug PRIVATE DEBUG_SERIAL=1)
pico_generate_pio_header(pio_testcard_debug ${PICO_SDK_PATH}/src/rp2_common/pico_status_led/ws2812.pio)
pico_generate_pio_header(pio_testcard_debug ${CMAKE_CURRENT_LIST_DIR}/src/spi_slave_rx.pio)
pico_enable_stdio_usb(pio_testcard_debug 0)
pico_enable_stdio_uart(pio_testcard_debug 1)
pico_add_extra_outputs(pio_testcard_debug)
