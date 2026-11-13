"""Generate the small straight-alpha PNG used to inspect transparent edges."""
import math
import struct
import zlib
from pathlib import Path

size = 32
rows = []
for y in range(size):
    row = bytearray([0])
    for x in range(size):
        distance = math.hypot(x - 15.5, y - 15.5)
        alpha = max(0, min(255, round((14.5 - distance) * 255)))
        row.extend((255, 130, 30, alpha))
    rows.append(row)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
Path(__file__).with_name("badge.png").write_bytes(png)
