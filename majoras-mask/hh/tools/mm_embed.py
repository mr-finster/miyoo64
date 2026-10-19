#!/usr/bin/env python3
"""ROM data compiled into the game's code (actor overlays' and the code segment's textures, the debug font...): the
decompilation builds them from the ROM, so the executable that ships must not carry them.

Every data symbol of the Miyoo executable whose bytes are exactly the ROM's bytes for that symbol (found by name and
size in the matching N64 ELF; its place in the ROM from its segment's vram/vrom) is zeroed in the shipped file, and an
entry {address, size (bit 31: in read-only data), offset in the data pack, CRC-32} goes into hh_embed_table (hh/src/hh_mm.c), which the game reads
at boot to copy the bytes back from the data pack (the decompressed ROM's layout, where code and overlays are the
ROM's bytes). The arrays stay where they are, so the layout (and save states) do not change; the table holds no ROM
data. A PERSONAL build need not be stripped: its arrays are then copied over with the same bytes.

  mm_embed.py list  <miyoo elf> <n64 elf> <pack>                       the entries, and what they cover
  mm_embed.py strip <miyoo elf> <n64 elf> <pack> <in exe> <out exe>    <in exe>: the stripped executable
  mm_embed.py check <miyoo elf> <shipped exe> <decompressed rom>       no 32-byte window of the ROM left in any
                                                                       texture, palette or font of the shipped file
"""
import struct
import subprocess
import sys
import zlib

TC = "/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-"
TABLE = "hh_embed_table"
MAGIC = b"HHEM"
MAXN = 4096  # entries hh_embed_table has room for (hh_mm.c)


