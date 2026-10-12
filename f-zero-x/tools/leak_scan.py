#!/usr/bin/env python3
"""Release gate 3: prove the files that ship contain no ROM content.

After mario-kart-64's tools/leak_scan.py. Every 32-byte window (32-byte aligned)
of each shipped file is looked up in an index of every 4-byte-aligned 32-byte
window of the ROM, in every byte order the port's data can take (as-is, 16-bit
swapped, 32-bit swapped, and the 2-1-1 / 1-1-2 word shapes), and of every MIO0
block of the ROM decompressed. Padding (all-zero / all-0xFF) and low-entropy
windows (fewer than 6 distinct byte values) are skipped.

F-Zero X's port has no asset region: game data is read from the player's ROM at
run time. So in the executable a hit is:
  - a LEAK when it lies in a symbol of the generated tables (build/<plat>/gen,
    --gen-object: numbers only, they must never match ROM content), anywhere in
    a non-ELF file, or in bytes neither a symbol nor the link map (--map)
    attributes to a compiled object; bytes outside any symbol (string literals,
    constant pools) count as their object's: a generated object's are LEAKS;
  - otherwise one of the decomp's own source tables (compiled from tracked C:
    audio tables, trig tables, literal pools), listed for the record and capped
    by --budget-kb.

usage: leak_scan.py --rom baserom.z64 [--gen-object obj.o ...] [--map link.map] [--budget-kb N] file...
"""
import argparse, bisect, re, struct, subprocess, sys

WIN = 32


def mio0_decode(d, off):
    if d[off:off + 4] != b'MIO0':
        return None
    size, coff, uoff = struct.unpack('>III', d[off + 4:off + 16])
    if size > 4 << 20:
        return None
    out = bytearray()
    layout = off + 16
    cp = off + coff
    up = off + uoff
    bits = 0
    nbits = 0
    try:
        while len(out) < size:
            if nbits == 0:
                bits = struct.unpack('>I', d[layout:layout + 4])[0]
                layout += 4
                nbits = 32
            if bits & 0x80000000:
                out.append(d[up])
                up += 1
            else:
                b0, b1 = d[cp], d[cp + 1]
                cp += 2
                length = (b0 >> 4) + 3
                dist = ((b0 & 0xF) << 8 | b1) + 1
                for _ in range(length):
                    out.append(out[-dist])
            bits = (bits << 1) & 0xffffffff
            nbits -= 1
    except (IndexError, struct.error):
        return None
    return bytes(out[:size])


def swap16(b):
    a = bytearray(b)
    n = len(b) // 2 * 2
    a[0:n:2], a[1:n:2] = b[1:n:2], b[0:n:2]
    return bytes(a)


def swap32(b):
    a = bytearray(b)
    n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4], a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4], b[1:n:4], b[0:n:4]
    return bytes(a)


def shape211(b):  # per word: a 2-byte field swapped, then two bytes
    a = bytearray(b)
    n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4] = b[1:n:4], b[0:n:4]
    return bytes(a)


def shape112(b):
    a = bytearray(b)
    n = len(b) // 4 * 4
    a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4]
    return bytes(a)


def scan_windows(d):
    """(offset, window) of every window of a shipped file worth checking"""
    for off in range(0, len(d) - WIN + 1, WIN):
        w = d[off:off + WIN]
        if w == b'\0' * WIN or w == b'\xff' * WIN or len(set(w)) < 6:
            continue
        yield off, w


def rom_matches(rom, wanted):
    """Of the shipped windows' hashes (wanted), the ones some ROM window has: the ROM and
    its MIO0 blocks in 5 byte orders, streamed past the small set (paper-mario
    eb1e5ea: a sorted index of every ROM window peaked at gigabytes in the shared VM)."""
    streams = [rom]
    for m in re.finditer(b'MIO0', rom):
        b = mio0_decode(rom, m.start())
        if b and len(b) >= WIN:
            streams.append(b)
    found, n = set(), 0
    for s in streams:
        for v in (s, swap16(s), swap32(s), shape211(s), shape112(s)):
            for i in range(0, len(v) - WIN + 1, 4):
                h = hash(v[i:i + WIN])
                if h in wanted:
                    found.add(h)
            n += max(0, (len(v) - WIN) // 4 + 1)
    return found, n, len(streams)


def readelf(args, path):
    return subprocess.run(['readelf'] + args + [path], capture_output=True, text=True).stdout


def elf_symbols(path):
    """(addr, size, name) of symbols; (name, vaddr, offset, size) of PROGBITS sections"""
    syms, secs = [], []
    for l in readelf(['-sW'], path).splitlines():
        p = l.split()
        if len(p) >= 8 and p[0].endswith(':') and p[3] in ('OBJECT', 'NOTYPE', 'FUNC') and not p[7].startswith('$'):
            try:
                syms.append((int(p[1], 16), int(p[2]), p[7]))
            except ValueError:
                pass
    for l in readelf(['-SW'], path).splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', l)
        if m and m.group(2) == 'PROGBITS' and int(m.group(3), 16):
            secs.append((m.group(1), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)))
    syms.sort()
    return syms, secs


