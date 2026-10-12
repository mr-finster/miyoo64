#!/usr/bin/env python3
"""Contact sheet of frame dumps at native size (1:1, never scaled), small dark
gaps between panels, a frame-number label under each.
usage: contact.py <out.png> <cols> <frame.ppm>...   (ROM-derived images: never tracked)"""
import sys
from PIL import Image, ImageDraw
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
ims = [Image.open(f).convert('RGB') for f in files]
w, h = ims[0].size
gap, lab = 4, 12
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w + (cols + 1) * gap, rows * (h + lab) + (rows + 1) * gap), (24, 24, 24))
d = ImageDraw.Draw(sheet)
for i, (im, f) in enumerate(zip(ims, files)):
    x = gap + (i % cols) * (w + gap)
    y = gap + (i // cols) * (h + lab + gap)
    sheet.paste(im, (x, y))
    d.text((x + 2, y + h), f.rsplit('/', 1)[-1].replace('frame_', '').replace('.ppm', ''), fill=(200, 200, 200))
sheet.save(out)
print(out, sheet.size)
