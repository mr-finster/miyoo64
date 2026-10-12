#!/usr/bin/env python3
"""The ROM-derived data compiled INTO game code (actor overlays, transitions, n64dd
error textures): list its symbols, or move their sections out of the code.

  embedded_assets.py list                 -> symbol names (one per line)
  embedded_assets.py split <objcopy> <obj> -> rename each listed symbol's own data
                                             section (.data.<sym>, .rodata.<sym>) of
                                             <obj> to .hhasset.<sym>, in place

Symbols come from the decomp's asset XMLs for the files game code #includes
(assets/xml/{overlays,code,n64dd}; ntsc-1.0 variants only). Objects are compiled
with -fdata-sections so every such symbol has its own section. Makefile.handheld
links .hhasset.* into a NOLOAD region after .bss: the shipped executable carries
the addresses, not the bytes (release gate, as mario-kart-64's M6).
"""
import re, subprocess, sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VARIANTS = ("_pal", "_pal_n64", "_pal_gc", "_ique", "_mq", "_v2", "_v3", "_gc")

DEF_RE = re.compile(r"^\s*(?:static\s+)?(?:const\s+)?[A-Za-z_]\w*\s+\**\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*=", re.M)
INC_RE = re.compile(r'#include "assets/[^"]+/([A-Za-z_]\w*)\.[^"]*inc\.c"')

def symbols():
    names = set()
    # Every top-level definition in the extracted asset sources game code includes
    # (the extractor names unnamed offsets itself, e.g. ovl_Boss_Ganon_00001E80_Tex).
    for sub in ("overlays", "code", "n64dd"):
        for c in (ROOT / "extracted" / "ntsc-1.0" / "assets" / sub).rglob("*.c"):
            if c.name.endswith(".inc.c"):
                continue
            names.update(DEF_RE.findall(c.read_text(errors="replace")))
    # Arrays defined in game sources around an #include "assets/...X.<fmt>.inc.c":
    # the included file's stem is the array's name.
    for src in (ROOT / "src").rglob("*.c"):
        if "/port/" in str(src):
            continue
        names.update(INC_RE.findall(src.read_text(errors="replace")))
    for sub in ("overlays", "code", "n64dd"):
        for xml in sorted((ROOT / "assets" / "xml" / sub).glob("*.xml")):
            if xml.stem.endswith(VARIANTS):
                continue
            for elem in ET.parse(xml).getroot().iter():
                name = elem.attrib.get("Name")
                if name and elem.tag not in ("Root", "File", "ExternalFile"):
                    names.add(name)
    return names

def split(objcopy, obj):
    names = symbols()
    out = subprocess.run([objcopy.replace("objcopy", "readelf"), "-SW", obj], capture_output=True, text=True).stdout
    renames = []
    for line in out.splitlines():
        parts = line.replace("[", " [").split()
        for tok in parts:
            for prefix in (".data.", ".rodata."):
                if tok.startswith(prefix):
                    sym = tok[len(prefix):]
                    base = sym.split(".")[0]  # .data.sVtx.1 for a static name clash
                    if base in names:
                        renames += ["--rename-section", f"{tok}=.hhasset.{sym}"]
    if renames:
        subprocess.run([objcopy, "--preserve-dates"] + renames + [obj], check=True)

if __name__ == "__main__":
    if sys.argv[1] == "list":
        print("\n".join(sorted(symbols())))
    else:
        split(sys.argv[2], sys.argv[3])
