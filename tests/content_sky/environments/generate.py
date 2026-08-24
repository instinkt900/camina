#!/usr/bin/env python3
"""Generates sky.hdr, the environment the sky-order check renders.

An equirectangular map whose colour depends on elevation and on nothing else.
That is the property the check needs, not the look: a purely vertical gradient
puts the same colour at (x, y) and at (-x, y) for a camera with no roll, so the
left and the right halves of a frame of open sky are the same. Any difference
between the two halves then came from what was drawn over one of them.

The file is Radiance RGBE with flat scanlines and no run-length encoding. It is
tiny because the cooker filters it into a cubemap anyway, and the check reads
colours rather than detail.

Writes sky.hdr to the current directory. Run it from this directory:

    python3 generate.py
"""

import struct

WIDTH = 64
HEIGHT = 32

# Warm near the horizon, deep blue at the top, dark below. Each is linear
# radiance, not sRGB.
TOP = (0.05, 0.16, 0.55)
HORIZON = (0.95, 0.72, 0.42)
BOTTOM = (0.03, 0.03, 0.04)


def mix(a, b, t):
    """Blends two colours. Returns the blend."""
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def to_rgbe(colour):
    """Packs one linear colour as Radiance RGBE. Returns four bytes."""
    peak = max(colour)
    if peak < 1e-8:
        return bytes((0, 0, 0, 0))
    mantissa, exponent = struct.unpack("<f", struct.pack("<f", peak))[0], 0
    while mantissa >= 1.0:
        mantissa /= 2.0
        exponent += 1
    while mantissa < 0.5:
        mantissa *= 2.0
        exponent -= 1
    scale = mantissa * 256.0 / peak
    return bytes((int(colour[0] * scale), int(colour[1] * scale),
                  int(colour[2] * scale), exponent + 128))


def build():
    """Builds the whole file. Returns its bytes."""
    header = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n"
    header += f"-Y {HEIGHT} +X {WIDTH}\n".encode("ascii")

    body = bytearray()
    for row in range(HEIGHT):
        # 0 at the top of the map, 1 at the bottom.
        down = (row + 0.5) / HEIGHT
        if down < 0.5:
            colour = mix(TOP, HORIZON, down * 2.0)
        else:
            colour = mix(HORIZON, BOTTOM, (down - 0.5) * 2.0)
        # The same colour across the whole row, which is the property the
        # check relies on.
        body.extend(to_rgbe(colour) * WIDTH)
    return header + bytes(body)


if __name__ == "__main__":
    with open("sky.hdr", "wb") as out:
        out.write(build())
    print("sky.hdr written")
