#!/usr/bin/env python3
"""Emit /lib/firmware/ili9486.bin for the panel-mipi-dbi DRM driver.

Format (drivers/gpu/drm/tiny/panel-mipi-dbi.c):
  16-byte header: "MIPI DBI" + 7 zero bytes + u8 file_format_version (1)
  then records of [u8 command, u8 num_params, u8 params...];
  command 0x00 with 1 param = delay in milliseconds.
Usage: gen-ili9486-bin.py <out-path>
"""
import struct
import sys

MAGIC = b"MIPI DBI" + b"\x00" * 7
VERSION = 1

# ILI9486 init (from the old fb_ili9486 init sequence); MADCTL 0x28 =
# MV|BGR -> 480x320 landscape on the MHS35/LCDWiki 3.5" panel.
COMMANDS = [
    (0xB0, [0x00]),
    (0x3A, [0x55]),  # 16bpp RGB565
    (0xC2, [0x44]),
    (0xC5, [0x00, 0x00, 0x00, 0x00]),
    (0xE0, [0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
            0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00]),
    (0xE1, [0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
            0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00]),
    (0xE2, [0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
            0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00]),
    (0x36, [0x28]),  # MADCTL: MV | BGR
    (0x11, []),      # sleep out
    (0x00, [120]),   # delay 120ms
    (0x29, []),      # display on
    (0x00, [25]),
]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = bytearray(MAGIC + bytes([VERSION]))
    for cmd, params in COMMANDS:
        out += bytes([cmd, len(params)]) + bytes(params)
    with open(sys.argv[1], "wb") as fh:
        fh.write(out)
    print(f"wrote {sys.argv[1]} ({len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
