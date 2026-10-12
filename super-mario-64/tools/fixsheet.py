#!/usr/bin/env python3
"""A fix sheet (the user's rule): before | after | the real game at the same instant,
native square at 1:1, with a labelled zoom of the densest changed area.

usage: fixsheet.py <out.png> <before_dir> <after_dir> <refdir> <frame>[,<frame>...]
                   [--offset 2] [--zoom 3] [--box 80x60] [--title TEXT]
  before_dir, after_dir: our dumps (frame_<N>.ppm, square)
  refdir: the emulator reference script (m64ref.sh) z64 frames (f<N>.png, 320x240), shot N+offset
One row per frame; under it the three zooms of the box where most pixels changed (the
box is outlined on the 1:1 panels). The reference is its centre 240 columns (our view).
"""
import argparse, os
from PIL import Image, ImageChops, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('before'); ap.add_argument('after'); ap.add_argument('ref')
ap.add_argument('frames')
ap.add_argument('--offset', type=int, default=2)
ap.add_argument('--zoom', type=int, default=3)
ap.add_argument('--box', default='80x60')
ap.add_argument('--title', default='')
ap.add_argument('--diff', type=int, default=0, help='add a 4th column: |after - before| x DIFF')
o = ap.parse_args()
bw, bh = (int(v) for v in o.box.split('x'))
font = ImageFont.load_default()
W, PAD, LAB = 240, 8, 14
cols = 4 if o.diff else 3

def densest(mask):
    """the bw x bh box (step 4) holding the most changed pixels"""
    w, h = mask.size
    px = mask.load()
    # integral image
    ii = [[0] * (w + 1) for _ in range(h + 1)]
    for y in range(h):
        run = 0
        for x in range(w):
            run += 1 if px[x, y] else 0
            ii[y + 1][x + 1] = ii[y][x + 1] + run
    best, bx, by = -1, 0, 0
    for y in range(0, h - bh + 1, 4):
        for x in range(0, w - bw + 1, 4):
            n = ii[y + bh][x + bw] - ii[y][x + bw] - ii[y + bh][x] + ii[y][x]
            if n > best:
                best, bx, by = n, x, y
    return bx, by, best

rows = []
for f in (int(v) for v in o.frames.split(',')):
    a = Image.open(os.path.join(o.before, 'frame_%06d.ppm' % f)).convert('RGB')
    b = Image.open(os.path.join(o.after, 'frame_%06d.ppm' % f)).convert('RGB')
    r = Image.open(os.path.join(o.ref, 'f%06d.png' % (f + o.offset))).convert('RGB')
    x0 = (r.width - W) // 2
    r = r.crop((x0, 0, x0 + W, a.height))
    mask = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v else 0)
    changed = sum(1 for v in (getattr(mask, 'get_flattened_data', None) or mask.getdata)() if v)
    bx, by, _ = densest(mask)
    rows.append((f, changed, a, b, r, bx, by))

zw, zh = bw * o.zoom, bh * o.zoom
colw = max(W, zw)
sheet_w = PAD + cols * (colw + PAD)
row_h = LAB + a.height + PAD + LAB + zh + PAD * 2
top = LAB + PAD if o.title else 0
sheet = Image.new('RGB', (sheet_w, top + len(rows) * row_h), (24, 24, 24))
d = ImageDraw.Draw(sheet)
if o.title:
    d.text((PAD, 4), o.title, fill=(230, 230, 230), font=font)
y = top
for f, changed, a, b, r, bx, by in rows:
    names = ('before', 'after', 'real game (z64)', 'change x%d' % o.diff)
    ims = [a, b, r]
    if o.diff:
        ims.append(ImageChops.difference(a, b).point(lambda v: min(255, v * o.diff)))
    for i, im in enumerate(ims):
        x = PAD + i * (colw + PAD)
        d.text((x, y + 1), '%s  frame %d%s' % (names[i], f, '  (%d px changed)' % changed if i == 1 else ''),
               fill=(230, 230, 230), font=font)
        sheet.paste(im, (x, y + LAB))
        d.rectangle((x + bx - 1, y + LAB + by - 1, x + bx + bw, y + LAB + by + bh), outline=(255, 64, 64))
        zy = y + LAB + im.height + PAD
        d.text((x, zy + 1), '%s, %dx zoom of the red box' % (names[i], o.zoom), fill=(230, 230, 230), font=font)
        sheet.paste(im.crop((bx, by, bx + bw, by + bh)).resize((zw, zh), Image.NEAREST), (x, zy + LAB))
    y += row_h
sheet.save(o.out)
print(o.out, sheet.size)
