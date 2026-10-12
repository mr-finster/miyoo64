#!/usr/bin/env python3
"""check_layout.py - the port places the N64's pure-BSS objects at their N64
offsets (tools/gen_link.py), so every variable in them must have the same size
natively as in the N64 build, or neighbours would overlap. Compares `nm -S`
of each object compiled natively against the decomp's MIPS object.

Usage: check_layout.py <decomp dir> <native nm> <native objdir> <objlist>
"""
import subprocess, sys

def sizes(nm, path):
    out = subprocess.run([nm, "-S", "--defined-only", path], capture_output=True, text=True, check=True).stdout
    r = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "BbCSs":
            r[p[3]] = int(p[1], 16)
    return r

decomp, nm, objdir, lst = sys.argv[1:5]
bad = 0
for o in open(lst).read().split():
    mips = sizes("mips-linux-gnu-nm", "%s/build/us/rev0/%s.o" % (decomp, o))
    nat = sizes(nm, "%s/%s.o" % (objdir, o))
    for sym, sz in sorted(mips.items()):
        if sym in nat and nat[sym] != sz:
            print("layout: %s %s: N64 0x%X bytes, native 0x%X" % (o, sym, sz, nat[sym]))
            bad += 1
print("check_layout: %d mismatches" % bad)
sys.exit(1 if bad else 0)
