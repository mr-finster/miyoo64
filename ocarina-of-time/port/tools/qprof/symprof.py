#!/usr/bin/env python3
"""Map insnprof block counts to functions of the game binary.
usage: symprof.py <nm -n output> <prof A> [<prof B>]   -> per-function counts (B - A if two)
Blocks outside the game's text (musl, SDL, the dynamic loader) are summed as <lib>.
TOP=<n> sets how many functions are listed (default 45)."""
import bisect
import os
import sys

syms = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) >= 3 and p[1] in "TtWw":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
text_lo, text_hi = addrs[0], addrs[-1] + 0x10000


def load(path):
    agg, total = {}, 0
    for line in open(path):
        k, v = line.split()
        if k == "total":
            total = int(v)
            continue
        a = int(k, 16)
        if text_lo <= a < text_hi:
            name = syms[bisect.bisect_right(addrs, a) - 1][1]
        else:
            name = "<lib>"
        agg[name] = agg.get(name, 0) + int(v)
    return agg, total


a, ta = load(sys.argv[2])
if len(sys.argv) > 3:
    b, tb = load(sys.argv[3])
    d = {k: b.get(k, 0) - a.get(k, 0) for k in set(a) | set(b)}
    tot = tb - ta
else:
    d, tot = a, ta
print(f"total {tot:,}")
for k, v in sorted(d.items(), key=lambda kv: -kv[1])[: int(os.environ.get("TOP", "45"))]:
    print(f"{v:>16,} {100.0 * v / tot if tot else 0:6.2f}%  {k}")
