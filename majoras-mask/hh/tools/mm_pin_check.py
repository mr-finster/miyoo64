#!/usr/bin/env python3
"""mm_pin_check.py <base.elf> <base.map> <new.elf>: does <new.elf> load <base.elf>'s save states?

A state is the raw .game_data/.game_bss of the build that wrote it. It loads in another build when:
  - both ranges are at the same addresses with the same sizes;
  - every game symbol (functions: actor update pointers; constant tables; data) is at the same address;
  - the port symbols a state was seen to point at (init-time values: the audio DMA handler, the audio ucode
    buffers, the cart handle, the VI mode) are at the same addresses.
Prints the differences; exit status 1 if any.
"""
import re, subprocess, sys, os, bisect

NM = os.environ.get('NM', '/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-nm')
RE = os.environ.get('READELF', '/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-readelf')
# seen in ALPHA4 states (exact symbol addresses in .game_data/.game_bss; mm-miyoo AUDIT.md section 6)
STATE_PORT_SYMS = ['DmaMgr_AudioDmaHandler', 'sCartHandle', 'aspMainTextStart', 'aspMainDataStart', 'aspMainStack',
                   'sPspDummyUCodeBoot', 'sPspDummyUCodeText', 'sPspDummyUCodeData', 'osViModeNtscLan1']
GAME_SECS = ('.text', '.rodata', '.data', '.game_data', '.bss', '.game_bss')


def syms(path):
    out = {}
    for l in subprocess.run([NM, '-S', path], capture_output=True, text=True).stdout.split('\n'):
        p = l.split()
        if len(p) == 4:
            out.setdefault(p[3], []).append((int(p[0], 16), int(p[1], 16), p[2]))
        elif len(p) == 3:
            out.setdefault(p[2], []).append((int(p[0], 16), 0, p[1]))
    return out


def sections(path):
    res = {}
    for l in subprocess.run([RE, '-SW', path], capture_output=True, text=True).stdout.split('\n'):
        m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)', l)
        if m:
            res[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return res


def game_ranges(base_map):
    sys.path.insert(0, os.path.dirname(__file__))
    from mm_pin_ld import read_map
    _, entries = read_map(base_map)
    r = []
    for out, sec, addr, size, obj in entries:
        if size and addr and '/hh/' not in obj and obj.startswith('build/') and 'build_stamp' not in obj:
            r.append((addr, addr + size))
    r.sort()
    return r


def main():
    base_elf, base_map, new_elf = sys.argv[1:4]
    bad = 0
    sb, sn = sections(base_elf), sections(new_elf)
    for s in ('.game_data', '.game_bss'):
        if sb.get(s) != sn.get(s):
            print('range %s: base %s, new %s' % (s, sb.get(s), sn.get(s)))
            bad += 1
    rngs = game_ranges(base_map)
    starts = [a for a, _ in rngs]

    def in_game(v):
        i = bisect.bisect_right(starts, v) - 1
        return i >= 0 and rngs[i][0] <= v < rngs[i][1]

    yb, yn = syms(base_elf), syms(new_elf)
    checked = 0
    for name, lst in yb.items():
        for addr, size, typ in lst:
            want = in_game(addr) or name in STATE_PORT_SYMS or name.startswith('__hhovl_')
            if not want:
                continue
            checked += 1
            if not any(a == addr for a, _, _ in yn.get(name, [])):
                print('moved: %s %s base 0x%08x new %s' % (typ, name, addr, [hex(a) for a, _, _ in yn.get(name, [])]))
                bad += 1
    print('%d symbols checked, %d differences' % (checked, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
