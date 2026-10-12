#!/usr/bin/env python3
"""sheet.py out.png label_step img...: tile screenshots into a labelled contact sheet (local viewing only)."""
import sys
from PIL import Image, ImageDraw
out, step, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
W, H, C = 160, 120, 8
rows = (len(files) + C - 1) // C
sheet = Image.new("RGB", (W * C, (H + 12) * rows), "black")
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB").resize((W, H))
    x, y = (i % C) * W, (i // C) * (H + 12)
    sheet.paste(im, (x, y + 12))
    d.text((x + 2, y), f"{(i + 1) * step}", fill="yellow")
sheet.save(out)
