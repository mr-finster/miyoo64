#!/usr/bin/env python3
"""Compare a runtime dump of the .hhasset region (SF64_HHDUMP) with the bytes
the recipe link (PROGBITS) holds there.

usage: check_hhdump.py <dump> <port ELF (PROGBITS link)>
"""
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from elf32 import Elf32

dump = open(sys.argv[1], 'rb').read()
sec = Elf32(sys.argv[2]).section('.hhasset')
E = Elf32(sys.argv[2])
want = E.data[sec['offset']:sec['offset'] + sec['size']]
if len(dump) != len(want):
    sys.exit(f'check_hhdump: sizes differ: dump {len(dump)}, link {len(want)}')
bad = [i for i in range(len(want)) if dump[i] != want[i]]
if bad:
    sys.exit(f'check_hhdump: {len(bad)} bytes differ, first at 0x{sec["addr"] + bad[0]:08x}')
print(f'check_hhdump: {len(want)} bytes at 0x{sec["addr"]:08x} identical')
