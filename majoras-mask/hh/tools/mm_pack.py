#!/usr/bin/env python3
"""The data pack: the decompressed ROM's file layout (vrom), with every C-built asset file (objects, scenes,
rooms, interface/misc statics, text) replaced by the same source compiled for ARM and linked at its segment
address, so the game reads native-endian structs with segmented in-file pointers. Everything else (audio
sequences/samples, yar archives, skyboxes, raw baserom files) stays as the ROM's bytes.

Each native file must come out the same size as the N64 file and every global symbol at the same offset
(checked here), so the vrom layout, dmadata and the asset symbol table (from the N64 ELF) stay valid.

ROM-derived: the pack and everything under the work dir are local only.

usage: mm_pack.py <n64 elf> <n64 uncompressed rom> <spec> <out pack> <work dir> [-j N]
Run from the decomp root, after the matching N64 build (make rom).
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

TC = os.environ.get("MIYOO_TOOLCHAIN", "/opt/miyoomini-toolchain") + "/usr/bin/arm-linux-gnueabihf-"
CFLAGS = ["-O1", "-march=armv7ve+neon-vfpv4", "-mfpu=neon-vfpv4", "-mfloat-abi=hard", "-std=gnu11", "-fsigned-char",
          "-fno-common", "-fwrapv", "-w", "-fdata-sections", "-fno-zero-initialized-in-bss", "-fno-toplevel-reorder",
          "-D_LANGUAGE_C", "-DNON_MATCHING", "-DAVOID_UB", "-DCOMPILER_GCC", "-DMM_VERSION=N64_US",
          "-DF3DEX_GBI_2", "-DF3DEX_GBI_PL", "-DGBI_DOWHILE", "-DTARGET_HANDHELD=1"]
INCS = ["-Ibuild/hh-texbe", "-Iinclude", "-Iinclude/libc", "-Isrc", "-Ibuild/n64-us", "-I.", "-Iextracted/n64-us"]


def nm(elf, tool):
    syms = {}
    out = subprocess.run([tool, elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms.setdefault(p[2], (int(p[0], 16), p[1]))
    return syms


def parse_spec(path):
    segs = []
    cur = None
    for line in open(path):
        s = line.strip()
        if s == "beginseg":
            cur = {"includes": []}
        elif s == "endseg":
            segs.append(cur)
            cur = None
        elif cur is not None:
            m = re.match(r'(\w+)\s+(.*)', s)
            if not m:
                continue
            k, v = m.group(1), m.group(2).strip()
            if k == "include":
                cur["includes"].append(v.strip('"'))
            else:
                cur[k] = v.strip('"')
    return segs


def source_for(obj):
    if "/audio/soundfonts/" in obj:
        return obj[:-2] + ".c"
    rel = obj[len("build/n64-us/"):-2] + ".c"
    for cand in ("extracted/n64-us/" + rel, rel):
        if os.path.exists(cand):
            return cand
    return None


def build_one(seg, vram, size, work, provide_ld, n64syms):
    name = seg["name"]
    objs = []
    for inc in seg["includes"]:
        src = source_for(inc)
        if src is None:
            return name, None, "no source for " + inc
        o = os.path.join(work, name + "_" + os.path.basename(inc))
        r = subprocess.run([TC + "gcc", "-c"] + CFLAGS + INCS + ["-Iinclude/audio", "-o", o, src], capture_output=True, text=True)
        if r.returncode != 0:
            return name, None, "compile: " + r.stderr[-800:]
        objs.append(o)
    ld = os.path.join(work, name + ".ld")
    with open(ld, "w") as f:
        f.write("SECTIONS {\n . = 0x%08X;\n .seg : { %s(.text .text.* .data .data.* .rodata .rodata.* .bss .bss.* COMMON) }\n"
                " /DISCARD/ : { *(.ARM.* .comment .note.* .debug*) }\n}\n" % (vram, "*"))
    elf = os.path.join(work, name + ".elf")
    r = subprocess.run([TC + "ld", "-T", ld, "-T", provide_ld, "--no-check-sections", "-o", elf] + objs,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return name, None, "link: " + r.stderr[-1500:]
    binf = os.path.join(work, name + ".bin")
    subprocess.run([TC + "objcopy", "-O", "binary", "-j", ".seg", elf, binf], check=True)
    data = open(binf, "rb").read()
    if len(data) < size:
        data += b"\0" * (size - len(data))
    problems = []
    if len(data) != size:
        problems.append("size %#x != n64 %#x" % (len(data), size))
    # every global symbol at its N64 address
    out = subprocess.run([TC + "nm", "-g", "--defined-only", elf], capture_output=True, text=True).stdout
    nbad = 0
    for line in out.splitlines():
        p = line.split()
        if len(p) != 3 or p[1] in "aA":
            continue
        want = n64syms.get(p[2])
        if want is not None and want[0] != int(p[0], 16):
            nbad += 1
            if nbad <= 3:
                problems.append("%s at %s, n64 %08x" % (p[2], p[0], want[0]))
    for o in objs:
        os.remove(o)
    os.remove(elf)
    os.remove(binf)
    if problems:
        return name, None, "; ".join(problems)
    return name, data, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("rom")
    ap.add_argument("spec")
    ap.add_argument("out")
    ap.add_argument("work")
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("--only", help="regex of segment names")
    args = ap.parse_args()

    os.makedirs(args.work, exist_ok=True)
    n64 = nm(args.elf, "mips-linux-gnu-nm")
    # PROVIDE every asset symbol (segmented address) for cross-file references
    provide_ld = os.path.join(args.work, "provide.ld")
    with open(provide_ld, "w") as f:
        for n, (v, t) in sorted(n64.items()):
            if ((0x01000000 <= v < 0x10000000 and t in "DdBbRrTtAa") or (n.startswith("SampleBank_") and t in "Aa")) \
                    and re.match(r"^[A-Za-z_]\w*$", n):
                f.write("PROVIDE(%s = 0x%08X);\n" % (n, v))

    rom = bytearray(open(args.rom, "rb").read())
    jobs = []
    for seg in parse_spec(args.spec):
        incs = seg["includes"]
        if not incs or not all(i.startswith("build/n64-us/assets/") and i.endswith(".o") for i in incs):
            continue
        if any("/audio/" in i or "/archives/" in i or i.endswith(".yar.o") for i in incs):
            continue
        name = seg["name"]
        if args.only and not re.search(args.only, name):
            continue
        rs, re_ = n64.get("_%sSegmentRomStart" % name), n64.get("_%sSegmentRomEnd" % name)
        vs = n64.get("_%sSegmentStart" % name)
        if rs is None or re_ is None or vs is None:
            continue
        jobs.append((seg, vs[0], rs[0], re_[0] - rs[0]))

    # Audiobank: every soundfont compiled for ARM, linked at 0 (its pointers are font-relative offsets that the
    # audio engine relocates at load), at its offset in the bank
    ab = n64.get("_AudiobankSegmentRomStart")
    for seg in parse_spec(args.spec):
        if seg.get("name") != "Audiobank" or ab is None or (args.only and not re.search(args.only, "Audiobank")):
            continue
        for inc in seg["includes"]:
            fn = os.path.basename(inc)[:-2]
            st, sz = n64.get(fn + "_Start"), n64.get(fn + "_Size")
            if st is None or sz is None:
                continue
            jobs.append(({"name": fn, "includes": [inc]}, 0, ab[0] + st[0], sz[0]))

    ok = bad = 0
    with concurrent.futures.ThreadPoolExecutor(args.j) as ex:
        futs = {ex.submit(build_one, seg, vram, size, args.work, provide_ld, n64): (seg, rom_start, size)
                for seg, vram, rom_start, size in jobs}
        for fu in concurrent.futures.as_completed(futs):
            seg, rom_start, size = futs[fu]
            name, data, err = fu.result()
            if err:
                bad += 1
                print("FAIL %s: %s" % (name, err), file=sys.stderr)
                continue
            rom[rom_start:rom_start + size] = data
            ok += 1
    with open(args.out, "wb") as f:
        f.write(rom)
    print("native files: %d ok, %d failed (left as ROM bytes)" % (ok, bad), file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
