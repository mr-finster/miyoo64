#!/usr/bin/env python3
"""Summarise an on-device prof.txt (src/port/hh/hh_prof.c) per function.

usage (in the build container): prof_report.py prof.txt build/hh-miyoo/oot [sysroot] [--top N]

PCs inside the game binary (non-PIE, linked at 0x40000000) are matched against
`nm -n` of the unstripped binary; PCs in shared objects (musl, SDL, ...) are
mapped through the recorded /proc/self/maps to the library's own symbols
(`nm -D -n` of the file under the SDK sysroot) or reported as lib+offset.
The lr table attributes time spent in leaf library code to the game caller.
"""
import bisect
import os
import subprocess
import sys

NM = os.environ.get("NM", "/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-nm")


def symbols(path, dynamic=False):
    args = [NM, "-n", "--defined-only"] + (["-D"] if dynamic else []) + [path]
    try:
        out = subprocess.run(args, capture_output=True, text=True).stdout
    except OSError:
        return [], []
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def lookup(table, addr):
    addrs, names = table
    i = bisect.bisect_right(addrs, addr) - 1
    return names[i] if i >= 0 else "?"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
    prof, binary = args[0], args[1]
    sysroot = args[2] if len(args) > 2 else "/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot"
    game = symbols(binary)
    pcs, lrs, maps, header = [], [], [], ""
    for line in open(prof, errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "samples":
            header = line.strip()
        elif p[0] in ("pc", "lr"):
            (pcs if p[0] == "pc" else lrs).append((int(p[1], 16), int(p[2])))
        elif p[0] == "map" and len(p) >= 7 and "x" in p[2]:
            lo, hi = (int(x, 16) for x in p[1].split("-"))
            maps.append((lo, hi, int(p[3], 16), p[6]))
    libsyms = {}

    def name_of(addr):
        if game[0] and game[0][0] <= addr <= game[0][-1] + 0x10000:
            return lookup(game, addr)
        for lo, hi, off, path in maps:
            if lo <= addr < hi:
                base = os.path.basename(path)
                if path not in libsyms:
                    cand = os.path.join(sysroot, path.lstrip("/"))
                    libsyms[path] = symbols(cand, dynamic=True) if os.path.exists(cand) else ([], [])
                rel = addr - lo + off
                sym = lookup(libsyms[path], rel) if libsyms[path][0] else "?"
                return "%s:%s" % (base, sym if sym != "?" else hex(rel))
        return "?%08x" % addr

    def report(title, rows):
        total = sum(c for _, c in rows) or 1
        agg = {}
        for addr, c in rows:
            n = name_of(addr)
            agg[n] = agg.get(n, 0) + c
        print("== %s (%d samples)" % (title, total))
        for n, c in sorted(agg.items(), key=lambda kv: -kv[1])[:top]:
            print("%6.2f%%  %6d  %s" % (100.0 * c / total, c, n))
        print()

    print(header)
    report("self time by function", pcs)
    report("callers (lr) -- who calls the hot leaf code", lrs)
    # --lines FUNC: that function's samples by source line (needs -g).
    if "--lines" in sys.argv:
        func = sys.argv[sys.argv.index("--lines") + 1]
        rows = [(a, c) for a, c in pcs if name_of(a) == func]
        total = sum(c for _, c in rows) or 1
        a2l = os.environ.get("ADDR2LINE", NM.replace("-nm", "-addr2line").replace("/nm", "/addr2line"))
        out = subprocess.run([a2l, "-e", binary] + ["%x" % a for a, _ in rows], capture_output=True, text=True).stdout.split("\n")
        agg = {}
        for (a, c), where in zip(rows, out):
            where = where.split(" (")[0].rsplit("/", 1)[-1]
            agg[where] = agg.get(where, 0) + c
        print("== %s by line (%d samples)" % (func, total))
        for w, c in sorted(agg.items(), key=lambda kv: -kv[1])[:top]:
            print("%6.2f%%  %6d  %s" % (100.0 * c / total, c, w))


main()
