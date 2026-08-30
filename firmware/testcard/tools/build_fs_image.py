#!/usr/bin/env python3
# Builds the LittleFS2 image packed into the RP2350's emulated EEPROM
# (rp2350-hdmi-hexpansion testcard firmware) and emits it as a C byte
# array, src/fs_image.h, for main.c to memcpy into the eeprom buffer at
# fs_offset. Run this whenever badge_app/ changes; the generated header
# is checked in like any other generated artifact, not rebuilt by CMake
# (keeps the C build reproducible without needing littlefs-python
# installed at every build).
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
# 65536): (65536-64)//512 = 127 blocks exactly. Built with EXACTLY 127
# blocks here so every block this image references is one the badge's
# partition wrapper will actually let it read -- building with more
# would silently reference bytes past what the badge exposes; building
# with fewer just wastes trailing EEPROM space, which is harmless but
# not done here since 127 was already the natural, matching number.
#
# Usage: python tools/build_fs_image.py
# Requires: pip install littlefs-python

import pathlib
import sys

from littlefs import LittleFS

HERE = pathlib.Path(__file__).resolve().parent
BADGE_APP_DIR = HERE.parent / "badge_app"
OUT_HEADER = HERE.parent / "src" / "fs_image.h"

BLOCK_SIZE = 512
BLOCK_COUNT = 127  # (eeprom_total_size=65536 - fs_offset=64) // 512 -- must match src/eeprom_i2c.c


def build_image() -> bytes:
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=BLOCK_COUNT)

    for path in sorted(BADGE_APP_DIR.rglob("*")):
        if path.is_dir():
            continue
        rel = path.relative_to(BADGE_APP_DIR).as_posix()
        data = path.read_bytes()
        with fs.open(rel, "wb") as f:
            f.write(data)
        print(f"  packed {rel} ({len(data)} bytes)")

    return bytes(fs.context.buffer)


def emit_header(image: bytes, out_path: pathlib.Path) -> None:
    lines = [
        "// GENERATED FILE -- do not hand-edit.",
        "// Produced by tools/build_fs_image.py from badge_app/. Re-run that",
        "// script (pip install littlefs-python first) after changing the",
        "// badge-side app source, then rebuild this firmware.",
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
    if not BADGE_APP_DIR.is_dir():
        print(f"error: {BADGE_APP_DIR} not found", file=sys.stderr)
        return 1

    print(f"Building LittleFS2 image ({BLOCK_COUNT} x {BLOCK_SIZE}-byte blocks = "
          f"{BLOCK_COUNT * BLOCK_SIZE} bytes) from {BADGE_APP_DIR}...")
    image = build_image()
    assert len(image) == BLOCK_COUNT * BLOCK_SIZE, \
        f"image size {len(image)} != expected {BLOCK_COUNT * BLOCK_SIZE}"

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    emit_header(image, OUT_HEADER)
    print(f"Wrote {OUT_HEADER} ({len(image)} bytes as a C array)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
