#!/usr/bin/env python3
"""gen_link.py - generate the native (handheld / desktop proxy) link description
from the decomp's own N64 build, so the port links the game with the N64's
numbers wherever the game depends on them.

Three kinds of symbol, decided per symbol from the N64 ELF + linker script:

1. Asset / ROM constants (N64 value outside RAM, e.g. 0x0803A8F8 segmented
   display lists, 0x0016C8A0 ROM offsets, 0x8F000000 audio VRAM): kept at their
   exact N64 value -> PROVIDE(sym = value). The game then DMAs assets from the
   player's ROM and resolves segments exactly as it does on the N64.

2. RDRAM layout (N64 value in 0x80000000-0x80400000 that the native build does
   not define itself): the pure-BSS segments (framebuffers, gfxpool, buffers,
   game context, audio heap, ...) are NOT compiled; every symbol in them lives
   in one 4 MB block at RDRAM_BASE at the same offset as on the N64
   (0x80225800 -> RDRAM_BASE + 0x225800). Overlaps, address chains and the
   heap arenas that start where the N64 overlays end all keep working.
   Segment VRAM / VRAM_END markers are translated the same way.

3. Overlay data/bss markers (<ovl>_DATA_START, _BSS_START, ...): real native
   extents from linker-script sections, so loading an overlay can restore its
   pristine .data and zero its .bss like the N64's DMA + bzero did.
   TEXT markers are only used for cache flushes (no-ops) -> 0-size dummies.

Outputs (in <outdir>):
  n64_syms.ld    PROVIDE() lines for kinds 1 and 2 (numbers only)
  overlays.ld    INSERT-style fragment with the overlay data/bss sections
  overlays.h     table of overlay extents for port_overlay_load()
  excluded.txt   the pure-BSS objects the Makefile must not compile

Usage: gen_link.py <decomp dir> <outdir> <native objdir prefix>
"""
import os, re, subprocess, sys

RDRAM_BASE = 0x20000000
RDRAM_SIZE = 0x400000
N64_RAM = 0x80000000

# Segments that hold only BSS and are placed at fixed N64 RAM: not compiled natively.
BSS_SEGMENTS = ["framebuffer_unused", "audio_context", "audio_heap", "framebuffer1", "framebuffer2",
                "unk_gfx_segment", "unk_bss_segment", "gfxpool", "game_context", "buffers", "framebuffer3"]
# Code overlays whose .data/.bss get native marker sections (cartridge build).
OVERLAYS = ["ovl_i2", "ovl_i3", "ovl_i4", "course_select", "ovl_i6", "ending", "records", "ovl_i9",
            "ovl_i10", "ovl_i11"]
MARKER = re.compile(r"^(\w+?)_(VRAM|VRAM_END|ROM_START|ROM_END|TEXT_START|TEXT_END|DATA_START|DATA_END|"
                    r"DATA_SIZE|RODATA_END|BSS_START|BSS_END)$")


def sh(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True).stdout.decode()


COMPRESSED = ["course_track_gfx", "machine_models", "mute_city_textures", "port_town_textures", "big_blue_textures",
              "sand_ocean_textures", "devils_forest_textures", "white_land_textures", "sector_textures",
              "red_canyon_textures", "fire_field_textures", "silence_textures", "ending_venue_textures", "podium_gfx"]


