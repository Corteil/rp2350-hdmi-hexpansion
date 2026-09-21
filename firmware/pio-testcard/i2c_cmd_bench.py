# Badge-side bench for the runtime I2C command channel between a badge
# app and this hexpansion -- see HISTORY.md section 3.5, which this
# script exists to confirm (or contradict) on real hardware. Everything
# in 3.5 is derived from badge firmware constants; nothing there has
# been measured on a bench yet, and the one cost 3.5 cannot derive is
# MicroPython's own per-call overhead.
#
# Runs against the CURRENT pio-testcard firmware with no RP2350-side
# change at all. That works because the emulated EEPROM's ISR
# (src/eeprom_i2c.c) already services reads and writes across the whole
# 8 KiB buffer, and bytes 7744-8191 sit outside the LittleFS partition
# the badge mounts ((8192-64)/512 = 15 blocks, covering 64-7743). So
# this region is a real, writable, badge-visible mailbox today -- it
# just has nothing at the other end reading it yet.
#
# What it measures, per payload size: the true wall-clock cost of a
# writeto_mem/readfrom_mem against the hexpansion, as an app would
# actually issue it -- wire time + MicroPython interpreter overhead +
# TCA9548A mux switching + whatever mutex wait the frontboard's own
# button polling imposes. min/median/max are all reported because the
# spread IS the interesting number: the mux mutex
# (tca9548a_master_cmd_begin) is held for a whole transaction, so on a
# 2026 frontboard, which polls six AW9523B pins every 10 ms, the worst
# case is what an app would actually feel.
#
# Run via: mpremote connect COMx run i2c_cmd_bench.py
# (NOT exec "$(cat ...)" -- PowerShell mangles that form.) Interrupts
# whatever app is in the foreground, which is expected for a deliberate
# test run. Safe to run with the mirror attached and actively running:
# the port's SDA/SCL are physically separate from the SPI mirror pins
# (HS_F..HS_I), so this measures the command channel under exactly the
# conditions it would really be used in -- and doing so with mirroring
# ON is the more interesting run of the two.

from machine import I2C
import time

# The badge port the hexpansion is in (1-6). Check the HDMI Manager
# app's status tile if you are not sure.
PORT = 3

EEPROM_ADDR = 0x50
EEPROM_SIZE = 8192
FS_OFFSET = 64
BLOCK_SIZE = 512

# Mailbox base: first byte past the end of the badge's mounted
# partition, computed below rather than hardcoded, so this stays correct
# if eeprom_i2c.c's HEX_EEPROM_SIZE ever changes again.
FS_BLOCKS = (EEPROM_SIZE - FS_OFFSET) // BLOCK_SIZE
MAILBOX_BASE = FS_OFFSET + FS_BLOCKS * BLOCK_SIZE
MAILBOX_LEN = EEPROM_SIZE - MAILBOX_BASE

SIZES = (1, 4, 8, 16, 32, 64, 128)
REPEATS = 50

# badge-2024-software drivers/tildagon_i2c/tildagon_i2c.h
BUS_HZ = 133000


def percentile(xs, frac):
    s = sorted(xs)
    return s[min(len(s) - 1, int(len(s) * frac))]


def bench(fn, repeats):
    """Returns (min, median, max) microseconds over `repeats` calls."""
    samples = []
    for _ in range(repeats):
        t0 = time.ticks_us()
        fn()
        samples.append(time.ticks_diff(time.ticks_us(), t0))
    return min(samples), percentile(samples, 0.5), max(samples)


def main():
    i2c = I2C(PORT)

    print("=" * 62)
    print("I2C command-channel bench -- port %d, bus %d Hz" % (PORT, BUS_HZ))
    print("=" * 62)

    found = i2c.scan()
    print("scan:", [hex(a) for a in found])
    if EEPROM_ADDR not in found:
        print("FAIL: nothing at 0x50 on port %d. Wrong port, or the "
              "hexpansion is not powered." % PORT)
        return

    # Identity check -- header layout is HISTORY.md section 1.4.
    hdr = i2c.readfrom_mem(EEPROM_ADDR, 0, 32, addrsize=16)
    if hdr[0:4] != b"THEX":
        print("FAIL: no THEX magic at offset 0 -- not a hexpansion EEPROM.")
        return
    vid = hdr[16] | (hdr[17] << 8)
    pid = hdr[18] | (hdr[19] << 8)
    print("header: magic=%s ver=%s vid=0x%04X pid=0x%04X name=%s"
          % (hdr[0:4].decode(), hdr[4:8].decode(), vid, pid,
             hdr[22:31].split(b"\x00")[0].decode()))
    if (vid, pid) != (0x1969, 0x4544):
        print("WARN: not HDMI-HEX (expected 0x1969/0x4544) -- continuing "
              "anyway, but the mailbox geometry below assumes this "
              "project's own firmware.")

    print("mailbox: %d bytes at 0x%04X-0x%04X (partition is %d blocks, "
          "ends 0x%04X)"
          % (MAILBOX_LEN, MAILBOX_BASE, EEPROM_SIZE - 1, FS_BLOCKS,
             MAILBOX_BASE - 1))

    # Prove the mailbox really is writable and really does read back --
    # if the badge's filesystem layer were caching or overlapping this
    # region, this is where it would show.
    probe = bytes((0x5A ^ i) & 0xFF for i in range(16))
    i2c.writeto_mem(EEPROM_ADDR, MAILBOX_BASE, probe, addrsize=16)
    back = i2c.readfrom_mem(EEPROM_ADDR, MAILBOX_BASE, 16, addrsize=16)
    if back != probe:
        print("FAIL: mailbox did not read back what was written.")
        print("  wrote:", probe)
        print("  read :", back)
        return
    print("mailbox round-trip: OK")
    print()

    print("%-7s %-26s %-26s" % ("bytes", "write us (min/med/max)",
                                "read us (min/med/max)"))
    print("-" * 62)

    results = {}
    for n in SIZES:
        if n > MAILBOX_LEN:
            continue
        payload = bytes((i * 7 + n) & 0xFF for i in range(n))

        def do_write(p=payload):
            i2c.writeto_mem(EEPROM_ADDR, MAILBOX_BASE, p, addrsize=16)

        def do_read(n=n):
            i2c.readfrom_mem(EEPROM_ADDR, MAILBOX_BASE, n, addrsize=16)

        w = bench(do_write, REPEATS)
        r = bench(do_read, REPEATS)
        results[n] = (w, r)

        # Integrity, not just timing: a torn or dropped byte here would
        # mean the ISR is missing transfers under whatever else the
        # badge and the RP2350 are doing right now.
        if i2c.readfrom_mem(EEPROM_ADDR, MAILBOX_BASE, n, addrsize=16) != payload:
            print("  !! %d-byte payload did not survive the round trip" % n)

        print("%-7d %-26s %-26s"
              % (n,
                 "%d / %d / %d" % w,
                 "%d / %d / %d" % r))

    print()
    print("Derived, using the median write+read pair as one "
          "command+status exchange:")
    print("-" * 62)
    for n in sorted(results):
        rt = results[n][0][1] + results[n][1][1]
        print("  %3d B exchange: %6.2f ms round trip, "
              "%5.1f%% of the bus at 20 Hz, max %4.0f Hz sustained"
              % (n, rt / 1000.0, (rt * 20) / 10000.0, 1000000.0 / rt))
    print()
    print("20 Hz is the scheduler's foregrounded update() cadence "
          "(mark_update_finished's 50 ms sleep) -- the fastest an app "
          "can issue one command per frame without its own task.")


main()
