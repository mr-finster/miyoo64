#!/usr/bin/env python3
"""make_icon_game.py <your GoldenEye 007 (USA) dump> [out.png]: the release icon.

The user's call (2026-10-05, as Diddy Kong Racing's balloon on 2026-09-26): the game folder's launcher
icon is the game's own remote mine, the image the HUD draws next to the remote mine count
(the global image table's s_remotemineammoimage: texture 2234, 14x14 RGBA32). It is cut from
the player's ROM into miyoo/icon_game.png (untracked: never in git), at 1:1 centred on a
transparent 32x32 (scaled down, nearest, only if a texture were larger). Unused since 2026-10-06: the
release icon is miyoo/icon.png, drawn. Needs the m1 map
(tools/m1_build.sh) for where the tables sit in the ROM:
    python3 tools/make_icon_game.py "<your dump>"
The texture is decoded the way texLoad / texInflateNonZlib do (port/src/game/image.c).
"""
import hashlib
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import romtables as rt  # noqa: E402

MAP = os.path.join(ROOT, 'm1/ge007/build/u/ge007.u.map')
ENTRY = 's_remotemineammoimage'


class Bits:
    """texSetBitstring / texReadBits: MSB-first."""
    def __init__(self, data, pos):
        self.d, self.p, self.acc, self.n = data, pos, 0, 0

    def read(self, k):
        while self.n < k:
            self.acc = (self.acc << 8) | self.d[self.p]
            self.p += 1
            self.n += 8
        self.n -= k
        return (self.acc >> self.n) & ((1 << k) - 1)


def inflate_rle(b, total):
    """texInflateRle, for blocks of 8 bits or less."""
    btf, rlf, bs = b.read(3), b.read(3), b.read(4)
    assert bs <= 8
    cost, fudge = btf + rlf + bs + 1, 0
    while cost > 0:
        cost -= bs + 1
        fudge += 1
    out = []
    while len(out) < total:
        if b.read(1) == 0:
            out.append(b.read(bs))
        else:
            start = len(out) - b.read(btf) - 1
            run = b.read(rlf) + fudge
            for i in range(start, start + run):
                out.append(out[i])
            out.append(b.read(bs))
    return out


def texture_rgba(rom, m, texnum):
    """texLoad + texInflateNonZlib for a single-level RGBA32 texture: (w, h, [(r, g, b, a)])."""
    rs, re_, va = m['_cdataSegmentRomStart'], m['_cdataSegmentRomEnd'], m['_csegmentSegmentStart']
    seg = zlib.decompressobj(-15).decompress(rom[rs + 2:re_])
    t = m['g_Textures'] - va
    off = 0
    for n in range(texnum):  # image_entries_load: the table holds sizes; offsets are their sums
        off += struct.unpack('>I', seg[t + 8 * n:t + 8 * n + 4])[0] & 0xFFFFFF
    src = rom[m['_imagesSegmentRomStart'] + off:]
    head = src[0]
    assert head & 0x40 == 0 and head & 0x3F == 0, 'texture %d: not a single-level non-zlib texture' % texnum
    b = Bits(src, 1)
    fmt, w, h, comp = b.read(4), b.read(8), b.read(8), b.read(4)
    assert fmt == 0 and comp == 4, 'texture %d: format %d method %d (expected RGBA32, RLE)' % (texnum, fmt, comp)
    ch = inflate_rle(b, 4 * w * h)  # texChannelsToPixels: four planes, R G B A
    n = w * h
    return w, h, [(ch[i], ch[i + n], ch[i + 2 * n], ch[i + 3 * n]) for i in range(n)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'miyoo/icon_game.png')
    rom = rt.load_rom(sys.argv[1])
    m = rt.load_map(MAP)
    e = m[ENTRY] - m['_GlobalimagetableSegmentStart'] + m['_GlobalimagetableSegmentRomStart']
    texnum, ew, eh = struct.unpack('>IBB', rom[e:e + 6])
    w, h, px = texture_rgba(rom, m, texnum)
    assert (w, h) == (ew, eh), 'image entry says %dx%d, texture %d is %dx%d' % (ew, eh, texnum, w, h)
    if w > 32 or h > 32:  # nearest, to fit (not the case for the remote mine)
        s = max(w, h) / 32.0
        nw, nh = max(1, int(w / s)), max(1, int(h / s))
        px = [px[int(y * s) * w + int(x * s)] for y in range(nh) for x in range(nw)]
        w, h = nw, nh
    k = 32 // max(w, h)  # whole-pixel upscale to fill the icon (the user, 2026-10-05: the 14x14 mine doubled to 28x28)
    if k > 1:
        px = [px[(y // k) * w + (x // k)] for y in range(h * k) for x in range(w * k)]
        w, h = w * k, h * k
    ox, oy = (32 - w) // 2, (32 - h) // 2
    img = [[(0, 0, 0, 0)] * 32 for _ in range(32)]
    for y in range(h):
        for x in range(w):
            img[oy + y][ox + x] = px[y * w + x]
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in img)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(out_path, 'wb').write(png)
    print('%s: texture %d (%s), %dx%d at 1:1 on 32x32; pixels sha256 %s'
          % (out_path, texnum, ENTRY, w, h, hashlib.sha256(raw).hexdigest()[:16]))


if __name__ == '__main__':
    main()
