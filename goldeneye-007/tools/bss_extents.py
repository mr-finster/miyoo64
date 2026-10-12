#!/usr/bin/env python3
"""Find game variables declared smaller than the memory the N64 gives them.

The decomp splits some .bss/.data regions into small "label" declarations that only work
because the N64 linker lays the next variables out right after them (e.g.
`char g_ModelHitEntries[0xC]` is really a ModelHitEntry[600] pool). A native build
places variables anywhere, so code that indexes past such a label overwrites unrelated
memory. This compares, for every symbol of the game's objects, the N64 extent (distance
to the next symbol in the M1 link map) with the size the port's build gave it (nm -S).

Usage: tools/bss_extents.py <ge007.u.map> <port ELF nm -S output> [min_excess]
"""
import re, sys

def n64_symbols(mapfile):
    syms = []  # (addr, name, section, object)
    cur_sec = cur_obj = None
    for line in open(mapfile):
        m = re.match(r"^ (\.\w+)\s+0x([0-9a-f]{8})\s+0x([0-9a-f]+) (\S+)$", line)
        if m:
            cur_sec, cur_obj = m.group(1), m.group(4)
            continue
        m = re.match(r"^\s+0x([0-9a-f]{8})\s+([A-Za-z_][A-Za-z0-9_]*)$", line)
        if m and cur_sec in (".bss", ".data", ".rodata", "COMMON") and cur_obj and "src/" in cur_obj:
            syms.append((int(m.group(1), 16), m.group(2), cur_sec, cur_obj))
    return sorted(set(syms))

def main():
    mp, nmfile = sys.argv[1], sys.argv[2]
    minx = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    ours = {}
    for line in open(nmfile):
        p = line.split()
        if len(p) == 4 and p[2].lower() in "bdgs":
            ours[p[3]] = int(p[1], 16)
    syms = n64_symbols(mp)
    rows = []
    for i, (addr, name, sec, obj) in enumerate(syms):
        if i + 1 >= len(syms):
            break
        ext = syms[i + 1][0] - addr
        if name not in ours or ext <= 0 or ext > 0x100000:
            continue
        if ext - ours[name] >= minx:
            rows.append((ext - ours[name], name, ours[name], ext, obj, sec))
    for d, name, sz, ext, obj, sec in sorted(rows, reverse=True):
        print(f"{name:40s} ours {sz:#8x}  n64 extent {ext:#8x}  (+{d:#x})  {obj} {sec}")
    print(f"{len(rows)} symbols smaller than their N64 extent by >= {minx} bytes", file=sys.stderr)

if __name__ == "__main__":
    main()
