#!/usr/bin/env python3
"""The launcher icon: a 32x32 RGBA five-point star, drawn here (our own
drawing, nothing from the game). Image chunks only, no metadata.
usage: make_icon.py out.png"""
import math, struct, sys, zlib

W = H = 32
def inside(px, py, pts):
    c = False
    for i in range(len(pts)):
        (x1, y1), (x2, y2) = pts[i], pts[(i + 1) % len(pts)]
        if (y1 > py) != (y2 > py) and px < (x2 - x1) * (py - y1) / (y2 - y1) + x1:
            c = not c
    return c
def star(cx, cy, r_out, r_in):
    return [(cx + (r_out if k % 2 == 0 else r_in) * math.sin(k * math.pi / 5),
             cy - (r_out if k % 2 == 0 else r_in) * math.cos(k * math.pi / 5)) for k in range(10)]
outer, inner = star(16, 17, 15.5, 6.6), star(16, 17, 12.5, 5.2)
rows = []
for y in range(H):
    row = bytearray([0])
    for x in range(W):
        # 4x4 supersampling for smooth edges
        o = sum(inside(x + (i + .5) / 4, y + (j + .5) / 4, outer) for i in range(4) for j in range(4))
        n = sum(inside(x + (i + .5) / 4, y + (j + .5) / 4, inner) for i in range(4) for j in range(4))
        if o == 0:
            row += bytes([0, 0, 0, 0]); continue
        t = n / 16  # inner share: yellow; rim: orange
        r = 255; g = int(150 + 70 * t + 20 * (1 - y / H)); b = int(20 + 40 * t)
        row += bytes([r, min(g, 255), b, int(255 * o / 16)])
    rows.append(bytes(row))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0)) \
      + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
