#!/usr/bin/env python3
"""ppm2png.py in.ppm... [-o dir] [-s scale] [-sheet out.png cols]: the port's PPM frames as PNG (no dependencies)."""
import struct, sys, zlib, os

def read_ppm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace(): i += 1
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def write_png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def scale(w, h, rgb, s):
    out = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w): row += rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] * s
        out += bytes(row) * s
    return w * s, h * s, bytes(out)

args = sys.argv[1:]
outdir, s, sheet, cols = None, 1, None, 4
if '-o' in args: i = args.index('-o'); outdir = args[i + 1]; del args[i:i + 2]
if '-s' in args: i = args.index('-s'); s = int(args[i + 1]); del args[i:i + 2]
if '-sheet' in args: i = args.index('-sheet'); sheet, cols = args[i + 1], int(args[i + 2]); del args[i:i + 3]
frames = [read_ppm(p) for p in args]
if sheet:
    w, h = frames[0][0], frames[0][1]
    rows = (len(frames) + cols - 1) // cols
    W, H, gap = cols * w + (cols - 1) * 4, rows * h + (rows - 1) * 4, 4
    img = bytearray(b'\x40' * (W * H * 3))
    for k, (fw, fh, rgb) in enumerate(frames):
        ox, oy = (k % cols) * (w + gap), (k // cols) * (h + gap)
        for y in range(fh):
            img[((oy + y) * W + ox) * 3:((oy + y) * W + ox + fw) * 3] = rgb[y * fw * 3:(y + 1) * fw * 3]
    write_png(sheet, *scale(W, H, bytes(img), s) if s > 1 else (W, H, bytes(img)))
else:
    for p, (w, h, rgb) in zip(args, frames):
        name = os.path.join(outdir or os.path.dirname(p), os.path.basename(p)[:-4] + '.png')
        write_png(name, *(scale(w, h, rgb, s) if s > 1 else (w, h, rgb)))
