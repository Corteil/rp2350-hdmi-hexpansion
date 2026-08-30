# Phase 0 C3, second half -- badge-side counterpart to src/spi_frame_test.c.
# Pushes one real 115,200-byte frame (240x240 RGB565, the badge's actual
# mirrored-framebuffer size, main README section 3.1) over SPI0 and times
# it end to end from the master's own clock. Same pins/mode as
# badge_test.py / spi_speed_sweep.py.
#
# Run via: mpremote connect COMx run spi_frame_timing.py
#
# RATE_HZ defaults to 10 MHz -- comfortably inside the clean 100kHz-20MHz
# range spi_speed_sweep.py found over dupont-wire bench wiring. Change it
# to try other rates once the wiring is a real PCB trace.
#
# One small throwaway transfer first, unmeasured, matching
# src/spi_frame_test.c's warmup -- see that file's header for why (the
# sweep found the first transfer after constructing a fresh machine.SPI
# object is reliably corrupted, a badge-side ESP32-S3 peripheral-init
# artifact, self-corrects from the second transfer on).

from machine import SPI, Pin
import time

MOSI_PIN = 34
CS_PIN = 33
SCK_PIN = 47
MISO_PIN = 48

RATE_HZ = 20_000_000
WARMUP_LEN = 16
FRAME_LEN = 240 * 240 * 2  # 115200

spi = SPI(1, baudrate=RATE_HZ, polarity=1, phase=1,
          sck=Pin(SCK_PIN), mosi=Pin(MOSI_PIN), miso=Pin(MISO_PIN))
cs = Pin(CS_PIN, Pin.OUT, value=1)

print("Phase 0 C3: one-shot %d-byte frame timing at %d Hz" % (FRAME_LEN, RATE_HZ))

# Warmup -- discarded.
warm_tx = bytearray(WARMUP_LEN)
warm_rx = bytearray(WARMUP_LEN)
cs.value(0)
spi.write_readinto(warm_tx, warm_rx)
cs.value(1)
time.sleep_ms(20)

tx_buf = bytearray(i & 0xff for i in range(FRAME_LEN))
rx_buf = bytearray(FRAME_LEN)

start_us = time.ticks_us()
cs.value(0)
spi.write_readinto(tx_buf, rx_buf)
cs.value(1)
elapsed_us = time.ticks_diff(time.ticks_us(), start_us)

mismatches = 0
sample = None
for i in range(FRAME_LEN):
    if rx_buf[i] != (i & 0xff):
        mismatches += 1
        if sample is None:
            sample = (i, rx_buf[i])

mbps = (FRAME_LEN / elapsed_us) if elapsed_us > 0 else 0.0  # bytes/us == MB/s
fps_equiv = (1_000_000 / elapsed_us) if elapsed_us > 0 else 0.0

print("elapsed: %d us  (%.2f MB/s, %.1f full-frame-equivalent fps)" % (elapsed_us, mbps, fps_equiv))
print("mismatches: %d/%d" % (mismatches, FRAME_LEN), ("first at %r" % (sample,)) if sample else "")
