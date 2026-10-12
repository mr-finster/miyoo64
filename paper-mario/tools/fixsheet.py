#!/usr/bin/env python3
# fixsheet.py: the sheet the user reviews for every fix that changes the screen (their rule, 24 Sep 2026):
#   BEFORE | AFTER | REAL GAME (m64ref; --ref-mode names the mode) at native square, 1:1, then the same three zoomed
#   on the changed area.
#
#   fixsheet.py --before B.ppm --after A.ppm [--ref R.png --ref-mode angrylion --ref-note "poll 3912"] --zoom x0,y0,x1,y1[,z]
#               --title "what changed" --count "N of 240 sweep frames change" --out sheet.png
#
# The reference (m64ref: the 320x240 framebuffer scaled to 4:3, 640x480 by default) is brought to our output
# geometry: 320x240, then squeezed 4:3 to a square (box filters), as the bench presents. The zoom is nearest
# neighbour, labelled with its box and factor. Fails loudly (exit 1) on missing inputs or wrong sizes.
# Sheets hold ROM-derived pictures: keep them local (refs/), never in git or on the card.
import argparse, os, sys
from PIL import Image, ImageDraw, ImageFont

def die(msg):
    print('fixsheet.py: ' + msg, file=sys.stderr)
    sys.exit(1)

ap = argparse.ArgumentParser()
ap.add_argument('--before', required=True); ap.add_argument('--after', required=True)
ap.add_argument('--ref'); ap.add_argument('--ref-note', default='')
ap.add_argument('--ref-mode', default='z64')  # the m64ref mode the reference came from (ang = angrylion, the default since 30 Sep)
ap.add_argument('--zoom', required=True); ap.add_argument('--title', required=True)
ap.add_argument('--count', required=True); ap.add_argument('--out', required=True)
a = ap.parse_args()
for f in [a.before, a.after] + ([a.ref] if a.ref else []):
    if not os.path.isfile(f): die(f'no such file: {f}')
before = Image.open(a.before).convert('RGB'); after = Image.open(a.after).convert('RGB')
for im, n in ((before, a.before), (after, a.after)):
    if im.size != (240, 240): die(f'{n} is {im.size}, not 240 square')
panels = [('BEFORE', before), ('AFTER', after)]
if a.ref:
    ref = Image.open(a.ref).convert('RGB')
    if ref.size[0] * 3 != ref.size[1] * 4: die(f'reference {a.ref} is {ref.size}, not 4:3')
    panels.append((f'REAL GAME ({a.ref_mode}' + (f', {a.ref_note}' if a.ref_note else '') + ')',
                   ref.resize((320, 240), Image.BOX).resize((240, 240), Image.BOX)))
z = [int(v) for v in a.zoom.split(',')]
if len(z) == 4: z.append(2)
x0, y0, x1, y1, zf = z
if not (0 <= x0 < x1 <= 240 and 0 <= y0 < y1 <= 240 and zf >= 1): die(f'bad zoom box {a.zoom}')

def font(size, bold=False):
    for p in ('/usr/share/fonts/truetype/dejavu/DejaVuSans%s.ttf' % ('-Bold' if bold else ''),):
        if os.path.isfile(p): return ImageFont.truetype(p, size)
    return ImageFont.load_default()
F, FB = font(13), font(15, True)
FS = font(11)
gap, pad, lab = 10, 12, 24
zw, zh = (x1 - x0) * zf, (y1 - y0) * zf
colw = max(240, zw)
n = len(panels)
W = pad * 2 + n * colw + (n - 1) * gap
# title lines wrapped to the sheet width
draw0 = ImageDraw.Draw(Image.new('RGB', (1, 1)))
def wrap(text, f, width):
    out, line = [], ''
    for w in text.split():
        t = (line + ' ' + w).strip()
        if draw0.textlength(t, font=f) <= width: line = t
        else: out.append(line); line = w
    if line: out.append(line)
    return out
note = f'Top row: native square at 1:1. Bottom row: zoom x{zf} of {x0},{y0}-{x1},{y1} (red marks).'
tl = [(t, FB) for t in wrap(a.title, FB, W - 2 * pad)] + [(t, F) for t in wrap(a.count, F, W - 2 * pad)] + \
     [(t, F) for t in wrap(note, F, W - 2 * pad)]
th = len(tl) * 19 + 8
H = pad + th + lab + 240 + gap + lab + zh + pad
s = Image.new('RGB', (W, H), (255, 255, 255)); d = ImageDraw.Draw(s)
y = pad
for t, f in tl:
    d.text((pad, y), t, fill=(0, 0, 0), font=f); y += 19
y = pad + th
for i, (name, im) in enumerate(panels):
    x = pad + i * (colw + gap)
    d.text((x, y + 2), name, fill=(0, 0, 0), font=F)
    s.paste(im, (x, y + lab))
    # the zoomed area, marked outside the picture (the native pixels stay untouched): red bars along the top
    # and left edges spanning its columns and rows
    d.rectangle((x + x0, y + lab - 4, x + x1 - 1, y + lab - 2), fill=(230, 0, 0))
    d.rectangle((x - 5, y + lab + y0, x - 3, y + lab + y1 - 1), fill=(230, 0, 0))
y2 = y + lab + 240 + gap
for i, (name, im) in enumerate(panels):
    x = pad + i * (colw + gap)
    d.text((x, y2 + 2), f'{name.split(" (")[0]}, zoom x{zf}', fill=(0, 0, 0), font=F)
    s.paste(im.crop((x0, y0, x1, y1)).resize((zw, zh), Image.NEAREST), (x, y2 + lab))
s.save(a.out)
print(f'fixsheet.py: wrote {a.out} ({W}x{H})')
