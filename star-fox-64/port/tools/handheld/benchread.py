#!/usr/bin/env python3
"""benchread.py <console.log>: one row per BENCH window: work, r3d (PERF avg), GFX3D tris/px, r3d per picture, replay"""
import re, sys, collections
L = open(sys.argv[1], errors='replace').read().splitlines()
perf = gfx = noz = None; rows = []
for l in L:
    if l.startswith('PERF '): perf = l; gfx = noz = None
    elif l.startswith('GFX3D'): gfx = l
    elif l.startswith('NOZDEFER'): noz = l
    elif l.startswith('BENCH ') and perf:
        w = float(re.search(r'PERF work ([\d.]+)', perf).group(1))
        rend = re.search(r'rendered (\d+)/(\d+)', perf).groups()
        r3 = float(re.search(r'r3d ([\d.]+)', perf).group(1))
        it = float(re.search(r'interp ([\d.]+)', perf).group(1))
        kpx = float(re.search(r'GFX3D ([\d.]+)k', gfx).group(1)) if gfx else 0
        tr = float(re.search(r'\| ([\d.]+) tris', gfx).group(1)) if gfx else 0
        r3p = float(re.search(r'r3d ([\d.]+) ms', gfx).group(1)) if gfx else 0
        rp = re.search(r'replay ([\d.]+) ms', noz) if noz else None
        rows.append((l[6:], w, r3, r3p, it, kpx, tr, float(rp.group(1)) if rp else 0, '/'.join(rend)))
print(f"{'window':44s} {'work':>6s} {'r3d':>6s} {'r3d/pic':>7s} {'interp':>6s} {'kpx':>6s} {'tris':>6s} {'replay':>6s} rendered")
for r in rows: print(f"{r[0][:44]:44s} {r[1]:6.2f} {r[2]:6.2f} {r[3]:7.2f} {r[4]:6.2f} {r[5]:6.0f} {r[6]:6.0f} {r[7]:6.2f} {r[8]}")
agg = collections.defaultdict(list)
for r in rows:
    m = re.match(r'(steady|bomb) (\S+)', r[0])
    if m: agg[(m.group(1), m.group(2))].append(r)
print("\nmeans (work, r3d, r3d/pic, replay) and spread of work")
for k in sorted(agg):
    v = agg[k]; n = len(v)
    mw = sum(x[1] for x in v)/n
    print(f"{k[0]:6s} {k[1]:15s} n={n} work {mw:6.2f} (min {min(x[1] for x in v):.2f} max {max(x[1] for x in v):.2f}) r3d {sum(x[2] for x in v)/n:6.2f} r3d/pic {sum(x[3] for x in v)/n:6.2f} replay {sum(x[7] for x in v)/n:5.2f} kpx {sum(x[5] for x in v)/n:5.0f} tris {sum(x[6] for x in v)/n:5.0f}")
