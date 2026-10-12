#!/usr/bin/env python3
"""The game folder's 32x32 launcher icon, drawn here from code (no game art): "FZX" in a
blocky 4x7 font over a dark blue field with a speed stripe underneath. Writes a PNG with
only the image chunks (no text/time metadata).

usage: make_icon.py out.png
"""
import struct, sys, zlib

GLYPHS = {
    'F': ["1111", "1000", "1000", "1110", "1000", "1000", "1000"],
    'Z': ["1111", "0001", "0010", "0100", "1000", "1000", "1111"],
    'X': ["1001", "1001", "0110", "0110", "0110", "1001", "1001"],
}

def main():
    w = h = 32
    px = [[(10, 18, 48, 255) for _ in range(w)] for _ in range(h)]
    for y in range(25, 28):                 # a slanted speed stripe under the letters
        for x in range(3 + (27 - y), 30 - (y - 25)):
            px[y][x] = (200, 30, 40, 255)
    x0, y0, s = 3, 8, 2                     # 4x7 glyphs at 2x, one column apart
    for ci, ch in enumerate("FZX"):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == '1':
                    for dy in range(s):
                        for dx in range(s):
                            px[y0 + gy * s + dy][x0 + ci * 9 + gx * s + dx] = (250, 210, 40, 255)
    raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in px)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(sys.argv[1], 'wb').write(png)

if __name__ == '__main__':
    main()
