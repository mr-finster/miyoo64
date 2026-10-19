#!/usr/bin/env python3
"""The first-start recipe (mm_recipes.bin): how to rebuild the data pack on the device from the player's own ROM.

The pack (mm_pack.py) keeps the decompressed ROM's vrom layout; its native files are the ROM's bytes in another
order. So the device rebuilds it as: the decompressed ROM image (every dmadata file Yaz0-decompressed at its vrom,
zeros elsewhere), then one op per 8-byte block (hh/src/hh_recipe_ops.h, the Ocarina of Time port's format):
  0..55 one of the 56 byte permutations, 56 zero, 57 two word ops follow;
  word op: 0..23 a permutation of the source word's bytes, 24 zero, 27 (MM) the N64 Sample header word
  (u32 bit-fields unk_0:1 codec:3 medium:2 unk_bit26:1 isRelocated:1 size:24, MSB first) as the handheld's compiler
  lays them out (LSB first). No literal: the recipe holds op numbers only (checked: no literal is ever emitted),
  so it carries no ROM data. Blocks the game never reads (the ROM header's checksum, dmadata, which the port's DMA
  does not use) take the identity op where nothing else matches; the CRC-32 in the header is of the pack the
  device will build (the reconstruction below), so the device checks its result.

File: "MMR1", u32 version 1, u32 pack size, u32 dmadata rom offset, u32 crc32 of the rebuilt pack, u32 op count,
char rom_sha1[40], then the ops, zlib-compressed. All little-endian.

usage: mm_recipes.py <rom.z64 (big-endian, the supported dump)> <pack> <out mm_recipes.bin> [--dmadata 0x1A500]
"""
import argparse
import hashlib
import itertools
import struct
import sys
import zlib

import numpy as np


