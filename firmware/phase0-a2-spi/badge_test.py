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
# Run via: mpremote connect COMx exec "$(cat badge_test.py)"
# or copy to the badge and mpremote run it -- either interrupts whatever
# app is currently in the foreground (expected/fine, this is a
# deliberate test run, not a passive watch -- see the mpremote-gotcha
# memory note for the distinction).

from machine import SPI, Pin
import time

MOSI_PIN = 34
CS_PIN = 33
SCK_PIN = 47
MISO_PIN = 48

XFER_LEN = 16

spi = SPI(1, baudrate=100000, polarity=0, phase=0,
          sck=Pin(SCK_PIN), mosi=Pin(MOSI_PIN), miso=Pin(MISO_PIN))
cs = Pin(CS_PIN, Pin.OUT, value=1)

# Ascending pattern -- the RP2350 firmware's console will show this
# arriving on its "Received from badge (MOSI)" line if the link works.
tx_buf = bytearray(range(XFER_LEN))
rx_buf = bytearray(XFER_LEN)

print("SPI0 link test: sending", XFER_LEN, "bytes, 3 rounds, 500ms apart")
for round_num in range(3):
    cs.value(0)
    spi.write_readinto(tx_buf, rx_buf)
    cs.value(1)
    print("round", round_num, "sent:", list(tx_buf), "received (from RP2350 TX):", list(rx_buf))
    time.sleep_ms(500)

print("Done. Expected received bytes: 0xa0..0xaf (RP2350's fixed test pattern).")
