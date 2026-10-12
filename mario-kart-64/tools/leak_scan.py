#!/usr/bin/env python3
"""Release gate: prove the files that ship contain no ROM content.

Every 32-byte window (32-byte aligned) of each file is looked up in an index
of every 4-byte-aligned 32-byte window of the ROM, in every byte order the
port's data can be in (as-is, 16-bit swapped, 32-bit swapped, and the two
mixed 2-1-1 / 1-1-2 word shapes), and of every MIO0 block of the ROM
decompressed. Padding (all-zero / all-0xFF) and low-entropy windows (fewer
than 6 distinct byte values: a handful of small integers matches somewhere
in 12 MB by chance) are skipped. Each hit is attributed to the ELF symbol it
lies in. A hit is a LEAK when that symbol is one of the asset region's
(--assets build/<plat>/assets.bin lists all ~20k of them), or when the file
is not an ELF; every other hit is a table the decomp's tracked SOURCE
contains (trig tables, audio constants, literal pools of the decompiled
code) and is listed for the record. --budget-kb caps their total (45 KB in
this port): a large new match outside the asset names fails too.

Not covered: data byte-swapped per field with a period longer than one word
(structs of mixed s16/u8 fields). Those come only from the asset objects,
which the build's provenance check keeps out of the executable.

usage: leak_scan.py --rom baserom.z64 --assets assets.bin [--budget-kb N] file...
"""
import argparse, bisect, re, struct, subprocess, sys
from array import array

WIN = 32

def mio0_decode(d, off):
    if d[off:off+4] != b'MIO0': return None
    size, coff, uoff = struct.unpack('>III', d[off+4:off+16])
    if size > 4 << 20: return None
    out = bytearray(); layout = off + 16; cp = off + coff; up = off + uoff
    bits = 0; nbits = 0
    try:
        while len(out) < size:
            if nbits == 0:
                bits = struct.unpack('>I', d[layout:layout+4])[0]; layout += 4; nbits = 32
            if bits & 0x80000000:
                out.append(d[up]); up += 1
            else:
                b0, b1 = d[cp], d[cp+1]; cp += 2
                length = (b0 >> 4) + 3; dist = ((b0 & 0xF) << 8 | b1) + 1
                for _ in range(length): out.append(out[-dist])
            bits = (bits << 1) & 0xffffffff; nbits -= 1
    except (IndexError, struct.error):
        return None
    return bytes(out[:size])

def swap16(b):
    a = bytearray(b); n = len(b) // 2 * 2
    a[0:n:2], a[1:n:2] = b[1:n:2], b[0:n:2]; return bytes(a)
def swap32(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4], a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4], b[1:n:4], b[0:n:4]; return bytes(a)
def shape211(b):  # per word: 2-byte field then two bytes
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4] = b[1:n:4], b[0:n:4]; return bytes(a)
def shape112(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4]; return bytes(a)

def rom_matches(rom, wanted):
    """Which of the wanted window hashes occur in the ROM. Streams every 4-byte-aligned window of the ROM and of each
    decompressed block, in the 5 byte orders, against the (small) wanted set: the shipped files' windows are hashed
    first, so no index of the ROM's windows is kept (the sorted Python list peaked at 2.6 GB here and got OOM-killed
    at 3-5 GB in the Paper Mario port, sharing the build VM; the Paper Mario port eb1e5ea). Same matches as the old sorted index."""
    streams = [rom]
    for m in re.finditer(b'MIO0', rom):
        b = mio0_decode(rom, m.start())
        if b and len(b) >= WIN: streams.append(b)
    found = set(); total = 0
    for s in streams:
        for v in (s, swap16(s), swap32(s), shape211(s), shape112(s)):
            n = len(v) - WIN + 1
            if n <= 0: continue
            total += (n + 3) // 4
            found.update(h for h in (hash(v[i:i+WIN]) for i in range(0, n, 4)) if h in wanted)
    return found, total, len(streams)

