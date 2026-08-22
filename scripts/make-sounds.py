#!/usr/bin/env python3
"""Generates the sandbox sound effects.

The sandbox owns its content, and a sound library would bring a license
question with it. These are synthesized instead, the way the room and the
sphere models were. Nothing here is meant to be pretty. Each one has to be
recognizable, short, and obviously the sound of the thing it is named after.

The output is 16-bit mono WAV at 48000 Hz, which is what the cooker decodes to
PCM. Run it from the repository root:

    python3 scripts/make-sounds.py

It writes into sandbox/content/sounds/ and it overwrites what is there. A sound
is generated rather than authored, so an edit belongs in this file.
"""

import math
import pathlib
import struct
import sys

RATE = 48000


def write_wav(path, samples):
    """Writes one mono 16-bit WAV."""
    frames = len(samples)
    data = b"".join(
        struct.pack("<h", max(-32767, min(32767, int(round(value * 32767)))))
        for value in samples
    )
    header = b"".join(
        [
            b"RIFF",
            struct.pack("<I", 36 + len(data)),
            b"WAVEfmt ",
            struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16),
            b"data",
            struct.pack("<I", len(data)),
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + data)
    print(f"{path}: {frames} frames, {frames / RATE:.3f} s")


def envelope(index, frames, attack_frames, curve):
    """A quick rise and an exponential fall, which is what a short effect is."""
    if index < attack_frames:
        return index / max(1, attack_frames)
    fall = (index - attack_frames) / max(1, frames - attack_frames)
    return math.exp(-curve * fall)


def click(seconds=0.06, frequency=880.0):
    """A short bright tick, for a button."""
    frames = int(RATE * seconds)
    attack = int(RATE * 0.001)
    out = []
    for i in range(frames):
        # Two tones a fifth apart read as a click rather than as a note.
        tone = math.sin(2 * math.pi * frequency * i / RATE)
        tone += 0.5 * math.sin(2 * math.pi * frequency * 1.5 * i / RATE)
        out.append(0.5 * tone * envelope(i, frames, attack, 9.0))
    return out


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    sounds = root / "sandbox" / "content" / "sounds"
    write_wav(sounds / "click.wav", click())
    return 0


if __name__ == "__main__":
    sys.exit(main())
