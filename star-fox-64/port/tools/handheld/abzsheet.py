#!/usr/bin/env python3
"""abzsheet.py -- a before | after | real-game sheet for a visible fix (the owner's rule).

  abzsheet.py OUT.png "title line" [--zoom X0 Y0 X1 Y1 SCALE] --row LABEL BEFORE.ppm AFTER.ppm Z64.png|- [--row ...]

BEFORE/AFTER are the device build's square panel dumps (native, shown 1:1). Z64 is an m64ref frame
(640x480, the console's own microcode): the N64 video gamma the emulator skips is applied (sqrt per
channel, as the port does) and it is squeezed to a square the way the port squeezes its frame, so the
three are comparable (BUILDLOG round 10). --zoom adds, under each row, the same box of all three enlarged SCALE times
(nearest), labelled as a zoom. Local reference frames only: never in git, on the card or shared."""
import math, sys
from PIL import Image, ImageDraw

def ppm(p):
    d = open(p, 'rb').read(); parts = d.split(maxsplit=4); w, h = int(parts[1]), int(parts[2])
    return Image.frombytes('RGB', (w, h), d[len(d) - w * h * 3:])

LUT = [int(round(math.sqrt(i / 255.0) * 255)) for i in range(256)]
def z64(p):
    im = Image.open(p).convert('RGB').point(lambda v: LUT[v])
    return im.resize((240, 240), Image.BOX)

a = sys.argv[1:]
out, title = a.pop(0), a.pop(0)
zoom = None; rows = []
while a:
    k = a.pop(0)
    if k == '--zoom':
        zoom = tuple(int(a.pop(0)) for _ in range(5))
    elif k == '--row':
        rows.append((a.pop(0), a.pop(0), a.pop(0), a.pop(0)))
    else:
        sys.exit('unknown argument ' + k)
G = 6; HEAD = 16; TITLE = 20
cols = ['before', 'after', 'real game (emulator z64; gamma, squeezed to 240)']
zw = zh = 0
if zoom:
    x0, y0, x1, y1, s = zoom; zw, zh = (x1 - x0) * s, (y1 - y0) * s
colw = max(240, zw)
W = 3 * colw + 4 * G
H = TITLE + sum(HEAD + 240 + G + ((HEAD + zh + G) if zoom else 0) for _ in rows) + G
sheet = Image.new('RGB', (W, H), (24, 24, 28)); dr = ImageDraw.Draw(sheet)
dr.text((G, 4), title, fill=(240, 240, 240))
y = TITLE
for label, b, af, z in rows:
    ims = [ppm(b), ppm(af), z64(z) if z != '-' else None]
    for i, im in enumerate(ims):
        x = G + i * (colw + G)
        dr.text((x, y + 2), '%s: %s' % (label, cols[i]) if i == 0 else cols[i], fill=(200, 200, 200))
        if im is not None:
            sheet.paste(im, (x, y + HEAD))
            if zoom:
                dr.rectangle((x + zoom[0] - 1, y + HEAD + zoom[1] - 1, x + zoom[2], y + HEAD + zoom[3]), outline=(255, 220, 0))
        else:
            dr.text((x + 8, y + HEAD + 110), '(not reachable at this instant)', fill=(160, 160, 160))
    y += HEAD + 240 + G
    if zoom:
        for i, im in enumerate(ims):
            x = G + i * (colw + G)
            dr.text((x, y + 2), 'zoom %dx of the yellow box' % zoom[4], fill=(255, 220, 0))
            if im is not None:
                sheet.paste(im.crop(zoom[:4]).resize((zw, zh), Image.NEAREST), (x, y + HEAD))
        y += HEAD + zh + G
sheet.save(out)
print('%s: %dx%d, %d rows' % (out, W, H, len(rows)))
