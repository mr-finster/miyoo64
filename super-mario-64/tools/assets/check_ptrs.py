#!/usr/bin/env python3
"""Every code pointer in the asset data must land on a function of the executable that ships.

usage: check_ptrs.py <assets.bin> <executable ELF (unstripped)>

The data's pointer words are taken from a second (PROGBITS) link of the game. When that link
places the code elsewhere (2026-10-06: a region on a 64 KB boundary gave it one more program
header, and the code moved by 32 bytes), every function pointer in the level scripts, behaviours
and geo layouts pointed into the middle of some function, and the game hung after boot.
make_assets.py now moves each pointer to the shipped link's addresses; this check refuses an
assets.bin whose code pointers don't all start a function (or a named code label) of the
executable. Exits 1 with the first few bad ones."""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from make_assets import parse_elf

ad = open(sys.argv[1], 'rb').read()
magic, ver, addr, size, nrel, nsym, nnames, bid, crc, pad = struct.unpack_from('<8sIIIIIIIII', ad, 0)
assert magic == b'SM64ASST', 'not an assets.bin'
relocs = struct.unpack_from('<%dI' % nrel, ad, 44)
off = pad if pad else (44 + nrel * 4 + nsym * 16 + nnames + 15) & ~15
data = ad[off:off + size]

d, secs, byname = parse_elf(sys.argv[2])
text = byname[b'.text']
lo, hi = text['addr'], text['addr'] + text['size']
st = byname[b'.symtab']
starts = set()
for i in range(st['size'] // 16):
    name, value, ssize, info, other, shndx = struct.unpack_from('<IIIBBH', d, st['offset'] + i * 16)
    if lo <= (value & ~1) < hi:
        starts.add(value & ~1)
bad, n = [], 0
for r in relocs:
    (v,) = struct.unpack_from('<I', data, r)
    if lo <= (v & ~1) < hi:
        n += 1
        if (v & ~1) not in starts:
            bad.append((r, v))
if bad:
    for r, v in bad[:10]:
        print(f'  region+{r:#x}: {v:#010x} is inside .text but starts no function')
    sys.exit(f'ERROR: {len(bad)} of {n} code pointers in the asset data start no function of the executable')
print(f'{n} code pointers, each at a function of the executable')
