# Hexi-GFX testcard app -- badge-side half of the end-to-end link test
# (rp2350-hdmi-hexpansion, VID=0x1969 PID=0x4544). This file is NOT run
# directly on the badge from a loose checkout: it is packed into the
# LittleFS image that lives inside the RP2350's emulated EEPROM (see
# ../tools/build_fs_image.py and ../src/fs_image.h) and served up to the
# badge exactly as a real, shipped hexpansion would -- matching the
# product's own "the RP2350 owns the driver image, so it updates itself
# with the hexpansion firmware" design (main README section 2, item 2).
#
# On insertion, the badge auto-detects the emulated EEPROM (I2C0 target
# on the RP2350, main README sections 1.4/5), mounts this LittleFS image,
# imports this module, and instantiates TestcardApp -- the standard
# hexpansion app-launch contract
# (badge-2024-software/modules/system/hexpansion/app.py's
# _launch_hexpansion_app / __app_export__ convention).
#
# What it does: opens machine.SPI on this hexpansion's own hs_1..4 pins
# (config.pin, in HS_F/HS_G/HS_H/HS_I order -- main README section 4.1's
# resolved MOSI/CS/SCK/MISO role assignment), mode 3 (CPOL=1, CPHA=1),
# matching the RP2350 SPI0-slave protocol already confirmed working on
# real hardware in Phase 0 A2/C3 (firmware/phase0-a2-spi/). L/R (or
# CONFIRM) cycles through 4 test patterns on the connected monitor by
# sending a 1-byte pattern-select command over that link; CANCEL exits.
#
# Known, already-documented quirk (Phase 0 C3, firmware/phase0-a2-spi/
# README.md): the very first SPI transfer after constructing a fresh
# machine.SPI object is occasionally corrupted on the badge's own
# ESP32-S3 side (a repeatable master-side peripheral-init artifact, not
# a link problem) -- so the very first button press right after this app
# launches may occasionally not take effect. Every press after that is
# reliable. Not worked around here (the SPI object is constructed once
# in __init__ and reused for the app's whole lifetime, same as the C3
# sweep script did), since hiding it would misrepresent what the actual
# link behaves like.
#
# Also by design: the on-screen "status" line reports the RP2350's reply
# to the PREVIOUS transfer, not the one just sent -- full-duplex SPI
# means a single blocking transfer can't have its own reply depend on
# the byte it's still receiving. The monitor is the real, near-instant
# (well under one HSTX frame) proof that a command arrived; the status
# line is a courtesy that self-corrects one press later. See
# ../src/main.c's SPI0 command loop for the RP2350 side of this.

import app
from machine import SPI, Pin
from events.input import Buttons, BUTTON_TYPES
from app_components.tokens import clear_background

PATTERN_NAMES = ["Colour bars", "Solid red", "Solid green", "Checkerboard"]
PATTERN_COUNT = len(PATTERN_NAMES)

XFER_LEN = 16
ACK_BYTE = 0xC0  # must match src/main.c's ACK_BYTE


class TestcardApp(app.App):
    def __init__(self, config):
        super().__init__()
        self.config = config
        self.buttons = Buttons(self)
        self.pattern = 0
        self.status = "starting..."
        self.spi = None
        self.cs = None
        self._init_spi()
        self._send_pattern(self.pattern)

    def _init_spi(self):
        try:
            # config.pin is [hs_1, hs_2, hs_3, hs_4] for this hexpansion's
            # own port -- HS_F=MOSI, HS_G=CS, HS_H=SCK, HS_I=MISO (main
            # README section 4.1's resolved role assignment).
            mosi, cs, sck, miso = self.config.pin
            cs.init(Pin.OUT, value=1)
            self.cs = cs
            self.spi = SPI(
                1, baudrate=1_000_000, polarity=1, phase=1,
                sck=sck, mosi=mosi, miso=miso,
            )
        except Exception as e:
            self.status = "SPI init failed: {!r}".format(e)
            print(self.status)

    def _send_pattern(self, pattern_id):
        if self.spi is None:
            return False
        tx = bytearray(XFER_LEN)
        tx[0] = pattern_id
        rx = bytearray(XFER_LEN)
        try:
            self.cs.value(0)
            self.spi.write_readinto(tx, rx)
            self.cs.value(1)
        except Exception as e:
            self.status = "SPI xfer failed: {!r}".format(e)
            print(self.status)
            return False

        # rx reflects the pattern that was showing BEFORE this transfer
        # (see file header) -- report it plainly rather than pretending
        # it confirms *this* request.
        if rx[0] == ACK_BYTE and rx[1] < PATTERN_COUNT:
            self.status = "RP2350 says showing: " + PATTERN_NAMES[rx[1]]
        else:
            self.status = "no/bad reply: " + " ".join("%02x" % b for b in rx[:4])
        print("sent pattern", pattern_id, "->", self.status)
        return True

    def update(self, delta):
        if self.buttons.pressed(BUTTON_TYPES["CANCEL"]):
            self.buttons.clear()
            self.minimise()
            return True

        moved = False
        if self.buttons.pressed(BUTTON_TYPES["RIGHT"]) or self.buttons.pressed(BUTTON_TYPES["CONFIRM"]):
            self.pattern = (self.pattern + 1) % PATTERN_COUNT
            moved = True
        elif self.buttons.pressed(BUTTON_TYPES["LEFT"]):
            self.pattern = (self.pattern - 1) % PATTERN_COUNT
            moved = True

        if moved:
            self._send_pattern(self.pattern)
            return True
        return False

    def draw(self, ctx):
        ctx.save()
        clear_background(ctx)
        ctx.text_align = ctx.CENTER
        ctx.text_baseline = ctx.MIDDLE
        ctx.rgb(1, 1, 1).move_to(0, -50).text("Hexi-GFX testcard")
        ctx.rgb(0, 1, 0.3).move_to(0, -10).text("requesting: " + PATTERN_NAMES[self.pattern])
        ctx.rgb(1, 1, 0).move_to(0, 30).text(self.status)
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 70).text("L/R: pattern   CANCEL: exit")
        ctx.restore()


__app_export__ = TestcardApp
