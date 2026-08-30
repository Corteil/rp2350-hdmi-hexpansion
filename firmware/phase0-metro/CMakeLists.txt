cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD adafruit_metro_rp2350)

include(pico_sdk_import.cmake)

project(phase0_metro C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(bringup
    src/main.c
    src/ctx_bench.c
)

target_include_directories(bringup PRIVATE third_party/ctx)

target_link_libraries(bringup
    pico_stdlib
    hardware_psram
)

pico_enable_stdio_usb(bringup 1)
pico_enable_stdio_uart(bringup 0)

pico_add_extra_outputs(bringup)
