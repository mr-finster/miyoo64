#!/usr/bin/env python3
"""State-file tool (kit k1.5 format, 64-byte header): ssx.py expand <sparse> <out> [stamp]: a sparse state as a full-size one (header flag off,
every page written, missing pages zero); with stamp, the header's build id is rewritten too.
ssx.py restamp <state> <out> <stamp>: only the build id. ssx.py info <state>."""
import struct, sys
HDR = struct.Struct('<II3I3I32s')
def load(p):
    b = open(p, 'rb').read()
    magic, nr, s0, s1, s2, z0, z1, z2, build = HDR.unpack_from(b, 0)
    return b, magic, nr, [s0, s1, s2], [z0, z1, z2], build
def hdr(magic, nr, st, sz, build):
    return HDR.pack(magic, nr, *st, *sz, build)
def bid(stamp): return (b'oot build %08X' % int(stamp, 16)).ljust(32, b'\0')
cmd = sys.argv[1]
b, magic, nr, st, sz, build = load(sys.argv[2])
n = nr & 0xFF
if cmd == 'info':
    print('nranges %d sparse %d sizes %s build %s file %d' % (n, bool(nr & 0x100), sz[:n], build.rstrip(b'\0').decode(), len(b)))
    if nr & 0x100:
        at = HDR.size
        for i in range(n):
            np_ = (sz[i] + 4095) // 4096; nb = (np_ + 7) // 8
            bm = b[at:at + nb]; kept = sum(1 for p in range(np_) if bm[p >> 3] >> (p & 7) & 1)
            print('  range %d: %d of %d pages kept (%.1f%%)' % (i, kept, np_, 100.0 * kept / np_))
            at += nb + sum(min(4096, sz[i] - p * 4096) for p in range(np_) if bm[p >> 3] >> (p & 7) & 1)
    sys.exit(0)
if cmd == 'restamp':
    open(sys.argv[3], 'wb').write(hdr(magic, nr, st, sz, bid(sys.argv[4])) + b[HDR.size:]); sys.exit(0)
assert cmd == 'expand' and nr & 0x100
out = bytearray(); at = HDR.size
for i in range(n):
    np_ = (sz[i] + 4095) // 4096; nb = (np_ + 7) // 8
    bm = b[at:at + nb]; at += nb
    for p in range(np_):
        ln = min(4096, sz[i] - p * 4096)
        if bm[p >> 3] >> (p & 7) & 1: out += b[at:at + ln]; at += ln
        else: out += bytes(ln)
assert at == len(b), (at, len(b))
nb_ = bid(sys.argv[4]) if len(sys.argv) > 4 else build
open(sys.argv[3], 'wb').write(hdr(magic, n, st, sz, nb_) + bytes(out))
