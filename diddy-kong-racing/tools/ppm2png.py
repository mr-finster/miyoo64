#!/usr/bin/env python3
"""ppm2png.py in.ppm [out.png] [scale] -- P6 PPM to PNG (debug frame dumps), no PIL."""
import struct, sys, zlib

def read_ppm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace(): i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] != b'\n': i += 1
            continue
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def write_png(path, w, h, rgb, scale=1):
    rows = []
    for y in range(h):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        if scale > 1:
            row = b''.join(row[x * 3:x * 3 + 3] * scale for x in range(w))
        rows.extend([b'\0' + row] * scale)
    raw = b''.join(rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

if __name__ == '__main__':
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.png'
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb, scale)
