#!/usr/bin/env python3
"""leak_scan.py: does a shipped file carry the ROM's bytes?

Every file is scanned at every byte offset for 16-byte windows that also occur in the
player's ROM: the whole ROM (16-byte aligned windows) and the unpacked data segment (every
4-byte aligned window, raw and with 2- and 4-byte fields swapped, the way the game's tables
sit in a little-endian executable). Windows of fewer than 6 distinct bytes don't count.
Hits in the executable are attributed to its symbols (--elf: the unstripped copy of it).

Gate: no hit may fall in a table romtables.py fills at start (those must be zero in the
file) and the rest -- the decomp's own source tables and strings, listed -- must stay under
--cap bytes.

usage: leak_scan.py --rom <ROM> --map ge007.u.map --src port --elf <unstripped> [--cap N] files...
"""
import argparse
import bisect
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romtables as rt  # noqa: E402

W = 16


def trivial(w):
    """Padding and low-entropy windows: a handful of small integers matches somewhere in
    12 MB by chance (MK64's rule: fewer than 6 distinct byte values)."""
    return len(set(w)) < 6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--map', required=True)
    ap.add_argument('--src', required=True)
    ap.add_argument('--elf', required=True, help='the unstripped executable, for symbol names')
    ap.add_argument('--cap', type=int, default=48 * 1024)
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()

    rom = rt.load_rom(a.rom)
    nmap = rt.load_map(a.map)
    rs, re_, va = nmap['_cdataSegmentRomStart'], nmap['_cdataSegmentRomEnd'], nmap['_csegmentSegmentStart']
    seg = zlib.decompressobj(-15).decompress(rom[rs + 2:re_])

    ref = set()
    for o in range(0, len(rom) - W + 1, W):
        w = rom[o:o + W]
        if not trivial(w):
            ref.add(w)
    for o in range(0, len(seg) - W + 1, 4):
        w = seg[o:o + W]
        if trivial(w):
            continue
        ref.add(w)
        ref.add(b''.join(w[i:i + 4][::-1] for i in range(0, W, 4)))
        ref.add(b''.join(w[i:i + 2][::-1] for i in range(0, W, 2)))

    elf = rt.Elf(a.elf)
    syms = sorted((v, s, n) for n, lst in elf.syms.items() for v, s in lst if s)
    starts = [x[0] for x in syms]
    assets = rt.asset_symbols(a.src)
    elf_text = bytes(elf.data)

    fail = False
    total = 0
    for path in a.files:
        data = open(path, 'rb').read()
        hits = bytearray(len(data))
        for o in range(0, len(data) - W + 1):
            if data[o:o + W] in ref:
                hits[o:o + W] = b'\1' * W
        n = sum(hits)
        total += n
        print('%s: %d bytes in ROM windows' % (path, n))
        if not n:
            continue
        # attribute: locate each hit run in the unstripped ELF by content, then its symbol
        per = {}
        o = 0
        while o < len(data):
            if not hits[o]:
                o += 1
                continue
            e = o
            while e < len(data) and hits[e]:
                e += 1
            chunk = data[o:e]
            name = '?'
            fo = elf_text.find(chunk[:64])
            if fo >= 0:
                for s in elf.secs:
                    if s['type'] == 1 and s['addr'] and s['off'] <= fo < s['off'] + s['size']:
                        addr = s['addr'] + fo - s['off']
                        k = bisect.bisect_right(starts, addr) - 1
                        if k >= 0 and syms[k][0] <= addr < syms[k][0] + syms[k][1]:
                            name = syms[k][2]
                        else:
                            name = '(%s 0x%X)' % (s['name'], addr)
                        break
            per[name] = per.get(name, 0) + (e - o)
            o = e
        for name, cnt in sorted(per.items(), key=lambda x: -x[1]):
            bad = name in assets
            fail |= bad
            print('  %6d  %s%s' % (cnt, name, '   <-- a ROM table: must be scrubbed' if bad else ''))
    if total > a.cap:
        print('leak scan: %d bytes over the cap of %d' % (total, a.cap))
        fail = True
    print('leak scan: %s' % ('FAIL' if fail else 'pass'))
    sys.exit(1 if fail else 0)


if __name__ == '__main__':
    main()
