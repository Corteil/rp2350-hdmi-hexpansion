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
# burst per row at 1MHz (~0.77fps -- see the burst-granularity comment
# further down for why a faster protocol was tried and reverted), "as if
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
# One SPI burst per row, at 1MHz -- reverted here after a same-session
# detour (2026-08-31) that tried one burst per FRAME at 20MHz instead,
# to chase a 15fps target (matching the badge's own worst-case app
# render rate, main README section 3.1's C1 measurement). That got a
# real, cross-checked ~5fps -- 240 separate per-row bursts meant 240
# rounds of Python-loop + two GPIO calls + a spi.write() call per frame,
# and collapsing that to one big burst removed nearly all of it. But it
# also produced real, visible corruption on the actual monitor ("lines
# not straight, colours mixed") that persisted (reduced, not gone) even
# backed off to 10MHz -- most likely sustained bus contention between
# the RP2350's continuous SPI receive polling and HSTX's simultaneous
# DMA scanout over one long unbroken burst, a combination Phase 0 C3's
# own speed sweep never actually exercised (that measurement had no
# display output running at the same time). Per-row bursts at 1MHz are
# this session's last thoroughly-confirmed-clean-on-real-hardware
# configuration, so that's what ships. See "Frame rate" in
# ../README.md for the full investigation and the decision to revisit
# this at the PCB-prototype stage, where a real board (no devkit-to-Metro
# dupont leg) may tolerate a higher rate more cleanly.
#
# The RP2350 side's own receive function doesn't depend on CS-level
# framing at all any more (see ../src/main.c's spi0_receive_row()
# comment) -- it treats incoming bytes as one continuous,
# chunking-agnostic stream, so this side remains free to choose whatever
# burst granularity serves its own needs without any RP2350-side change,
# which is exactly what made it cheap to try the frame-burst experiment
# above and revert it just as easily.

import app
import display
import time
import ctx as ctxmod  # NOT the ctx passed into draw(ctx) -- see _render_icon()
from machine import SPI, Pin
from events.input import Buttons, BUTTON_TYPES
from app_components.tokens import clear_background, small_font_size, label_font_size
from system.eventbus import eventbus
from system.scheduler.events import RequestForegroundPushEvent
from system.hexpansion.events import HexpansionAppLauncherAddEvent

ROWS = 240
COLS = 240
BAR_COLOURS = (0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000)
BAR_WIDTH = COLS // len(BAR_COLOURS)  # 30px/bar
CHECKER_SQUARE = 20  # px

# EMFCampFont.h's own glyph-index comment lists five unusual codepoints
# that don't belong to any normal alphabet/symbol block -- Latin-1 and a
# handful of arrows/geometric shapes cover everything else in the font,
# but these five stand apart, and the font's own license header credits
# "Solder Party logo" and "Keebdeck icons" as bundled custom additions:
#   U+41E9 U+71E9 U+81E9 U+BA7A U+BA7B
# Confirmed on real hardware (2026-08-31), after correcting an earlier
# oversized/off-centre render (see font_size below): icon1 (U+71E9) and
# icon2 (U+81E9) both look spider-like (7-leg and 8-leg respectively) --
# genuinely two similar glyphs, not one misread as two via clipping.
# icon3 (U+BA7A) looks bat-like. icon0 (U+41E9) is a genuine,
# intentionally multi-coloured Easter-egg bunny -- its glyph definition
# opens with `{'g', 0, 0}, /* Nothing to see here */` (a coy joke
# comment, initially misread here as "this glyph is empty" -- 'g' is
# actually CTX_SAVE, a state push, not a no-op; real path data with its
# own embedded fill colours, e.g. `{'*', 0xFF0000FF, 0}` = opaque red,
# follows immediately after and IS the bunny). The glyph sets its own
# colour internally, overriding whatever surface.rgb() was set
# beforehand -- that's why it renders in its own red/blue, not white.
#
# icon4 is U+21E9 (down arrow), NOT the fifth of the five oddities above
# (U+BA7B, which also looked bat-like, swapped out 2026-08-31) -- a
# separately-sourced claim that U+21E9 is a repurposed "duck" icon
# looked implausible from the font source alone (it sits in the middle
# of a normal sequential run of directional arrows in the font's own
# glyph index, with an x-advance matching its arrow neighbours, not the
# five oddities above), and a fetch of a badge docs page repeating that
# claim wasn't trusted either (the fetch tool summarizes through a
# model regardless of prompt; two fetches of the same page gave
# different, inconsistent detail). Tested directly on real hardware
# instead of continuing to reason about it either way: **confirmed --
# U+21E9 genuinely renders as a rubber duck**, not a plain arrow. The
# positional inference above was a reasonable hypothesis that turned
# out wrong; direct hardware testing settled it where source-reading
# and an unreliable web fetch both fell short.
ICON_CODEPOINTS = (0x41E9, 0x71E9, 0x81E9, 0xBA7A, 0x21E9)
# Names matching each codepoint above, 1:1 -- all confirmed on real
# hardware 2026-08-31 except "bat" (looked bat-like, not tested against
# any independent source the way spider/duck were).
ICON_NAMES = ("bunny", "spider7", "spider8", "bat", "duck")

