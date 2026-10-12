#!/usr/bin/env python3
"""Build assets.bin: the ROM-derived data of the port, taken OUT of the executable.

From the MK64 port (tools/psp/make_assets.py, after beckerd's PSP asset
split); for SM64 only the magic differs. The asset objects are the decomp's
data (actors, levels, bin, sound, assets, data, Goddard's dynlists, the trig
tables): see the "Asset split" section of port/Makefile.

The asset objects' .data/.rodata are renamed to .assetdata/.assetrodata and
collected by the linker into one output section placed after .bss:
  * in the shipped link the section is NOLOAD -> symbols and addresses exist,
    the PRX carries no bytes for it;
  * in a second, identical link it is PROGBITS -> this tool takes its bytes,
    the R_MIPS_32 relocations inside it and the symbol table, and writes
    assets.bin.  At boot the port reads the file straight into the region
    and adds the module base to every relocated word.

usage: make_assets.py <progbits.elf> <noload.elf> -o assets.bin [--page N]

--page N (default 16): alignment of the data inside the file. The handheld build
uses 4096 (page-aligned region and data, so the cache could be mmapped over
the region; today it is read in); the offset is recorded in the header's last
word (0 = the legacy 16-byte pad).
Works for MIPS (PSP), ARM (handheld) and x86 (the desktop proxy) ELF32.
"""
import argparse, struct, sys, zlib

SECTION = b'.assetdata'
# The one absolute-32 relocation type each target uses for pointers in data.
ABS32 = {8: 2, 40: 2, 3: 1}  # EM_MIPS: R_MIPS_32, EM_ARM: R_ARM_ABS32, EM_386: R_386_32


