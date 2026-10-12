#!/usr/bin/env python3
"""refcloser.py REFDIR BEFORE AFTER [OFFSET] [--max TICK]
Is a fix closer to the real game? For each port frame pair (BEFORE/AFTER dump/frame_NNNNNN.ppm)
with a z64 reference frame fNNNNNN.png at port tick + OFFSET (default 460, the 1P route), take
the pixels the fix changed inside the N64 picture (port rows 30..209, where the reference is
box-filtered from 640x480 to 240x180) and compare the mean distance to the reference before and
after. Prints per frame and the total: changed px, mean |before - ref|, mean |after - ref|,
and how many changed pixels moved closer / further. --max TICK: stop there (the 1P route's
fight drifts from z64 after about port tick 3560: positions and damage differ, round 15)."""
import os, sys
from PIL import Image

argv = sys.argv[1:]
tmax = None
if '--max' in argv:
    k = argv.index('--max'); tmax = int(argv[k + 1]); del argv[k:k + 2]
a = [x for x in argv if not x.startswith('--')]
refdir, bdir, adir = a[0], a[1], a[2]
off = int(a[3]) if len(a) > 3 else 460
def frames(d):
    d = os.path.join(d, 'dump') if os.path.isdir(os.path.join(d, 'dump')) else d
    return d, sorted(int(f[6:12]) for f in os.listdir(d) if f.startswith('frame_') and f.endswith('.ppm'))
bd, bf = frames(bdir); ad, af = frames(adir)
tot = [0, 0.0, 0.0, 0, 0]
for t in bf:
    if t not in af or (tmax is not None and t > tmax): continue
    rp = os.path.join(refdir, 'f%06d.png' % (t + off))
    if not os.path.exists(rp): continue
    b = Image.open(os.path.join(bd, 'frame_%06d.ppm' % t)).convert('RGB')
    f = Image.open(os.path.join(ad, 'frame_%06d.ppm' % t)).convert('RGB')
    r = Image.open(rp).convert('RGB').resize((240, 180), Image.BOX)
    bp, fp, rpx = b.load(), f.load(), r.load()
    n = 0; eb = ea = 0.0; closer = further = 0
    for y in range(30, 210):
        for x in range(240):
            pb, pa = bp[x, y], fp[x, y]
            if pb == pa: continue
            q = rpx[x, y - 30]
            db = sum(abs(pb[i] - q[i]) for i in range(3)) / 3.0
            da = sum(abs(pa[i] - q[i]) for i in range(3)) / 3.0
            n += 1; eb += db; ea += da
            if da < db: closer += 1
            elif da > db: further += 1
    if n:
        print('tick %5d ref %5d: %5d px changed, |before-ref| %5.1f, |after-ref| %5.1f, closer %d, further %d' % (t, t + off, n, eb / n, ea / n, closer, further))
        tot[0] += n; tot[1] += eb; tot[2] += ea; tot[3] += closer; tot[4] += further
if tot[0]:
    print('TOTAL %d px changed: |before-ref| %.2f, |after-ref| %.2f; closer %d, further %d' % (tot[0], tot[1] / tot[0], tot[2] / tot[0], tot[3], tot[4]))
else:
    print('TOTAL: no changed pixel with a reference frame')