# (foreground, background) per icon, as (r, g, b) 0..1 floats -- "bunny"
# deliberately excluded, its own glyph data sets its own fill colours
# internally (see _render_icon()'s docs) so a foreground override here
# would have no visible effect; its background stays the plain default.
ICON_COLOURS = {
    "spider7": ((1, 1, 1), (0.5, 0, 0)),          # white on dark red
    "spider8": ((1, 1, 1), (0, 0.35, 0)),         # white on dark green
    "bat":     ((0.85, 0.75, 1), (0.12, 0, 0.2)), # pale lavender on dark purple
    "duck":    ((1, 1, 0), (0, 0.3, 0.85)),       # yellow on blue -- as requested
}

SCREENS = ["bars", "checker"] + list(ICON_NAMES)

# Frame-sync marker (2026-09-08): sent once, right before row 0 of every
# frame, so the RP2350 side can resynchronize its row counting even
# after a dropped or corrupted byte -- see src/main.c's
# spi0_rx_irq_handler() for the receiver side. Without this, the two
# sides' row counts have no way to realign once they drift apart (no
# CS-level framing is used -- see this file's own header comment on
# why), and a single dropped byte anywhere leaves every subsequent row
# permanently mislabeled until pure chance realigns them. 8 bytes,
# alternating bit pattern, picked to be unlikely to occur by chance in
# real image data; an occasional false match in unusual content just
# costs one extra, self-correcting resync (the next real marker fixes
# it), not a lasting problem -- a real product would want a more
# collision-proof scheme (e.g. a longer marker, or CRC-checked framing).
FRAME_MARKER = bytes([0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A])


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
        self.status2 = ""
        self._max_delta_ms = 0
        self._sum_delta_ms = 0
        # All 8 possible rows precomputed ONCE, not rebuilt via a
        # 240-iteration Python loop every single frame (2026-08-31: this
        # loop's cost was folding into unexplained per-frame overhead --
        # only 8 distinct rotation states ever exist, so an O(1) lookup
        # replaces it entirely). Trivial memory cost (8 x 480 bytes).
        self._rows = [self._compute_row(r) for r in range(len(BAR_COLOURS))]

        # Other screens (checker, icons) are rendered lazily on first
        # selection, then cached -- their content doesn't change frame to
        # frame the way the bars' rotation does, so there's no reason to
        # pay their (much higher, full-240-row) render cost more than
        # once. Keyed by screen name; value is a list of 240 480-byte
        # rows, one per screen row (unlike self._rows, which is one row
        # shape reused for the whole frame).
        self.screen_index = 0
        self._screen_cache = {}
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

    def _compute_row(self, rotation):
        # 8 vertical colour bars, FIXED in position -- each bar's colour
        # cycles through the palette over time instead (bar index x//
        # BAR_WIDTH never changes; which colour occupies it does).
        buf = bytearray(COLS * 2)
        n = len(BAR_COLOURS)
        for x in range(COLS):
            bar_index = x // BAR_WIDTH
            c = BAR_COLOURS[(bar_index + rotation) % n]
            buf[x * 2] = c & 0xFF        # low byte first -- matches src/main.c's uint16_t cast
            buf[x * 2 + 1] = (c >> 8) & 0xFF
        return bytes(buf)

    def _render_checker(self):
        # 20x20px black/white squares. Built once (see _screen_cache) --
        # a 240x240 nested Python loop is far too slow to redo every
        # frame, but is a fine one-time cost when switching screens.
        rows = []
        for y in range(ROWS):
            buf = bytearray(COLS * 2)
            for x in range(COLS):
                on = ((x // CHECKER_SQUARE) + (y // CHECKER_SQUARE)) % 2 == 0
                c = 0xFFFF if on else 0x0000
                buf[x * 2] = c & 0xFF
                buf[x * 2 + 1] = (c >> 8) & 0xFF
            rows.append(bytes(buf))
        return rows

    def _render_icon(self, codepoint, fg=(1, 1, 1), bg=(0, 0, 0)):
        # Off-screen ctx surface backed by OUR OWN bytearray, not the
        # badge's real display -- exactly the capability that would have
        # let Option A (a literal display.get_fb() mirror) work, except
        # here we're the ones creating and controlling the surface, so no
        # unmerged badge-firmware patch is needed. ctx.RGB565 (exposed
        # without the CTX_FORMAT_ prefix its C enum uses -- checked
        # against mp_uctx.c's MP_CTX_INT_CONSTANT macro) matches
        # src/main.c's expected low-byte-first layout directly -- the
        # real GC9A01 display driver specifically requests the OTHER
        # format, RGB565_BYTESWAPPED, for the physical panel's own wire
        # format (checked against ctx.h), meaning plain RGB565 is the
        # native/little-endian byte order, which is what we want here
        # since we're not talking to that panel.
        buf = bytearray(ROWS * COLS * 2)
        surface = ctxmod.Context(
            width=COLS, height=ROWS, stride=COLS * 2,
            format=ctxmod.RGB565, buffer=buf,
        )
        surface.rgb(*bg)
        surface.rectangle(0, 0, COLS, ROWS)
        surface.fill()
        surface.font = "EMF Camp Font"
        # 150, not a larger size -- these icon glyphs' own x-advance is
        # 126-160 (checked directly against EMFCampFont.h), so this is
        # close to their apparent "designed" scale rather than
        # exaggerating it. text_align=CENTER centers based on advance
        # width, a font metric -- not necessarily the visual ink bounding
        # box, which icon glyphs commonly don't fill symmetrically. No
        # ink-extents API is exposed to MicroPython here (only
        # text_width(), the same advance metric), so this is the best
        # correction available without one; found off-center on real
        # hardware (2026-08-31), not yet re-verified after this change.
        surface.font_size = 150
        surface.text_align = surface.CENTER
        surface.text_baseline = surface.MIDDLE
        surface.rgb(*fg)
        surface.move_to(COLS / 2, ROWS / 2)
        surface.text(chr(codepoint))
        row_bytes = COLS * 2
        return [bytes(buf[i * row_bytes:(i + 1) * row_bytes]) for i in range(ROWS)]

    def _rows_for_current_screen(self):
        name = SCREENS[self.screen_index]
        if name == "bars":
            return None  # signals "one row repeated" to _send_frame
        if name in self._screen_cache:
            return self._screen_cache[name]
        if name == "checker":
            rows = self._render_checker()
        else:
            codepoint = ICON_CODEPOINTS[ICON_NAMES.index(name)]
            fg, bg = ICON_COLOURS.get(name, ((1, 1, 1), (0, 0, 0)))
            rows = self._render_icon(codepoint, fg=fg, bg=bg)
        self._screen_cache[name] = rows
        return rows

    def _send_frame(self, delta):
        if self.spi is None:
            return
        static_rows = self._rows_for_current_screen()

        # Reverted to per-row bursts at 1MHz (2026-08-31, same session as
        # the single-burst/20MHz attempt above this comment used to
        # describe). That configuration reached ~5fps but produced real,
        # visible corruption on the actual monitor ("lines not straight,
        # colours mixed") -- backing off to 10MHz reduced but did not
        # eliminate it ("some of the time, breaks up"), meaning this
        # isn't simply a byte-rate-vs-CPU-poll-speed problem (10MHz gives
        # 2x more time per byte than 20MHz) -- more likely sustained bus
        # contention between the continuous SPI receive polling and
        # HSTX's simultaneous DMA scanout, over a single long (tens-of-ms)
        # unbroken burst, that Phase 0 C3's own speed sweep never actually
        # tested (that measurement had no display output running
        # concurrently). Rather than ship a demo that sometimes shows
        # garbled output, reverted to the per-row/1MHz configuration this
        # session had already thoroughly confirmed clean on real hardware
        # -- see "Frame rate" in ../README.md for the full story and the
        # decision to revisit this at the PCB-prototype stage instead.
        t_spi_start = time.ticks_ms()
        sent_ok = 0
        try:
            self.cs.value(0)
            self.spi.write(FRAME_MARKER)
            self.cs.value(1)
            if static_rows is None:
                row = self._rows[self.rotation]  # precomputed -- see __init__
                for _ in range(ROWS):
                    self.cs.value(0)
                    self.spi.write(row)
                    self.cs.value(1)
                    sent_ok += 1
            else:
                for row in static_rows:
                    self.cs.value(0)
                    self.spi.write(row)
                    self.cs.value(1)
                    sent_ok += 1
        except Exception as e:
            self.status = "row send failed: {!r}".format(e)
            print(self.status)
            return
        t_spi_ms = time.ticks_diff(time.ticks_ms(), t_spi_start)

        self.frames_sent += 1
        if static_rows is None:
            self.rotation = (self.rotation + self.rotation_step) % len(BAR_COLOURS)

        # `delta` is the FRAMEWORK's own measured gap since the previous
        # update() call (system/app.py's run(): delta_ticks passed
        # straight in). A single instantaneous reading swings a lot frame
        # to frame, so track a running average and worst-case stall
        # instead of just the latest value.
        self._sum_delta_ms += delta
        if delta > self._max_delta_ms:
            self._max_delta_ms = delta
        avg_delta_ms = self._sum_delta_ms // self.frames_sent
        avg_fps = 1000 // avg_delta_ms if avg_delta_ms > 0 else 0
        # Split across two short status lines -- one combined line
        # overflowed the round display's visible width (found
        # 2026-08-31: the reader could only see the last value, the rest
        # clipped by the circular mask). Compact single-letter labels for
        # the same reason.
        self.status = "%s #%d ~%dfps" % (SCREENS[self.screen_index], self.frames_sent, avg_fps)
        self.status2 = "avg%d max%d spi%d" % (
            avg_delta_ms, self._max_delta_ms, t_spi_ms)

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

        if self.buttons.pressed(BUTTON_TYPES["DOWN"]):
            self.screen_index = (self.screen_index + 1) % len(SCREENS)
        elif self.buttons.pressed(BUTTON_TYPES["UP"]):
            self.screen_index = (self.screen_index - 1) % len(SCREENS)

        self._send_frame(delta)
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
        ctx.rgb(1, 1, 0).move_to(0, -35).text(self.status)
        ctx.rgb(0, 1, 0.3).move_to(0, -10).text(self.status2)
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 25).text("U/D: screen  L/R: rotate")
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 50).text("CANCEL: exit")
        ctx.restore()


