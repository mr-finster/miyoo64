#!/usr/bin/env python3
"""Static half of the GoldenEye census (tools/census.sh is the runtime half): every combiner, render mode,
TLUT and CI use written in SM64's sources and data (levels/, actors/, bin/, src/), so
rooms and areas the runtime sweep never visits are covered too.
usage: python3 tools/census_static.py  (from the repo root)"""
import os, re, sys, collections

PORT = 'port'
gbi = open(os.path.join(PORT, 'include/PR/gbi.h')).read()

# G_CC_* = "a, b, c, d, Aa, Ab, Ac, Ad"
cc = {}
for m in re.finditer(r'#define\s+(G_CC_\w+)\s+([A-Z0-9_]+\s*,\s*[A-Z0-9_]+\s*,\s*[A-Z0-9_]+\s*,\s*[A-Z0-9_]+\s*,\s*[A-Z0-9_]+\s*,\s*[A-Z0-9_]+\s*,\s*[A-Z0-9_]+\s*,\s*[A-Z0-9_]+)', gbi):
    cc[m[1]] = [x.strip() for x in m[2].split(',')]

for m in re.finditer(r'#define\s+(G_CC_\w+)\s+(G_CC_\w+)\s*$', gbi, re.M):
    if m[2] in cc: cc[m[1]] = cc[m[2]]
RGB_C_ODD = {'COMBINED_ALPHA', 'TEXEL1_ALPHA', 'PRIMITIVE_ALPHA', 'SHADE_ALPHA', 'ENV_ALPHA', 'PRIM_LOD_FRAC', 'K5', 'SCALE'}

def odd_c1(m):
    a, b, c, d, Aa, Ab, Ac, Ad = m
    why = []
    if a != b and c in RGB_C_ODD: why.append('rgb C=' + c)
    if c != '0' and (b in ('CENTER', 'K4') or a == 'NOISE'): why.append('rgb A/B=' + a + '/' + b)
    if Aa != Ab and Ac in ('LOD_FRACTION', 'PRIM_LOD_FRAC'): why.append('alpha C=' + Ac)
    return why

def pass_c2(m):
    a, b, c, d, Aa, Ab, Ac, Ad = m
    return d == 'COMBINED' and (c == '0' or a == b) and Ad == 'COMBINED' and (Ac == '0' or Aa == Ab)

files = []
for root in ('levels', 'actors', 'bin', 'src'):
    for dp, dn, fn in os.walk(os.path.join(PORT, root)):
        if '/pc/' in dp + '/' or dp.endswith('/pc'): continue
        for f in fn:
            if f.endswith(('.c', '.h', '.inc.c')): files.append(os.path.join(dp, f))

combos = collections.defaultdict(set)
lerps = collections.defaultdict(set)
rmodes = collections.defaultdict(set)
tluts = collections.defaultdict(set)
for p in files:
    try: s = open(p, errors='replace').read()
    except OSError: continue
    for m in re.finditer(r'g[s]?DPSetCombineMode\(\s*(?:[^,()]+,\s*)?(G_CC_\w+)\s*,\s*(G_CC_\w+)\s*\)', s):
        combos[(m[1], m[2])].add(p)
    for m in re.finditer(r'g[s]?DPSetCombineLERP\(([^;]*?)\)\s*[,;]', s, re.S):
        args = [x.strip() for x in m[1].split(',')]
        if args and not re.match(r'^[A-Z0-9_]+$', args[0]): args = args[1:]  # gDP form: the packet
        if len(args) == 16: lerps[tuple(args)].add(p)
    for m in re.finditer(r'g[s]?DPSetRenderMode\(\s*(?:[^,()]+,\s*)?(G_RM_\w+)\s*,\s*(G_RM_\w+)\s*\)', s):
        rmodes[(m[1], m[2])].add(p)
    for m in re.finditer(r'(G_TT_\w+|G_IM_FMT_CI|gsDPLoadTLUT\w*|gDPLoadTLUT\w*|G_LOADTLUT)', s):
        tluts[m[1]].add(p)

print(f'{len(files)} files; {len(combos)} SetCombineMode pairs, {len(lerps)} SetCombineLERPs, {len(rmodes)} render-mode pairs')
print('\n== combiners the cycle-1 model drops, or 2-cycle with a non-pass cycle 2 (2-cycle use not known statically):')
for (c1, c2), ps in sorted(combos.items()):
    if c1 not in cc or c2 not in cc: print('  ?? unknown', c1, c2); continue
    why = odd_c1(cc[c1])
    if not pass_c2(cc[c2]) and c1 != c2: why.append('cycle 2 ' + c2 + ' not a pass (' + ' '.join(cc[c2]) + ')')
    if why: print(f'  {c1}, {c2}: {"; ".join(why)} -- {len(ps)} files: ' + ' '.join(sorted(os.path.relpath(x, PORT) for x in ps))[:200])
for args, ps in sorted(lerps.items()):
    why = odd_c1(args[:8])
    if not pass_c2(args[8:]) and args[:8] != args[8:]: why.append('cycle 2 not a pass')
    if why: print(f'  LERP {" ".join(args)}: {"; ".join(why)} -- ' + ' '.join(sorted(os.path.relpath(x, PORT) for x in ps))[:200])

# render modes: blender B = 1-A in the LAST cycle without FORCE_BL (and no CVG_X_ALPHA)
rm = {}
for m in re.finditer(r'#define\s+(RM_\w+)\(clk\)\s*\\?\s*((?:.*\\\n)*.*)', gbi):
    rm[m[1]] = m[2].replace('\\\n', ' ')
def rm_bits(name):
    m = re.match(r'G_RM_(\w+?)(2?)$', name)
    if not m: return None
    base = 'RM_' + m[1]
    return rm.get(base), bool(m[2])
print('\n== render-mode pairs: blender B = 1-A (1-cycle: the first mode; 2-cycle: the second) without FORCE_BL / CVG_X_ALPHA in either:')
def mux_b(name):
    b = rm_bits(name)
    body = b[0] if b and b[0] else None
    if body is None: return None, ''
    g = re.search(r'GBL_c##clk\(([^)]*)\)', body)
    return (g[1].split(',')[3].strip() if g else '0'), body
n = 0
for (r1, r2), ps in sorted(rmodes.items()):
    b1, body1 = mux_b(r1); b2, body2 = mux_b(r2)
    both = body1 + ' ' + body2
    flag = ('G_BL_1MA' in (b1, b2)) and 'FORCE_BL' not in both and 'CVG_X_ALPHA' not in both
    if flag: n += 1
    print(f'  {"!!" if flag else "  "} {r1:26} B1={b1 or "?":10} {r2:26} B2={b2 or "?":10} FORCE_BL={"FORCE_BL" in both} CVG_X_ALPHA={"CVG_X_ALPHA" in both} ({len(ps)} files)')
print(f'  flagged: {n}')
print('\n== TLUT / CI uses:')
for k, ps in sorted(tluts.items()):
    print(f'  {k}: {len(ps)} files: ' + ' '.join(sorted(os.path.relpath(x, PORT) for x in ps))[:300])
