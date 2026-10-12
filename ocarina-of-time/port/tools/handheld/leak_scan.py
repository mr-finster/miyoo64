#!/usr/bin/env python3
"""Release gate: the files that ship hold no ROM content (mario-kart-64's tools/leak_scan.py,
for Ocarina of Time).

Every 32-byte window (32-byte aligned) of each shipped file is looked up among every
4-byte-aligned 32-byte window of the player's decompressed ROM (the decomp's
baserom-decompressed.z64: OoT's ROM files are Yaz0-compressed, so the raw dump would hide
most of its data), in every byte order the port's data can be in: as-is, 16-bit swapped,
32-bit swapped, and the two mixed 2-1-1 / 1-1-2 word shapes. Padding (all-zero / all-0xFF)
and low-entropy windows (fewer than 6 distinct byte values) are skipped.

Each hit in the executable is traced to the symbol it lies in (the unstripped ELF, --elf)
and to the file that symbol is DEFINED in (its DWARF declaration, nm -l):
  - a decomp or port source tracked in git: a table the decomp's own source spells out
    (trig tables, audio tables, literal pools), listed and capped by --budget-kb;
  - the generated tables (build/hh-gen: asset, object/scene and audio tables, numbers made
    by tracked tools): listed, and counted in the budget;
  - no file (compiler-made: switch tables, literals): listed, and counted in the budget;
  - anything else -- an extracted or byte-swapped asset (extracted/, build/, a .inc.c the
    split in tools/handheld/embedded_assets.py missed): a LEAK, and the gate fails.
A hit in a file that is not an ELF (the icon, the .desktop) is a LEAK.

usage: leak_scan.py --rom baserom-decompressed.z64 --elf <unstripped> --repo <root> [--budget-kb N] file...
"""
import argparse, bisect, os, re, subprocess, sys

WIN = 32
# binutils prefix (readelf, nm -l); LEAK_SCAN_TC=<prefix> for another toolchain (the Miyoo build's)
TC = os.environ.get('LEAK_SCAN_TC') or '/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-'

def swap16(b):
    a = bytearray(b); n = len(b) // 2 * 2
    a[0:n:2], a[1:n:2] = b[1:n:2], b[0:n:2]; return bytes(a)
def swap32(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4], a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4], b[1:n:4], b[0:n:4]; return bytes(a)
def shape211(b):  # per word: a 2-byte field swapped, then two bytes
    a = bytearray(b); n = len(b) // 4 * 4
    a[0:n:4], a[1:n:4] = b[1:n:4], b[0:n:4]; return bytes(a)
def shape112(b):
    a = bytearray(b); n = len(b) // 4 * 4
    a[2:n:4], a[3:n:4] = b[3:n:4], b[2:n:4]; return bytes(a)

def windows(d):
    """The candidate windows of a shipped file: (offset, hash); padding and low entropy skipped."""
    for off in range(0, len(d) - WIN + 1, WIN):
        w = d[off:off + WIN]
        if w == b'\0' * WIN or w == b'\xff' * WIN or len(set(w)) < 6:
            continue
        yield off, hash(w)

def rom_matches(rom, wanted):
    """Which wanted window hashes occur in the ROM, streaming its windows in the 5 byte orders
    against the small wanted set (no index of the ROM is kept: paper-mario eb1e5ea)."""
    found, total = set(), 0
    for v in (rom, swap16(rom), swap32(rom), shape211(rom), shape112(rom)):
        n = len(v) - WIN + 1
        total += (n + 3) // 4
        found.update(h for h in (hash(v[i:i + WIN]) for i in range(0, n, 4)) if h in wanted)
    return found, total

def sections(path):
    """(name, vaddr, file offset, size) of the PROGBITS sections that have an address"""
    out = subprocess.run([TC + 'readelf', '-SW', path], capture_output=True, text=True).stdout
    secs = []
    for l in out.splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', l)
        if m and m.group(2) == 'PROGBITS' and int(m.group(3), 16):
            secs.append((m.group(1), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)))
    return secs

