/*
 * Board header for the Adafruit Metro RP2350 (with PSRAM), used as the
 * Part B (PSRAM + microSD) bench in Phase 0 of the rp2350-hdmi-hexpansion
 * design (see ../../README.md section 8).
 *
 * Not an official pico-sdk board header — Adafruit does not ship one for
 * bare pico-sdk (only Arduino-Pico / CircuitPython). Pin numbers below are
 * from Adafruit's own pinout page (learn.adafruit.com/adafruit-metro-rp2350
 * /pinouts) plus the RP2350B's fixed QMI CS1 pin for PSRAM (GPIO47 — the
 * same pin Pimoroni's Pico Plus 2 RP2350 board header uses; it's a
 * silicon-fixed QMI alternate function, not a board-specific routing
 * choice, so it's shared by every RP2350B+PSRAM board).
 *
 * SPDX-License-Identifier: BSD-3-Clause (matches pico-sdk board headers)
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_ADAFRUIT_METRO_RP2350_H
#define _BOARDS_ADAFRUIT_METRO_RP2350_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define ADAFRUIT_METRO_RP2350

// --- RP2350 VARIANT ---
// RP2350B (QFN-80, 48 GPIO) — needed for the extra GPIOs this board exposes
// (SD card, USB host, PSRAM CS on GPIO47) and to match §4.1's B3 comparison.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// --- BOARD SPECIFIC ---
#define ADAFRUIT_METRO_RP2350_PSRAM_CS_PIN 47

#define ADAFRUIT_METRO_RP2350_NEOPIXEL_PIN 25

#define ADAFRUIT_METRO_RP2350_SD_SCK_PIN 34
#define ADAFRUIT_METRO_RP2350_SD_MOSI_PIN 35
#define ADAFRUIT_METRO_RP2350_SD_MISO_PIN 36
#define ADAFRUIT_METRO_RP2350_SD_CS_PIN 39
#define ADAFRUIT_METRO_RP2350_SD_CARD_DETECT_PIN 40

// --- UART --- (labelled TX/RX on the board; UART0, RX/TX switch in its
// default position)
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED --- (onboard red LED, next to BOOT/RESET)
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 23
#endif

// --- I2C --- (STEMMA QT socket)
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 20
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 21
#endif

// --- SPI --- (microSD slot; no separate general-purpose SPI header on this
// board, so the default SPI is the SD card's)
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN ADAFRUIT_METRO_RP2350_SD_SCK_PIN
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN ADAFRUIT_METRO_RP2350_SD_MOSI_PIN
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN ADAFRUIT_METRO_RP2350_SD_MISO_PIN
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN ADAFRUIT_METRO_RP2350_SD_CS_PIN
#endif

// --- FLASH --- (16 MB, matches the hexpansion BOM per README §8)
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#endif
