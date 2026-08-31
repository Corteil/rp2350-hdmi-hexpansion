# Hexi-GFX mirror demo -- badge-side half of the end-to-end link test
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
# What it does: builds a 240x240 RGB565 frame directly in Python -- 8
# vertical colour bars, fixed in position, with the colour occupying each
# bar rotating through the palette over time -- one row-shape reused for
# all 240 rows -- and streams it to the RP2350 continuously, one SPI
# burst per row, "as if
# mirrored from the badge". Not a literal mirror of this badge's own
# rendered ctx screen -- that needs display.get_fb() (Phase 0 C2's
# patch, written but never verified, and would mean flashing custom
# firmware onto a real physical badge, deliberately out of scope here).
# This generates real, changing pixel content in Python instead and
# streams it over the same link at the same bandwidth the real thing
# needs, which is what actually exercises the pipeline end to end.
#
# SPI: config.pin (hs_1..4 -- HS_F=MOSI, HS_G=CS, HS_H=SCK, HS_I=MISO,
# main README section 4.1's resolved role assignment), mode 3 (CPOL=1,
# CPHA=1), matching the RP2350 SPI0-slave protocol proven in Phase 0
# A2/C3 (firmware/phase0-a2-spi/).
#
# Known, already-documented quirk (Phase 0 C3): the very first SPI
# transfer after constructing a fresh machine.SPI object is occasionally
# corrupted on the badge's own ESP32-S3 side (a repeatable master-side
# peripheral-init artifact, not a link problem) -- so the very first row
# of the very first frame may occasionally not land correctly. Every
# transfer after that is reliable. Not worked around here, since hiding
# it would misrepresent what the actual link behaves like.
#
# One SPI burst per row, not one burst for the whole 115,200-byte frame
# (2026-08-30, found via real hardware testing): the RP2350 doesn't
# reset when this hexpansion is unplugged (it's USB-powered on the bench
# rig, not badge-powered), so a physical removal mid-transfer can leave
# its blocking SPI read waiting for bytes that never arrive -- the next
# transfer after reinsertion then lands on top of that stale one with no
# resync, permanently misaligning everything after it. Reproduced on
# real hardware: correct after a fresh boot, corrupted and stuck after
# an unplug/replug cycle. Per-row bursts cap the blast radius at one row
# instead of an entire frame or stream. See ../src/main.c's own comment
# on this for the RP2350 side, including the accepted remaining gap (no
# frame-sync marker -- a corrupted burst can leave row alignment briefly
# out of phase, self-correcting within one 240-row cycle).

import app
from machine import SPI, Pin
from events.input import Buttons, BUTTON_TYPES
from app_components.tokens import clear_background, small_font_size, label_font_size
from system.eventbus import eventbus
from system.scheduler.events import RequestForegroundPushEvent

ROWS = 240
COLS = 240
BAR_COLOURS = (0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000)
BAR_WIDTH = COLS // len(BAR_COLOURS)  # 30px/bar


class TestcardApp(app.App):
    def __init__(self, config):
        super().__init__()
        self.config = config
        self.buttons = Buttons(self)
        self.spi = None
        self.cs = None
        self.rotation = 0
        self.rotation_step = 1
        self.frames_sent = 0
        self.status = "starting..."
        self._row_buf = bytearray(COLS * 2)  # reused every row -- avoid reallocating 480B/row
        self._init_spi()

        # See the long comment below for why this can't just fire here.
        self._want_fg = True

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

    def _build_row(self, rotation):
        # 8 vertical colour bars, FIXED in position -- each bar's colour
        # cycles through the palette over time instead (bar index x//
        # BAR_WIDTH never changes; which colour occupies it does). Content
        # is identical for every row of this particular pattern, so this
        # only needs to run once per frame, not once per row -- see
        # _send_frame().
        buf = self._row_buf
        n = len(BAR_COLOURS)
        for x in range(COLS):
            bar_index = x // BAR_WIDTH
            c = BAR_COLOURS[(bar_index + rotation) % n]
            buf[x * 2] = c & 0xFF        # low byte first -- matches src/main.c's uint16_t cast
            buf[x * 2 + 1] = (c >> 8) & 0xFF
        return buf

    def _send_frame(self):
        if self.spi is None:
            return
        row = self._build_row(self.rotation)
        sent_ok = 0
        for _ in range(ROWS):
            try:
                self.cs.value(0)
                self.spi.write(row)
                self.cs.value(1)
                sent_ok += 1
            except Exception as e:
                self.status = "row send failed: {!r}".format(e)
                print(self.status)
                return
        self.frames_sent += 1
        self.rotation = (self.rotation + self.rotation_step) % len(BAR_COLOURS)
        self.status = "frame %d sent (%d rows)" % (self.frames_sent, sent_ok)

    def background_update(self, delta):
        # The badge launches hexpansion apps in the BACKGROUND only
        # (system.hexpansion.app._launch_hexpansion_app emits
        # RequestStartAppEvent(app) with foreground defaulting to False,
        # and this badge firmware snapshot never actually emits
        # HexpansionAppLauncherAddEvent to surface a launcher-list entry
        # to select it from either -- confirmed by reading both files
        # directly, not assumed). Without self-foregrounding, draw() is
        # never called even though the app runs correctly.
        #
        # Can't just eventbus.emit(RequestForegroundPushEvent(self)) in
        # __init__: _launch_hexpansion_app constructs this object
        # (App(config=config), i.e. __init__ running) and only emits
        # RequestStartAppEvent(app) -- which is what actually registers
        # the app with the scheduler -- AFTER __init__ returns. A push
        # fired from inside __init__ therefore targets an app the
        # scheduler doesn't know about yet and is silently dropped.
        # Confirmed against a real, working reference hexpansion app
        # (Corteil/tildagon-space-unicorn's app.py) doing exactly this:
        # it defers the push to the first background_update() tick via a
        # one-shot flag, since background_update only starts running
        # once start_app()/start_update_tasks() have actually registered
        # the app. Same pattern here.
        if self._want_fg:
            self._want_fg = False
            eventbus.emit(RequestForegroundPushEvent(self))

    def update(self, delta):
        if self.buttons.pressed(BUTTON_TYPES["CANCEL"]):
            self.buttons.clear()
            self.minimise()
            return True

        if self.buttons.pressed(BUTTON_TYPES["RIGHT"]):
            self.rotation_step = 1
        elif self.buttons.pressed(BUTTON_TYPES["LEFT"]):
            self.rotation_step = -1

        self._send_frame()
        return True

    def draw(self, ctx):
        # Real font-size tokens from the badge's own UI kit
        # (app_components.tokens), not guessed pixel values -- ctx's
        # unset default is far too large for a sentence on a round
        # 240x240 display.
        ctx.save()
        clear_background(ctx)
        ctx.text_align = ctx.CENTER
        ctx.text_baseline = ctx.MIDDLE

        ctx.font_size = label_font_size
        ctx.rgb(1, 1, 1).move_to(0, -70).text("Hexi-GFX")

        ctx.font_size = small_font_size
        ctx.rgb(0, 1, 0.3).move_to(0, -35).text("mirroring...")
        ctx.rgb(1, 1, 0).move_to(0, 0).text(self.status)
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 35).text("L/R: rotate dir")
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 60).text("CANCEL: exit")
        ctx.restore()


__app_export__ = TestcardApp
