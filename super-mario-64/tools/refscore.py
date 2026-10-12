#!/usr/bin/env python3
"""Score two variants of our frames against the real game (m64ref z64 frames).

usage: refscore.py <refdir> <dirA> <dirB> [--offset 2] [--quiet]
  refdir: the emulator reference script (m64ref.sh) output, f<N>.png at 320x240
  dirA, dirB: our dumps (SM64_DUMP), frame_<N>.ppm on a square output
For each of our frames N present in A and B whose reference frame N+offset exists (the
emulator's shot shows the frame drawn two swaps earlier, so ours N = its N+2): the pixels
where A and B differ, and the mean |A - ref| and |B - ref| over them (RGB, 0..255). The
reference is cropped to its centre 240 columns (our square view). A lower mean is
closer to the real game. Totals are weighted by pixel count. Exits 1 when nothing was
compared.
"""
import argparse, glob, os, sys
from PIL import Image, ImageChops

def pixels(im):  # Pillow 12.2 deprecates getdata
    return (getattr(im, 'get_flattened_data', None) or im.getdata)()

ap = argparse.ArgumentParser()
ap.add_argument('ref'); ap.add_argument('a'); ap.add_argument('b')
ap.add_argument('--offset', type=int, default=2)
ap.add_argument('--quiet', action='store_true')
o = ap.parse_args()

tot_px = tot_a = tot_b = compared = 0
for fa in sorted(glob.glob(os.path.join(o.a, 'frame_*.ppm'))):
    name = os.path.basename(fa)
    n = int(name[6:12])
    fb = os.path.join(o.b, name)
    fr = os.path.join(o.ref, 'f%06d.png' % (n + o.offset))
    if not (os.path.exists(fb) and os.path.exists(fr)):
        continue
    compared += 1
    ia, ib = Image.open(fa).convert('RGB'), Image.open(fb).convert('RGB')
    ir = Image.open(fr).convert('RGB')
    x0 = (ir.width - ia.width) // 2
    ir = ir.crop((x0, 0, x0 + ia.width, ia.height))
    mask = ImageChops.difference(ia, ib).convert('L').point(lambda v: 255 if v else 0)
    px = sum(1 for v in pixels(mask) if v)
    if not px:
        if not o.quiet: print('%s  0 px differ' % name)
        continue
    sa = sb = 0
    for pa, pb, pr, m in zip(pixels(ia), pixels(ib), pixels(ir), pixels(mask)):
        if m:
            sa += abs(pa[0] - pr[0]) + abs(pa[1] - pr[1]) + abs(pa[2] - pr[2])
            sb += abs(pb[0] - pr[0]) + abs(pb[1] - pr[1]) + abs(pb[2] - pr[2])
    tot_px += px; tot_a += sa; tot_b += sb
    if not o.quiet:
        print('%s  %6d px differ   A %6.2f   B %6.2f' % (name, px, sa / 3.0 / px, sb / 3.0 / px))
if not compared:
    print('ERROR: no frame compared (check the dirs and --offset)')
    sys.exit(1)
if tot_px:
    print('total: %d frames, %d px differ   A %.2f   B %.2f' % (compared, tot_px, tot_a / 3.0 / tot_px, tot_b / 3.0 / tot_px))
else:
    print('total: %d frames, A and B identical' % compared)
