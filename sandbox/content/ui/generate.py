#!/usr/bin/env python3
"""Writes panel.png, the one image the sandbox UI draws.

The sandbox holds no authored art, and M6.3 needs an image that shows a wrong
draw at a glance. So this generates one rather than committing a picture whose
licence somebody has to check.

The pattern is asymmetric on purpose. A mirrored or turned quad moves the
corner mark, and a source rectangle read the wrong way up swaps the two halves.
A flat colour or a symmetric checker would hide both.

Run it from this directory:

    python3 generate.py
"""

from __future__ import annotations

import struct
import zlib

SIZE = 64
BORDER = 3
MARK = 16

# Straight sRGB bytes. The cooker reads the colour space out of the sidecar.
BACKGROUND = (32, 38, 64)
BORDER_COLOUR = (196, 204, 232)
MARK_COLOUR = (214, 64, 48)
BAR_COLOUR = (96, 176, 128)


def texel(x: int, y: int) -> tuple[int, int, int, int]:
    """Returns the RGBA of one texel of the tile."""
    on_border = x < BORDER or y < BORDER or x >= SIZE - BORDER or y >= SIZE - BORDER
    if on_border:
        return (*BORDER_COLOUR, 255)
    if x < MARK and y < MARK:
        # The top left corner mark. This is what says which way is up.
        return (*MARK_COLOUR, 255)
    if y >= SIZE - MARK - BORDER and x < SIZE // 2:
        # A bar along the bottom left, so the two halves cannot be confused.
        return (*BAR_COLOUR, 255)
    return (*BACKGROUND, 255)


def write_png(path: str) -> None:
    """Writes a 64 by 64 RGBA PNG with no dependency on an image library."""
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)  # The per-row filter. Zero is "none".
        for x in range(SIZE):
            raw.extend(texel(x, y))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        head = struct.pack(">I", len(payload)) + kind
        return head + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    data = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as out:
        out.write(data)


if __name__ == "__main__":
    write_png("panel.png")
    print(f"Wrote panel.png, {SIZE} by {SIZE}.")
