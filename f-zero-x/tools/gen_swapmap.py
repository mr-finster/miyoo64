#!/usr/bin/env python3
"""gen_swapmap.py - derive the big-endian -> little-endian byte-order map of every
ROM asset segment, so the port can DMA assets straight from the player's ROM
(exactly as the N64 does) and fix their byte order in place.

Ground truth, not guesswork: each asset segment's Torch C source is compiled a
second time for the little-endian target (ARM, same 32-bit ABI as the device),
linked at the segment's N64 VRAM, and compared word by word with the matching
big-endian section of the decomp's own MIPS ELF. For every aligned 32-bit word we
record the byte permutation that turns the BE word into the LE word (identity for
u8 data, 1032 for u16 pairs, 3210 for u32/float/pointers, anything else for mixed
structs). Runs of equal permutations are merged.

Output: a C file of numbers only (segment VRAM, sizes, run lengths, permutation
indices) - no ROM bytes. Ambiguous words (e.g. 0x00000000, where every
permutation matches) extend the current run, which is harmless: the map only has
to reproduce THIS ROM's LE image, and the game build checks the ROM hash.

Compressed data: a segment stored in the ROM as one MIO0 block (course_track_gfx,
machine_models, ...) gets its map applied after mio0Decode, keyed by the block's ROM
offset; so do the individual COMPRESSED_TEXTURE blocks (u16 texels -> sw16, u8 ->
identity), read from the decomp's asset yamls.

Usage: gen_swapmap.py <decomp dir> <out.c> [--check]
  --check  also re-applies the map to every BE image and asserts the LE image.
"""
import itertools, os, re, subprocess, sys, tempfile

ARM = "/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-"
DEFS = ("-D_LANGUAGE_C -DLANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI_2 -DNDEBUG -DVERSION_US=1 "
        "-DBUILD_REVISION=REVISION_B -DLANGUAGE=LANGUAGE_ENG -DOS_TV_TYPE=OS_TV_TYPE_NTSC "
        "-DBUILD_VERSION=VERSION_I -DASSET_VERSION=us -DASSET_REVISION=rev0 -DNON_MATCHING -DAVOID_UB").split()
PERMS = list(itertools.permutations(range(4)))  # index 0 = identity
ID, SW16, SW32 = PERMS.index((0, 1, 2, 3)), PERMS.index((1, 0, 3, 2)), PERMS.index((3, 2, 1, 0))
# Try the common shapes first so ambiguous words pick a sensible one.
ORDER = [ID, SW16, SW32] + [i for i in range(24) if i not in (ID, SW16, SW32)]


def sh(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw).stdout