def map_sections(path):
    """(addr, size, object) of every input section the GNU ld map places"""
    out = []
    pending = None
    for l in open(path, errors='replace'):
        m = re.match(r'^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$', l)
        if m:
            out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
            pending = None
            continue
        m = re.match(r'^ (\.\S+)\s*$', l)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$', l)
        if m and pending:
            out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        pending = None
    out.sort()
    return out


def defined_names(obj):
    names = set()
    for l in readelf(['-sW'], obj).splitlines():
        p = l.split()
        if len(p) >= 8 and p[0].endswith(':') and p[6] != 'UND' and p[3] in ('OBJECT', 'NOTYPE', 'FUNC'):
            names.add(p[7])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--gen-object', action='append', default=[], help='object built from generated tables')
    ap.add_argument('--map', default=None, help='GNU ld map of the executable: attributes bytes outside any symbol')
    ap.add_argument('--budget-kb', type=int, default=64, help='fail if decomp source-table matches exceed this')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    gen_names = set()
    for o in a.gen_object:
        gen_names |= defined_names(o)
    print('generated tables: %d symbols from %s' % (len(gen_names), ', '.join(a.gen_object) or '-'))
    gen_objs = set(o.split('/')[-1] for o in a.gen_object)
    msecs = map_sections(a.map) if a.map else []
    maddrs = [m[0] for m in msecs]
    rom = open(a.rom, 'rb').read()
    if rom[:4] != b'\x80\x37\x12\x40' or len(rom) < 8 << 20:
        sys.exit('%s: not a big-endian (.z64) N64 ROM' % a.rom)
    data = [(path, open(path, 'rb').read()) for path in a.files]
    wanted = set(hash(w) for _, d in data for _, w in scan_windows(d))
    matched, nwin, nstreams = rom_matches(rom, wanted)
    print('index: %d windows from the ROM + %d MIO0 blocks, 5 byte orders' % (nwin, nstreams - 1))
    if nstreams - 1 < 100:
        sys.exit('only %d MIO0 blocks decoded - wrong ROM? (F-Zero X US has several hundred)' % (nstreams - 1))
    bad_total = 0
    src_bytes = 0
    for path, d in data:
        syms, secs = elf_symbols(path) if d[:4] == b'\x7fELF' else ([], [])
        addrs = [s[0] for s in syms]

        def locate(off):
            for name, vaddr, foff, size in secs:
                if foff <= off < foff + size:
                    va = vaddr + (off - foff)
                    i = bisect.bisect_right(addrs, va) - 1
                    if i >= 0 and va < syms[i][0] + max(syms[i][1], 1):
                        return name, syms[i][2]
                    j = bisect.bisect_right(maddrs, va) - 1
                    if j >= 0 and va < msecs[j][0] + msecs[j][1]:
                        obj = msecs[j][2].split('/')[-1]
                        return name, ('?gen:' if obj in gen_objs else '(in ') + obj + ('' if obj in gen_objs else ')')
                    return name, '?'
            return '?', '?'

        hits = {}
        n = 0
        for off, w in scan_windows(d):
            n += 1
            if hash(w) in matched:
                k = locate(off)
                hits.setdefault(k, [0, off])[0] += 1
        is_elf = bool(secs)
        leaks = {k: v for k, v in hits.items()
                 if (not is_elf) or k[1] in gen_names or k[1] == '?' or k[1].startswith('?gen:')}
        source = {k: v for k, v in hits.items() if k not in leaks}
        print('%s: %d windows scanned, %d match ROM content: %d in decomp source tables, %d LEAKS' % (
            path, n, sum(v[0] for v in hits.values()), sum(v[0] for v in source.values()),
            sum(v[0] for v in leaks.values())))
        for (sec, sym), (cnt, first) in sorted(source.items(), key=lambda kv: -kv[1][0])[:16]:
            print('  source   %5d windows  %-10s %s' % (cnt, sec, sym))
        if len(source) > 16:
            print('  source   ... %d more symbols' % (len(source) - 16))
        for (sec, sym), (cnt, first) in sorted(leaks.items(), key=lambda kv: -kv[1][0]):
            print('  LEAK     %5d windows  %-10s %s (first at file offset %#x)' % (cnt, sec, sym, first))
        bad_total += sum(v[0] for v in leaks.values())
        src_bytes += sum(v[0] for v in source.values()) * WIN
    if bad_total:
        print('FAIL: %d windows of ROM content outside the decomp source in the shipped files' % bad_total)
        sys.exit(1)
    if src_bytes > a.budget_kb * 1024:
        print('FAIL: %d KB of ROM-matching decomp source tables exceeds the %d KB budget - inspect' % (
            src_bytes // 1024, a.budget_kb))
        sys.exit(1)
    print('OK: no ROM content outside the decomp source (%d KB of its tables match the ROM, within budget)' % (
        src_bytes // 1024))


if __name__ == '__main__':
    main()
