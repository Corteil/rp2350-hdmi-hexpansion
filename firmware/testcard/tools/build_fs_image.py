#!/usr/bin/env python3
# Builds the LittleFS2 image packed into the RP2350's emulated EEPROM
# (rp2350-hdmi-hexpansion testcard firmware) and emits it as a C byte
# array, src/fs_image.h, for main.c to memcpy into the eeprom buffer at
# fs_offset. The generated header is checked in like any other generated
# artifact, not rebuilt by CMake (keeps the C build reproducible without
# needing littlefs-python/mpy-cross installed at every build).
#
# Block layout MUST match what the badge itself computes (see main
# README section 5 / badge-2024-software's
# modules/system/hexpansion/util.py get_hexpansion_block_devices() and
# modules/eeprom_partition.py EEPROMPartition.ioctl(4)):
#
#   block_size  = 512 bytes (fixed by the badge for any EEPROM >= 8 KiB)
#   block_count = (eeprom_total_size - fs_offset) // block_size
#
# With this project's header values (fs_offset=64, eeprom_total_size=
# 8192): (8192-64)//512 = 15.875 -> 15 blocks. Built with EXACTLY 15
# blocks here so every block this image references is one the badge's
# partition wrapper will actually let it read.
#
# 2026-09-08: badge_app/app.py is packed again, as PRE-COMPILED BYTECODE
# (app.mpy via mpy-cross), not raw source -- reverses the 2026-09-07
# decision to leave this image empty. That decision was purely a size
# problem, not an architectural one: HEX_EEPROM_SIZE shrank from 64 KiB
# to 8 KiB to free RP2350 SRAM for a real double-buffered framebuf[2],
# and app.py's 32 KiB of raw source (most of it historical comments)
# doesn't fit in 15 blocks (7680 bytes) alongside that. Compiling first
# removes the comments/docstrings and uses a compact bytecode
# representation instead of text -- 32141 bytes -> 4389 bytes, comfortably
# under budget with the double-buffering win kept intact. MicroPython's
# import system resolves `import app` against either app.py or app.mpy
# transparently, so no badge-2024-software-side change was needed for
# this to work.
#
# hexi_mirror.py (badge-2024-software's launcher-menu entry, also in
# this dir) is deliberately NOT packed here -- it's badge-2024-software
# UI code, not something the hexpansion itself should serve; it's copied
# into that other checkout for local testing instead (see that file's
# own header comment).
#
# CRITICAL: the .mpy bytecode format is versioned and must come from an
# mpy-cross build EXACTLY matching the MicroPython commit the RP2350
# firmware embeds (a generic pip-installed mpy-cross is NOT guaranteed
# to match and can silently produce a .mpy the badge's own runtime
# refuses to load) -- badge-2024-software vendors that exact source at
# micropython/mpy-cross/; build it there (`cd micropython/mpy-cross &&
# make`) and point MPY_CROSS at the resulting binary, e.g.:
#   MPY_CROSS=/path/to/badge-2024-software/micropython/mpy-cross/build/mpy-cross \
#       python tools/build_fs_image.py
#
# Usage: python tools/build_fs_image.py
# Requires: pip install littlefs-python

import os
import pathlib
import subprocess
import sys
import tempfile

from littlefs import LittleFS

HERE = pathlib.Path(__file__).resolve().parent
BADGE_APP_DIR = HERE.parent / "badge_app"
APP_SOURCE = BADGE_APP_DIR / "app.py"
OUT_HEADER = HERE.parent / "src" / "fs_image.h"

BLOCK_SIZE = 512
BLOCK_COUNT = 15  # (eeprom_total_size=8192 - fs_offset=64) // 512 -- must match src/eeprom_i2c.c

# ESP32/ESP32-S3's Xtensa target -- must match badge-2024-software's own
# frozen-module build flag (see that repo's ports/esp32/boards/tildagon
# manifest build: `makemanifest.py ... -f-march=xtensawin`), since
# app.py uses a @micropython.viper function (native-code-compiled, so
# genuinely architecture-specific, unlike plain bytecode).
MPY_CROSS_ARCH = "xtensawin"


