cmake_minimum_required(VERSION 3.13)

# mirror_debug: a purpose-built diagnostic firmware, separate from
# firmware/testcard/'s own bringup/bringup_debug -- built 2026-09-12 to
# test hazanjon's REAL flow3r_bsp_display_mirror.c wire protocol
# specifically (SPI mode 0, "TDHD" 4-byte header, one continuous CS-low
# burst per frame), which differs from what firmware/testcard/src/main.c's
# receiver was actually written against (the older badge_app/app.py
# synthetic-frame protocol: SPI mode 3/CPHA1, an 8-byte 0xA55A... marker
# embedded in a markerless continuous byte stream, no real CS framing).
# See hexigfx-mirror-status memory for the full history -- reusing
# testcard's own proven board file rather than duplicating it.
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/../testcard/boards)
set(PICO_BOARD adafruit_metro_rp2350)

include(pico_sdk_import.cmake)

project(mirror_debug C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(mirror_debug
    src/main.c
)

target_link_libraries(mirror_debug
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_spi
    hardware_vreg
)

# UART, not USB -- same reasoning as testcard/bringup_debug: stdio_usb
# shares clock infra with clk_sys, which clk_hstx follows undivided at
# 126MHz for 640x480@60, confirmed disrupting HSTX's own zero-margin video
# timing on real hardware. Wire the Debug Probe's UART bridge (/dev/ttyACM1)
# to read this.
pico_enable_stdio_usb(mirror_debug 0)
pico_enable_stdio_uart(mirror_debug 1)

pico_add_extra_outputs(mirror_debug)
