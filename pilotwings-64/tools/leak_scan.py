#!/usr/bin/env python3
"""Leak scan (pw64-miyoo, from drm-miyoo's): tools/leak_scan.py <rom.z64> <budget-for-exe> <exe> [other files...]

Every 32-byte block (with at least 8 distinct byte values) of the ROM and of every MIO0 chunk in it decompressed
(Pilotwings 64's GZIP file-system chunks: levels, textures, models), each also with 16- and 32-bit units swapped (the
port's host order), against every byte offset of each shipped file. The first shipped file (the executable) may hold up
to <budget> such windows; every other file none."""
import struct, sys
import numpy as np

M = np.uint64(0x9E3779B97F4A7C15)
def h4(q):
    x = q[:, 0] * M; x ^= q[:, 1]; x *= M; x ^= q[:, 2]; x *= M; x ^= q[:, 3]; x *= M
    return x

def blocks(raw):
    n = len(raw) // 32 * 32
    if n == 0:
        return np.zeros(0, np.uint64)
    b = np.frombuffer(raw[:n], np.uint8).reshape(-1, 32)
    srt = np.sort(b, axis=1)
    distinct = 1 + (np.diff(srt.astype(np.int16), axis=1) != 0).sum(1)
    return h4(np.frombuffer(raw[:n], '<u8').reshape(-1, 4)[distinct >= 8])

def mio0(rom, i):
    size, comp, raw = struct.unpack('>III', rom[i + 4:i + 16])
    if size == 0 or size > 0x400000 or comp >= 0x400000 or raw >= 0x400000:
        return None
    out = bytearray(); bit = i + 16; ci = i + comp; ri = i + raw; word = 0; left = 0
    try:
        while len(out) < size:
            if left == 0:
                word = struct.unpack('>I', rom[bit:bit + 4])[0]; bit += 4; left = 32
            if word & 0x80000000:
                out.append(rom[ri]); ri += 1
            else:
                v = (rom[ci] << 8) | rom[ci + 1]; ci += 2
                n, back = (v >> 12) + 3, (v & 0xFFF) + 1
                if back > len(out):
                    return None
                for _ in range(n):
                    out.append(out[-back])
            word = (word << 1) & 0xFFFFFFFF; left -= 1
    except IndexError:
        return None
    return bytes(out[:size])

def inflated_assets(rom):
    out, i = [], rom.find(b'MIO0')
    while i >= 0:
        d = mio0(rom, i)
        if d is not None and len(d) >= 512:
            out.append(d)
        i = rom.find(b'MIO0', i + 4)
    return out, len(out)

def swapped(raw, w):
    n = len(raw) // w * w
    return np.frombuffer(raw[:n], np.uint8).reshape(-1, w)[:, ::-1].tobytes()
rom = open(sys.argv[1], 'rb').read()
if rom[:2] == b'\x37\x80':
    rom = swapped(rom, 2)
elif rom[:2] == b'\x40\x12':
    rom = swapped(rom, 4)
budget = int(sys.argv[2])
srcs = [rom]
assets, n = inflated_assets(rom)
srcs += assets
# the port swaps words and halfwords to host order: look for those too
srcs += [swapped(s, w) for s in list(srcs) for w in (2, 4)]
H = np.unique(np.concatenate([blocks(s) for s in srcs]))
print('  sources: ROM, %d MIO0 chunks: %d distinct blocks' % (n, len(H)))
bad = False
for k, f in enumerate(sys.argv[3:]):
    d = open(f, 'rb').read()
    hits = 0
    for o in range(8):
        m = (len(d) - o) // 8
        if m < 4:
            continue
        w = np.frombuffer(d[o:o + m * 8], '<u8')
        q = np.stack([w[i:len(w) - 3 + i] for i in range(4)], 1)
        hits += int(np.isin(h4(q), H).sum())
    b = budget if k == 0 else 0
    print('  %s: %d ROM blocks (budget %d)' % (f.rsplit('/', 1)[-1], hits, b))
    bad |= hits > b
sys.exit(1 if bad else 0)
