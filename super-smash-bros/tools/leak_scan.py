#!/usr/bin/env python3
"""Release gate 3: prove the files that ship contain no ROM content.

Adapted from mario-kart-64 tools/leak_scan.py. Every 32-byte window (32-byte
aligned) of each shipped file is looked up in an index of every 4-byte
aligned 32-byte window of the player's ROM AND of every relocData file
decompressed (tools/reloc_dump.c), each in five byte orders (as-is, 16-bit
swapped, 32-bit swapped, and the 2-1-1 / 1-1-2 word shapes the port's
byte-swap layer produces). Padding and low-entropy windows (fewer than 6
distinct byte values) are skipped.

Smash has no asset region: nothing ROM-derived is ever compiled in, so any
hit is either a table in the decomp's own tracked source (literal pools,
constants, the credits text) or a leak. Non-ELF files (icon, desktop entry)
may not match at all; ELF matches are listed by symbol and capped by
--budget-kb.

usage: leak_scan.py --rom baserom.z64 --reloc-dir DIR [--budget-kb N] file...
"""
import argparse, bisect, glob, os, re, subprocess, sys

WIN = 32

def swap16(b):
    a = bytearray(b); n = len(b) // 2 * 2
    a[0:n:2], a[1:n:2] = b[1:n:2], b[0:n:2]; return bytes(a)
def swap32(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4], a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4], b[1:n:4], b[0:n:4]; return bytes(a)
def shape211(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4] = b[1:n:4], b[0:n:4]; return bytes(a)
def shape112(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4]; return bytes(a)

def data_matches(streams, wanted):
    """Which of the wanted window hashes occur in the game data: every 4-byte
    aligned window of each stream, in the 5 byte orders, streamed against the
    small set of the shipped files' windows. No index of the ~42M data windows
    is kept: sorting one took ~3 GB, an OOM risk in the shared build VM
    (paper-mario eb1e5ea). Same matches as the old sorted index."""
    found = set(); total = 0
    for s in streams:
        for v in (s, swap16(s), swap32(s), shape211(s), shape112(s)):
            n = len(v) - WIN + 1
            if n <= 0: continue
            total += (n + 3) // 4
            found.update(h for h in (hash(v[i:i+WIN]) for i in range(0, n, 4)) if h in wanted)
    return found, total

def windows(d):
    """The scanned 32-byte windows of a shipped file, (offset, hash): 32-byte
    aligned, padding and low-entropy windows (< 6 distinct bytes) skipped."""
    for off in range(0, len(d) - WIN + 1, WIN):
        w = d[off:off+WIN]
        if len(set(w)) < 6: continue
        yield off, hash(w)

def elf_symbols(path, readelf):
    syms, secs = [], []
    out = subprocess.run([readelf, '-sW', path], capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 8 and p[0].endswith(':') and p[3] in ('OBJECT', 'NOTYPE', 'FUNC') and not p[7].startswith('$'):
            try: syms.append((int(p[1], 16), int(p[2]), p[7]))
            except ValueError: pass
    out = subprocess.run([readelf, '-SW', path], capture_output=True, text=True).stdout
    for l in out.splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', l)
        if m and m.group(2) == 'PROGBITS' and int(m.group(3), 16):
            secs.append((m.group(1), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)))
    syms.sort()
    return syms, secs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--reloc-dir', required=True)
    ap.add_argument('--budget-kb', type=int, default=16)
    ap.add_argument('--readelf', default='readelf')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    if rom[:4] != b'\x80\x37\x12\x40':
        sys.exit('%s: not a big-endian (.z64) ROM (the port normalises; give the scan a .z64)' % a.rom)
    relocs = [open(f, 'rb').read() for f in sorted(glob.glob(os.path.join(a.reloc_dir, 'reloc_*.bin')))]
    if len(relocs) < 2000:
        sys.exit('only %d reloc files in %s - run tools/reloc_dump first' % (len(relocs), a.reloc_dir))
    shipped = {path: open(path, 'rb').read() for path in a.files}
    wanted = set(h for d in shipped.values() for _, h in windows(d))
    found, total = data_matches([rom] + relocs, wanted)
    print('index: %d windows (ROM + %d decompressed reloc files, 5 byte orders)' % (total, len(relocs)))
    leak_windows = 0; src_bytes = 0
    for path in a.files:
        d = shipped[path]
        is_elf = d[:4] == b'\x7fELF'
        syms, secs = elf_symbols(path, a.readelf) if is_elf else ([], [])
        addrs = [s[0] for s in syms]
        def locate(off):
            for name, vaddr, foff, size in secs:
                if foff <= off < foff + size:
                    va = vaddr + (off - foff)
                    i = bisect.bisect_right(addrs, va) - 1
                    if i >= 0 and va < syms[i][0] + max(syms[i][1], 1): return name, syms[i][2]
                    return name, '?'
            return '?', '?'
        hits = {}; n = 0
        for off, h in windows(d):
            n += 1
            if h in found:
                k = locate(off)
                hits.setdefault(k, [0, off])[0] += 1
        total = sum(v[0] for v in hits.values())
        print('%s: %d windows scanned, %d match ROM/game data' % (os.path.basename(path), n, total))
        for (sec, sym), (cnt, first) in sorted(hits.items(), key=lambda kv: -kv[1][0])[:16]:
            print('  %5d windows  %-10s %s (first at %#x)' % (cnt, sec, sym, first))
        if not is_elf:
            leak_windows += total
        else:
            src_bytes += total * WIN
    if leak_windows:
        print('FAIL: %d windows of game data in non-code files' % leak_windows); sys.exit(1)
    if src_bytes > a.budget_kb * 1024:
        print('FAIL: %d KB of ROM-matching bytes in the executable exceeds the %d KB budget (decomp source tables) - inspect' % (src_bytes // 1024, a.budget_kb)); sys.exit(1)
    print('OK: no game data in the shipped files (%d KB of the executable matches ROM bytes: decomp source tables, budget %d KB)' % (src_bytes // 1024, a.budget_kb))

if __name__ == '__main__':
    main()
