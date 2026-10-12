# ab.py <dirA> <dirB> <outprefix> [scene ...]: per scene, the most-changed frame as A | B | changed-pixel mask (native size), with stats
# Fails loudly (exit 1) when there is nothing to compare: no frames in A, a named scene with no frames, a frame
# missing on either side, or frames of different sizes (Smash T11: a compare that finds no frames must not say "0 differ").
import sys, os, glob
from PIL import Image, ImageChops

def die(msg):
    print('ab.py: ' + msg, file=sys.stderr)
    sys.exit(1)

A, B, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
for d in (A, B):
    if not os.path.isdir(d): die(f'not a directory: {d}')
names_a = {os.path.basename(f) for f in glob.glob(A + '/*.ppm')}
names_b = {os.path.basename(f) for f in glob.glob(B + '/*.ppm')}
if not names_a: die(f'no .ppm frames in {A}')
scenes = sys.argv[4:] or sorted({n.rsplit('_', 1)[0] for n in names_a})
total = changed = 0
for sc in scenes:
    fa_all = sorted(n for n in names_a if n.rsplit('_', 1)[0] == sc)
    fb_all = sorted(n for n in names_b if n.rsplit('_', 1)[0] == sc)
    if not fa_all: die(f'scene {sc}: no frames in {A}')
    if fa_all != fb_all: die(f'scene {sc}: {len(fa_all)} frames in {A} but {len(fb_all)} in {B}, or different names')
    best = None; nch = 0
    for n in fa_all:
        a_ = Image.open(f'{A}/{n}').convert('RGB'); b_ = Image.open(f'{B}/{n}').convert('RGB')
        if a_.size != b_.size: die(f'{n}: {a_.size} in {A} but {b_.size} in {B}')
        d = ImageChops.difference(a_, b_).convert('L'); h = d.histogram(); cnt = sum(h[1:])
        if cnt: nch += 1
        if cnt and (best is None or cnt > best[0]): best = (cnt, n, d.getbbox(), max(i for i, v in enumerate(h) if v))
    total += len(fa_all); changed += nch
    if not best: print(f'{sc}: 0 of {len(fa_all)} frames change'); continue
    cnt, n, box, mx = best
    print(f'{sc}: {nch} of {len(fa_all)} frames change; most: {n} {cnt} px, box {box}, max diff {mx}')
    a_ = Image.open(f'{A}/{n}').convert('RGB'); b_ = Image.open(f'{B}/{n}').convert('RGB'); w, h = a_.size
    m = ImageChops.difference(a_, b_).convert('L').point(lambda v: 255 if v else 0).convert('RGB')
    s = Image.new('RGB', (w * 3 + 16, h), (255, 255, 255)); s.paste(a_, (0, 0)); s.paste(b_, (w + 8, 0)); s.paste(m, (2 * w + 16, 0))
    s.save(f'{OUT}_{sc}.png')
print(f'total: {changed} of {total} frames change')