def nm(elf, tool):
    out = subprocess.run([tool, "-S", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms.append((int(p[0], 16), int(p[1], 16), p[2], p[3]))
        elif len(p) == 3:
            syms.append((int(p[0], 16), 0, p[1], p[2]))
    return syms


def sections(elf):
    """(name, addr, file offset, size, type) of every allocated section"""
    out = subprocess.run([TC + "readelf", "-SW", elf], capture_output=True, text=True, check=True).stdout
    res = []
    for line in out.splitlines():
        line = line.replace("[ ", "[")
        p = line.split()
        if len(p) > 7 and p[0].startswith("[") and p[0] != "[Nr]":
            try:
                res.append((p[1], int(p[3], 16), int(p[4], 16), int(p[5], 16), p[2]))
            except ValueError:
                pass
    return res


def file_off(secs, addr, size):
    for name, a, off, sz, typ in secs:
        if typ != "NOBITS" and a <= addr and addr + size <= a + sz and a != 0:
            return off + addr - a, name
    return None, None


def entries(elf, n64elf, pack):
    pk = open(pack, "rb").read()
    n64 = nm(n64elf, TC + "nm")
    seg = {}
    for a, sz, t, name in n64:
        if name.startswith("_") and name.endswith("SegmentStart"):
            seg.setdefault(name[1:-len("SegmentStart")], {})["vram"] = a
        elif name.startswith("_") and name.endswith("SegmentEnd") and not name.endswith("BssEnd"):
            seg.setdefault(name[1:-len("SegmentEnd")], {})["vend"] = a
        elif name.startswith("_") and name.endswith("SegmentRomStart"):
            seg.setdefault(name[1:-len("SegmentRomStart")], {})["rom"] = a
    segs = [(v["vram"], v["vend"], v["rom"], k) for k, v in seg.items() if {"vram", "vend", "rom"} <= v.keys() and v["vend"] > v["vram"] >= 0x80000000]
    byname = {}
    for a, sz, t, name in n64:
        if sz >= 32 and t in "dDrRtTbB":
            byname.setdefault((name, sz), []).append(a)
    secs = sections(elf)
    data = open(elf, "rb").read()
    out = []
    for a, sz, t, name in nm(elf, TC + "nm"):
        if sz < 32 or t not in "dDrR" or (name, sz) not in byname:
            continue
        fo, sec = file_off(secs, a, sz)
        if fo is None:
            continue
        mine = data[fo:fo + sz]
        if len(set(mine)) < 4:
            continue
        for va in byname[(name, sz)]:
            src = next(((va - s + r) for s, e, r, k in segs if s <= va and va + sz <= e), None)
            if src is not None and pk[src:src + sz] == mine:
                out.append((a, sz, src, zlib.crc32(mine) & 0xFFFFFFFF, name, sec))
                break
    return out


def check(elf, exe, rom):
    import bisect
    import re
    import numpy as np
    M = np.uint64(0x9E3779B97F4A7C15)

    def h4(q):
        x = q[:, 0] * M; x ^= q[:, 1]; x *= M; x ^= q[:, 2]; x *= M; x ^= q[:, 3]; x *= M
        return x
    raw = open(rom, "rb").read()
    n = len(raw) // 32 * 32
    b = np.frombuffer(raw[:n], np.uint8).reshape(-1, 32)
    distinct = 1 + (np.diff(np.sort(b, axis=1).astype(np.int16), axis=1) != 0).sum(1)
    H = np.unique(h4(np.frombuffer(raw[:n], "<u8").reshape(-1, 4)[distinct >= 8]))
    d = open(exe, "rb").read()
    offs = []
    for o in range(8):
        m = (len(d) - o) // 8
        w = np.frombuffer(d[o:o + m * 8], "<u8")
        q = np.stack([w[i:len(w) - 3 + i] for i in range(4)], 1)
        offs += [o + 8 * int(i) for i in np.nonzero(np.isin(h4(q), H))[0]]
    secs = [s for s in sections(exe) if s[1]]
    syms = sorted((a, sz, name) for a, sz, t, name in nm(elf, TC + "nm"))
    starts = [s[0] for s in syms]
    art = re.compile(r"(Tex|TLUT|Tlut|Pal|Font)(_|$|[0-9A-F])")
    bad = {}
    for off in offs:
        for name, a, fo, sz, typ in secs:
            if fo <= off < fo + sz:
                va = a + off - fo
                k = bisect.bisect_right(starts, va) - 1
                if k >= 0 and va < syms[k][0] + max(syms[k][1], 1) and art.search(syms[k][2]):
                    bad[syms[k][2]] = bad.get(syms[k][2], 0) + 1
                break
    print("mm_embed check: %d ROM windows in %s, %d in textures/palettes/fonts" % (len(offs), exe.rsplit("/", 1)[-1], sum(bad.values())))
    for k, v in sorted(bad.items(), key=lambda kv: -kv[1])[:20]:
        print("  %d %s" % (v, k))
    return not bad


def main():
    if sys.argv[1] == "check":
        sys.exit(0 if check(*sys.argv[2:5]) else 1)
    mode, elf, n64elf, pack = sys.argv[1:5]
    ent = entries(elf, n64elf, pack)
    tot = sum(e[1] for e in ent)
    bysec = {}
    for e in ent:
        bysec[e[5]] = bysec.get(e[5], 0) + e[1]
    print("mm_embed: %d arrays, %d bytes the ROM's (%s)" % (len(ent), tot, ", ".join("%s %d" % kv for kv in sorted(bysec.items()))))
    if mode == "list":
        for e in sorted(ent, key=lambda e: -e[1])[:40]:
            print("  %08x %6d pack %08x %s (%s)" % (e[0], e[1], e[2], e[4], e[5]))
        return
    assert mode == "strip"
    src, dst = sys.argv[5:7]
    if len(ent) > MAXN:
        sys.exit("mm_embed: %d entries, the table holds %d" % (len(ent), MAXN))
    tab = [s for s in nm(elf, TC + "nm") if s[3] == TABLE]
    if len(tab) != 1 or tab[0][1] < 8 + 16 * MAXN:
        sys.exit("mm_embed: %s missing or too small" % TABLE)
    out = bytearray(open(src, "rb").read())
    ssecs = sections(src)
    for a, sz, s, crc, name, sec in ent:
        fo, _ = file_off(ssecs, a, sz)
        if fo is None:
            sys.exit("mm_embed: %s not in %s" % (name, src))
        out[fo:fo + sz] = bytes(sz)
    blob = MAGIC + struct.pack("<I", len(ent)) + b"".join(struct.pack("<4I", a, sz | (0x80000000 if sc == ".rodata" else 0), s, crc) for a, sz, s, crc, n, sc in ent)
    fo, _ = file_off(ssecs, tab[0][0], len(blob))
    if fo is None or any(out[fo:fo + len(blob)]):
        sys.exit("mm_embed: %s is not an empty array in %s" % (TABLE, src))
    out[fo:fo + len(blob)] = blob
    open(dst, "wb").write(out)
    print("mm_embed: %s: %d arrays zeroed, the table written" % (dst, len(ent)))


if __name__ == "__main__":
    main()
