#!/usr/bin/env python3
"""Leak scan: tools/leak_scan.py <rom.z64> <decompressed.z64> <budget-for-exe> <exe> [other shipped files...]

Every 32-byte block (with at least 8 distinct byte values) of the ROM, of the decompressed ROM (the decomp's
bk_rom_decompress: core1/core2/overlays inflated) and of every rarezip-packed asset inflated (11 72 headers in the
asset area), each also with 16- and 32-bit units swapped (the port's host order), against every byte offset of each shipped file. The first shipped file (the executable) may hold up to
<budget> such windows (tables the decompilation's own C sources compile to); every other file none."""
import sys, zlib
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

def inflated_assets(rom):
    out, i, n = [], 0, 0
    while True:
        i = rom.find(b'\x11\x72', i)
        if i < 0:
            break
        try:
            d = zlib.decompressobj(wbits=-15).decompress(rom[i + 6:i + 6 + 0x100000])
            if len(d) >= 64:
                out.append(d); n += 1
        except zlib.error:
            pass
        i += 2
    return out, n

rom = open(sys.argv[1], 'rb').read()
dec = open(sys.argv[2], 'rb').read()
budget = int(sys.argv[3])
srcs = [rom, dec]
assets, n = inflated_assets(rom)
srcs += assets
def swapped(raw, w):
    n = len(raw) // w * w
    return np.frombuffer(raw[:n], np.uint8).reshape(-1, w)[:, ::-1].tobytes()
# the port swaps words and halfwords to host order: look for those too
srcs += [swapped(s, w) for s in list(srcs) for w in (2, 4)]
H = np.unique(np.concatenate([blocks(s) for s in srcs]))
print('  sources: ROM, decompressed ROM, %d inflated assets: %d distinct blocks' % (n, len(H)))
bad = False
for k, f in enumerate(sys.argv[4:]):
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
