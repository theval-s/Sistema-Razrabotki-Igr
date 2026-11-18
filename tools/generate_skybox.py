#!/usr/bin/env python3
"""Generate ExampleGame's original analytic DX10 sRGB cubemap (Python 3)."""
import argparse
import math
from pathlib import Path
import struct

OUTPUT = Path(__file__).resolve().parents[1] / "example_game/materials/example-sky.dds"
SIZE = 256


def direction(face, u, v):
    # DirectX cube sampling: u increases right, v increases down on each face.
    vectors = ((1, -v, -u), (-1, -v, u), (u, 1, v),
               (u, -1, -v), (u, -v, 1), (-u, -v, -1))
    x, y, z = vectors[face]
    length = math.sqrt(x*x + y*y + z*z)
    return x / length, y / length, z / length


def srgb(value):
    value = max(0.0, min(1.0, value))
    encoded = 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055
    return round(encoded * 255)


def pixel(face, x, y, size):
    dx, dy, dz = direction(face, 2 * (x + 0.5) / size - 1, 2 * (y + 0.5) / size - 1)
    # Smooth blue zenith, warm horizon, dusky ground and a soft western glow.
    upper = max(0.0, dy) ** 0.55
    lower = max(0.0, -dy) ** 0.45
    horizon = (0.46, 0.29, 0.22)
    zenith = (0.025, 0.12, 0.32)
    ground = (0.035, 0.045, 0.075)
    base = tuple(horizon[i] * (1 - upper - lower) + zenith[i] * upper + ground[i] * lower for i in range(3))
    glow = max(0.0, dx * -0.68 + dy * 0.22 + dz * -0.70) ** 32
    return bytes((*[srgb(base[i] + glow * c) for i, c in enumerate((0.46, 0.25, 0.09))], 255))


def generate():
    mip_count = SIZE.bit_length()
    header = [0] * 31
    header[0:7] = [124, 0x2100f, SIZE, SIZE, SIZE * 4, 0, mip_count]
    header[18:21] = [32, 4, 0x30315844]  # DDS_PIXELFORMAT, FOURCC 'DX10'
    header[26:28] = [0x401008, 0xfe00]  # texture, complex, mipmap; all cube faces
    data = bytearray(b"DDS " + struct.pack("<31I", *header))
    data.extend(struct.pack("<5I", 29, 3, 4, 1, 0))  # RGBA8 sRGB, 2D, cube, one cube
    for face in range(6):
        for level in range(mip_count):
            size = max(1, SIZE >> level)
            for y in range(size):
                for x in range(size):
                    data.extend(pixel(face, x, y, size))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check the existing DDS without writing")
    args = parser.parse_args()
    data = generate()
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_bytes() != data:
            raise SystemExit(f"Mismatch: {OUTPUT}")
        print(f"Verified {OUTPUT}: {len(data)} bytes")
    else:
        OUTPUT.write_bytes(data)
        print(f"Wrote {OUTPUT}: {len(data)} bytes")


if __name__ == "__main__":
    main()
