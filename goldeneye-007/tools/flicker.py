#!/usr/bin/env python3
"""flicker.py -- the floor flash as numbers: over consecutive frame dumps (GE_DUMP_EVERY=1), the
pixels that jump and come straight back (|f[t] - f[t-1]| > THR on some channel while f[t+1] is
back within THR/2 of f[t-1]): camera motion moves a pixel one way, a flash moves it and back.
DKR 467ad1e's measure of its road (round 16), for R33/P17 here (round 12).

    tools/flicker.py DIR [DIR2] [--thr 8] [--rows Y0,Y1] [--block N] [--heat OUT.png]

Prints the flicker pixels per frame (mean, max) and the frames with the most; with DIR2 (the
same run under another setting), both. --rows limits the count to a band (the floor under
the player: e.g. 160,240). --block N compares NxN block means instead of pixels: texture
shimmer from walking (sub-texel motion over a noisy texture) averages out, a triangle whose
shading jumps does not (counts are then blocks). --heat writes where DIR's flicker happens."""
import argparse
import os
from PIL import Image


def frames(d):
    fs = sorted(f for f in os.listdir(d) if f.endswith('.ppm') or f.endswith('.png'))
    return [os.path.join(d, f) for f in fs]


def load(p, block=1):
    im = Image.open(p).convert('RGB')
    if block > 1:
        w, h = im.size
        im = im.resize((w // block, h // block), Image.BOX)
    return im


def flicker(d, thr, rows, heat=None, block=1):
    fs = frames(d)
    if len(fs) < 3:
        raise SystemExit('%s: need 3+ consecutive frames' % d)
    ims = [load(p, block) for p in fs]
    w, h = ims[0].size
    y0, y1 = (rows[0] // block, rows[1] // block) if rows else (0, h)
    counts = []
    acc = [0] * (w * h) if heat else None
    for t in range(1, len(ims) - 1):
        a, b, c = ims[t - 1].load(), ims[t].load(), ims[t + 1].load()
        n = 0
        for y in range(y0, y1):
            for x in range(w):
                pa, pb, pc = a[x, y], b[x, y], c[x, y]
                if max(abs(pb[i] - pa[i]) for i in range(3)) > thr and max(abs(pc[i] - pa[i]) for i in range(3)) <= thr // 2:
                    n += 1
                    if acc is not None:
                        acc[y * w + x] += 1
        counts.append((n, os.path.basename(fs[t])))
    if heat:
        m = max(acc) or 1
        img = Image.new('L', (w, h))
        img.putdata([min(255, v * 255 // m) for v in acc])
        img.save(heat)
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--thr', type=int, default=8)
    ap.add_argument('--rows')
    ap.add_argument('--heat')
    ap.add_argument('--block', type=int, default=1)
    a = ap.parse_args()
    rows = tuple(int(v) for v in a.rows.split(',')) if a.rows else None
    for k, d in enumerate(a.dirs):
        c = flicker(d, a.thr, rows, a.heat if k == 0 else None, a.block)
        ns = [n for n, _ in c]
        top = sorted(c, reverse=True)[:5]
        print('%-40s %3d frames: flicker %s/frame mean %.2f max %d, frames with any %d; most: %s' % (
            d, len(c), 'blocks' if a.block > 1 else 'px', sum(ns) / len(ns), max(ns), sum(1 for n in ns if n),
            ', '.join('%s %d' % (f[6:12], n) for n, f in top)))


if __name__ == '__main__':
    main()
