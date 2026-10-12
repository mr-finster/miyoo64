#!/usr/bin/env python3
"""make_icon.py out.png: the launcher icon, drawn here (no game art): a 32x32 red disc with a white "P" and "M"."""
import struct, sys, zlib

W = H = 32
img = [[(0, 0, 0, 0)] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        d = ((x - 15.5) ** 2 + (y - 15.5) ** 2) ** 0.5
        if d <= 15.5:
            a = 255 if d <= 14.5 else int(255 * (15.5 - d))
            img[y][x] = (206, 32, 41, a) if d <= 13 else (120, 12, 18, a)
P = ["1110", "1001", "1001", "1110", "1000", "1000", "1000"]
M = ["10001", "11011", "10101", "10101", "10001", "10001", "10001"]
def glyph(g, x0, y0, s=2):
    for gy, row in enumerate(g):
        for gx, c in enumerate(row):
            if c == '1':
                for dy in range(s):
                    for dx in range(s):
                        img[y0 + gy * s + dy][x0 + gx * s + dx] = (255, 255, 255, 255)
glyph(P, 5, 9)
glyph(M, 15, 9)
raw = b''.join(b'\0' + b''.join(struct.pack('4B', *px) for px in row) for row in img)
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
