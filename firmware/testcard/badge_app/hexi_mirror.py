# Hexi-GFX mirror -- launcher menu entry (dev/testing convenience).
#
# Lives in THIS repo (not badge-2024-software) because it's Hexi-GFX-
# project-specific badge-side code, same as app.py alongside it --
# badge-2024-software's own checkout keeps a plain COPY of this file at
# modules/firmware_apps/hexi_mirror.py (gitignored there, not committed
# to that fork's history) so the mount-based dev-testing workflow can
# import it as `firmware_apps.hexi_mirror`.
#
# Deliberately a copy, not a symlink: a symlink there crashed the badge
# outright (2026-09-08) with `ImportError: no module named
# 'firmware_apps.hexi_mirror'`, and needed a physical USB power cycle to
# recover -- `mpremote`'s own mount implementation lists directory
# entries with `os.lstat()` (transport_serial.py), which doesn't follow
# symlinks, so the mounted virtual filesystem never sees the real file.
# There is no live sync: after editing this file, re-copy it into
# badge-2024-software's modules/firmware_apps/ by hand before testing
# there again.
#
# MirrorApp now genuinely lives on the hexpansion's own emulated EEPROM
# filesystem again (2026-09-08, tools/build_fs_image.py) -- it packs
# app.py pre-compiled to bytecode (app.mpy via mpy-cross) instead of raw
# source, which is what makes it fit: 32141 bytes of mostly-comments
# source compiles down to ~4.4KB, comfortably under the 8KiB EEPROM's
# 7680-byte usable budget (the double-buffered framebuf[2] SRAM win from
# 2026-09-07 is kept -- no need to grow HEX_EEPROM_SIZE back). This
# reverses the same day's earlier sideload-based workaround (importing
# `sideload_test.app` by hand) -- back to the straightforward path:
# badge-2024-software's own `_launch_hexpansion_app()` mounts the real fs
# and imports `app` from it exactly like any other hexpansion.
#
# Port detection works the normal way: hexpansion header auto-detection
# (badge-2024-software's modules/system/hexpansion/app.py's
# handle_hexpansion_insertion) reads and stores each port's EEPROM
# header, so scanning hexpansion_headers by VID/PID below finds which
# port to launch on.
#
# Deliberately minimises itself back to the launcher (rather than staying
# in the foreground) once it's confirmed mirroring is running: MirrorApp
# mirrors whatever app currently HOLDS the foreground (display.get_fb()),
# so leaving this status screen in the foreground would mirror ITSELF,
# not whatever app the user actually wants shown on the external
# monitor. Intended flow: open this once per session to (re)arm
# mirroring, then go pick the app you actually want mirrored.
import app
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

        mgr._launch_hexpansion_app(port)
        if port in mgr.hexpansion_apps:
            self.status = f"Mirroring started (port {port})"
        else:
            self.status = f"Failed to start (port {port})"

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
