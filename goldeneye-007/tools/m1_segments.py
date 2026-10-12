#!/usr/bin/env python3
"""M1: regenerate the decomp's committed data segments from the player's own ROM.

n64decomp/007 builds nine ROM segments only from C files it commits (ROM-derived data we
never fetch): font_dl, font_chardataj/e, animationtable_entries/_data, oddtextures
(the "Globalimagetable" segment), rarewarelogo, and the two assets/font/*.c fonts. Each
is a self-contained segment at a fixed ROM range, so an assembly file that .incbin's that
exact range of the ROM reproduces it byte for byte. The only other thing the link needs is
the symbols the game code references inside them; their offsets come from:
  - names that encode them (globalDL_0x078, D_020043E8 = VMA 0x02000000 + 0x43E8),
  - assets/animationtable_data.h (PTR_ANIM_x = offset of ANIM_DATA_x),
  - the font DL layout (checked against the G_ENDDL positions in the bytes),
  - tools/m1_symbols.json: offsets recovered from the ROM's own code by
    tools/m1_recover_syms.py (s_*image tables, DL_RAREWARETEXT).
No ROM bytes are written by this script: the .s files reference the extracted .bin files.

Usage: tools/m1_segments.py <decomp_dir>   (after scripts/extract_baserom.u.sh and an
       all-rows extraction, i.e. m1/filelist.all.u.csv)
"""
import csv, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))

def main() -> int:
    root = sys.argv[1]
    rows = {r[2]: (int(r[0]), int(r[1])) for r in csv.reader(open(f"{root}/scripts/filelist.u.csv"))}
    def rng(name): return rows[name]
    recovered = {}
    p = f"{HERE}/m1_symbols.json"
    if os.path.exists(p):
        recovered = json.load(open(p))["offsets"]

    anim = {}
    for m in re.finditer(r"#define\s+PTR_ANIM_(\w+)\s+(0x[0-9A-Fa-f]+)", open(f"{root}/assets/animationtable_data.h").read()):
        anim["ANIM_DATA_" + m.group(1)] = int(m.group(2), 16)
    gdl = {f"globalDL_0x{m.group(1)}": int(m.group(1), 16)
           for m in re.finditer(r"extern Gfx globalDL_0x([0-9a-fA-F]+);", open(f"{root}/assets/oddtextures.h").read())}
    simg = [m.group(1) for m in re.finditer(r"extern sImageTableEntry (s_\w+)\[\];", open(f"{root}/assets/oddtextures.h").read())]
    logo_named = {}
    for src in ("src/game/title.c",):
        for m in re.finditer(r"\b(D_02([0-9A-Fa-f]{6}))\b", open(f"{root}/{src}").read()):
            logo_named[m.group(1)] = int(m.group(2), 16)

    git_start, _ = rng("assets/ge007.u.29D160.Globalimagetable.bin")
    logo_start, logo_len = rng("assets/rarewarelogo.bin")
    segs = {
        # object path: (source .bin, rom offset of that .bin, length, {symbol: offset})
        "assets/font_dl.s": ("assets/ge007.u.117880.jfont_dl.bin", *rng("assets/ge007.u.117880.jfont_dl.bin"),
                             {"dlBasicGeometry": 0x00, "dlZBufferGeometry": 0x20, "dlFastPipelineSetup": 0x40}),
        "assets/font_chardataj.s": ("assets/ge007.u.117940.jfont_chardata.bin", *rng("assets/ge007.u.117940.jfont_chardata.bin"), {}),
        "assets/font_chardatae.s": ("assets/ge007.u.123040.efont_chardata.bin", *rng("assets/ge007.u.123040.efont_chardata.bin"), {}),
        "assets/animationtable_entries.s": ("assets/animationtable_entries.bin", *rng("assets/animationtable_entries.bin"), {}),
        "assets/animationtable_data.s": ("assets/animationtable_data.bin", *rng("assets/animationtable_data.bin"), anim),
        # the filelist's Globalimagetable row covers only the display lists; the segment runs to
        # the Rareware logo (the s_* image tables sit in the gap) -- 0x1400 bytes in NTSC-U
        "assets/oddtextures.s": ("baserom.u.z64", git_start, logo_start - git_start,
                                 {**gdl, **{s: recovered.get(s, 0) for s in simg}}),
        "assets/rarewarelogo.s": ("assets/rarewarelogo.bin", logo_start, logo_len,
                                  {**logo_named, "DL_RAREWARETEXT": recovered.get("DL_RAREWARETEXT", 0)}),
    }
    for font in ("fontBankGothic", "fontZurichBold"):
        k = f"assets/font/{font}_kerning.bin"; c = f"assets/font/{font}_fontchartable.bin"
        (ko, kl), (co, cl) = rng(k), rng(c)
        assert co == ko + kl, font
        segs[f"assets/font/{font}.s"] = ("baserom.u.z64", ko, kl + cl, {})

    placeholders = []
    for out, (binf, rom_off, length, syms) in segs.items():
        base = rom_off if binf == "baserom.u.z64" else None
        lines = [".section .data"]
        pos = 0
        for name, off in sorted(syms.items(), key=lambda kv: (kv[1], kv[0])):
            assert 0 <= off <= length, (out, name, off)
            if off > pos:
                lines.append(f'.incbin "{binf}", {(base or 0) + pos}, {off - pos}')
                pos = off
            lines += [f".global {name}", f"{name}:"]
            if name in simg + ["DL_RAREWARETEXT"] and name not in recovered:
                placeholders.append(name)
        if length > pos:
            lines.append(f'.incbin "{binf}", {(base or 0) + pos}, {length - pos}')
        open(f"{root}/{out}", "w").write("\n".join(lines) + "\n")
        print(f"{out}: {length} bytes from {binf}, {len(syms)} symbols")
    if placeholders:
        print(f"PLACEHOLDERS (offset 0): {len(placeholders)} -> run tools/m1_recover_syms.py after linking")
    return 0

if __name__ == "__main__":
    sys.exit(main())
