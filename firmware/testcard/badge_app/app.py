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
from events.input import Buttons, BUTTON_TYPES
from system.eventbus import eventbus
try:
    # The A-F buttons live here, not in events.input's BUTTON_TYPES.
    from frontboards.common import FRONTBOARD_BUTTON_TYPES as _FB
except ImportError:
    _FB = {}
from system.hexpansion.events import HexpansionAppLauncherAddEvent

# (The retired TestcardApp demo lives in testcard_old.py, not packed.)

# No sck/mosi/cs/dc override (2026-09-10 -> superseded 2026-09-20):
# hazanjon's PORT_PINS table (badge-2024-software feature/hdmi-mirror,
# flow3r_bsp_display_mirror.c) disagreed with this repo's own README
# §4.1 role assignment for GPIO21/22 (SCK vs CS swapped) -- resolved by
# physically rewiring the bench connection instead (HS_G/HS_H swapped to
# the RP2350's GPIO22/GPIO21 respectively), see main.c's own
# PIN_SCK/PIN_CS comment. An early version of this file also overrode
# `dc` (to GPIO6) after port 1's default dc pin (GPIO42 = HS_I = the
# RP2350's SPI0 MISO line) crashed the badge on attach with the OLD
# synthetic driver dict -- proven no longer needed: today's real
# HDMI_DRIVER dict (see _attach() below) attaches cleanly on port 1 with
# no dc override at all, matching display_manager/app.py's own working
# HDMI-mode calls exactly.


# Delay (2026-09-20) before pulsing LS_A to reset the RP2350 -- gives the
# physical connector's insertion bounce a moment to settle before yanking
# RUN low, rather than pulsing during a still-unstable electrical
# connection. Deferred the same way _ATTACH_DELAY_MS below is (via
# background_update(), not a blocking time.sleep()).
_RESET_DELAY_MS = 500

# TEST (2026-09-11): delay before the first attach_mirror() call, to check
# whether the permanent hang (isolated to attach_mirror() itself -- app
# load/registration alone is confirmed clean via a real-hardware test with
# this call skipped entirely) is a race against something still settling
# right at insertion (I2C/EEPROM housekeeping, hexpansion power-on, etc.)
# rather than a fault that develops during ongoing transmission. Deferred
# via background_update() (ticked every ~50ms, see App.background_task())
# rather than a blocking time.sleep() in __init__, so this doesn't itself
# stall the badge during the delay. Now starts counting only AFTER the
# reset pulse above fires (2026-09-20), not concurrently with it, so the
# RP2350 has actually finished rebooting before attach_mirror() is tried.
_ATTACH_DELAY_MS = 1500


def _auto_attach_get():
    # Shared with tildagon-hdmi-manager: settings key hdmi_auto_attach,
    # default on when unset.
    try:
        import settings
        v = settings.get("hdmi_auto_attach")
        return True if v is None else v
    except Exception:
        return True


def _auto_attach_set(value):
    try:
        import settings
        settings.set("hdmi_auto_attach", value)
        settings.save()
    except Exception as e:
        print("auto-attach save:", e)


def _boost_get():
    # Shared with tildagon-hdmi-manager: settings key hdmi_boost (the
    # 3V3->5V boost for the HDMI +5V pin), default on when unset.
    try:
        import settings
        v = settings.get("hdmi_boost")
        return True if v is None else v
    except Exception:
        return True


def _boost_set(value):
    try:
        import settings
        settings.set("hdmi_boost", value)
        settings.save()
    except Exception as e:
        print("boost save:", e)


def _boost_apply(port, on):
    # The firmware keeps the boost enable in byte 0x20 of its emulated
    # EEPROM (bit 0, see the firmware's eeprom_i2c.h). It boots with the
    # boost on, so the saved setting is re-applied after each reset.
    try:
        from machine import I2C
        I2C(port).writeto_mem(0x50, 0x20, bytes([1 if on else 0]), addrsize=16)
    except Exception as e:
        print("boost write:", e)