def final_rom_values(decomp, elf):
    """The ELF's *_ROM_START/_ROM_END are for the uncompressed layout; the
    decomp's tools/compress.py then MIO0-compresses COMPRESSED and relocates
    every reference in the code. Reproduce its walk: each PROGBITS|ALLOC
    section in ELF order advances the ROM by its size, a compressed one by its
    compressed size - read from the real ROM's layout (the split yaml's start
    of the next segment), so nothing is recompressed."""
    starts = []
    for yf in ("header", "main", "overlays", "assets"):
        y = open(os.path.join(decomp, "yamls/us/rev0/%s.yaml" % yf)).read()
        for m in re.finditer(r"- name: (\w+)\n(?:.*\n)*?\s+start: (0x[0-9A-Fa-f]+)", y):
            starts.append((int(m.group(2), 16), m.group(1)))
    starts.sort()
    yaml_start = {n: v for v, n in starts}
    def next_start(v):
        later = [x for x, _ in starts if x > v]
        return min(later) if later else 0x1000000

    out = {}
    pos = 0
    for line in sh("mips-linux-gnu-readelf", "-S", "-W", elf).splitlines():
        m = re.search(r"\]\s+\.(\S+)\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S+)", line)
        if not m or "A" not in m.group(4):
            continue
        name, vram, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if name in COMPRESSED:
            start = yaml_start[name]
            if start != pos:
                raise SystemExit("gen_link: ROM walk out of step at %s (0x%X vs yaml 0x%X)" % (name, pos, start))
            csize = next_start(start) - start
            out[name + "_ROM_START"], out[name + "_ROM_END"] = pos, pos + csize
            out[name + "_VRAM_END"] = vram + csize
            pos += csize
        else:
            out[name + "_ROM_START"], out[name + "_ROM_END"] = pos, pos + size
            pos += size
    return out


