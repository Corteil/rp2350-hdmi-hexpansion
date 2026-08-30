cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD adafruit_metro_rp2350)

# The SD/FatFs library (below) needs pioasm to build its PIO-based SDIO
# backend, even though this project only uses the SPI backend. Building
# pioasm from source needs a host C++ compiler, which this machine
# doesn't have -- so point at the prebuilt Windows binary from
# raspberrypi/pico-sdk-tools instead (see firmware/phase0-metro/README.md
# for where it came from). Only set as a default; an explicit
# -Dpioasm_DIR=... on the command line still wins.
if (WIN32 AND NOT DEFINED pioasm_DIR)
    set(pioasm_HINT "$ENV{USERPROFILE}/.pico-sdk/tools/2.3.0/pioasm")
    if (EXISTS ${pioasm_HINT})
        set(pioasm_DIR ${pioasm_HINT})
    endif ()
endif ()

include(pico_sdk_import.cmake)

project(phase0_metro C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src fatfs_build)

add_executable(bringup
    src/main.c
    src/ctx_bench.c
    src/sd_bench.c
    src/hw_config.c
    src/psram_dma_bench.c
)

target_include_directories(bringup PRIVATE third_party/ctx)

target_compile_definitions(bringup PRIVATE USE_PRINTF)

target_link_libraries(bringup
    pico_stdlib
    hardware_psram
    hardware_dma
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

pico_enable_stdio_usb(bringup 1)
pico_enable_stdio_uart(bringup 0)

pico_add_extra_outputs(bringup)
