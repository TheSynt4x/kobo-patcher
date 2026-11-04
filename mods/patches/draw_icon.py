#!/usr/bin/env python3
"""Draw icons/koreader.png, the KOReader row's icon: an open book, drawn to match Kobo's thin line
icons on the More page. It's run by hand, and the picture is kept beside it."""
import math
import struct
import zlib
from pathlib import Path

SIZE, SCALE = 40, 8
LINE = 0.85


def segment(x, y, ax, ay, bx, by):
    """Distance from (x, y) to the segment from a to b."""
    dx, dy = bx - ax, by - ay
    t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - ax - t * dx, y - ay - t * dy)


def page(x, y, side):
    """One page's outline, from the spine out to its edge, with its top and bottom curving down
    towards the spine the way an open book's pages do. side is -1 for the left page, 1 for the right."""
    x *= side
    if x < -LINE:
        return False
    top = -11 + 2.5 * (1 - min(x, 15) / 15) ** 2
    bottom = 13 - 1.5 * (min(x, 15) / 15)
    outline = (abs(y - top) < LINE or abs(y - bottom) < LINE) and 0 <= x <= 15
    outline = outline or (abs(x - 15) < LINE and top <= y <= bottom)
    # Lines of text.
    text = any(abs(y - (-3 + 4 * n)) < 0.6 and 4 <= x <= 12 for n in range(3))
    return outline or text


def ink(x, y):
    spine = segment(x, y, 0, -8.5, 0, 13) < LINE
    return spine or page(x, y, -1) or page(x, y, 1)


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
out = Path(__file__).resolve().parent / "icons"
out.mkdir(exist_ok=True)
(out / "koreader.png").write_bytes(png)
