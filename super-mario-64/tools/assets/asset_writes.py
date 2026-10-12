#!/usr/bin/env python3
"""Name the asset-region bytes a run wrote (SM64_ASSET_DIFF=<file> dumps).

usage: asset_writes.py <elf> <diff files...>
Prints, per symbol, how many runs wrote it and how many bytes changed. The ELF
is the build that ran (its symbol table places the region's symbols). Runs in
the build container (uses nm)."""
import bisect, subprocess, sys
from collections import defaultdict

elf, diffs = sys.argv[1], sys.argv[2:]
syms = []
for line in subprocess.run(['nm', '-S', '--defined-only', elf], capture_output=True, text=True, check=True).stdout.splitlines():
    f = line.split()
    if len(f) == 4:
        syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    elif len(f) == 3 and f[2] in ('__assets_start', '__assets_end'):
        syms.append((int(f[0], 16), 0, f[2]))
start = next(a for a, _, n in syms if n == '__assets_start')
end = next(a for a, _, n in syms if n == '__assets_end')
syms = sorted((a, s, n) for a, s, n in syms if start <= a < end and s > 0)
addrs = [a for a, _, _ in syms]

def owner(addr):
    i = bisect.bisect_right(addrs, addr) - 1
    if i >= 0 and syms[i][0] <= addr < syms[i][0] + syms[i][1]:
        return syms[i]
    return (addr, 0, '?%08x' % addr)

runs = defaultdict(set)
nbytes = defaultdict(int)
for path in diffs:
    with open(path) as fh:
        hdr = fh.readline().split()
        base = int(hdr[1], 16)
        if base != start:
            sys.exit(f'{path}: region at {base:#x}, this ELF has it at {start:#x}: another build')
        for line in fh:
            off, n = map(int, line.split())
            for b in range(off, off + n):
                s = owner(base + b)
                runs[s].add(path)
                nbytes[s] += 1
for s in sorted(runs, key=lambda s: s[0]):
    a, size, name = s
    print(f'{a - start:#09x} {size:7d} {name:48s} {len(runs[s]):3d} runs {nbytes[s]:7d} bytes')
