# Hexi-GFX mirror -- launcher menu entry (dev/testing convenience).
#
# Lives in THIS repo (not badge-2024-software) because it's Hexi-GFX-
# project-specific badge-side code, same as app.py alongside it --
# badge-2024-software's own checkout just symlinks
# modules/firmware_apps/hexi_mirror.py to this file so the mount-based
# dev-testing workflow ([[feedback-hexigfx-sideload-over-reflash]]) can
# still import it as `firmware_apps.hexi_mirror`, without committing it
# to that fork's git history.
#
# MirrorApp does NOT come from the hexpansion's own emulated EEPROM
# filesystem -- confirmed empty on real hardware (2026-09-08: badge log
# showed "Mounted eeprom to /hexpansion_1" / "Hexpansion files: []" /
# "no module named 'hexpansion_1.app'"). That's deliberate, not a bug:
# tools/build_fs_image.py (2026-09-07) shrank HEX_EEPROM_SIZE from 64KiB
# to 8KiB to free RP2350 SRAM for a real double-buffered framebuf[2], and
# the 20KB+ badge_app/app.py no longer fits alongside -- so this repo's
# own README instead sideloads badge_app/app.py straight onto the
# BADGE's flash at :/sideload_test (`mpremote fs cp badge_app/app.py
# :/sideload_test/app.py`, a one-time, persists-across-reboots step,
# separate from badge-2024-software's own `modules/` mount) and launches
# it by hand with a manually constructed HexpansionConfig(port). This app
# automates exactly that hand-built sequence from a normal menu tap,
# importing `sideload_test.app` instead of going through the (currently
# always-empty) hexpansion-mount path badge-2024-software's own
# `_launch_hexpansion_app()` uses -- see [[hexigfx-mirror-status]].
#
# Port detection still works the normal way: hexpansion header
# auto-detection (badge-2024-software's
# modules/system/hexpansion/app.py's handle_hexpansion_insertion) already
# reads and stores each port's EEPROM header regardless of whether the fs
# it points to has any files on it, so scanning hexpansion_headers by
# VID/PID below is unaffected by the empty-fs issue above -- only
# importing FROM that specific port's mount is.
#
# Deliberately minimises itself back to the launcher (rather than staying
# in the foreground) once it's confirmed mirroring is running: MirrorApp
# mirrors whatever app currently HOLDS the foreground (display.get_fb()),
# so leaving this status screen in the foreground would mirror ITSELF,
# not whatever app the user actually wants shown on the external
# monitor. Intended flow: open this once per session to (re)arm
# mirroring, then go pick the app you actually want mirrored.
import app
import os
from events.input import Buttons, BUTTON_TYPES
from app_components.tokens import clear_background, small_font_size, label_font_size

# Main README section 1.4 / firmware/testcard's own header comment.
HEXI_GFX_VID = 0x1969
HEXI_GFX_PID = 0x4544


class HexiMirrorLauncherApp(app.App):
    def __init__(self):
        super().__init__()
        self.buttons = Buttons(self)
        self.status = "Looking for Hexi-GFX..."
        self._checked = False

    def _try_start(self):
        # Lazy import, matching badge-2024-software's own
        # system/capabilities/utils.py pattern for reaching this same
        # singleton -- system.hexpansion.app can't be imported at module
        # load time here without risking a circular import against the
        # launcher/hexpansion boot sequence.
        from system.hexpansion.app import _hexpansion_manager as mgr

        if mgr is None:
            self.status = "Hexpansion system not ready"
            return

        port = None
        for p, header in mgr.hexpansion_headers.items():
            if (
                header is not None
                and header.vid == HEXI_GFX_VID
                and header.pid == HEXI_GFX_PID
            ):
                port = p
                break

        if port is None:
            self.status = "Hexi-GFX not found - insert it"
            return

        if port in mgr.hexpansion_apps:
            self.status = f"Already mirroring (port {port})"
            return

        # A bare `import sideload_test.app` only resolves against whatever
        # directory happens to be the current working one -- matches
        # _launch_hexpansion_app()'s own os.chdir("/") before its import,
        # needed for the same reason (confirmed on real hardware
        # 2026-09-08: the import failed with "no module named" from
        # inside this app's own running context until this chdir was
        # added, despite :/sideload_test/app.py genuinely existing on the
        # badge's flash).
        old_cwd = os.getcwd()
        os.chdir("/")
        try:
            import sideload_test.app as mirror_pkg
        except ImportError as e:
            self.status = "No sideload: {!r}".format(e)
            return
        finally:
            os.chdir(old_cwd)

        from system.hexpansion.config import HexpansionConfig
        from system.scheduler.events import RequestStartAppEvent
        from system.eventbus import eventbus

        try:
            config = HexpansionConfig(port)
            instance = mirror_pkg.__app_export__(config=config)
        except Exception as e:
            self.status = "Launch failed: {!r}".format(e)
            return

        eventbus.emit(RequestStartAppEvent(instance))
        mgr.hexpansion_apps[port] = instance
        self.status = f"Mirroring started (port {port})"

    def update(self, delta):
        if not self._checked:
            self._checked = True
            self._try_start()
            return True

        if self.buttons.pressed(BUTTON_TYPES["CANCEL"]):
            self.buttons.clear()
            self.minimise()
            return True

    def draw(self, ctx):
        ctx.save()
        clear_background(ctx)
        ctx.text_align = ctx.CENTER
        ctx.text_baseline = ctx.MIDDLE
        ctx.font_size = label_font_size
        ctx.rgb(1, 1, 1).move_to(0, -30).text("HDMI Mirror")
        ctx.font_size = small_font_size
        ctx.rgb(1, 1, 0).move_to(0, 10).text(self.status)
        ctx.rgb(0.6, 0.6, 0.6).move_to(0, 45).text("CANCEL: back")
        ctx.restore()


__app_export__ = HexiMirrorLauncherApp
