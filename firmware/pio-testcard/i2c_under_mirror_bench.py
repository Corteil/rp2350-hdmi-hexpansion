# Does the running mirror leave the badge any time to talk I2C?
#
# HISTORY.md section 3.5 shows the I2C *bus* is almost entirely idle
# during mirroring, and that is true but not the whole question. The
# real constraint is on the badge's CPU, not its wire:
#
#   display.end_frame()  ->  tildagon_blit_fb()
#                        ->  flow3r_bsp_display_dispatch_sinks()
#                        ->  mirror_sink_send_frame()
#                        ->  spi_device_polling_transmit()  x29
#
# That whole chain is synchronous inside one MicroPython C call, issued
# from the scheduler's _render_task coroutine, and ESP-IDF's *polling*
# transmit busy-waits rather than blocking on a semaphore. 115,200 bytes
# at 10 MHz is ~92 ms of wire time per frame, and for all of it the
# MicroPython interpreter is inside that C call -- so no app update(),
# no frontboard button poll, and no I2C transaction can be issued, no
# matter how idle the bus is.
#
# This script measures the consequence directly, which is the only
# honest way to answer "is there enough time": it drives its own render
# loop with the mirror attached, interleaves I2C exchanges against the
# hexpansion's mailbox, and reports what the app layer actually gets --
# frames per second, exchanges per second, and the worst gap between
# consecutive exchanges, which is the latency an app would really feel.
# It then repeats with the mirror detached, so the mirror's contribution
# is isolated rather than inferred.
#
# Needs no RP2350-side change: the mailbox (see section 3.5) is already
# live in the current pio-testcard firmware.
#
# Run via: mpremote connect COMx run i2c_under_mirror_bench.py
# (NOT exec "$(cat ...)" -- PowerShell mangles that form.) mpremote's
# unconditional Ctrl-C kills the badge's scheduler first, which is what
# this script wants: it then owns the render loop with nothing else
# competing for it. Reset the badge afterwards to get its UI back.

from machine import I2C
import display
import time

PORT = 3                  # badge port the hexpansion is in (1-6)
BAUDRATE = 10_000_000     # must match the RP2350 receiver -- see the app
DRIVER = {"header": b"TDHD", "baudrate": BAUDRATE}

EEPROM_ADDR = 0x50
EEPROM_SIZE = 8192
FS_OFFSET = 64
BLOCK_SIZE = 512
MAILBOX_BASE = FS_OFFSET + ((EEPROM_SIZE - FS_OFFSET) // BLOCK_SIZE) * BLOCK_SIZE

CMD_BYTES = 16            # a plausible command
STATUS_BYTES = 8          # and its status readback
RUN_MS = 3000             # per phase

FRAME_BYTES = 240 * 240 * 2


def draw_something(ctx):
    """Cheap but not free -- a real app's draw() is the other half of
    the frame budget, and leaving it out would flatter the result."""
    ctx.rgb(0.1, 0.1, 0.12).rectangle(-120, -120, 240, 240).fill()
    ctx.rgb(0.9, 0.4, 0.1).arc(0, 0, 60, 0, 6.28, 0).fill()


def phase(label, i2c, run_ms):
    """Render as fast as the badge will, issuing one I2C exchange per
    frame, and report what each side actually got."""
    cmd = bytes((i * 13) & 0xFF for i in range(CMD_BYTES))

    frames = 0
    exchanges = 0
    end_frame_us = []
    exchange_us = []
    gap_us = []          # wall-clock between consecutive exchanges

    last_exchange = None
    t_start = time.ticks_ms()

    while time.ticks_diff(time.ticks_ms(), t_start) < run_ms:
        ctx = display.get_ctx()
        draw_something(ctx)

        t0 = time.ticks_us()
        display.end_frame(ctx)
        end_frame_us.append(time.ticks_diff(time.ticks_us(), t0))
        frames += 1

        # The gap between frames is the only place an app gets to run.
        t1 = time.ticks_us()
        try:
            i2c.writeto_mem(EEPROM_ADDR, MAILBOX_BASE, cmd, addrsize=16)
            i2c.readfrom_mem(EEPROM_ADDR, MAILBOX_BASE + 64, STATUS_BYTES,
                             addrsize=16)
        except OSError as e:
            print("  I2C failed mid-run:", e)
            break
        now = time.ticks_us()
        exchange_us.append(time.ticks_diff(now, t1))
        exchanges += 1
        if last_exchange is not None:
            gap_us.append(time.ticks_diff(now, last_exchange))
        last_exchange = now

    elapsed_ms = time.ticks_diff(time.ticks_ms(), t_start)

    def stats(xs):
        if not xs:
            return (0, 0, 0)
        s = sorted(xs)
        return (s[0], s[len(s) // 2], s[-1])

    ef = stats(end_frame_us)
    ex = stats(exchange_us)
    gp = stats(gap_us)

    print("--- %s ---" % label)
    print("  %.1f fps, %.1f I2C exchanges/s over %d ms"
          % (frames * 1000.0 / elapsed_ms, exchanges * 1000.0 / elapsed_ms,
             elapsed_ms))
    print("  end_frame():   %6d / %6d / %6d us  (min/med/max)" % ef)
    print("  I2C exchange:  %6d / %6d / %6d us" % ex)
    print("  exchange gap:  %6d / %6d / %6d us" % gp)
    if ef[1]:
        busy = 100.0 * ef[1] / (ef[1] + ex[1])
        print("  -> %.0f%% of the loop is inside end_frame; an app sees "
              "the other %.0f%%" % (busy, 100.0 - busy))
    return ef, ex, gp


def main():
    print("=" * 66)
    print("I2C messaging under a running mirror -- port %d, %d Hz SPI"
          % (PORT, BAUDRATE))
    print("frame is %d bytes = %.1f ms of SPI wire time per frame"
          % (FRAME_BYTES, FRAME_BYTES * 8.0 / BAUDRATE * 1000.0))
    print("=" * 66)

    i2c = I2C(PORT)
    if EEPROM_ADDR not in i2c.scan():
        print("FAIL: nothing at 0x50 on port %d." % PORT)
        return

    # Baseline first, so a failure to attach still leaves a useful number.
    try:
        display.detach_mirror(0)
    except Exception:
        pass
    time.sleep_ms(100)
    base = phase("mirror DETACHED (baseline)", i2c, RUN_MS)

    print()
    try:
        display.attach_mirror(port=PORT, baudrate=BAUDRATE, driver=DRIVER)
    except Exception as e:
        print("attach_mirror failed:", e)
        return
    time.sleep_ms(200)
    if not display.is_mirror_active(PORT):
        print("WARN: attach returned but is_mirror_active() is False -- "
              "the numbers below are NOT a mirrored run.")
    mir = phase("mirror ATTACHED", i2c, RUN_MS)

    try:
        display.detach_mirror(0)
    except Exception:
        pass

    print()
    print("=" * 66)
    added = mir[0][1] - base[0][1]
    print("Mirror adds %d us to the median end_frame()." % added)
    print("Command->status round trip, median: %d us detached, "
          "%d us attached." % (base[1][1], mir[1][1]))
    print("Worst gap between exchanges: %d us detached, %d us attached "
          "-- this is the latency an app would actually feel."
          % (base[2][2], mir[2][2]))
    print("=" * 66)
    print("Reset the badge to get its UI back.")


main()
