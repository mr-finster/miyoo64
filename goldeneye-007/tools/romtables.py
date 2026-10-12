#!/usr/bin/env python3
"""romtables.py: keep the ROM's data tables out of a shipped executable.

The game code compiles in tables the decomp commits as C from the ROM's data segment
(weapon stats, model headers, skeletons, the image table...: the assets/ files the game
#includes, port/assets). A shipped executable must not carry those bytes. For each such
table this compares the executable's bytes with the ROM's own data segment (rarezip at
_cdataSegmentRomStart.._cdataSegmentRomEnd, unpacked at _csegmentSegmentStart) and
classifies every field: equal after a 1/2/4/8-byte swap -> filled from the ROM at start;
anything else (pointers into the executable) stays. The strings the tables point at are
followed and filled the same way.

Outputs:
  recipes.bin     numbers only: where each run comes from in the unpacked data segment,
                  where it goes in the executable, its swap width, plus the ROM range and
                  CRC-32s that check the player's ROM and the result (hh_romtables.c)
  <out executable> a copy with every filled byte zeroed and hh_romtables_scrubbed = 1

usage: romtables.py --elf build/miyoo/ge007 --map m1/.../ge007.u.map --rom <ROM>
                    --src port --out-elf <path> --out-recipes <path> [--report <path>]
"""
import argparse
import glob
import os
import re
import struct
import sys
import zlib

MAGIC = b'GERCP1\0\0'


def load_rom(path):
    rom = bytearray(open(path, 'rb').read())
    if rom[:2] == b'\x37\x80':  # v64: byte-swapped halfwords
        rom[0::2], rom[1::2] = rom[1::2], rom[0::2]
    elif rom[:2] == b'\x40\x12':  # n64: little-endian words
        for i in range(0, len(rom) - 3, 4):
            rom[i:i + 4] = rom[i:i + 4][::-1]
    if rom[:4] != b'\x80\x37\x12\x40':
        sys.exit('romtables: not an N64 ROM: %s' % path)
    if rom[0x20:0x29] != b'GOLDENEYE' or rom[0x3E:0x3F] != b'E':
        sys.exit('romtables: not GoldenEye 007 (USA): %s' % path)
    return bytes(rom)


def load_map(path):
    syms = {}
    pat = re.compile(r'^\s+0x([0-9a-fA-F]{8,16})\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*=.*)?$')
    for line in open(path, encoding='latin1'):
        m = pat.match(line)
        if m and m.group(2) not in syms:
            syms[m.group(2)] = int(m.group(1), 16) & 0xFFFFFFFF
    return syms


class Elf:
    def __init__(self, path):
        self.data = bytearray(open(path, 'rb').read())
        d = self.data
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
            sys.exit('romtables: %s is not a little-endian ELF32' % path)
        shoff, = struct.unpack_from('<I', d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        secs = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize)
            secs.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link, entsize=entsize))
        strtab = secs[shstrndx]
        for s in secs:
            s['name'] = self.cstr(strtab['off'] + s['name'])
        self.secs = secs
        self.syms = {}
        for s in secs:
            if s['type'] != 2:  # SHT_SYMTAB
                continue
            names = secs[s['link']]
            for k in range(s['size'] // 16):
                nm, val, sz, info, other, shndx = struct.unpack_from('<IIIBBH', d, s['off'] + k * 16)
                if (info & 15) != 1:  # STT_OBJECT
                    continue
                name = self.cstr(names['off'] + nm)
                # locals of the same name in several files: keep all
                self.syms.setdefault(name, []).append((val, sz))

    def cstr(self, off):
        e = self.data.index(b'\0', off)
        return self.data[off:e].decode('latin1')

    def file_off(self, addr):
        for s in self.secs:
            if s['type'] == 1 and s['addr'] and s['addr'] <= addr < s['addr'] + s['size']:  # PROGBITS
                return s['off'] + addr - s['addr']
        return None

    def in_image(self, addr):
        return self.file_off(addr) is not None


def asset_symbols(src):
    """Objects defined by the assets/ files the game #includes, and g_Textures (image.c
    includes images.def inside its initializer)."""
    names = set(['g_Textures'])
    incs = set()
    for f in glob.glob(os.path.join(src, 'src', '**', '*.c'), recursive=True) + glob.glob(os.path.join(src, 'src', '**', '*.h'), recursive=True):
        for line in open(f, encoding='latin1'):
            m = re.match(r'\s*#\s*include\s*[<"](assets/[^>"]+)[>"]', line)
            if m:
                incs.add(m.group(1))
    defn = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^]]*\]\s*)*=')
    for inc in sorted(incs):
        p = os.path.join(src, inc)
        if not os.path.isfile(p):
            continue
        for line in open(p, encoding='latin1'):
            m = defn.match(line)
            if m:
                names.add(m.group(1))
    return names


