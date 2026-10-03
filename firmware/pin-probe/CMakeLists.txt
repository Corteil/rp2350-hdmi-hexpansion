cmake_minimum_required(VERSION 3.13)

# pin-probe: minimal Pico 2 bench tool. Watches every header GPIO as a
# plain input (no pulls) and counts level transitions per pin, so a badge
# pin-tester app blinking HS_F/G/H/I shows up on whichever Pico pin the
# wire actually lands on. Results are read over SWD (no USB/UART needed):
#   toggles[N], level_mask, and a 1Hz heartbeat on the onboard LED (GP25),
#   which goes solid-on for 300ms after any transition.
set(PICO_BOARD pico2)
include(pico_sdk_import.cmake)

project(pin_probe C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(pin_probe src/main.c)
target_link_libraries(pin_probe pico_stdlib)
pico_enable_stdio_usb(pin_probe 0)
pico_enable_stdio_uart(pin_probe 0)
pico_add_extra_outputs(pin_probe)
