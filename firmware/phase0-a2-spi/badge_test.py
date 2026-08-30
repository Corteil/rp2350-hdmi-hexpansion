# Phase 0 A2, SPI0-slave half: badge-side (ESP32-S3) SPI master test
# script, run via mpremote against the RP2350's SPI0-slave firmware in
# this same directory (src/main.c).
#
# Port-specific: this hexpansion is in badge port 3 (C), whose
# hs_1..hs_4 are ESP32-S3 GPIO 34,33,47,48 (see main README section 1.2's
# port table). hs_1=HS_F, hs_2=HS_G, hs_3=HS_H, hs_4=HS_I (section 1.2).
# Our chosen role mapping (section 4.1's "resolved" note): HS_F=MOSI,
# HS_G=CS, HS_H=SCK, HS_I=MISO -- giving MOSI=34, CS=33, SCK=47, MISO=48.
#
# If the hexpansion is in a different port, recompute from the port
# table before running this.
#
# Run via: mpremote connect COMx run badge_test.py
# (NOT exec "$(cat ...)" -- PowerShell mangles that form; see the
# mpremote-gotcha memory note.) Interrupts whatever app is currently in
# the foreground (expected/fine, this is a deliberate test run, not a
# passive watch).
#
# v3: CS held low continuously for the whole burst (like v1), but using
# SPI mode 3 (CPOL=1, CPHA=1) instead of mode 0.
#
# Per the ARM PL022 spec itself (quoted in
# https://github.com/raspberrypi/pico-sdk/issues/941): CPHA=0 requires CS
# to pulse high between EVERY byte for back-to-back transfers (that's
# what v2 tried); CPHA=1 is the opposite -- CS stays low continuously for
# the whole burst, same as normal SPI, and the peripheral only returns to
# idle after the final bit of the last word. v2 kept per-byte CS toggling
# while switching to mode 3 (CPHA=1) on the RP2350 side -- combining the
# two mismatched recipes, which is why it still didn't work. This is the
# correct pairing for CPHA=1. RP2350 firmware (src/main.c) already uses
# spi_set_format(..., SPI_CPOL_1, SPI_CPHA_1, ...) to match.

from machine import SPI, Pin
import time

MOSI_PIN = 34
CS_PIN = 33
SCK_PIN = 47
MISO_PIN = 48

XFER_LEN = 16

spi = SPI(1, baudrate=100000, polarity=1, phase=1,
          sck=Pin(SCK_PIN), mosi=Pin(MOSI_PIN), miso=Pin(MISO_PIN))
cs = Pin(CS_PIN, Pin.OUT, value=1)

tx_buf = bytearray(range(XFER_LEN))
rx_buf = bytearray(XFER_LEN)

print("SPI0 link test v3: sending", XFER_LEN, "bytes, CS held low for the whole burst, mode 3")
for round_num in range(3):
    cs.value(0)
    spi.write_readinto(tx_buf, rx_buf)
    cs.value(1)
    print("round", round_num, "sent:", list(tx_buf), "received (from RP2350 TX):", list(rx_buf))
    time.sleep_ms(500)

print("Done. Expected received bytes: 0xa0..0xaf (RP2350's fixed test pattern).")
