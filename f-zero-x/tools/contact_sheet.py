#!/usr/bin/env python3
"""contact_sheet.py <out.png> <cols> <scale> <ppm...> - frame dumps side by side, labelled."""
import sys, os
from PIL import Image, ImageDraw
out, cols, scale, files = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
ims = [Image.open(f).convert("RGB") for f in files]
w, h = ims[0].width * scale, ims[0].height * scale
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * (h + 12)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, (f, im) in enumerate(zip(files, ims)):
    x, y = (i % cols) * w, (i // cols) * (h + 12)
    sheet.paste(im.resize((w, h), Image.NEAREST), (x, y + 12))
    d.text((x + 2, y), os.path.basename(f).replace(".ppm", ""), fill=(255, 255, 0))
sheet.save(out)