def elf_symbols(path):
    """(addr, size, name) of OBJECT/NOTYPE symbols; (name, vaddr, offset, size) of PROGBITS sections"""
    syms, secs = [], []
    try:
        out = subprocess.run(['readelf', '-sW', path], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return syms, secs
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 8 and p[0].endswith(':') and p[3] in ('OBJECT', 'NOTYPE', 'FUNC') and not p[7].startswith('$'):
            try: syms.append((int(p[1], 16), int(p[2]), p[7]))
            except ValueError: pass
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    for l in out.splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', l)
        if m and m.group(2) == 'PROGBITS' and int(m.group(3), 16):
            secs.append((m.group(1), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)))
    syms.sort()
    return syms, secs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--assets', default=None, help='assets.bin of this build: its symbol names are the asset region')
    ap.add_argument('--budget-kb', type=int, default=96, help='fail if the source-table matches exceed this many KB')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    asset_names = set()
    if a.assets:
        d = open(a.assets, 'rb').read()
        magic, ver, addr, size, nrel, nsym, nnames, bid, crc, doff = struct.unpack_from('<8sIIIIIIIII', d, 0)
        if magic != b'MK64ASST': sys.exit('%s: not an assets.bin' % a.assets)
        p = 44 + nrel * 4; symtab = p; p += nsym * 16; names = d[p:p + nnames]
        for i in range(nsym):
            off, sz, h, nm = struct.unpack_from('<IIII', d, symtab + i * 16)
            asset_names.add(names[nm:names.index(b'\x00', nm)].decode())
        print('asset region: %d symbol names from %s' % (len(asset_names), a.assets))
    rom = open(a.rom, 'rb').read()
    # a byte-swapped dump would index nothing useful (no MIO0 headers found): refuse it
    if rom[:4] != b'\x80\x37\x12\x40' or len(rom) < 8 << 20:
        sys.exit('%s: not a big-endian (.z64) N64 ROM' % a.rom)
    def windows(d):  # the candidate 32-byte windows of a shipped file: (offset, hash), padding and low entropy skipped
        for off in range(0, len(d) - WIN + 1, WIN):
            w = d[off:off+WIN]
            if w == b'\0' * WIN or w == b'\xff' * WIN or len(set(w)) < 6: continue
            yield off, hash(w)
    wanted = set(h for path in a.files for _, h in windows(open(path, 'rb').read()))
    found, total, nstreams = rom_matches(rom, wanted)
    print('index: %d windows from the ROM + %d MIO0 blocks, 5 byte orders' % (total, nstreams - 1))
    if nstreams - 1 < 40:
        sys.exit('only %d MIO0 blocks decoded - wrong ROM? (Mario Kart 64 US has ~50)' % (nstreams - 1))
    bad_total = 0; src_bytes = 0
    for path in a.files:
        d = open(path, 'rb').read()
        syms, secs = elf_symbols(path) if d[:4] == b'\x7fELF' else ([], [])
        addrs = [s[0] for s in syms]
        def locate(off):
            for name, vaddr, foff, size in secs:
                if foff <= off < foff + size:
                    va = vaddr + (off - foff)
                    i = bisect.bisect_right(addrs, va) - 1
                    # inside the symbol's extent only: bytes after an allowed table are not "allowed"
                    if i >= 0 and va < syms[i][0] + max(syms[i][1], 1): return name, syms[i][2]
                    return name, '?'
                    
            return '?', '?'
        hits = {}
        n = 0
        for off, h in windows(d):
            n += 1
            if h in found:
                k = locate(off)
                hits.setdefault(k, [0, off])[0] += 1
        is_elf = bool(secs)
        leaks = {k: v for k, v in hits.items() if (not is_elf) or k[1] in asset_names}
        source = {k: v for k, v in hits.items() if k not in leaks}
        print('%s: %d windows scanned, %d match ROM content: %d in decomp source tables, %d LEAKS' % (
            path, n, sum(v[0] for v in hits.values()), sum(v[0] for v in source.values()), sum(v[0] for v in leaks.values())))
        for (sec, sym), (cnt, first) in sorted(source.items(), key=lambda kv: -kv[1][0])[:12]:
            print('  source   %5d windows  %-10s %s' % (cnt, sec, sym))
        if len(source) > 12: print('  source   ... %d more symbols' % (len(source) - 12))
        for (sec, sym), (cnt, first) in sorted(leaks.items(), key=lambda kv: -kv[1][0]):
            print('  LEAK     %5d windows  %-10s %s (first at file offset %#x)' % (cnt, sec, sym, first))
        bad_total += sum(v[0] for v in leaks.values())
        src_bytes += sum(v[0] for v in source.values()) * WIN
    if bad_total:
        print('FAIL: %d windows of asset-region content in the shipped files' % bad_total); sys.exit(1)
    if src_bytes > a.budget_kb * 1024:
        print('FAIL: %d KB of ROM-matching bytes outside the asset names exceeds the %d KB budget - inspect' % (src_bytes // 1024, a.budget_kb)); sys.exit(1)
    print('OK: no asset-region content in the shipped files (%d KB of decomp source tables match the ROM, within budget)' % (src_bytes // 1024))

if __name__ == '__main__':
    main()