def classify(h, n):
    """Per byte: the swap width (1, 2, 4, 8) that maps the N64 bytes onto ours, or 0."""
    size = len(h)
    kind = [0] * size
    i = 0
    while i < size:
        if i % 8 == 0 and i + 8 <= size and h[i:i + 8] == n[i:i + 8][::-1] and h[i:i + 4] != n[i:i + 4][::-1]:
            kind[i:i + 8] = [8] * 8
            i += 8
            continue
        if i % 4 == 0 and i + 4 <= size and h[i:i + 4] == n[i:i + 4][::-1]:
            kind[i:i + 4] = [4] * 4
            i += 4
            continue
        if i % 2 == 0 and i + 2 <= size and h[i:i + 2] == n[i:i + 2][::-1] and h[i:i + 2] != n[i:i + 2]:
            kind[i:i + 2] = [2] * 2
            i += 2
            continue
        if h[i] == n[i]:
            kind[i] = 1
        i += 1
    return kind


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--map', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--src', required=True)
    ap.add_argument('--out-elf', required=True)
    ap.add_argument('--out-recipes', required=True)
    ap.add_argument('--report')
    a = ap.parse_args()

    rom = load_rom(a.rom)
    nmap = load_map(a.map)
    rs, re_, va = nmap['_cdataSegmentRomStart'], nmap['_cdataSegmentRomEnd'], nmap['_csegmentSegmentStart']
    packed = rom[rs:re_]
    if packed[:2] != b'\x11\x72':
        sys.exit('romtables: no rarezip header at ROM 0x%X' % rs)
    seg = zlib.decompressobj(-15).decompress(packed[2:])
    elf = Elf(a.elf)
    wanted = asset_symbols(a.src)

    runs = []    # (host, n64, len, width)
    kept = []    # report lines
    done = set()

    def add_runs(host, n64, kind):
        i = 0
        while i < len(kind):
            w = kind[i]
            if w == 0:
                i += 1
                continue
            j = i
            while j < len(kind) and kind[j] == w:
                j += w if w > 1 else 1
            runs.append((host + i, n64 + i, j - i, w))
            i = j

    def seg_bytes(n64, size):
        o = n64 - va
        if o < 0 or o + size > len(seg):
            return None
        return seg[o:o + size]

    def do_object(name, host, n64, size, depth):
        if (host, size) in done:
            return
        done.add((host, size))
        fo = elf.file_off(host)
        n = seg_bytes(n64, size)
        if fo is None or n is None:
            kept.append('%s: not in the data segment (host 0x%X n64 0x%X)' % (name, host, n64))
            return
        h = bytes(elf.data[fo:fo + size])
        kind = classify(h, n)
        add_runs(host, n64, kind)
        # fields that did not match: pointers into our image; follow them to strings
        k = 0
        while k + 4 <= size:
            if 0 in kind[k:k + 4]:
                hv, = struct.unpack_from('<I', h, k)
                nv, = struct.unpack_from('>I', n, k)
                if depth < 2 and elf.in_image(hv) and va <= nv < va + len(seg):
                    o = nv - va
                    z = seg.find(b'\0', o, o + 256)
                    tf = elf.file_off(hv)
                    if z > o and bytes(elf.data[tf:tf + z - o + 1]) == seg[o:z + 1]:
                        do_object(name + '->str', hv, nv, z - o + 1, depth + 1)
            k += 4
        filled = sum(1 for x in kind if x)
        kept.append('%-40s %6d bytes, %6d filled, %4d words kept' % (name, size, filled, (size - filled + 3) // 4))

    for name in sorted(wanted):
        if name not in elf.syms or name not in nmap:
            continue
        for host, size in elf.syms[name]:
            if size:
                do_object(name, host, nmap[name], size, 0)

    # merge adjacent runs
    runs.sort()
    merged = []
    for r in runs:
        if merged and merged[-1][3] == r[3] and merged[-1][0] + merged[-1][2] == r[0] and merged[-1][1] + merged[-1][2] == r[1]:
            m = merged[-1]
            merged[-1] = (m[0], m[1], m[2] + r[2], m[3])
        else:
            merged.append(r)

    # every filled byte in .romtables (hh/romtables.ld: page-aligned, no code, the only read-only pages the
    # fill unprotects) or in a writable section
    rt = [x for x in elf.secs if x['name'] == '.romtables']
    if not rt or rt[0]['addr'] % 4096 or rt[0]['size'] % 4096:
        sys.exit('romtables: no page-aligned .romtables section (link with hh/romtables.ld)')
    okr = [(x['addr'], x['addr'] + x['size']) for x in elf.secs
           if x['addr'] and ((x['flags'] & 1) or x['name'] == '.romtables')]  # SHF_WRITE
    for host, n64, ln, w in merged:
        if not any(a <= host and host + ln <= b for a, b in okr):
            sys.exit('romtables: a filled run at 0x%X (%d bytes) is outside .romtables and the writable sections:'
                     ' add its object to hh/romtables.ld' % (host, ln))

    # the expected result, then the scrub
    out = bytearray(elf.data)
    crc_res = 0
    for host, n64, ln, w in merged:
        fo = elf.file_off(host)
        crc_res = zlib.crc32(bytes(out[fo:fo + ln]), crc_res)
        out[fo:fo + ln] = b'\0' * ln
    flag = elf.syms.get('hh_romtables_scrubbed')
    if not flag:
        sys.exit('romtables: the executable has no hh_romtables_scrubbed (hh_romtables.c)')
    fo = elf.file_off(flag[0][0])
    out[fo:fo + 4] = struct.pack('<I', 1)

    total = sum(r[2] for r in merged)
    hdr = MAGIC + struct.pack('<IIIIIIII', len(merged), rs, re_, va, len(seg), zlib.crc32(packed) & 0xFFFFFFFF,
                              crc_res & 0xFFFFFFFF, total)
    body = b''.join(struct.pack('<IIII', *r) for r in merged)
    open(a.out_recipes, 'wb').write(hdr + body)
    open(a.out_elf, 'wb').write(out)
    os.chmod(a.out_elf, 0o755)
    rep = ['romtables: %d runs, %d bytes filled from the ROM at start' % (len(merged), total)] + kept
    if a.report:
        open(a.report, 'w').write('\n'.join(rep) + '\n')
    print(rep[0])


if __name__ == '__main__':
    main()
