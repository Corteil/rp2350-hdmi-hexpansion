cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD adafruit_metro_rp2350)

include(pico_sdk_import.cmake)

project(phase0_a2_spi C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(bringup
    src/main.c
)

target_link_libraries(bringup
    pico_stdlib
    hardware_spi
)

pico_enable_stdio_usb(bringup 1)
pico_enable_stdio_uart(bringup 0)

pico_add_extra_outputs(bringup)

# Diagnostic: raw GPIO edge counter, bypasses the SPI peripheral -- see
# src/gpio_probe.c header comment for why this exists.
add_executable(gpio_probe
    src/gpio_probe.c
)

target_link_libraries(gpio_probe
    pico_stdlib
)

pico_enable_stdio_usb(gpio_probe 1)
pico_enable_stdio_uart(gpio_probe 0)

pico_add_extra_outputs(gpio_probe)

# Diagnostic: reads SPI0's own SSPSR/CR0/CR1 registers directly -- see
# src/spi_status_probe.c header comment for why this exists.
add_executable(spi_status_probe
    src/spi_status_probe.c
)

target_link_libraries(spi_status_probe
    pico_stdlib
    hardware_spi
)

pico_enable_stdio_usb(spi_status_probe 1)
pico_enable_stdio_uart(spi_status_probe 0)

pico_add_extra_outputs(spi_status_probe)

# Diagnostic: SPI0 internal loopback self-test (LBM), no external pins
# involved -- see src/spi_loopback_test.c header comment for why this
# exists. Target name kept short (not spi_loopback_test) -- the combined
# OneDrive + pico-sdk path for this target's object files hits Windows'
# 260-char MAX_PATH right at custom_panic_function.S.obj.d otherwise.
add_executable(spi_lbm
    src/spi_loopback_test.c
)

target_link_libraries(spi_lbm
    pico_stdlib
    hardware_spi
)

pico_enable_stdio_usb(spi_lbm 1)
pico_enable_stdio_uart(spi_lbm 0)

pico_add_extra_outputs(spi_lbm)

# Phase 0 C3: one real 115,200-byte frame, timed end to end -- see
# src/spi_frame_test.c header comment.
add_executable(spi_frame
    src/spi_frame_test.c
)

target_link_libraries(spi_frame
    pico_stdlib
    hardware_spi
)

pico_enable_stdio_usb(spi_frame 1)
pico_enable_stdio_uart(spi_frame 0)

pico_add_extra_outputs(spi_frame)
