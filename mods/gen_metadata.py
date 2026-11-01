#!/usr/bin/env python3
"""Write Qt's plugin header for a mod: gen_metadata.py path/to/libkimg.so > kit/metadata.inc

It's the block from Kobo's own image plugin, with the key and names swapped for ones of the same
length so that its offsets stay right."""
import sys

data = open(sys.argv[1], "rb").read()
start = data.index(b"QTMETADATA  ")
size = int.from_bytes(data[start + 20:start + 24], "little")  # the qbjs header's size field
block = data[start:start + 20 + size]
for old, new in ((b"KoboImagePlugin", b"KoboPatcherMods"), (b"kimg", b"knkm"), (b"image/x-kobo", b"image/x-knkm")):
    assert block.count(old) == 1
    block = block.replace(old, new)
print(",".join(str(byte) for byte in block))
