#!/usr/bin/env python3
"""Write icon.inc, the row's icon: a sewn-on patch, drawn to match Kobo's thin line icons."""
import math
import struct
import zlib
from pathlib import Path

SIZE, SCALE = 40, 8


def inside_rounded(x, y, half, radius):
    dx, dy = max(abs(x) - half + radius, 0), max(abs(y) - half + radius, 0)
    return math.hypot(dx, dy) - radius


def ink(x, y):
    outline = abs(inside_rounded(x, y, 16.5, 4)) < 0.85
    edge = inside_rounded(x, y, 11.5, 2)
    # Stitches: a dashed line just inside the outline.
    stitch = abs(edge) < 0.75 and (int((x + 40) // 3) + int((y + 40) // 3)) % 2 == 0
    return outline or stitch


rows = []
for row in range(SIZE):
    line = bytearray([0])
    for column in range(SIZE):
        hits = sum(
            ink((column + (i + 0.5) / SCALE) - SIZE / 2, (row + (j + 0.5) / SCALE) - SIZE / 2)
            for i in range(SCALE) for j in range(SCALE)
        )
        line += bytes([0, round(255 * hits / SCALE**2)])
    rows.append(bytes(line))


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 4, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
(Path(__file__).parent / "icon.inc").write_text(",".join(str(byte) for byte in png) + "\n")
