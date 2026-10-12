#!/usr/bin/env python3
"""tour_cs_pick.py <tourdir> <outdir>: keep the cutscene runs (letterbox bars in a middle frame) of a
tour_cs.txt tour, drop layers whose frames repeat an earlier layer of the same scene, and symlink the
keepers into <outdir> for tour_sheets.py."""
import os, sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
def bars(p):
    im = Image.open(p).convert('L'); px = im.load(); w, h = im.size
    top = sum(px[x, y] for y in range(0, 12) for x in range(0, w, 4)) / (12 * w / 4)
    bot = sum(px[x, y] for y in range(h - 12, h) for x in range(0, w, 4)) / (12 * w / 4)
    return top < 6 and bot < 6
def sig(p):
    return Image.open(p).convert('L').resize((24, 24)).tobytes()
kept = {}
for n in sorted(os.listdir(src)):
    d = os.path.join(src, n)
    if not os.path.isdir(d): continue
    fs = sorted(f for f in os.listdir(d) if f.endswith('.ppm'))
    if len(fs) < 3: continue
    if not any(bars(os.path.join(d, f)) for f in fs[1:4]): continue
    scene = n.rsplit(' ', 1)[0]
    s = tuple(sig(os.path.join(d, f)) for f in fs[1:4])
    if any(sum(abs(a - b) for a, b in zip(s[1], t[1])) < 24 * 24 * 4 for t in kept.get(scene, [])): continue
    kept.setdefault(scene, []).append(s)
    link = os.path.join(dst, n)
    if not os.path.exists(link): os.symlink(os.path.abspath(d), link)
print(sum(len(v) for v in kept.values()), 'cutscene runs kept across', len(kept), 'scenes')
