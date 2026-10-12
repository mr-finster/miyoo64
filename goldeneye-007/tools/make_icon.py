#!/usr/bin/env python3
"""make_icon.py <out.png>: the game folder's 32x32 launcher icon, drawn here (a gun-barrel
spiral), so no image file is tracked or shipped with authoring metadata: the PNG holds
only its IHDR, IDAT and IEND chunks."""
import math
import sys

from PIL import Image, ImageDraw

S = 128
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.ellipse((4, 4, S - 4, S - 4), fill=(12, 12, 12, 255))
for k in range(8):  # rifling grooves from the rim toward the bore
    pts = []
    for i in range(40):
        t = i / 39.0
        r = (S / 2 - 6) * (1 - 0.62 * t)
        a = k * math.pi / 4 + t * 1.9
        pts.append((S / 2 + r * math.cos(a), S / 2 + r * math.sin(a)))
    d.line(pts, fill=(170, 170, 170, 255), width=5)
d.ellipse((S / 2 - 24, S / 2 - 24, S / 2 + 24, S / 2 + 24), fill=(235, 235, 235, 255))
d.ellipse((S / 2 - 24, S / 2 - 24, S / 2 + 24, S / 2 + 24), outline=(120, 0, 0, 255), width=4)
img.resize((32, 32), Image.LANCZOS).save(sys.argv[1], optimize=True)
