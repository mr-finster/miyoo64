#!/usr/bin/env python3
"""M1: recover the offsets of data symbols whose defining C files we never fetched.

Link the decomp once with every unknown symbol parked at offset 0 of its segment (see
tools/m1_segments.py), with the linker keeping relocations (-q). All segment sizes already
match the ROM, so the layout is the retail one and the only code bytes that can differ are
the instructions that address the parked symbols. For each relocation against such a
symbol, decode the address the instruction(s) form in OUR image and in the retail ROM at the
same place: retail - ours = the symbol's true offset (addends cancel). Every reference must
agree. Writes tools/m1_symbols.json (numbers only).

Usage: tools/m1_recover_syms.py <decomp_dir> sym1 sym2 ...
"""
import json, os, re, subprocess, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = "mips-linux-gnu-"

def run(*a): return subprocess.run(a, capture_output=True, text=True, check=True).stdout

def main() -> int:
    root, wanted = sys.argv[1], set(sys.argv[2:])
    elf = f"{root}/build/u/ge007.u.elf"
    rom = open(f"{root}/baserom.u.z64", "rb").read()
    ours = open(f"{root}/build/u/ge007.u.bin", "rb").read()
    # section VMA/LMA/size for VMA -> image offset (objcopy -O binary lays out by LMA)
    secs = []
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+2\*\*\d+\n\s+CONTENTS[^\n]*LOAD",
                         run(TOOL + "objdump", "-h", elf), re.M):
        name, size, vma, lma = m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
        if size: secs.append((name, vma, lma, size))
    lma0 = min(s[2] for s in secs)
    def img_off(vma, secname):
        for name, v, l, sz in secs:
            if name == secname and v <= vma < v + sz:
                return l - lma0 + (vma - v)
        return None
    # relocations: readelf -rW prints "Relocation section '.rel.X'" blocks
    relocs = []
    cur = None
    for line in run(TOOL + "readelf", "-rW", elf).splitlines():
        m = re.match(r"Relocation section '\.rel(\S+)'", line)
        if m: cur = m.group(1); continue
        f = line.split()
        if cur and len(f) >= 5 and re.fullmatch(r"[0-9a-f]{8}", f[0]):
            relocs.append((cur, int(f[0], 16), f[2], f[4]))
    def word(buf, off): return int.from_bytes(buf[off:off + 4], "big")
    def sext16(x): return x - 0x10000 if x & 0x8000 else x
    deltas = defaultdict(list)
    pending = {}
    for sec, vma, typ, sym in relocs:
        if sym not in wanted: continue
        off = img_off(vma, sec)
        if off is None or off + 4 > len(rom): continue
        if typ == "R_MIPS_32":
            deltas[sym].append((word(rom, off) - word(ours, off)) & 0xFFFFFFFF)
        elif typ == "R_MIPS_HI16":
            pending[(sec, sym)] = off
        elif typ == "R_MIPS_LO16" and (sec, sym) in pending:
            hi = pending.pop((sec, sym))
            val = lambda b: ((word(b, hi) & 0xFFFF) << 16) + sext16(word(b, off) & 0xFFFF)
            deltas[sym].append((val(rom) - val(ours)) & 0xFFFFFFFF)
    result, bad = {}, []
    for sym in sorted(wanted):
        d = set(deltas.get(sym, []))
        if len(d) == 1:
            result[sym] = d.pop()
        else:
            bad.append((sym, sorted(d), len(deltas.get(sym, []))))
    path = f"{HERE}/m1_symbols.json"
    prev = json.load(open(path))["offsets"] if os.path.exists(path) else {}
    prev.update(result)
    json.dump({"what": "offsets within their segment of data symbols the game code uses, recovered from the NTSC-U ROM's code (tools/m1_recover_syms.py)",
               "offsets": dict(sorted(prev.items()))}, open(path, "w"), indent=1)
    for s, v in sorted(result.items()): print(f"{s:32s} 0x{v:X}")
    for s, d, n in bad: print(f"UNRESOLVED {s}: {n} refs, deltas {[hex(x) for x in d]}")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
