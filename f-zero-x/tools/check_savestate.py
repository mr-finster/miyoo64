#!/usr/bin/env python3
"""Release gate 5: a save state holds game memory only (src/port/savestate.ld).

Reads the link map and lists every input section that lands in the two saved ranges,
[__ss_game_start, __ss_game_end) and [__ss_ram_start, __ss_ram_end). Allowed there:
the decomp's objects (src/game, src/sys, src/audio/rom, src/overlays, src/framebuffers,
src/libultra), the OS layer (src/port/ultra/ultra_os.o), sections tagged
SAVESTATE_INCLUDE (.data.savestate), the generated tables and the toolchain's start
files. Any other port object in a saved range would put heap, FILE* or SDL pointers
into states, which crash a load on the device (it moves the heap every boot).

    tools/check_savestate.py <build/plat/fzerox.map>
"""
import re
import sys

GAME_DIRS = ('src/game/', 'src/sys/', 'src/audio/rom/', 'src/overlays/', 'src/framebuffers/', 'src/libultra/')


def main():
    path = sys.argv[1]
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    syms = {}
    for line in lines:
        m = re.match(r'\s+0x([0-9a-f]+)\s+(__ss_(?:game|ram)_(?:start|end))\s*=', line)
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    missing = [s for s in ('__ss_game_start', '__ss_game_end', '__ss_ram_start', '__ss_ram_end') if s not in syms]
    if missing:
        sys.exit('check_savestate: %s not in the map (src/port/savestate.ld not linked?)' % ', '.join(missing))
    ranges = [('game', syms['__ss_game_start'], syms['__ss_game_end']),
              ('ram', syms['__ss_ram_start'], syms['__ss_ram_end'])]

    # input sections: " .name 0xADDR 0xSIZE file", the name alone on its line when long
    sections, pending = [], None
    for line in lines:
        m = re.match(r'^ (\.\S+|COMMON)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if m:
            sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()))
            pending = None
            continue
        m = re.match(r'^ (\.\S+|COMMON)\s*$', line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if m and pending:
            sections.append((pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
        pending = None

    bad, kinds = [], {}
    for name, addr, size, obj in sections:
        if size == 0:
            continue
        for rname, lo, hi in ranges:
            if addr >= hi or addr + size <= lo:
                continue
            if any(d in obj for d in GAME_DIRS) and '/src/port/' not in obj:
                kind = 'game'
            elif obj.endswith('src/port/ultra/ultra_os.o'):
                kind = 'OS layer'
            elif name.startswith('.data.savestate'):
                kind = 'SAVESTATE_INCLUDE'
            elif '/gen/' in obj:
                kind = 'generated tables'
            elif '/src/' not in obj:
                kind = 'toolchain'
            else:
                bad.append('%s range: %s %s (0x%X bytes at 0x%08X)' % (rname, obj, name, size, addr))
                continue
            kinds[kind] = kinds.get(kind, 0) + size
    for rname, lo, hi in ranges:
        print('check_savestate: %s range 0x%08X-0x%08X, %u KB' % (rname, lo, hi, (hi - lo) // 1024))
    print('check_savestate: ' + ', '.join('%s %u KB' % (k, v // 1024) for k, v in sorted(kinds.items())))
    for b in bad:
        print('NOT GAME STATE in a saved range: ' + b)
    if bad:
        sys.exit(1)
    print('check_savestate: ok')


if __name__ == '__main__':
    main()