# No sck/mosi/cs override (2026-09-10): hazanjon's PORT_PINS table
# (badge-2024-software feature/hdmi-mirror, flow3r_bsp_display_mirror.c)
# disagreed with this repo's own README §4.1 role assignment for GPIO21/22
# (SCK vs CS swapped) -- GPIO21/22 are hardware-fixed silicon roles on the
# RP2350 side (confirmed against RP2350.svd, README §4.1/§9 risk 19), not
# software-reassignable, so this can't be patched in either firmware's
# code. Resolved instead by physically rewiring the bench connection
# (HS_G/HS_H swapped to the RP2350's GPIO22/GPIO21 respectively) so both
# sides' own unmodified defaults already agree -- see main.c's own
# PIN_SCK/PIN_CS comment for the full story. Only dc still needs
# overriding below.

# hazanjon's mirror driver always drives a `dc` pin -- mirror_init_pins()
# in flow3r_bsp_display_mirror.c configures it GPIO_MODE_OUTPUT and drives
# it HIGH unconditionally on attach, whether or not an init sequence is
# actually used (ours isn't). Left at its per-port default (PORT_PINS[port].dc,
# which for port 1 is GPIO42 = this repo's own HS_I = the RP2350's SPI0 MISO
# line, README §4.1), this crashes the badge the instant the hexpansion is
# plugged in -- confirmed on real hardware (2026-09-10): the badge hard-resets
# in a fast loop (flashing status LED, NeoPixel flashing red) rather than
# raising a catchable Python error, consistent with a genuine electrical
# contention between the badge driving that pin high and the RP2350's own
# SPI0 peripheral driving its MISO output, not a software-level failure.
# Redirect dc to GPIO6 instead: port 6's own DC line in hazanjon's own
# PORT_PINS table, and genuinely unconnected as long as no hexpansion is
# plugged into port 6 (only port 1 is populated here).
_DC_OVERRIDE_PIN = 6


