#!/usr/bin/env python3
"""Derive the asset-bank recipes: how each port bank is built from the ROM.

usage: derive_recipes.py <decompressed baserom> <N64 ELF> <bank dir> <banks.mk> <out recipes.bin>
                         [--radio <port radio bin> <port address>]

Each port bank (<bank dir>/<name>.bin, ARM-built, same size and symbol
addresses as the N64 bank) differs from the ROM's copy of that bank only in
byte order: every field is the same value, stored little-endian. So a bank is
described completely by runs of (length, swap unit), unit 1 = copy, 2 = swap
16-bit halves, 4 = swap 32-bit words. The recipe carries those numbers and
nothing else -- no byte of the ROM -- and the device replays it on the
player's decompressed ROM (port_romimage.c).

Every bank is verified here: replaying the recipe on the ROM must give the
port bank exactly, or the build fails with the first mismatching offset.

recipes.bin layout (little-endian u32 words):
  magic "SF64RCP2", u32 bank count, then per bank:
    u32 rom_start, u32 size, u32 run count, runs as u32 (length << 3 | unit)
  then the message bank (ast_radio), linked into the executable's NOLOAD
  region on the port and at a RAM address on the N64, so its pointers move:
    u32 rom_start, u32 size, u32 n64_base, u32 port_base, u32 run count, runs
  where unit 5 = swap the 32-bit word, then add (port_base - n64_base).
  The device checks port_base against its own region before replaying.
"""
import re, struct, subprocess, sys

rom_path, elf_path, bank_dir, banks_mk, out_path = sys.argv[1:6]
radio = None
if len(sys.argv) > 6:
    if sys.argv[6] != '--radio' or len(sys.argv) != 9:
        sys.exit('usage: ... --radio <port radio bin> <port address>')
    radio = (sys.argv[7], int(sys.argv[8], 0))
rom = open(rom_path, 'rb').read()
mk = open(banks_mk).read()
banks = mk.split('BANKS := ', 1)[1].split('\n', 1)[0].split()
size = {m.group(1): int(m.group(2)) for m in re.finditer(r'^BANK_SIZE_(\w+) := (\d+)$', mk, re.M)}
syms = {}
for line in subprocess.run(['mips-linux-gnu-nm', elf_path], capture_output=True, text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        # binutils built with a 64-bit BFD print MIPS32 addresses sign-extended
        # (ffffffff80179010); the N64's are 32-bit
        syms[p[2]] = int(p[0], 16) & 0xFFFFFFFF


DELTA = 0


def apply(src, unit):
    if unit == 5:  # pointer: swap, then relocate into the port's region
        # ROM word is big-endian (the N64 pointer); the port stores it little-endian
        return b''.join(struct.pack('<I', (struct.unpack('>I', src[i:i + 4])[0] + DELTA) & 0xFFFFFFFF)
                        for i in range(0, len(src), 4))
    if unit == 1:
        return src
    if unit == 2:
        return bytes(src[i ^ 1] for i in range(len(src)))
    return bytes(src[(i & ~3) | (3 - (i & 3))] for i in range(len(src)))


def derive(src, dst):
    """Runs of (length, unit) at 2-byte granularity (4-byte for unit 4)."""
    runs = []
    i, n = 0, len(dst)
    while i < n:
        # candidate units for the chunk at i, preferring the current run's unit
        cands = []
        if i % 4 == 0 and i + 4 <= n and apply(src[i:i + 4], 4) == dst[i:i + 4]:
            cands.append((4, 4))
        if DELTA and i % 4 == 0 and i + 4 <= n and apply(src[i:i + 4], 5) == dst[i:i + 4]:
            cands.append((5, 4))
        if i + 2 <= n and apply(src[i:i + 2], 2) == dst[i:i + 2]:
            cands.append((2, 2))
        if src[i:i + 2] == dst[i:i + 2]:
            cands.append((1, 2))
        if not cands and i + 1 == n and src[i] == dst[i]:
            cands.append((1, 1))
        if not cands:
            raise SystemExit(f'no byte-order explains offset 0x{i:X}')
        prev = runs[-1][1] if runs else None
        unit, step = next((c for c in cands if c[0] == prev), cands[0])
        if runs and runs[-1][1] == unit:
            runs[-1][0] += step
        else:
            runs.append([step, unit])
        i += step
    return runs


out = bytearray(b'SF64RCP2' + struct.pack('<I', len(banks)))
total_runs = 0
for b in banks:
    start, n = syms[b + '_ROM_START'], size[b]
    src, dst = rom[start:start + n], open(f'{bank_dir}/{b}.bin', 'rb').read()
    try:
        runs = derive(src, dst)
    except SystemExit as e:
        sys.exit(f'derive_recipes: {b}: {e}')
    # replay check: exactly the port bank, from the ROM and the numbers alone
    pos, rebuilt = 0, bytearray()
    for length, unit in runs:
        rebuilt += apply(src[pos:pos + length], unit)
        pos += length
    if bytes(rebuilt) != dst:
        sys.exit(f'derive_recipes: {b}: replay mismatch')
    out += struct.pack('<III', start, n, len(runs))
    out += b''.join(struct.pack('<I', (length << 3) | unit) for length, unit in runs)
    total_runs += len(runs)

radio_msg = 'no radio section'
if radio is not None:
    rbin, port_base = open(radio[0], 'rb').read(), radio[1]
    start = syms['ast_radio_ROM_START']
    n64_base = syms['ast_radio_VRAM']
    src = rom[start:start + len(rbin)]
    DELTA = (port_base - n64_base) & 0xFFFFFFFF
    try:
        runs = derive(src, rbin)
    except SystemExit as e:
        sys.exit(f'derive_recipes: ast_radio: {e}')
    pos, rebuilt = 0, bytearray()
    for length, unit in runs:
        rebuilt += apply(src[pos:pos + length], unit)
        pos += length
    if bytes(rebuilt) != rbin:
        sys.exit('derive_recipes: ast_radio: replay mismatch')
    out += struct.pack('<IIIII', start, len(rbin), n64_base, port_base, len(runs))
    out += b''.join(struct.pack('<I', (length << 3) | unit) for length, unit in runs)
    radio_msg = f'radio {len(rbin)} bytes in {len(runs)} runs ({sum(1 for _, u in runs if u == 5)} pointer runs)'
else:
    out += struct.pack('<IIIII', 0, 0, 0, 0, 0)

open(out_path, 'wb').write(out)
print(f'derive_recipes: {len(banks)} banks, {total_runs} runs, {radio_msg}, {len(out)} bytes, all replay exactly')
