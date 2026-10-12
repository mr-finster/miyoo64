#!/usr/bin/env python3
"""Build assets.bin: the ROM-derived data of the port, taken OUT of the EBOOT.

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
    if rel is not None:
        for i in range(rel['size'] // 8):
            r_offset, r_info = struct.unpack_from('<II', d, rel['offset'] + i * 8)
            if (r_info & 0xff) != abs32:
                sys.exit(f'unexpected relocation type {r_info & 0xff} at {r_offset:#x}')
            relocs.append(r_offset - sec['addr'])
    relocs.sort()

    syms = symbols(d, secs, byname, idx)
    for i, sym in enumerate(syms):
        nxt = syms[i + 1][1] if i + 1 < len(syms) else sec['addr'] + sec['size']
        sym[2] = nxt - sym[1]
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
    hdr = struct.pack('<8sIIIIIIIII', b'MK64ASST', 1, sec['addr'], sec['size'], len(relocs), len(syms), len(names),
                      build_id, zlib.crc32(data) & 0xffffffff, data_off if a.page != 16 else 0)
    with open(a.o, 'wb') as f:
        f.write(hdr); f.write(reltab); f.write(symtab); f.write(names)
        f.write(b'\x00' * (data_off - f.tell()))
        f.write(data)
    print(f'{a.o}: region {sec["addr"]:#x}+{sec["size"]:#x} ({sec["size"] / 1048576:.1f} MB), '
          f'{len(relocs)} relocs, {len(syms)} symbols, build id {build_id:08x}')


if __name__ == '__main__':
    main()
