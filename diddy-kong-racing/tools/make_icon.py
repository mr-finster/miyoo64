#!/usr/bin/env python3
"""Draw the launcher icon (32x32 RGBA PNG): a golden balloon on blue.
Drawn from scratch here -- no game artwork -- so it can ship in the zip."""
import math, struct, sys, zlib

W = H = 32
px = [[(0, 0, 0, 0)] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        # rounded blue tile
        dx, dy = max(0, abs(x - 15.5) - 12), max(0, abs(y - 15.5) - 12)
        if dx * dx + dy * dy <= 12:
            px[y][x] = (40, 90, 200, 255)
        # balloon body (ellipse), shaded gold
        ex, ey = (x - 15.5) / 9.0, (y - 13.0) / 10.5
        d = ex * ex + ey * ey
        if d <= 1.0:
            light = max(0.0, 1.0 - math.hypot(x - 12.5, y - 9.0) / 14.0)
            r = int(200 + 55 * light); g = int(150 + 80 * light); b = int(20 + 60 * light)
            px[y][x] = (min(r, 255), min(g, 255), min(b, 255), 255)
        elif d <= 1.18:
            px[y][x] = (120, 80, 0, 255)
# knot and string
for (x, y) in ((15, 24), (16, 24), (15, 25), (16, 25)):
    px[y][x] = (150, 100, 10, 255)
for y in range(26, 31):
    x = 16 + (1 if (y // 2) % 2 else 0)
    px[y][x] = (235, 235, 235, 255)

raw = b''.join(b'\0' + b''.join(struct.pack('4B', *p) for p in row) for row in px)
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(sys.argv[1] if len(sys.argv) > 1 else 'miyoo/dkr.png', 'wb').write(png)
