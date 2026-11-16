"""Generate the CC0 example clips; Python 3 standard library only.

Integer-only synthesis and explicit little-endian PCM make output byte-identical
across platforms. Run: python example_game/audio/generate.py
"""
from pathlib import Path
import hashlib
import io
import struct
import sys
import wave

RATE = 48000
ROOT = Path(__file__).resolve().parent


def triangle(frame, period):
    phase = frame % period
    return (4 * abs(phase - period // 2) * 32767 // period) - 32767


def write(name, channels, seconds, sample):
    frames = RATE * seconds
    pcm = bytearray()
    for frame in range(frames):
        for channel in range(channels):
            pcm.extend(struct.pack("<h", sample(frame, channel, frames)))
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setparams((channels, 2, RATE, frames, "NONE", "not compressed"))
        output.writeframes(pcm)
    data = buffer.getvalue()
    if "--check" in sys.argv:
        if (ROOT / name).read_bytes() != data:
            raise SystemExit(f"Generated asset differs: {name}")
    else:
        (ROOT / name).write_bytes(data)
    print(f"{name}: {channels} channel(s), {seconds}s, {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}")


def effect(frame, channel, frames):
    envelope = min(frame, 240, frames - 1 - frame)
    return triangle(frame, 80) * envelope * (frames - 1 - frame) // (240 * frames * 5)


def orbit(frame, channel, frames):
    # Integer periods divide the clip length, so the loop is phase-continuous.
    return (triangle(frame, 400) + triangle(frame, 240) // 3) // 8


def ambience(frame, channel, frames):
    # Different channels, long common repeat period; not a mono spatial source.
    periods = (800, 600) if channel == 0 else (960, 640)
    return (triangle(frame, periods[0]) + triangle(frame, periods[1]) // 2) // 12


if __name__ == "__main__":
    write("effect.wav", 1, 1, effect)
    write("orbit-mono.wav", 1, 2, orbit)
    write("ambience-stereo.wav", 2, 8, ambience)