# TEST (2026-09-11): delay before the first attach_mirror() call, to check
# whether the permanent hang (isolated to attach_mirror() itself -- app
# load/registration alone is confirmed clean via a real-hardware test with
# this call skipped entirely) is a race against something still settling
# right at insertion (I2C/EEPROM housekeeping, hexpansion power-on, etc.)
# rather than a fault that develops during ongoing transmission. Deferred
# via background_update() (ticked every ~50ms, see App.background_task())
# rather than a blocking time.sleep() in __init__, so this doesn't itself
# stall the badge during the delay.
_ATTACH_DELAY_MS = 1500


class MirrorApp(app.App):
    def __init__(self, config):
        super().__init__()
        self.config = config
        self.buttons = Buttons(self)
        self.status = "starting..."
        self._pending_attach_port = None
        self._attach_delay_remaining_ms = 0

        port = getattr(config, "port", None) if config else None
        if port is not None:
            print("DEBUG: before HexpansionAppLauncherAddEvent emit")
            eventbus.emit(HexpansionAppLauncherAddEvent(port, "HDMI Mirror"))
            print("DEBUG: after HexpansionAppLauncherAddEvent emit")
            print("DEBUG: deferring attach_mirror by %dms" % _ATTACH_DELAY_MS)
            self._pending_attach_port = port
            self._attach_delay_remaining_ms = _ATTACH_DELAY_MS

    def background_update(self, delta):
        if self._pending_attach_port is not None:
            self._attach_delay_remaining_ms -= delta
            if self._attach_delay_remaining_ms <= 0:
                port = self._pending_attach_port
                self._pending_attach_port = None
                self._attach(port)

    def _attach(self, port):
        print("DEBUG: _attach entered, port=%d" % port)
        driver = {
            # 20MHz tried and reverted (2026-09-10): real-hardware UART
            # capture showed frame_synced stuck at 0 and corrupted marker
            # values despite 1M+ bytes arriving -- genuine signal
            # integrity limits on this bench dupont-wire setup at that
            # rate. 1MHz tried (2026-09-11) to test whether a 1-byte
            # row-alignment drift seen at 5MHz was signal-integrity-
            # related -- it wasn't: the identical drift (same repeating
            # byte pattern, shifted by exactly one byte between captures)
            # showed up at 1MHz too, at a similar rate, ruling out clock
            # speed as the cause. Most likely an occasional interrupt/
            # task-switch hiccup on the badge's own ESP32-S3 corrupting a
            # byte mid-transfer, independent of SPI rate -- fixing it
            # would need changes inside hazanjon's C driver (out of scope;
            # this project's own plan explicitly avoids patching that).
            # Back to 5MHz: same artifact either way, but meaningfully
            # more responsive (~184ms/frame block vs ~0.92s at 1MHz).
            "baudrate": 5_000_000,
            "init": [], "prefix": [], "postfix": [],
            "header": FRAME_MARKER,  # bytes([0xA5, 0x5A] * 4) -- unchanged
        }
        print("DEBUG: calling display.attach_mirror dc=%d (sck/mosi/cs at hazanjon's defaults)" % (
            _DC_OVERRIDE_PIN,))
        try:
            display.attach_mirror(port=port, driver=driver, dc=_DC_OVERRIDE_PIN)
            print("DEBUG: attach_mirror returned OK")
            self.status = "mirroring (attach_mirror)"
        except Exception as e:
            self.status = "attach_mirror failed: {!r}".format(e)
            print(self.status)

    def deinit(self):
        self._pending_attach_port = None  # cancel a still-pending delayed attach
        port = getattr(self.config, "port", None) if self.config else None
        if port is not None:
            try:
                display.detach_mirror(port)
            except Exception as e:
                print("detach_mirror failed: {!r}".format(e))

    def update(self, delta):
        if self.buttons.pressed(BUTTON_TYPES["CANCEL"]):
            self.buttons.clear()
            self.minimise()
            return True

    def draw(self, ctx):
        ctx.save()
        clear_background(ctx)
        ctx.text_align = ctx.CENTER
        ctx.text_baseline = ctx.MIDDLE
        ctx.rgb(1, 1, 1).move_to(0, -30).text("HDMI Mirror")
        ctx.font_size = small_font_size
        ctx.rgb(1, 1, 0).move_to(0, 10).text(self.status)
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 45).text("CANCEL: back")
        ctx.restore()


__app_export__ = MirrorApp
