#!/usr/bin/env python3
# Builds the LittleFS2 image packed into the RP2350's emulated EEPROM
# (rp2350-hdmi-hexpansion testcard firmware) and emits it as a C byte
# array, src/fs_image.h, for main.c to memcpy into the eeprom buffer at
# fs_offset. The generated header is checked in like any other generated
# artifact, not rebuilt by CMake (keeps the C build reproducible without
# needing littlefs-python installed at every build).
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
# 2026-09-07: deliberately builds an EMPTY filesystem -- badge_app/ is no
# longer packed in here. HEX_EEPROM_SIZE shrank from 64 KiB to 8 KiB
# (see eeprom_i2c.c's own comment) to free RP2350 SRAM for a real
# double-buffered framebuf[2], which the 20 KiB+ badge_app/app.py no
# longer fits alongside. badge_app/ is now side-loaded onto the badge
# directly for testing instead of hosted in this emulated EEPROM; the
# badge's own hexpansion manager handles an empty mount gracefully (logs
# "App module not found", does nothing further -- see
# badge-2024-software's modules/system/hexpansion/app.py
# _launch_hexpansion_app()). Revert to packing badge_app/ (and grow
# HEX_EEPROM_SIZE back) once the real product wants the self-updating
# EEPROM-hosted-app property back and can afford single-buffered video.
#
# Usage: python tools/build_fs_image.py
# Requires: pip install littlefs-python

import pathlib

from littlefs import LittleFS

HERE = pathlib.Path(__file__).resolve().parent
OUT_HEADER = HERE.parent / "src" / "fs_image.h"

BLOCK_SIZE = 512
BLOCK_COUNT = 15  # (eeprom_total_size=8192 - fs_offset=64) // 512 -- must match src/eeprom_i2c.c


def build_image() -> bytes:
    # Empty on purpose -- see this file's header comment.
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=BLOCK_COUNT)
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
    print(f"Building empty LittleFS2 image ({BLOCK_COUNT} x {BLOCK_SIZE}-byte blocks = "
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