class MirrorApp(app.App):
    def _reset_rp2350(self):
        # Pulse LS_A low to hardware-reset the RP2350 -- HISTORY.md section
        # 4.2's own documented pin table: "RUN pin <- LS_A (badge-controlled
        # reset) and SW2 reset button" (cross-checked against two
        # independent real hexpansion designs). Requires LS_A physically
        # wired to the Metro's RUN pin on the bench rig -- if it isn't,
        # this just prints and does nothing (no exception propagates).
        #
        # Added 2026-09-20: reinsertion sometimes left the RP2350's
        # HSTX/core1 video output blank while its SPI0 receive path (core0)
        # kept working fine underneath -- confirmed via live UART
        # diagnostics (frames landing continuously, swap_count advancing,
        # while the monitor stayed dark), so the bug is on the core1/HSTX
        # side, not anything core0-side firmware changes so far have
        # touched. A hardware reset on every insertion sidesteps it
        # entirely, matching the project's own always-intended pin design
        # rather than chasing the exact HSTX fault from the badge side.
        try:
            ls_a = self.config.ls_pin[0]
            ls_a.init(ls_a.OUT)
            ls_a.value(0)
            time.sleep_ms(10)
            ls_a.init(ls_a.IN)  # release -- RUN's own pull-up takes back over
        except Exception as e:
            print("LS_A reset failed:", e)

    def __init__(self, config):
        super().__init__()
        self.config = config
        self.buttons = Buttons(self)
        self.status = "starting..."
        self._pending_reset_port = None
        self._reset_delay_remaining_ms = 0
        self._pending_attach_port = None
        self._attach_delay_remaining_ms = 0
        self._boost_sync_ms = 0

        port = getattr(config, "port", None) if config else None
        if port is not None:
            eventbus.emit(HexpansionAppLauncherAddEvent(port, "HDMI Mirror"))
            # Honour the tildagon-hdmi-manager app's "Auto-attach" toggle
            # (settings key hdmi_auto_attach, default on): when it's off,
            # skip the reset+attach and leave the mirror to the user.
            if _auto_attach_get():
                self._pending_reset_port = port
                self._reset_delay_remaining_ms = _RESET_DELAY_MS
            else:
                self._boost_sync_ms = 2500

    def background_update(self, delta):
        if self._pending_reset_port is not None:
            self._reset_delay_remaining_ms -= delta
            if self._reset_delay_remaining_ms <= 0:
                port = self._pending_reset_port
                self._pending_reset_port = None
                self._reset_rp2350()
                self._pending_attach_port = port
                self._attach_delay_remaining_ms = _ATTACH_DELAY_MS
                return

        if self._pending_attach_port is not None:
            self._attach_delay_remaining_ms -= delta
            if self._attach_delay_remaining_ms <= 0:
                port = self._pending_attach_port
                self._pending_attach_port = None
                self._attach(port)

    def _attach(self, port):
        _boost_apply(port, _boost_get())
        # FIXED (2026-09-20): this used to build its own driver dict --
        # baudrate 5MHz, header=FRAME_MARKER (bytes([0xA5, 0x5A] * 4)) --
        # left over from testcard's OLD hardware-SPI0/PL022 receiver and
        # its synthetic frame protocol. pio-testcard's PIO-based SPI0
        # receiver (the current firmware -- see spi_slave_rx.pio) only
        # ever recognizes hazanjon's REAL protocol: a 4-byte "TDHD"
        # header at 10MHz, proven on real hardware (also the exact dict
        # display_manager/display_drivers.py's own HDMI_DRIVER uses).
        # Since this file is packed into the SAME fake EEPROM now served
        # to pio-testcard, the old dict meant this auto-launched relay's
        # own attach_mirror() call produced zero real frames -- silently,
        # since a header mismatch just never triggers pio-testcard's
        # frame-sync logic rather than raising an error. No dc= override
        # either: display_manager's own working HDMI-mode calls don't
        # pass one, and _DC_OVERRIDE_PIN was only ever needed for the old
        # protocol's electrical-contention bug (see that constant's own
        # comment above).
        driver = {"header": b"TDHD", "baudrate": 10_000_000}
        try:
            display.attach_mirror(port=port, driver=driver, baudrate=driver["baudrate"])
            self.status = "attached"
        except Exception as e:
            self.status = "attach_mirror failed: {!r}".format(e)
            print(self.status)

    def deinit(self):
        self._pending_reset_port = None  # cancel a still-pending delayed reset
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
        if "B" in _FB and self.buttons.pressed(_FB["B"]):
            on = not _boost_get()
            _boost_set(on)
            _boost_apply(getattr(self.config, "port", None), on)
        if "D" in _FB and self.buttons.pressed(_FB["D"]):
            # Manual attach/detach, regardless of the auto-attach setting.
            port = getattr(self.config, "port", None) if self.config else None
            if port is not None:
                try:
                    active = display.is_mirror_active(port)
                except Exception:
                    active = False
                if active:
                    display.detach_mirror(port)
                else:
                    self._pending_reset_port = port
                    self._reset_delay_remaining_ms = _RESET_DELAY_MS
        if self.buttons.pressed(BUTTON_TYPES["CONFIRM"]):
            on = not _auto_attach_get()
            _auto_attach_set(on)
            # Turning it on does what insertion does: reset the RP2350,
            # wait, then attach -- unless the mirror is already up.
            port = getattr(self.config, "port", None) if self.config else None
            if on and port is not None:
                try:
                    active = display.is_mirror_active(port)
                except Exception:
                    active = False
                if not active:
                    self._pending_reset_port = port
                    self._reset_delay_remaining_ms = _RESET_DELAY_MS

    def _live_status(self):
        # Ask the firmware whether the mirror is really attached (works
        # without the manager app: display.is_mirror_active is badge
        # firmware). self.status only records that attach_mirror() ran.
        port = getattr(self.config, "port", None) if self.config else None
        if port is None:
            return self.status
        try:
            if display.is_mirror_active(port):
                return "Mirroring"
        except Exception:
            return "state unknown"
        if self._pending_reset_port is not None or self._pending_attach_port is not None:
            return "starting..."
        if self.status.startswith("attach_mirror failed"):
            return self.status
        return "Not mirroring"

    def draw(self, ctx):
        ctx.save()
        # Colours match the tildagon-hdmi-manager app.
        ctx.rgb(0.05, 0.06, 0.10).rectangle(-120, -120, 240, 240).fill()
        ctx.text_align = ctx.CENTER
        ctx.text_baseline = ctx.MIDDLE
        # Explicit sizes (px), as in tildagon-hdmi-manager: small_font_size is
        # ~18px, which overran the round screen's edges.
        ctx.font_size = 18
        ctx.rgb(0.8, 0.85, 1.0).move_to(0, -84).text("HDMI-HEX")
        ctx.font_size = 16
        st = self._live_status()
        if st == "Mirroring":
            col = (0.0, 0.85, 0.3)
        elif st in ("Not mirroring", "starting..."):
            col = (1.0, 0.75, 0.0)
        else:
            col = (0.93, 0.14, 0.0)
        ctx.rgb(*col).move_to(0, -62).text(st[:22])  # clip long error text
        ctx.font_size = 14
        ctx.rgb(0.65, 0.8, 1.0).move_to(0, -40).text(
            "Auto-attach: " + ("on" if _auto_attach_get() else "off")
        )
        ctx.move_to(0, -22).text("5V boost: " + ("on" if _boost_get() else "off"))
        ctx.text_align = ctx.LEFT
        y = 6
        for k, v in (("B:", "toggle 5V boost"), ("C:", "toggle auto-attach"),
                     ("D:", "attach / detach"), ("F:", "back")):
            ctx.rgb(1.0, 0.95, 0.4).move_to(-80, y).text(k)
            ctx.rgb(0.9, 0.9, 0.9).move_to(-58, y).text(v)
            y += 18
        ctx.restore()


__app_export__ = MirrorApp
