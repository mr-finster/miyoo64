#!/usr/bin/env python3
"""bestmatch.py -- which port frame shows the same instant as a real-game (m64ref z64) frame
(T13, after mario-kart-64's bx/env/bestmatch.py). The emulator runs a few frames behind in the intro
and ~20 in Corneria (BUILDLOG round 11), so pair by picture, not by number.

  bestmatch.py <z64 png> <port dir> <from> <to> [every]    -> the best port frame, its score, runner-up

Port frames are the desktop dumps (<port dir>/frame_NNNNNN.ppm, 320x240, gamma applied); the z64
frame gets the N64's video gamma (sqrt per channel, which the emulator skips) and a 2x2 box to
320x240. The HUD rows at the top, the radio box at the lower left and the crosshair middle are
masked. PIL only (no numpy on the Mac). Local reference frames only: never in git or on the card."""
import math, sys
from PIL import Image, ImageChops, ImageDraw, ImageStat
LUT = [int(round(math.sqrt(i / 255.0) * 255)) for i in range(256)]
def ppm(p):
    d = open(p, 'rb').read(); parts = d.split(maxsplit=4); w, h = int(parts[1]), int(parts[2])
    return Image.frombytes('RGB', (w, h), d[len(d) - w * h * 3:])
mask = Image.new('L', (320, 240), 255); dr = ImageDraw.Draw(mask)
dr.rectangle((0, 0, 319, 44), fill=0)        # life bar, score, bombs
dr.rectangle((0, 170, 230, 239), fill=0)     # the radio box
dr.rectangle((130, 90, 190, 150), fill=0)    # the crosshair
ref = Image.open(sys.argv[1]).convert('RGB').resize((320, 240), Image.BOX).point(lambda v: LUT[v])
d, lo, hi = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
step = int(sys.argv[5]) if len(sys.argv) > 5 else 1
scores = []
for n in range(lo, hi + 1, step):
    try:
        t = ppm('%s/frame_%06d.ppm' % (d, n))
    except FileNotFoundError:
        continue
    scores.append((sum(ImageStat.Stat(ImageChops.difference(ref, t), mask).mean) / 3, n))
scores.sort()
if not scores:
    sys.exit('no port frames in %s between %d and %d' % (d, lo, hi))
print('best %d (%.2f levels), runner-up %s' % (scores[0][1], scores[0][0],
      '%d (%.2f)' % (scores[1][1], scores[1][0]) if len(scores) > 1 else '-'))
