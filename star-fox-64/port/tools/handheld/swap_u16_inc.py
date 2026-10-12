#!/usr/bin/env python3
"""Byte-swap the 16-bit (or, for .rgba32 files, 32-bit) hex literals of a
Torch-generated .inc.c file (tool from the MK64 handheld port).

Torch emits RGBA16/IA16 textures and TLUTs as `u16 name[] = { 0x1234, ... }`.
Compiled for a little-endian CPU those land byte-swapped in memory, but the
renderer's texture importers read texel data big-endian, byte for byte, like
the N64 RDP. The build shadows those .inc.c files with swapped copies so the
bytes in memory match the ROM.

RGBA32 textures are `u32 name[] = { 0xRRGGBBAA, ... }`: the same problem a
word wide. Unswapped, the importer read each texel's red as its alpha: the
explosion fire drew pink with its transparent edges as a grey triangle.
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()

wide = src.endswith('.rgba32.inc.c')

def swap(m):
    v = int(m.group(1), 16)
    if wide:
        if v > 0xFFFFFFFF:
            raise SystemExit(f"{src}: value {m.group(0)} is not 32-bit")
        return "0x%08X" % int.from_bytes(v.to_bytes(4, 'big'), 'little')
    if v > 0xFFFF:
        raise SystemExit(f"{src}: value {m.group(0)} is not 16-bit")
    return "0x%04X" % (((v & 0xFF) << 8) | (v >> 8))

open(dst, "w").write(re.sub(r"0x([0-9A-Fa-f]+)", swap, text))
