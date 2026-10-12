#!/usr/bin/env python3
"""Black border of each frame: rows/columns from each edge that are all near-black (<= 8).
usage: tools/borders.py <dir or ppm...>  -> per frame: left right top bottom"""
import sys, glob, os
from PIL import Image
def border(im):
    im = im.convert('L'); w, h = im.size; px = im.load()
    def dark_col(x): return all(px[x, y] <= 8 for y in range(h))
    def dark_row(y): return all(px[x, y] <= 8 for x in range(w))
    l = 0
    while l < w and dark_col(l): l += 1
    r = 0
    while r < w and dark_col(w - 1 - r): r += 1
    t = 0
    while t < h and dark_row(t): t += 1
    b = 0
    while b < h and dark_row(h - 1 - b): b += 1
    return l, r, t, b
fs = []
for a in sys.argv[1:]:
    fs += sorted(glob.glob(os.path.join(a, '*.ppm'))) if os.path.isdir(a) else [a]
for f in fs:
    print(f, *border(Image.open(f)))
