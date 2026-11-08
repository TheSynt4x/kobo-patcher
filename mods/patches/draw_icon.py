#!/usr/bin/env python3
"""Draw the icons of the rows this mod adds to the More page, to match Kobo's thin line icons there:
icons/patcher.png, Kobo Patcher's, a sticking plaster: a patch, and icons/koreader.png, KOReader's, an open book.
It's run by hand, and the pictures are kept beside it."""
import math
import struct
import zlib
from pathlib import Path

SIZE, SCALE = 40, 8
OUT = 2  # drawn at twice the size, 80 pixels, for the kit to scale to Kobo's own icons
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


def book(x, y):
    spine = segment(x, y, 0, -8.5, 0, 13) < LINE
    return spine or page(x, y, -1) or page(x, y, 1)


# The plaster lies at 45 degrees: a capsule, LENGTH long and WIDTH across, with its pad in the middle.
LENGTH, WIDTH, PAD = 34, 13, 5.5


def plaster(x, y):
    u, v = (x - y) / math.sqrt(2), (x + y) / math.sqrt(2)  # along it and across it
    radius = WIDTH / 2
    end = LENGTH / 2 - radius
    edge = math.hypot(max(abs(u) - end, 0), v)
    if edge > radius + LINE:
        return False
    if abs(edge - radius) < LINE:
        return True
    # The pad's two sides, and its holes.
    if abs(abs(u) - PAD) < LINE * 0.9:
        return True
    return any(math.hypot(u - du, v - dv) < 0.95 for du in (-2.4, 2.4) for dv in (-2.4, 2.4))


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def draw(ink):
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
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE * OUT, SIZE * OUT, 8, 4, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


out = Path(__file__).resolve().parent / "icons"
out.mkdir(exist_ok=True)
(out / "koreader.png").write_bytes(draw(book))
(out / "patcher.png").write_bytes(draw(plaster))