def elf_sections(elf, tool):
    out = sh(tool + "readelf", "-S", "-W", elf).decode()
    secs = {}
    for m in re.finditer(r"\]\s+(\.\S+)\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out):
        secs[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
    return secs


def section_bytes(elf, name, tool):
    with tempfile.NamedTemporaryFile() as t:
        sh(tool + "objcopy", "-O", "binary", "--only-section=" + name, elf, t.name)
        return open(t.name, "rb").read()


def asset_segments(decomp):
    """(name, rom_start) of every asset segment, from the decomp's split yaml."""
    y = open(os.path.join(decomp, "yamls/us/rev0/assets.yaml")).read()
    segs = []
    for m in re.finditer(r"- name: (\w+)\n(?:.*\n)*?\s+start: (0x[0-9A-Fa-f]+)", y):
        segs.append((m.group(1), int(m.group(2), 16)))
    return segs


BPP = {"RGBA16": 16, "RGBA32": 32, "IA16": 16, "IA8": 8, "IA4": 4, "I8": 8, "I4": 4, "CI8": 8, "CI4": 4, "TLUT": 16}


def compressed_textures(decomp, seg_rom):
    """(rom offset, decoded size, perm, symbol) of every COMPRESSED_TEXTURE."""
    out = []
    ydir = os.path.join(decomp, "assets/yaml/us/rev0")
    for fn in sorted(os.listdir(ydir)):
        if not fn.endswith(".yaml"):
            continue
        seg = fn[:-5]
        if seg not in seg_rom:
            continue
        text = open(os.path.join(ydir, fn)).read()
        for m in re.finditer(r"\n(\w+):\n((?:  .*\n)+)", text):
            body = dict(re.findall(r"  (\w+): (\S+)", m.group(2)))
            if body.get("type") != "COMPRESSED_TEXTURE":
                continue
            fmt = body["format"].upper()
            w, h = int(body["width"], 0), int(body["height"], 0)
            dsize = w * h * BPP[fmt] // 8
            perm = SW16 if body.get("ctype") == "u16" else (SW32 if body.get("ctype") == "u32" else ID)
            out.append((seg_rom[seg] + int(body["offset"], 0), dsize, perm, m.group(1)))
    return out


def derive(be, le):
    runs = []  # [perm, words]
    cur = None
    for i in range(0, len(be) - len(be) % 4, 4):
        b, l = be[i:i + 4], le[i:i + 4]
        ok = [p for p in ORDER if bytes(b[PERMS[p][k]] for k in range(4)) == l]
        if not ok:
            raise ValueError("word at +0x%X: no byte permutation maps %s -> %s" % (i, b.hex(), l.hex()))
        p = cur if cur in ok else ok[0]
        if runs and runs[-1][0] == p:
            runs[-1][1] += 1
        else:
            runs.append([p, 1])
        cur = p
    if len(be) % 4:
        if be[-(len(be) % 4):] != le[-(len(be) % 4):]:
            raise ValueError("tail bytes differ")
    return runs


def apply(be, runs):
    out = bytearray(be)
    i = 0
    for p, n in runs:
        perm = PERMS[p]
        for _ in range(n):
            w = be[i:i + 4]
            out[i:i + 4] = bytes(w[perm[k]] for k in range(4))
            i += 4
    return bytes(out)


def main():
    decomp, outc = os.path.abspath(sys.argv[1]), sys.argv[2]
    check = "--check" in sys.argv
    elf = os.path.join(decomp, "build/us/rev0/fzerox.us.rev0.elf")
    rombytes = open(os.path.join(decomp, "baserom.us.rev0.z64"), "rb").read()
    secs = elf_sections(elf, "mips-linux-gnu-")
    tmp = tempfile.mkdtemp()
    inc = ["-I" + os.path.join(decomp, d) for d in ("include", "bin/us/rev0", ".")]
    rows, stats = [], []
    mips_syms = {}
    for line in sh("mips-linux-gnu-nm", elf).decode().splitlines():
        parts = line.split()
        if len(parts) == 3:
            mips_syms[parts[2]] = int(parts[0], 16) & 0xFFFFFFFF  # nm may sign-extend (gen_link.py)
    for name, rom in asset_segments(decomp):
        src = os.path.join(decomp, "src/assets/us/rev0", name, name + ".c")
        sec = "." + name
        if not os.path.exists(src) or sec not in secs:
            stats.append("%-28s skipped (%s)" % (name, "no source" if not os.path.exists(src) else "not in ELF"))
            continue
        vram, _, size = secs[sec]
        obj, lelf, ld = (os.path.join(tmp, name + x) for x in (".o", ".elf", ".ld"))
        try:
            sh(ARM + "gcc", "-c", "-O0", "-fno-common", "-fno-zero-initialized-in-bss", *DEFS, *inc, src, "-o", obj,
               cwd=decomp)
        except subprocess.CalledProcessError as e:
            stats.append("%-28s COMPILE FAILED: %s" % (name, e.stderr.decode().strip().splitlines()[-1][:120]))
            continue
        open(ld, "w").write("SECTIONS { .seg 0x%X : SUBALIGN(16) { *(.data) *(.data.*) *(.rodata) *(.rodata.*) *(.bss) } "
                            "/DISCARD/ : { *(*) } }\n" % vram)
        undef = [l.split()[-1] for l in sh(ARM + "nm", "-u", obj).decode().splitlines() if l.strip()]
        missing = [u for u in undef if u not in mips_syms]
        if missing:
            stats.append("%-28s UNRESOLVED: %s" % (name, " ".join(missing[:5])))
            continue
        sh(ARM + "ld", "-T", ld, "-o", lelf, obj, *["--defsym=%s=0x%X" % (u, mips_syms[u]) for u in undef])
        be = section_bytes(elf, sec, "mips-linux-gnu-")
        le = section_bytes(lelf, ".seg", ARM)
        if len(le) < len(be):
            le = le + bytes(len(be) - len(le))
        if len(le) != len(be):
            raise SystemExit("%s: LE image 0x%X bytes vs BE 0x%X - layouts differ" % (name, len(le), len(be)))
        try:
            runs = derive(be, le)
        except ValueError as e:
            stats.append("%-28s MISMATCH %s" % (name, e)); continue
        if check and apply(be, runs) != le:
            raise SystemExit("%s: map does not reproduce the LE image" % name)
        head = rombytes[rom:rom + 4]
        if rombytes[rom:rom + len(be)] == be:
            comp = 0
        elif head == b"MIO0":
            comp = 1
        else:
            stats.append("%-28s ROM bytes are neither the raw image nor MIO0 (%s)" % (name, head.hex()))
            continue
        rows.append((name, rom, vram, size, runs, comp))
        kinds = {}
        for p, n in runs:
            kinds[p] = kinds.get(p, 0) + n
        stats.append(("%-28s " + ("MIO0 " if comp else "raw  ") + "vram %08X size %6X runs %5d  id %d sw16 %d sw32 %d other %d") % (
            name, vram, size, len(runs), kinds.get(ID, 0), kinds.get(SW16, 0), kinds.get(SW32, 0),
            sum(v for k, v in kinds.items() if k not in (ID, SW16, SW32))))

    blocks = compressed_textures(decomp, {r[0]: r[1] for r in rows})
    for rom, dsize, perm, sym in blocks:
        if rombytes[rom:rom + 4] != b"MIO0":
            raise SystemExit("%s: no MIO0 header at ROM 0x%X" % (sym, rom))
        if int.from_bytes(rombytes[rom + 4:rom + 8], "big") != dsize:
            raise SystemExit("%s: MIO0 decoded size %X != %X" % (sym, int.from_bytes(rombytes[rom + 4:rom + 8], "big"), dsize))
    with open(outc, "w") as f:
        f.write("/* Generated by tools/gen_swapmap.py - numbers only, no ROM data. Do not edit. */\n")
        f.write('#include "port_swapmap.h"\n\n')
        f.write("const u8 gSwapPerms[24][4] = {\n")
        for p in PERMS:
            f.write("    {%d, %d, %d, %d},\n" % p)
        f.write("};\n\n")
        for name, rom, vram, size, runs, comp in rows:
            f.write("static const SwapRun sRuns_%s[%d] = {\n" % (name, len(runs)))
            for i in range(0, len(runs), 8):
                f.write("    " + " ".join("{%d, %d}," % (n, p) for p, n in runs[i:i + 8]) + "\n")
            f.write("};\n")
        f.write("\nconst SwapSegment gSwapSegments[] = {\n")
        for name, rom, vram, size, runs, comp in rows:
            f.write('    {"%s", 0x%06X, 0x%08X, 0x%06X, %d, %d, sRuns_%s},\n' % (name, rom, vram, size, comp, len(runs), name))
        f.write("};\nconst int gSwapSegmentCount = %d;\n\n" % len(rows))
        f.write("/* Individually MIO0-compressed textures: ROM offset of the block, decoded size, permutation. */\n")
        f.write("const SwapBlock gSwapBlocks[] = {\n")
        for rom, dsize, perm, sym in blocks:
            f.write("    {0x%06X, 0x%05X, %d}, /* %s */\n" % (rom, dsize, perm, sym))
        f.write("};\nconst int gSwapBlockCount = %d;\n" % len(blocks))
    print("\n".join(stats))
    print("compressed texture blocks: %d" % len(blocks))
    print("wrote %s: %d segments%s" % (outc, len(rows), " (checked)" if check else ""))


if __name__ == "__main__":
    main()