def main():
    decomp, outdir, objprefix = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    elf = os.path.join(decomp, "build/us/rev0/fzerox.us.rev0.elf")
    ldscript = open(os.path.join(decomp, "linker_scripts/us/rev0/fzerox.ld")).read()

    # segment -> list of object paths (relative to the decomp root, without build prefix)
    seg_objs, cur = {}, None
    for line in ldscript.splitlines():
        m = re.match(r"\s+(\w+)_ROM_START = __romPos;", line)
        if m:
            cur = m.group(1)
            seg_objs.setdefault(cur, [])
        m = re.search(r"build/us/rev0/(src/\S+?)\.o\(", line)
        if m and cur:
            if m.group(1) not in seg_objs[cur]:
                seg_objs[cur].append(m.group(1))

    excluded = []
    for seg in BSS_SEGMENTS:
        excluded += seg_objs.get(seg, [])
    excluded_set = set(excluded)

    syms = {}
    for line in sh("mips-linux-gnu-nm", elf).splitlines():
        p = line.split()
        if len(p) == 3:
            # 32 bits: binutils 2.40's nm prints MIPS32 addresses sign-extended
            # (ffffffff80025c00), 2.42's as 80025c00
            syms.setdefault(p[2], int(p[0], 16) & 0xFFFFFFFF)
    syms.update(final_rom_values(decomp, elf))

    # symbols defined by the excluded objects (their N64 build objects)
    excl_syms, data_objs = set(), set()
    for o in excluded:
        path = os.path.join(decomp, "build/us/rev0", o + ".o")
        for line in sh("mips-linux-gnu-nm", "--defined-only", path).splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "BbCSs":
                excl_syms.add(p[2])
            elif len(p) == 3 and p[1] in "DdGgRr":
                data_objs.add(o)

    def rdram(v):
        return RDRAM_BASE + (v - N64_RAM)

    lines = ["/* Generated by tools/gen_link.py - N64 symbol values (numbers only). */",
             "RDRAM_BASE = 0x%X;" % RDRAM_BASE]
    ovl_set = set(OVERLAYS)
    n_const = n_ram = 0
    for name, v in sorted(syms.items()):
        if name.startswith("$") or name.startswith(".") or not re.match(r"^[A-Za-z_]\w*$", name):
            continue
        m = MARKER.match(name)
        if m:
            seg, kind = m.group(1), m.group(2)
            if seg in ovl_set and kind in ("DATA_START", "DATA_END", "RODATA_END", "BSS_START", "BSS_END"):
                continue  # native, from overlays.ld
            if kind in ("TEXT_START", "TEXT_END"):
                lines.append("PROVIDE(%s = 0);" % name)
                continue
            if kind in ("ROM_START", "ROM_END") and (seg in ovl_set or seg in ("main", "leo", "entry", "cartridge_offsets")):
                # code is linked in: nothing to DMA (size 0)
                lines.append("PROVIDE(%s = 0x%X);" % (name, syms.get(seg + "_ROM_START", v)))
                continue
        if m and N64_RAM <= v < N64_RAM + 2 * RDRAM_SIZE and m.group(2) in ("VRAM", "VRAM_END", "BSS_START", "BSS_END",
                                                                          "DATA_START", "DATA_END", "RODATA_END"):
            # segment markers (incl. the end of RDRAM and the 8 MB 64DD region): translate, they are just numbers
            lines.append("PROVIDE(%s = 0x%X);" % (name, rdram(v)))
            n_ram += 1
            continue
        if N64_RAM <= v < N64_RAM + RDRAM_SIZE:
            if name in excl_syms or (m and m.group(2) in ("VRAM", "VRAM_END", "BSS_START", "BSS_END", "DATA_START",
                                                          "DATA_END", "RODATA_END")):
                lines.append("PROVIDE(%s = 0x%X);" % (name, rdram(v)))
                n_ram += 1
            continue  # other RAM symbols are defined natively (or by the shim)
        if N64_RAM + RDRAM_SIZE <= v < 0x8F000000:
            continue  # 8 MB / 64DD region: not used by the cartridge game on this port
        lines.append("PROVIDE(%s = 0x%X);" % (name, v))
        n_const += 1
    open(os.path.join(outdir, "n64_syms.ld"), "w").write("\n".join(lines) + "\n")

    # overlay data/bss sections
    ld = ["/* Generated by tools/gen_link.py: per-overlay .data/.bss extents. */", "SECTIONS {"]
    for ovl in OVERLAYS:
        objs = [objprefix + o + ".o" for o in seg_objs.get(ovl, [])]
        if not objs:
            continue
        pat = " ".join("*%s(.data .data.* .sdata .sdata.*)" % o for o in objs)
        ld.append("  .data.ovl.%s : ALIGN(16) { %s_DATA_START = .; %s %s_DATA_END = .; %s_RODATA_END = .; }"
                  % (ovl, ovl, pat, ovl, ovl))
    ld.append("} INSERT AFTER .data;")
    ld.append("SECTIONS {")
    for ovl in OVERLAYS:
        objs = [objprefix + o + ".o" for o in seg_objs.get(ovl, [])]
        if not objs:
            continue
        pat = " ".join("*%s(.bss .bss.* .sbss .sbss.* COMMON)" % o for o in objs)
        ld.append("  .bss.ovl.%s (NOLOAD) : ALIGN(16) { %s_BSS_START = .; %s %s_BSS_END = .; }"
                  % (ovl, ovl, pat, ovl))
    ld.append("} INSERT AFTER .bss;")
    ld.append("SECTIONS {")
    ld.append("  .n64_rdram 0x%X (NOLOAD) : { gPortRdram = .; . = . + 0x%X; gPortRdramEnd = .; }" % (RDRAM_BASE, RDRAM_SIZE))
    ld.append("} INSERT AFTER .bss;")
    open(os.path.join(outdir, "overlays.ld"), "w").write("\n".join(ld) + "\n")

    with open(os.path.join(outdir, "overlays.h"), "w") as f:
        f.write("/* Generated by tools/gen_link.py */\n")
        for ovl in OVERLAYS:
            if seg_objs.get(ovl):
                f.write("PORT_OVERLAY(%s)\n" % ovl)
    open(os.path.join(outdir, "excluded.txt"), "w").write("\n".join(o for o in excluded if o not in data_objs) + "\n")
    # excluded objects that also carry initialised data: compiled natively with their .bss stripped
    open(os.path.join(outdir, "dataonly.txt"), "w").write("\n".join(sorted(data_objs)) + "\n")
    open(os.path.join(outdir, "rdram_objs.txt"), "w").write("\n".join(excluded) + "\n")
    print("gen_link: %d constants, %d rdram symbols, %d excluded objects, %d overlays" % (
        n_const, n_ram, len(excluded), sum(1 for o in OVERLAYS if seg_objs.get(o))))


if __name__ == "__main__":
    main()