def symbols(elf):
    """(addr, size, name) of the data and code symbols, and each name's defining file (nm -l)"""
    syms = []
    out = subprocess.run([TC + 'readelf', '-sW', elf], capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 8 and p[0].endswith(':') and p[3] in ('OBJECT', 'NOTYPE', 'FUNC') and not p[7].startswith('$'):
            try:
                syms.append((int(p[1], 16), int(p[2]), p[7]))
            except ValueError:
                pass
    syms.sort()
    files = {}
    out = subprocess.run([TC + 'nm', '-l', '--defined-only', elf], capture_output=True, text=True).stdout
    for l in out.splitlines():
        m = re.match(r'([0-9a-f]+) \S (\S+)\t(.+):\d+$', l)
        if m:
            files[(int(m.group(1), 16), m.group(2))] = m.group(3)
    return syms, files

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--elf', required=True, help='the unstripped executable the shipped one was stripped from')
    ap.add_argument('--repo', required=True, help='the git work tree (tracked = decomp or port source)')
    ap.add_argument('--budget-kb', type=int, default=64)
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    repo = os.path.abspath(a.repo)
    rom = open(a.rom, 'rb').read()
    if rom[:4] != b'\x80\x37\x12\x40' or len(rom) < 32 << 20:
        sys.exit('%s: not a big-endian (.z64) decompressed N64 ROM' % a.rom)
    tracked = set(subprocess.run(['git', '-c', 'safe.directory=*', '-C', a.repo, 'ls-files'],
                                 capture_output=True, text=True).stdout.split('\n'))
    if len(tracked) < 1000:
        sys.exit('git ls-files listed only %d files - the check cannot tell source from assets' % len(tracked))
    wanted = set(h for path in a.files for _, h in windows(open(path, 'rb').read()))
    found, total = rom_matches(rom, wanted)
    print('ROM: %d windows in 5 byte orders; %d distinct windows from the shipped files' % (total, len(wanted)))
    syms, files = symbols(a.elf)
    addrs = [s[0] for s in syms]

    def classify(va):
        i = bisect.bisect_right(addrs, va) - 1
        if i < 0 or va >= syms[i][0] + max(syms[i][1], 1):
            return 'nofile', '?'
        name = syms[i][2]
        f = files.get((syms[i][0], name))
        if f is None:
            return 'nofile', name
        # DWARF paths are absolute: relative to the work tree (/work in the build container, the
        # checkout itself on a host build such as the Miyoo one)
        rel = os.path.relpath(os.path.normpath(f), repo) if f.startswith(repo + '/') else f
        if rel.startswith('port/build/hh-gen/'):
            return 'generated', '%s (%s)' % (name, rel)
        if rel in tracked:
            return 'source', '%s (%s)' % (name, rel)
        return 'LEAK', '%s (%s)' % (name, rel)

    leaks = budget = 0
    for path in a.files:
        d = open(path, 'rb').read()
        is_elf = d[:4] == b'\x7fELF'
        secs = sections(path) if is_elf else []
        hits, n = {}, 0
        for off, h in windows(d):
            n += 1
            if h not in found:
                continue
            kind, what = 'LEAK', '(not an executable)'
            if is_elf:
                kind, what = 'nofile', '? (outside every section)'
                for name, vaddr, foff, size in secs:
                    if foff <= off < foff + size:
                        kind, what = classify(vaddr + (off - foff))
                        break
            hits.setdefault((kind, what), [0, off])[0] += 1
        by = {}
        for (kind, what), (cnt, _) in hits.items():
            by[kind] = by.get(kind, 0) + cnt
        print('%s: %d windows scanned, %d match ROM content (%s)' % (
            os.path.basename(path), n, sum(v[0] for v in hits.values()),
            ', '.join('%s %d' % kv for kv in sorted(by.items())) or 'none'))
        for (kind, what), (cnt, first) in sorted(hits.items(), key=lambda kv: (kv[0][0] != 'LEAK', -kv[1][0]))[:25]:
            print('  %-9s %5d windows  %s%s' % (kind, cnt, what, ('  (file offset %#x)' % first) if kind == 'LEAK' else ''))
        if len(hits) > 25:
            print('  ... %d more symbols' % (len(hits) - 25))
        leaks += by.get('LEAK', 0)
        budget += sum(by.get(k, 0) for k in ('source', 'generated', 'nofile')) * WIN
    if leaks:
        print('FAIL: %d windows of ROM content that no tracked source spells out' % leaks)
        sys.exit(1)
    if budget > a.budget_kb * 1024:
        print('FAIL: %d KB of ROM-matching source tables exceeds the %d KB budget - inspect' % (budget // 1024, a.budget_kb))
        sys.exit(1)
    print('OK: no ROM content beyond the decomp\'s own source tables (%d KB, budget %d KB)' % (budget // 1024, a.budget_kb))

if __name__ == '__main__':
    main()
