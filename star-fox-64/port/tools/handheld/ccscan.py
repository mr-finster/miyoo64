#!/usr/bin/env python3
"""Every G_SETCOMBINE in Star Fox's F3DEX display lists, read from the local
uncompressed ROM (the decomp's build/starfox64.us.rev1.uncompressed.z64, big-endian):

    python3 tools/handheld/ccscan.py build/starfox64.us.rev1.uncompressed.z64

A 0xFC word counts when both neighbouring 8-byte words are commands of a tightly
checked shape (syncs, ENDDL, prim/env, othermode, geometry, DL/SETTIMG/VTX with a
plausible address, another SETCOMBINE), which leaves out texture bytes that only look
like one. Prints each distinct form, with the rgb multiplier (C) flagged where the
port's cc_id folds it to 0 (the alpha inputs) and "+TEX" for a texel in A, B or D:
the (ENV - TEXEL0) * ENV_ALPHA + TEXEL0 menu highlight diddy-kong-racing ac7de18 fixed.
Round 7: 89 commands, 17 forms, all in the setup table (fox_rcp_setup.c). Prints mux
names and ROM offsets only; the runtime side is gfx_pc.c's SF64_CC_CENSUS."""
import sys, collections
d = open(sys.argv[1], 'rb').read()
def w(o): return int.from_bytes(d[o:o+4], 'big'), int.from_bytes(d[o+4:o+8], 'big')
def addr_ok(a): return (a >> 24) in range(0, 16) and (a & 0xFFFFFF) < 0x800000 or (a >> 24) == 0x80
def cmd_ok(o, depth=0):
    if o < 0 or o + 8 > len(d): return False
    w0, w1 = w(o); op = w0 >> 24
    if (w0, w1) in ((0xE7000000, 0), (0xE6000000, 0), (0xE8000000, 0), (0xE9000000, 0), (0xB8000000, 0)): return True
    if op in (0xFA,): return (w0 & 0x00FFFF00) == 0 and (w0 & 0xFF) <= 0xFF
    if op in (0xFB, 0xF9, 0xF8, 0xF7): return (w0 & 0x00FFFFFF) == 0
    if op in (0xBA, 0xB9): return ((w0 >> 16) & 0xFF) == 0 and ((w0 >> 8) & 0xFF) < 32 and 1 <= (w0 & 0xFF) <= 32
    if op in (0xB7, 0xB6): return (w0 & 0x00FFFFFF) == 0
    if op == 0x06: return (w0 & 0x00FFFFFF) in (0, 0x010000) and addr_ok(w1) and (w1 & 7) == 0
    if op == 0xFD: return (w0 & 0x0000F000) == 0 and addr_ok(w1) and (w1 & 7) == 0
    if op == 0x04: return addr_ok(w1) and (w1 & 7) == 0
    if op == 0xBB: return ((w0 >> 16) & 0xFF) == 0
    if op == 0xFC and depth == 0: return cmd_ok(o - 8, 1) or cmd_ok(o + 8, 1)
    return False
FOLD = {6:'SCALE',7:'COMBINED_ALPHA',9:'TEXEL1_ALPHA',10:'PRIM_ALPHA',11:'SHADE_ALPHA',12:'ENV_ALPHA',14:'PRIM_LOD_FRAC',15:'K5'}
NA = {0:'COMB',1:'T0',2:'T1',3:'PRIM',4:'SHADE',5:'ENV',6:'1',7:'NOISE'}
NB = {0:'COMB',1:'T0',2:'T1',3:'PRIM',4:'SHADE',5:'ENV',6:'CENTER',7:'K4'}
NC = {0:'COMB',1:'T0',2:'T1',3:'PRIM',4:'SHADE',5:'ENV',6:'SCALE',7:'COMB_A',8:'T0_A',9:'T1_A',10:'PRIM_A',11:'SHADE_A',12:'ENV_A',13:'LOD',14:'PRIM_LOD',15:'K5'}
ND = {0:'COMB',1:'T0',2:'T1',3:'PRIM',4:'SHADE',5:'ENV',6:'1',7:'0'}
def form(a, b, c, dd): return '(%s - %s) * %s + %s' % (NA.get(a, '0'), NB.get(b, '0'), NC.get(c, '0'), ND.get(dd, '0'))
forms = collections.Counter(); where = collections.defaultdict(list); n = 0
for o in range(8, len(d) - 16, 8):
    if d[o] != 0xFC or not cmd_ok(o - 8, 1) or not cmd_ok(o + 8, 1): continue
    w0, w1 = w(o)
    c1 = ((w0 >> 20) & 15, (w1 >> 28) & 15, (w0 >> 15) & 31, (w1 >> 15) & 7)
    c2 = ((w0 >> 5) & 15, (w1 >> 24) & 15, w0 & 31, (w1 >> 6) & 7)
    n += 1; forms[(c1, c2)] += 1
    if len(where[(c1, c2)]) < 3: where[(c1, c2)].append(o)
print('setcombine commands: %d, distinct forms: %d' % (n, len(forms)))
flagged = 0
for (c1, c2), k in sorted(forms.items(), key=lambda x: -x[1]):
    flags = []
    for cyc, m in (('c1', c1), ('c2', c2)):
        if m[2] in FOLD:
            tex = m[0] in (1, 2) or m[1] in (1, 2) or m[3] in (1, 2)
            flags.append('%s:C=%s%s' % (cyc, FOLD[m[2]], '+TEX' if tex else ''))
    if flags: flagged += 1
    print('%5d  %-36s | %-36s %s  @%s' % (k, form(*c1), form(*c2), ' '.join(flags), ','.join('%X' % x for x in where[(c1, c2)])))
print('forms with a folded rgb multiplier: %d' % flagged)
