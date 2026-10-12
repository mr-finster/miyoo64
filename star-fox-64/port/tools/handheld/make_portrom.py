#!/usr/bin/env python3
"""Build the port ROM image (desktop / development use only; never shipped).

usage: make_portrom.py <decompressed baserom> <N64 ELF> <bank dir> <banks.mk> <out>

The image is the decompressed ROM with every asset bank's range replaced by
the port's build of that bank (<bank dir>/<name>.bin: native byte order, same
size and symbol addresses as the N64 bank). Offsets are unchanged, so the
game's DMA table and *_ROM_START symbols address it directly (port_rom.c).
On the device the same image is built at first boot from the player's ROM.
"""
import re, subprocess, sys

rom_path, elf_path, bank_dir, banks_mk, out_path = sys.argv[1:6]
rom = bytearray(open(rom_path, 'rb').read())

mk = open(banks_mk).read()
banks = mk.split('BANKS := ', 1)[1].split('\n', 1)[0].split()
size = {m.group(1): int(m.group(2)) for m in re.finditer(r'^BANK_SIZE_(\w+) := (\d+)$', mk, re.M)}

syms = {}
for line in subprocess.run(['mips-linux-gnu-nm', elf_path], capture_output=True, text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

replaced = 0
for b in banks:
    data = open(f'{bank_dir}/{b}.bin', 'rb').read()
    start = syms[b + '_ROM_START']
    if len(data) != size[b] or start + len(data) > len(rom):
        sys.exit(f'make_portrom: {b}: {len(data)} bytes, expected {size[b]}')
    rom[start:start + len(data)] = data
    replaced += len(data)

open(out_path, 'wb').write(rom)
print(f'make_portrom: {len(banks)} banks ({replaced} bytes) replaced in a {len(rom)}-byte image')