def parse_elf(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[5] == 1, 'need a little-endian ELF32'
    (e_machine,) = struct.unpack_from('<H', d, 0x12)
    parse_elf.machine = e_machine
    (e_shoff,) = struct.unpack_from('<I', d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2e)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from('<IIIIIIIIII', d, off)
        secs.append(dict(name=name, type=typ, flags=flags, addr=addr, offset=offset, size=size, link=link, info=info, entsize=entsize))
    shstr = secs[e_shstrndx]
    def sname(s):
        b = d[shstr['offset'] + s['name']:]
        return b[:b.index(b'\x00')]
    for s in secs:
        s['sname'] = sname(s)
    byname = {s['sname']: s for s in secs}
    return d, secs, byname


def symbols(d, secs, byname, sec_index):
    st = byname[b'.symtab']; strt = secs[st['link']]
    out = []
    for i in range(st['size'] // 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, st['offset'] + i * 16)
        if shndx == sec_index and (info & 0xf) in (0, 1):  # NOTYPE/OBJECT; asm glabels have size 0
            b = d[strt['offset'] + name:]
            nm = b[:b.index(b'\x00')].decode()
            # skip assembler locals and ARM's $a/$d/$t mapping symbols
            if nm and not nm.startswith('.L') and not nm.startswith('$'):
                out.append((nm, value, size))
    # One entry per address; extent = up to the next symbol (covers unsized asm
    # data and tables whose declared size is short), the last one to the end.
    out.sort(key=lambda s: (s[1], -s[2], s[0]))
    dedup = []
    for nm, value, size in out:
        if dedup and dedup[-1][1] == value:
            continue
        dedup.append([nm, value, size])
    return dedup


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('progbits')
    ap.add_argument('noload')
    ap.add_argument('-o', required=True)
    ap.add_argument('--page', type=int, default=16, help='data alignment inside the file (16 = legacy PSP layout)')
    a = ap.parse_args()

    d, secs, byname = parse_elf(a.progbits)
    abs32 = ABS32.get(parse_elf.machine)
    if abs32 is None:
        sys.exit(f'{a.progbits}: unsupported e_machine {parse_elf.machine}')
    sec = byname.get(SECTION)
    if sec is None or sec['type'] != 1:
        sys.exit(f'{a.progbits}: no PROGBITS {SECTION.decode()} section')
    idx = secs.index(sec)
    data = d[sec['offset']:sec['offset'] + sec['size']]

    dn, nsecs, nbyname = parse_elf(a.noload)
    nsec = nbyname.get(SECTION)
    if nsec is None or nsec['addr'] != sec['addr'] or nsec['size'] != sec['size']:
        sys.exit(f'layout mismatch: progbits {sec["addr"]:#x}/{sec["size"]:#x} vs noload '
                 f'{nsec["addr"] if nsec else 0:#x}/{nsec["size"] if nsec else 0:#x}')
    if nsec['type'] != 8:
        sys.exit(f'{a.noload}: {SECTION.decode()} is not NOBITS (type {nsec["type"]})')

    rel = byname.get(b'.rel' + SECTION)
    relocs = []
    # The pointer words hold the PROGBITS link's addresses, but the game runs the NOLOAD link. The two
    # can differ outside the region: when the region lands on a 64 KB boundary, the PROGBITS link gives
    # it a LOAD segment of its own, one more program header (32 bytes) shifts .interp, .text and all the
    # code after them, and every code pointer in the data (level script CALLs, behaviours, geo
    # functions) pointed 32 bytes into the wrong place (2026-10-06: the boot looped in the splash
    # screen's CALL, which landed inside lvl_intro_update's jump table). So each pointer is moved by
    # its own section's displacement between the two links (by name; the region's is 0).
    nsec_by_name = {s['sname']: s for s in nsecs}
    symtab_s = byname[b'.symtab']
    data = bytearray(data)
    moved = 0
    if rel is not None:
        for i in range(rel['size'] // 8):
            r_offset, r_info = struct.unpack_from('<II', d, rel['offset'] + i * 8)
            if (r_info & 0xff) != abs32:
                sys.exit(f'unexpected relocation type {r_info & 0xff} at {r_offset:#x}')
            off = r_offset - sec['addr']
            relocs.append(off)
            (shndx,) = struct.unpack_from('<H', d, symtab_s['offset'] + (r_info >> 8) * 16 + 14)
            if shndx == 0 or shndx >= 0xff00:
                continue  # undefined or absolute: no section to move with
            ps = secs[shndx]
            ns = nsec_by_name.get(ps['sname'])
            if ns is None:
                sys.exit(f'reloc at {r_offset:#x}: section {ps["sname"].decode()} missing from the NOLOAD link')
            if ns['size'] != ps['size']:
                sys.exit(f'section {ps["sname"].decode()} differs in size between the links')
            delta = ns['addr'] - ps['addr']
            if delta:
                (v,) = struct.unpack_from('<I', data, off)
                struct.pack_into('<I', data, off, (v + delta) & 0xffffffff)
                moved += 1
    relocs.sort()
    data = bytes(data)
    shifted = sorted({s['sname'].decode() for s in secs if s['flags'] & 2 and s['sname'] in nsec_by_name
                      and nsec_by_name[s['sname']]['addr'] != s['addr']})
    if shifted:
        print(f'note: the PROGBITS link places {", ".join(shifted)} elsewhere; {moved} pointers moved to the NOLOAD link\'s addresses')

    # Extent: up to the next symbol (covers unsized data). But where a symbol's
    # declared size ends early and non-zero bytes follow, those bytes are data
    # with no name of their own -- the linker's merged string pool (Goddard's
    # object names) lands after the last symbol -- so each run of them becomes
    # its own entry (anon_<offset>) for the recipes to match on its own.
    raw = symbols(d, secs, byname, idx)
    syms = []
    for i, (name, value, declared) in enumerate(raw):
        nxt = raw[i + 1][1] if i + 1 < len(raw) else sec['addr'] + sec['size']
        tail = data[value + declared - sec['addr']:nxt - sec['addr']] if 0 < declared < nxt - value else b''
        if not any(tail):
            syms.append([name, value, nxt - value])
            continue
        syms.append([name, value, declared])
        j = 0
        while j < len(tail):
            if tail[j] == 0:
                j += 1
                continue
            k = j
            while k < len(tail) and tail[k] != 0:
                k += 1
            a0 = value + declared + j
            syms.append([f'anon_{a0 - sec["addr"]:07x}', a0, k - j])
            j = k
    nanon = sum(1 for s in syms if s[0].startswith('anon_'))
    names = b''
    symtab = b''
    for name, value, size in syms:
        symtab += struct.pack('<IIII', value - sec['addr'], size, zlib.crc32(name.encode()) & 0xffffffff, len(names))
        names += name.encode() + b'\x00'

    reltab = b''.join(struct.pack('<I', r) for r in relocs)
    # layout id: the region's internal layout (symbol offsets/sizes, reloc
    # offsets, size) -- NOT its address, which moves with every code change
    # while the recipes (region-relative) stay valid
    build_id = zlib.crc32(reltab + symtab + struct.pack('<I', sec['size'])) & 0xffffffff
    hdrlen = 44 + len(reltab) + len(symtab) + len(names)
    data_off = (hdrlen + a.page - 1) & ~(a.page - 1)
    # last word: the data offset when it is not the legacy 16-byte pad (0 there,
    # so the PSP's assets_load.c keeps computing it)
    hdr = struct.pack('<8sIIIIIIIII', b'SM64ASST', 1, sec['addr'], sec['size'], len(relocs), len(syms), len(names),
                      build_id, zlib.crc32(data) & 0xffffffff, data_off if a.page != 16 else 0)
    with open(a.o, 'wb') as f:
        f.write(hdr); f.write(reltab); f.write(symtab); f.write(names)
        f.write(b'\x00' * (data_off - f.tell()))
        f.write(data)
    print(f'{a.o}: region {sec["addr"]:#x}+{sec["size"]:#x} ({sec["size"] / 1048576:.1f} MB), '
          f'{len(relocs)} relocs, {len(syms)} symbols ({nanon} unnamed runs), build id {build_id:08x}')


if __name__ == '__main__':
    main()
