#!/usr/bin/env python3
"""tnt_inflate.py <rom.z64> <out.bin>: every asset of The New Tetris inflated and concatenated (image, anim, dcm and
sample tables: 'H2OS' LZO1X / 'H2ON' raw entries; the fx table's 1428 assets), for the release gate's leak scan only.
The output is ROM-derived: keep it out of git and out of the zip (the build writes it to a scratch file and deletes it)."""
import sys, struct
sys.path.insert(0, __import__('os').path.dirname(__file__))
rom = open(sys.argv[1], 'rb').read()
import tntrom
tntrom.ROM = rom
u16, u32 = tntrom.u16, tntrom.u32
out = []
for name, base in tntrom.LUTS.items():
    n = u16(rom, base)
    for i in range(n):
        try:
            out.append(tntrom.lut(base, i)[3])
        except Exception:
            pass
for i in range(tntrom.fx_count()):
    out.append(tntrom.fx_entry(i)[5])
open(sys.argv[2], 'wb').write(b''.join(out))
print('inflated %d assets, %d bytes' % (len(out), sum(map(len, out))))
