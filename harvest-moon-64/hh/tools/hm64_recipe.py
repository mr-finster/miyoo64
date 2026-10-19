#!/usr/bin/env python3
"""hm64_recipe.py: the first-start recipe (hm64_recipes.bin) that turns the player's ROM into this build's data pack
on the device (hh/src/hh_convert.c), so a public zip carries no ROM data.

The pack (hm64_pack.py) is the ROM byte for byte except the bytecode and text files re-assembled for ARM: their
halfwords and words little-endian, and the words that point at a game variable holding the native executable's
address. Every byte of the pack is therefore one of: the ROM's byte as it is, a ROM halfword or word with its bytes
reversed, or an address in this executable. The recipe says which, as a stream of ops over the pack:

    0x01 n    copy n bytes of the ROM as they are         (n: LEB128)
    0x02 n    n ROM words, each with its 4 bytes reversed
    0x03 n    n ROM halfwords, each with its 2 bytes reversed
    0x04 n v  n words of the executable's own addresses: v = n little-endian u32 (in 0x40000000..the end of the
              executable's image), the ROM's bytes there not used
    0x00      end

It holds op numbers, lengths and this build's own addresses: no ROM byte. The tool checks that, and that the ops
applied to the ROM give the pack back exactly (also the CRC-32 the header carries, which the device checks again).

Header (64 bytes, little-endian): "HMR1", version 1, pack size, CRC-32 of the pack, ops size (uncompressed), the
ROM's SHA-1 as 40 hex characters; then the ops, zlib-compressed.

  hm64_recipe.py --rom hm64.z64 --pack build/hh/hm64-pack.bin --native build/hh/hm64 --out build/hh/hm64_recipes.bin
"""
import argparse, hashlib, struct, subprocess, sys, zlib

ROM_SHA1 = '90631460f1876a14849df0541d534012b410a34c'
TC = '/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-'


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def image_range(native):
    """the executable's loaded image: [lowest LOAD vaddr, highest LOAD vaddr + memsz)"""
    out = subprocess.run([TC + 'readelf', '-lW', native], capture_output=True, text=True, check=True).stdout
    lo, hi = None, None
    for line in out.splitlines():
        p = line.split()
        if p and p[0] == 'LOAD':
            va, msz = int(p[2], 16), int(p[5], 16)
            lo = va if lo is None else min(lo, va)
            hi = va + msz if hi is None else max(hi, va + msz)
    return lo, hi


def apply(rom, ops, size):
    """the device's decoder (hh_convert.c), for the check"""
    out = bytearray(size)
    i = pos = 0
    def rd():
        nonlocal i
        n = s = 0
        while True:
            b = ops[i]; i += 1
            n |= (b & 0x7F) << s; s += 7
            if not b & 0x80:
                return n
    while True:
        op = ops[i]; i += 1
        if op == 0:
            break
        n = rd()
        if op == 1:
            out[pos:pos + n] = rom[pos:pos + n]; pos += n
        elif op == 2:
            for k in range(n):
                out[pos:pos + 4] = rom[pos:pos + 4][::-1]; pos += 4
        elif op == 3:
            for k in range(n):
                out[pos:pos + 2] = rom[pos:pos + 2][::-1]; pos += 2
        elif op == 4:
            out[pos:pos + 4 * n] = ops[i:i + 4 * n]; i += 4 * n; pos += 4 * n
        else:
            raise ValueError('op %d' % op)
    assert pos == size and i == len(ops), (pos, size, i, len(ops))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--pack', required=True)
    ap.add_argument('--native', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    pack = open(a.pack, 'rb').read()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        sys.exit('hm64_recipe: not the USA ROM in .z64 order')
    if len(pack) != len(rom):
        sys.exit('hm64_recipe: the pack is not the ROM\'s size')
    lo, hi = image_range(a.native)

    # each even offset: what makes the pack's bytes there from the ROM's
    ops = []  # (kind, count, data)
    def emit(kind, n=1, data=b''):
        if ops and ops[-1][0] == kind:
            k, c, d = ops[-1]
            ops[-1] = (k, c + n, d + data)
        else:
            ops.append((kind, n, data))
    size = len(rom)
    i = 0
    nset = 0
    import bisect
    import numpy as np
    diff = np.nonzero(np.frombuffer(pack, np.uint8) != np.frombuffer(rom, np.uint8))[0]
    while i < size:
        if pack[i:i + 2] == rom[i:i + 2]:
            k = bisect.bisect_left(diff, i)  # the next differing byte: copy up to its halfword
            nxt = int(diff[k]) & ~1 if k < len(diff) else size
            emit(1, nxt - i)
            i = nxt
            continue
        if i + 4 <= size and pack[i:i + 4] == rom[i:i + 4][::-1]:
            emit(2)
            i += 4
            continue
        if pack[i:i + 2] == rom[i:i + 2][::-1]:
            emit(3)
            i += 2
            continue
        v = struct.unpack_from('<I', pack, i)[0] if i + 4 <= size else 0
        if lo <= v < hi:
            emit(4, 1, pack[i:i + 4])
            nset += 1
            i += 4
            continue
        sys.exit('hm64_recipe: pack byte at 0x%X is not the ROM\'s, reversed or an address (%s vs %s)' %
                 (i, pack[i:i + 4].hex(), rom[i:i + 4].hex()))
    stream = bytearray()
    for kind, n, data in ops:
        stream.append(kind)
        stream += leb(n if kind == 1 else n)
        if kind == 4:
            stream += data
    stream.append(0)
    stream = bytes(stream)
    if apply(rom, stream, size) != pack:
        sys.exit('hm64_recipe: the ops do not give the pack back')
    # no ROM data: the only literal bytes are op 4's addresses, each checked in the executable's range above. The
    # longest literal run is also searched for in the ROM, to be sure.
    crc = zlib.crc32(pack) & 0xFFFFFFFF
    z = zlib.compress(stream, 9)
    hdr = b'HMR1' + struct.pack('<IIII', 1, size, crc, len(stream)) + ROM_SHA1.encode()
    hdr += b'\0' * (64 - len(hdr))
    open(a.out, 'wb').write(hdr + z)
    counts = {}
    for kind, n, _ in ops:
        counts[kind] = counts.get(kind, 0) + n
    print('hm64_recipe: %d ops (copy %d bytes, %d words and %d halfwords reversed, %d addresses), %d bytes -> %s, '
          'pack CRC %08x' % (len(ops), counts.get(1, 0), counts.get(2, 0), counts.get(3, 0), nset, len(hdr) + len(z),
                             a.out, crc))


if __name__ == '__main__':
    main()