def yaz0(data):
    try:
        import crunch64
        return crunch64.yaz0.decompress(bytes(data))
    except ImportError:
        pass
    assert data[:4] == b"Yaz0"
    size = struct.unpack(">I", data[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = data[i]
        i += 1
        for b in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> b):
                out.append(data[i])
                i += 1
            else:
                b1, b2 = data[i], data[i + 1]
                i += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def decompressed_image(rom, dma_off, size):
    img = bytearray(size)
    o = dma_off
    files = 0
    last = 0
    while True:
        vs, ve, ps, pe = struct.unpack(">IIII", rom[o:o + 16])
        o += 16
        if vs == 0 and ve == 0 and ps == 0 and pe == 0 and files > 0:
            break
        files += 1
        if ps == 0xFFFFFFFF and pe == 0xFFFFFFFF:
            continue  # not present
        if ve <= vs:
            continue
        if pe != 0:
            d = yaz0(rom[ps:pe])[:ve - vs]
        else:
            d = rom[ps:ps + (ve - vs)]
        img[vs:vs + len(d)] = d
        last = max(last, ve)
    return bytes(img), files, last


def p8_perms():
    out = []

    def gen(rem, parts):
        if rem == 0:
            o, m = 0, []
            for w in parts:
                m += [o + j for j in range(w - 1, -1, -1)]
                o += w
            out.append(m)
            return
        for w in (8, 4, 2, 1):
            if w <= rem:
                gen(rem - w, parts + [w])

    gen(8, [])
    assert len(out) == 56
    return out


def p4_perms():
    # the order of hh_recipe_ops.h's hhr_perms: a, b, c, d nested, all distinct
    return [[a, b, c, d] for a in range(4) for b in range(4) for c in range(4) for d in range(4)
            if len({a, b, c, d}) == 4]


def sample_word(src4):
    w = (src4[:, 0].astype(np.uint32) << 24) | (src4[:, 1].astype(np.uint32) << 16) | \
        (src4[:, 2].astype(np.uint32) << 8) | src4[:, 3].astype(np.uint32)
    v = ((w >> 31) & 1) | (((w >> 28) & 7) << 1) | (((w >> 26) & 3) << 4) | (((w >> 25) & 1) << 6) | \
        (((w >> 24) & 1) << 7) | ((w & 0xFFFFFF) << 8)
    return np.stack([(v >> s) & 0xFF for s in (0, 8, 16, 24)], axis=1).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("pack")
    ap.add_argument("out")
    ap.add_argument("--dmadata", type=lambda s: int(s, 0), default=0x1A500)
    a = ap.parse_args()
    rom = open(a.rom, "rb").read()
    if rom[:4] != b"\x80\x37\x12\x40":
        sys.exit("the ROM must be big-endian (.z64)")
    sha1 = hashlib.sha1(rom).hexdigest()
    pack = open(a.pack, "rb").read()
    src, nfiles, lastEnd = decompressed_image(rom, a.dmadata, len(pack))
    R = np.frombuffer(src, dtype=np.uint8).reshape(-1, 8)
    N = np.frombuffer(pack, dtype=np.uint8).reshape(-1, 8)
    nb = len(R)
    p8, p4 = p8_perms(), p4_perms()
    ident = p8.index(list(range(8)))
    op = np.full(nb, 255, np.uint8)
    done = np.zeros(nb, bool)
    # identity first (the most common: best compression), then the rest
    order = [ident] + [i for i in range(56) if i != ident]
    for i in order:
        hit = (~done) & np.all(R[:, p8[i]] == N, axis=1)
        op[hit] = i
        done |= hit
    z = (~done) & np.all(N == 0, axis=1)
    op[z] = 56
    done |= z
    rest = np.where(~done)[0]
    words = np.full((len(rest), 2), 255, np.uint8)
    Rw = R[rest].reshape(-1, 2, 4)
    Nw = N[rest].reshape(-1, 2, 4)
    for k in range(2):
        got = np.zeros(len(rest), bool)
        for i, m in enumerate(p4):
            hit = (~got) & np.all(Rw[:, k, m] == Nw[:, k], axis=1)
            words[hit, k] = i
            got |= hit
        z = (~got) & np.all(Nw[:, k] == 0, axis=1)
        words[z, k] = 24
        got |= z
        sw = sample_word(Rw[:, k])
        hit = (~got) & np.all(sw == Nw[:, k], axis=1)
        words[hit, k] = 27
        got |= hit
    # blocks no op rebuilds: only the ROM header, dmadata and the padding after the last file may be left (identity there; the game never reads them)
    bad = rest[np.any(words == 255, axis=1)]
    allowed = lambda b: b * 8 < 0x40 or a.dmadata <= b * 8 < a.dmadata + 0x8000 or b * 8 >= lastEnd
    unexpected = [b for b in bad if not allowed(b)]
    if unexpected:
        sys.exit("no op rebuilds %d blocks, e.g. at %s" % (len(unexpected), ", ".join(hex(b * 8) for b in unexpected[:8])))
    split = np.ones(len(rest), bool)
    for j, b in enumerate(rest):
        if np.any(words[j] == 255):
            op[b] = ident
            split[j] = False
        else:
            op[b] = 57
    # the stream: one op per block, a split's two word ops right after it
    stream = bytearray()
    wmap = {int(b): words[j] for j, b in enumerate(rest) if split[j]}
    opb = op.tobytes()
    last = 0
    for b in sorted(wmap):
        stream += opb[last:b + 1]
        stream += bytes(wmap[b])
        last = b + 1
    stream += opb[last:]
    # the reconstruction the device makes (the decoder's rules), its CRC
    rebuilt = np.empty_like(R)
    for i in range(56):
        m = op == i
        rebuilt[m] = R[m][:, p8[i]]
    rebuilt[op == 56] = 0
    for b, w in wmap.items():
        for k in range(2):
            s = R[b, k * 4:k * 4 + 4]
            if w[k] < 24:
                rebuilt[b, k * 4:k * 4 + 4] = s[p4[w[k]]]
            elif w[k] == 24:
                rebuilt[b, k * 4:k * 4 + 4] = 0
            else:
                rebuilt[b, k * 4:k * 4 + 4] = sample_word(s.reshape(1, 4))[0]
    rb = rebuilt.tobytes()
    differs = sum(1 for b in range(0, len(pack), 8) if rb[b:b + 8] != pack[b:b + 8])
    crc = zlib.crc32(rb) & 0xFFFFFFFF
    comp = zlib.compress(bytes(stream), 9)
    hdr = b"MMR1" + struct.pack("<IIIII", 1, len(pack), a.dmadata, crc, len(stream)) + sha1.encode()
    open(a.out, "wb").write(hdr + comp)
    print("mm_recipes: %d files, %d blocks, %d split, %d left as they are (header/dmadata), ops %d -> %d bytes, crc %08x"
          % (nfiles, nb, int(split.sum()), len(bad), len(stream), len(comp), crc))
    print("mm_recipes: rebuilt pack differs from the build's in %d blocks (header/dmadata only)" % differs)


if __name__ == "__main__":
    main()
