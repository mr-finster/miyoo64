#!/usr/bin/env python3
"""Frame-by-frame diff of two dump dirs (same frame numbers).
usage: framediff.py <dirA> <dirB> [out_dir]
Prints changed-pixel counts per frame; with out_dir, writes <n>_diff.png for each
changed frame: A | B | changed pixels in red over a dimmed B (native size, 1:1).
Fails (exit 1) when nothing was compared at all -- an empty match must never read as
"no differences" (super-smash-bros's T11) -- or when B lacks a frame A has (B's run ended
early). Frames only in B are listed."""
import sys, os, glob
from PIL import Image, ImageChops
a, b = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else None
same = compared = missing = 0
fas = sorted(glob.glob(os.path.join(a, 'frame_*.ppm')))
only_b = [os.path.basename(f) for f in sorted(glob.glob(os.path.join(b, 'frame_*.ppm')))
          if not os.path.exists(os.path.join(a, os.path.basename(f)))]
if only_b: print('%d frame(s) only in B (%s .. %s)' % (len(only_b), only_b[0], only_b[-1]))
for fa in fas:
    n = os.path.basename(fa)
    fb = os.path.join(b, n)
    if not os.path.exists(fb):
        print(n, 'missing in B'); missing += 1; continue
    compared += 1
    ia, ib = Image.open(fa).convert('RGB'), Image.open(fb).convert('RGB')
    d = ImageChops.difference(ia, ib).convert('L')
    px = sum(1 for v in d.getdata() if v)
    if not px:
        same += 1; continue
    print(n, 'changed px', px, 'max', d.getextrema()[1])
    if out:
        os.makedirs(out, exist_ok=True)
        w, h = ia.size
        m = Image.eval(ib, lambda v: v // 3)
        red = Image.new('RGB', ia.size, (255, 0, 0))
        m.paste(red, mask=d.point(lambda v: 255 if v else 0))
        s = Image.new('RGB', (w * 3 + 8, h), (24, 24, 24))
        s.paste(ia, (0, 0)); s.paste(ib, (w + 4, 0)); s.paste(m, (2 * w + 8, 0))
        s.save(os.path.join(out, n.replace('.ppm', '_diff.png')))
if compared == 0:
    print('ERROR: no frames compared (%s: %d frames, %s: %d)' % (a, len(fas), b, len(glob.glob(os.path.join(b, 'frame_*.ppm')))))
    sys.exit(1)
print('identical frames:', same)
if missing:
    print('ERROR: %d frame(s) of A missing in B' % missing)
    sys.exit(1)
