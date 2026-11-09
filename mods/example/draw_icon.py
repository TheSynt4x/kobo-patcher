#!/usr/bin/env python3
"""Draw the example mod's icons: four blocks, for a mod built from the kit's blocks. icons/example.png
is its row on the More page, and icons/tab.png and icons/tab-active.png its tab, the open one with
the blocks filled. Thin lines, to match Kobo's own icons. It's run by hand, and the pictures are
kept beside it."""
import struct
import zlib
from pathlib import Path

SIZE, SCALE = 40, 8
LINE = 0.85
BLOCKS = ((-13, -13, -2, -2), (2, -13, 13, -2), (-13, 2, -2, 13), (2, 2, 13, 13))  # left, top, right, bottom


def segment(x, y, ax, ay, bx, by):
    """Distance from (x, y) to the segment from a to b."""
    dx, dy = bx - ax, by - ay
    t = max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return ((x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2) ** 0.5


def ink(x, y, filled):
    for left, top, right, bottom in BLOCKS:
        if filled and left <= x <= right and top <= y <= bottom:
            return True
        edges = ((left, top, right, top), (right, top, right, bottom), (right, bottom, left, bottom), (left, bottom, left, top))
        if any(segment(x, y, *edge) < LINE for edge in edges):
            return True
    return False


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(pixels, filled):
    """Drawn at pixels across, for the kit to scale to Kobo's own icons, black on clear."""
    out = pixels / SIZE
    rows = []
    for row in range(pixels):
        line = bytearray([0])
        for column in range(pixels):
            hits = sum(
                ink((column + (i + 0.5) / SCALE) / out - SIZE / 2, (row + (j + 0.5) / SCALE) / out - SIZE / 2, filled)
                for i in range(SCALE) for j in range(SCALE)
            )
            line += bytes([0, round(255 * hits / SCALE**2)])
        rows.append(bytes(line))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", pixels, pixels, 8, 4, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


icons = Path(__file__).resolve().parent / "icons"
icons.mkdir(exist_ok=True)
(icons / "example.png").write_bytes(png(80, False))  # 40 pixels on the Nia's More page
(icons / "tab.png").write_bytes(png(56, False))  # 28 in the Nia's bottom bar
(icons / "tab-active.png").write_bytes(png(56, True))
