#!/usr/bin/env python3
"""Compare two frame sets frame by frame: differing pixel counts.
  cmpframes.py <A> <B>
A and B are tools/sweep.sh outputs (<dir>/st*/dump/*.ppm) or any folders of frames: every
.ppm/.png under A is paired with the same relative path under B. It fails (exit 2) when no
frame was compared: it used to glob only <dir>/*/dump/*.ppm, so pointing it at a dump
folder compared nothing and printed "differing frames: 0" (round 13)."""
import sys, os
from PIL import Image, ImageChops
a, b = sys.argv[1], sys.argv[2]
bad = compared = found = 0
for dp, _, fns in sorted(os.walk(a)):
    for f in sorted(fns):
        if not f.lower().endswith(('.ppm', '.png')):
            continue
        fa = os.path.join(dp, f)
        found += 1
        rel = os.path.relpath(fa, a)
        fb = os.path.join(b, rel)
        if not os.path.exists(fb):
            print('missing', rel); bad += 1; continue
        d = ImageChops.difference(Image.open(fa).convert('RGB'), Image.open(fb).convert('RGB'))
        raw = d.tobytes()
        n = sum(1 for i in range(0, len(raw), 3) if raw[i] | raw[i + 1] | raw[i + 2])
        compared += 1
        if n:
            bad += 1
            print(rel, n, 'px differ, bbox', d.getbbox())
print('compared %d, differing frames: %d' % (compared, bad))
if compared == 0:
    print('NOTHING COMPARED: %d frames under %s, none of them under %s' % (found, a, b) if found
          else 'NOTHING COMPARED: no .ppm/.png frames under %s' % a)
    sys.exit(2)
sys.exit(1 if bad else 0)
