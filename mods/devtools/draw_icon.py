#!/usr/bin/env python3
"""Draw icons/more.png, the icon of devtools' row on the More page: a terminal window with a prompt,
in thin lines to match Kobo's own icons there. It's run by hand, and the picture is kept beside
it."""
import struct
import zlib
from pathlib import Path

SIZE, SCALE = 40, 8
OUT = 2  # drawn at twice the size, 80 pixels, for the kit to scale to Kobo's own icons
LINE = 0.85
WINDOW = (-15, -12, 15, 12)  # left, top, right, bottom, from the middle


def segment(x, y, ax, ay, bx, by):
    """Distance from (x, y) to the segment from a to b."""
    dx, dy = bx - ax, by - ay
    t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return ((x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2) ** 0.5


def ink(x, y):
    left, top, right, bottom = WINDOW
    edges = ((left, top, right, top), (right, top, right, bottom), (right, bottom, left, bottom),
             (left, bottom, left, top), (left, top + 5, right, top + 5))  # the last is the title bar
    prompt = ((-9, -2, -4, 2.5), (-4, 2.5, -9, 7), (-1, 7, 7, 7))  # > and _
    return any(segment(x, y, *line) < LINE for line in edges + prompt)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


rows = []
for row in range(SIZE * OUT):
    line = bytearray([0])
    for column in range(SIZE * OUT):
        hits = sum(
            ink((column + (i + 0.5) / SCALE) / OUT - SIZE / 2, (row + (j + 0.5) / SCALE) / OUT - SIZE / 2)
            for i in range(SCALE) for j in range(SCALE)
        )
        line += bytes([0, round(255 * hits / SCALE**2)])
    rows.append(bytes(line))

png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE * OUT, SIZE * OUT, 8, 4, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
out = Path(__file__).resolve().parent / "icons"
out.mkdir(exist_ok=True)
(out / "more.png").write_bytes(png)
