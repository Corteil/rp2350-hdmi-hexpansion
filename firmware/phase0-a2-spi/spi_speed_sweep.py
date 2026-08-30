# Phase 0 C3 -- SPI link speed sweep. Badge-side (ESP32-S3) SPI master,
# same wiring/roles as badge_test.py (badge port 3/C: MOSI=GPIO34,
# CS=GPIO33, SCK=GPIO47, MISO=GPIO48; mode 3, CS held low for the whole
# burst -- see badge_test.py's header for why). RP2350 side needs no
# rebuild between rates: in slave mode the PL022 derives its bit timing
# from whatever SCK the master actually drives (main.c's own comment),
# so the already-flashed bringup.uf2 self-adapts. Bench wiring for this
# run: dupont jumpers, not the official edge connector -- expect worse
# signal integrity / a lower ceiling than a real hexpansion PCB.
#
# Run via: mpremote connect COMx run spi_speed_sweep.py
# Interrupts the badge's foreground app, same as badge_test.py.
#
# For each rate: 5 rounds of a known 16-byte ascending pattern, CS low
# for the whole round, checked byte-for-byte against the RP2350's fixed
# 0xa0..0xaf reply. Reports a pass count per rate, then the highest rate
# with all 5 rounds clean.

from machine import SPI, Pin
import time

MOSI_PIN = 34
CS_PIN = 33
SCK_PIN = 47
MISO_PIN = 48

XFER_LEN = 16
ROUNDS = 5
EXPECTED = bytes(0xA0 + i for i in range(XFER_LEN))

RATES = [
    100_000, 250_000, 500_000, 1_000_000, 2_000_000, 4_000_000,
    6_000_000, 8_000_000, 10_000_000, 15_000_000, 20_000_000,
    30_000_000, 40_000_000,
]

cs = Pin(CS_PIN, Pin.OUT, value=1)

print("Phase 0 C3: SPI0 speed sweep,", ROUNDS, "rounds/rate, mode 3, CS held low per round")
print("rate_hz      pass/rounds   first_mismatch")

best_clean_rate = None

for rate in RATES:
    try:
        spi = SPI(1, baudrate=rate, polarity=1, phase=1,
                  sck=Pin(SCK_PIN), mosi=Pin(MOSI_PIN), miso=Pin(MISO_PIN))
    except Exception as e:
        print("%-12d construction failed: %r" % (rate, e))
        continue

    tx_buf = bytearray(range(XFER_LEN))
    passes = 0
    first_mismatch = None

    for round_num in range(ROUNDS):
        rx_buf = bytearray(XFER_LEN)
        try:
            cs.value(0)
            spi.write_readinto(tx_buf, rx_buf)
            cs.value(1)
        except Exception as e:
            if first_mismatch is None:
                first_mismatch = "round %d exception: %r" % (round_num, e)
            continue

        if bytes(rx_buf) == EXPECTED:
            passes += 1
        elif first_mismatch is None:
            first_mismatch = "round %d got %s" % (round_num, list(rx_buf))

        time.sleep_ms(20)

    spi.deinit()

    status = "%d/%d" % (passes, ROUNDS)
    print("%-12d %-13s %s" % (rate, status, first_mismatch or ""))

    if passes == ROUNDS:
        best_clean_rate = rate

    time.sleep_ms(100)

print()
if best_clean_rate is not None:
    print("Highest rate with all", ROUNDS, "rounds clean:", best_clean_rate, "Hz")
else:
    print("No rate achieved a fully clean run.")