def find_mpy_cross() -> str:
    override = os.environ.get("MPY_CROSS")
    if override:
        return override
    # Default guess: sibling badge-2024-software checkout, if present.
    guess = (
        HERE.parent.parent.parent.parent
        / "badge-2024-software"
        / "micropython"
        / "mpy-cross"
        / "build"
        / "mpy-cross"
    )
    if guess.is_file():
        return str(guess)
    sys.exit(
        "mpy-cross not found -- set MPY_CROSS to a binary built from "
        "badge-2024-software's own micropython/mpy-cross/ (see this "
        "script's header comment for why a generic pip install isn't "
        "safe to use here)."
    )


def compile_app_mpy(mpy_cross: str) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        out_path = pathlib.Path(tmp) / "app.mpy"
        subprocess.run(
            [mpy_cross, f"-march={MPY_CROSS_ARCH}", str(APP_SOURCE), "-o", str(out_path)],
            check=True,
        )
        return out_path.read_bytes()


def build_image() -> bytes:
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=BLOCK_COUNT)

    # TEMP DEV MODE (2026-09-10): EEPROM filesystem left deliberately empty
    # so badge_app/app.py can be sideloaded onto the badge's own flash at
    # /drivers/hex_1969_4544/app.py instead -- modules/system/hexpansion/
    # app.py's own _try_filesystem_driver() falls back there automatically
    # when the mounted EEPROM has no app, keyed by VID/PID from the real
    # header (still sent correctly; only the packed filesystem changes).
    # Real physical hexpansion insertion still triggers everything --
    # this only removes the RP2350-firmware-rebuild step from the Python
    # iteration loop while chasing the core0/core1 framebuffer race.
    # Revert to packing app.mpy (see git history) once that's done.
    if os.environ.get("EMPTY_FS"):
        print("  EMPTY_FS set -- leaving filesystem region empty (sideload mode)")
    else:
        mpy_cross = find_mpy_cross()
        data = compile_app_mpy(mpy_cross)
        print(f"  compiled app.py -> app.mpy ({APP_SOURCE.stat().st_size} -> {len(data)} bytes)")
        with fs.open("app.mpy", "wb") as f:
            f.write(data)
        print(f"  packed app.mpy ({len(data)} bytes)")

    return bytes(fs.context.buffer)


def emit_header(image: bytes, out_path: pathlib.Path) -> None:
    lines = [
        "// GENERATED FILE -- do not hand-edit.",
        "// Produced by tools/build_fs_image.py from badge_app/app.py. Re-run that",
        "// script (pip install littlefs-python; MPY_CROSS pointing at a matching",
        "// mpy-cross build -- see the script's header comment) after changing",
        "// app.py, then rebuild this firmware.",
        "",
        "#ifndef FS_IMAGE_H",
        "#define FS_IMAGE_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define FS_IMAGE_BLOCK_SIZE  {BLOCK_SIZE}",
        f"#define FS_IMAGE_BLOCK_COUNT {BLOCK_COUNT}",
        f"#define FS_IMAGE_SIZE {len(image)}",
        "",
        "static const uint8_t fs_image[FS_IMAGE_SIZE] = {",
    ]
    for i in range(0, len(image), 16):
        chunk = image[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")

    out_path.write_text("\n".join(lines))


def main() -> int:
    print(f"Building LittleFS2 image ({BLOCK_COUNT} x {BLOCK_SIZE}-byte blocks = "
          f"{BLOCK_COUNT * BLOCK_SIZE} bytes)...")
    image = build_image()
    assert len(image) == BLOCK_COUNT * BLOCK_SIZE, \
        f"image size {len(image)} != expected {BLOCK_COUNT * BLOCK_SIZE}"

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    emit_header(image, OUT_HEADER)
    print(f"Wrote {OUT_HEADER} ({len(image)} bytes as a C array)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
