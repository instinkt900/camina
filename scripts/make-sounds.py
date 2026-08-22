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


def noise(index):
    """Repeatable white noise.

    A generator seeded from the clock would give different files on every run,
    and the content tree has to be the same bytes for everybody. This is a
    small integer hash instead, which is the same on every machine and in every
    Python.
    """
    value = (index * 1103515245 + 12345) & 0x7FFFFFFF
    value ^= value >> 13
    return ((value % 20001) - 10000) / 10000.0


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


def thud(seconds=0.22, frequency=110.0):
    """A low knock, for a crate landing on something."""
    frames = int(RATE * seconds)
    attack = int(RATE * 0.002)
    out = []
    for i in range(frames):
        # A low tone that falls as it decays, which is what a solid thing
        # hitting the floor sounds like.
        sweep = frequency * (1.0 - 0.35 * i / frames)
        tone = math.sin(2 * math.pi * sweep * i / RATE)
        # A little noise at the front is the impact itself. Without it the
        # sound reads as a drum note rather than as wood on stone.
        knock = noise(i) * math.exp(-40.0 * i / frames)
        out.append(0.7 * (tone * envelope(i, frames, attack, 7.0) + 0.3 * knock))
    return out


def whoosh(seconds=0.18):
    """Filtered noise that falls away, for a crate leaving the camera."""
    frames = int(RATE * seconds)
    out = []
    smoothed = 0.0
    for i in range(frames):
        # A one pole low pass whose cutoff closes as the sound goes, so the
        # noise darkens the way something moving away does.
        alpha = 0.35 * (1.0 - 0.8 * i / frames)
        smoothed += alpha * (noise(i) - smoothed)
        out.append(0.6 * smoothed * envelope(i, frames, int(RATE * 0.01), 5.0))
    return out


def sweep(seconds=0.16, low=440.0, high=880.0):
    """A short rise, for the puzzle going back to the start."""
    frames = int(RATE * seconds)
    out = []
    phase = 0.0
    for i in range(frames):
        frequency = low + (high - low) * i / frames
        phase += 2 * math.pi * frequency / RATE
        out.append(0.45 * math.sin(phase) * envelope(i, frames, int(RATE * 0.004), 4.0))
    return out


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    sounds = root / "sandbox" / "content" / "sounds"
    write_wav(sounds / "click.wav", click())
    write_wav(sounds / "thud.wav", thud())
    write_wav(sounds / "throw.wav", whoosh())
    write_wav(sounds / "reset.wav", sweep())
    return 0


if __name__ == "__main__":
    sys.exit(main())
