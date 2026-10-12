#!/usr/bin/env python3
"""basheet.py OUT.png TITLE BEFORE AFTER [REF|-] [--zoom x,y,w,h] [--scale N] [--note TEXT]
The user's before | after | real game sheet (their rule: every on-screen fix gets one, BUILDLOG 13c). Row 1: the port's frames at native square, 1:1, and the
real game (m64ref 640x480) box-filtered to the port's uniform 0.75 (240x180) and centred in the port's square frame
the way the port places the N64 picture. Row 2 (with --zoom, in port pixels): the same area of all
three, nearest-neighbour xN, labelled as a zoom."""
import sys
from PIL import Image, ImageDraw

a = sys.argv[1:]
out, title, fb, fa, fr = a[0], a[1], a[2], a[3], a[4]
zoom = None; scale = 3; note = ''
i = 5
while i < len(a):
    if a[i] == '--zoom': zoom = tuple(int(v) for v in a[i + 1].split(',')); i += 2
    elif a[i] == '--scale': scale = int(a[i + 1]); i += 2
    elif a[i] == '--note': note = a[i + 1]; i += 2
    else: i += 1

before = Image.open(fb).convert('RGB')
after = Image.open(fa).convert('RGB')
ref = None
if fr != '-':
    r = Image.open(fr).convert('RGB')
    r = r.resize((240, 180), Image.BOX)
    ref = Image.new('RGB', (240, 240), (0, 0, 0))
    ref.paste(r, (0, 30))

panels = [('BEFORE (port, 1:1)', before), ('AFTER (port, 1:1)', after)]
if ref is not None:
    panels.append(('REAL GAME (z64, N64 x0.75)', ref))
gap, lab = 6, 14
W = len(panels) * 240 + (len(panels) - 1) * gap
rows_h = 22 + lab + 240
if zoom:
    zx, zy, zw, zh = zoom
    rows_h += gap + lab + zh * scale
    W = max(W, len(panels) * zw * scale + (len(panels) - 1) * gap)
if note:
    rows_h += 16
img = Image.new('RGB', (W, rows_h), (48, 48, 48))
d = ImageDraw.Draw(img)
d.text((2, 3), title, fill=(255, 255, 255))
y = 22
for k, (name, im) in enumerate(panels):
    x = k * (240 + gap)
    d.text((x + 2, y), name, fill=(255, 230, 120))
    img.paste(im, (x, y + lab))
    if zoom:
        d.rectangle([x + zx - 1, y + lab + zy - 1, x + zx + zw, y + lab + zy + zh], outline=(255, 0, 255))
y += lab + 240
if zoom:
    y += gap
    for k, (name, im) in enumerate(panels):
        x = k * (zw * scale + gap)
        d.text((x + 2, y), 'ZOOM x%d: %s' % (scale, name.split(' (')[0]), fill=(255, 0, 255))
        crop = im.crop((zx, zy, zx + zw, zy + zh)).resize((zw * scale, zh * scale), Image.NEAREST)
        img.paste(crop, (x, y + lab))
    y += lab + zh * scale
if note:
    d.text((2, y + 2), note, fill=(200, 200, 200))
img.save(out)
print(out, img.size)
