#!/usr/bin/env python3
"""Asset headers (and code-embedded asset stubs) from the decomp's XMLs alone, without a ROM.

ZAPD writes extracted/<ver>/assets/**/<file>.h (externs, limb enums, texture defines) from the XMLs
AND the ROM. Everything the headers need is in the XMLs (names, types, offsets, counts, limb enum
names), so this writes the same headers with no ROM: the port's code can then be compiled and linked
before a ROM is available. It also writes:

  - the asset symbol table (--symbols): every asset symbol in a segmented file = its segmented
    address (segment << 24 | offset), as the OoT port's oot_psp_asset_segments.S;
  - --stubs: zero-filled definitions for the assets that code #includes as C (overlay/code/boot
    XMLs, which have a BaseAddress) and empty .inc.c / .vtx.inc files, so that code compiles. These
    are placeholders with no game data (zeros); a real extraction (make assets) replaces them.

Run from the decomp root. Writes nothing ROM-derived.
"""
import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

TEX_BPP = {"rgba32": 32, "rgba16": 16, "ia16": 16, "ia8": 8, "ia4": 4, "i8": 8, "i4": 4, "ci8": 8, "ci4": 4}
LIMB_TYPES = {"Standard": "StandardLimb", "LOD": "LodLimb", "Skin": "SkinLimb", "Curve": "SkelCurveLimb",
              "Legacy": "LegacyLimb"}
SKEL_TYPES = {"Normal": "SkeletonHeader", "Flex": "FlexSkeletonHeader", "Curve": "CurveSkeletonHeader"}
SCALAR_TYPES = {"s8": "s8", "u8": "u8", "s16": "s16", "u16": "u16", "s32": "s32", "u32": "u32", "s64": "s64",
                "u64": "u64", "f32": "f32", "f64": "f64"}
VECTOR_TYPES = {"s16": "Vec3s", "s32": "Vec3i", "f32": "Vec3f"}


def snake(name):
    """ZAPD's StringHelper::camelCaseTo_SCREAMING_SNAKE_CASE(name, skipSP=true)."""
    out = ""
    i = 0
    n = len(name)
    if n == 0:
        return out
    ch = name[0]
    if ch in "gs":
        if not (n > 1 and name[1].isupper()):
            out += ch.upper()
        i = 1
        if i >= n:
            return out
        ch = name[i]
    out += ch.upper() if ch.islower() else ch
    i += 1
    while i < n:
        ch = name[i]
        if ch.islower():
            out += ch.upper()
        else:
            nxt = name[i + 1] if i + 1 < n else "\0"
            prv = name[i - 1]
            if ch.isupper() and not (nxt.isupper() and prv.isupper()):
                out += "_"
            out += ch
        i += 1
    return out


def ival(s, default=0):
    if s is None:
        return default
    return int(s, 0)


def decl_of(el):
    """(type, name, is_array, count, size_guess) of a top-level XML element, or None."""
    tag = el.tag
    name = el.get("Name")
    if name is None:
        return None
    if tag == "Texture":
        w, h = ival(el.get("Width")), ival(el.get("Height"))
        size = w * h * TEX_BPP.get(el.get("Format", "rgba16"), 16) // 8
        return ("u64", name, True, None, size)
    if tag == "Background":
        return ("u64", name, True, None, 8)
    if tag == "DList":
        return ("Gfx", name, True, None, 8)
    if tag == "Limb":
        return (LIMB_TYPES.get(el.get("Type", "Standard"), "StandardLimb"), name, False, None, 0)
    if tag == "Skeleton":
        return (SKEL_TYPES.get(el.get("Type", "Normal"), "SkeletonHeader"), name, False, None, 0)
    if tag == "LimbTable":
        return ("void*", name, True, ival(el.get("Count"), None), 0)
    if tag == "Animation":
        return ("AnimationHeader", name, False, None, 0)
    if tag == "PlayerAnimation":
        return ("PlayerAnimationHeader", name, False, None, 0)
    if tag == "PlayerAnimationData":
        return ("s16", name, True, None, 0)
    if tag == "CurveAnimation":
        return ("CurveAnimationHeader", name, False, None, 0)
    if tag == "LegacyAnimation":
        return ("LegacyAnimationHeader", name, False, None, 0)
    if tag == "KeyFrameSkel":
        return ("KeyFrameSkeleton", name, False, None, 0)
    if tag == "KeyFrameAnimation":
        return ("KeyFrameAnimation", name, False, None, 0)
    if tag == "Collision":
        return ("CollisionHeader", name, False, None, 0)
    if tag == "CollisionPoly":
        return ("CollisionPoly", name, True, ival(el.get("Count"), None), 0)
    if tag in ("Scene", "Room"):
        return ("SceneCmd", name, True, None, 0)
    if tag == "Cutscene":
        return ("CutsceneData", name, True, None, 0)
    if tag == "TextureAnimation":
        return ("AnimatedMaterial", name, True, None, 0)
    if tag == "Blob":
        return ("u8", name, True, None, ival(el.get("Size"), 1))
    if tag == "Mtx":
        return ("Mtx", name, False, None, 0)
    if tag == "Vtx":
        return ("Vtx", name, True, ival(el.get("Count"), None), 0)
    if tag == "Path":
        return ("Path", name, True, None, 0)
    if tag == "Scalar":
        return (SCALAR_TYPES.get(el.get("Type"), "u32"), name, False, None, 0)
    if tag == "Vector":
        return (VECTOR_TYPES.get(el.get("Type"), "Vec3s"), name, False, None, 0)
    if tag == "Pointer":
        return (el.get("Type", "void") + "*", name, False, None, 0)
    if tag == "Array":
        count = ival(el.get("Count"), None)
        child = list(el)
        if not child:
            return None
        c = child[0]
        if c.tag == "Vtx":
            t = "Vtx"
        elif c.tag == "Vector":
            t = VECTOR_TYPES.get(c.get("Type"), "Vec3s")
        elif c.tag == "Scalar":
            t = SCALAR_TYPES.get(c.get("Type"), "u32")
        elif c.tag == "Pointer":
            t = c.get("Type", "void") + "*"
        else:
            sub = decl_of(c) if c.get("Name") else None
            t = sub[0] if sub else "u8"
        return (t, name, True, count, 0)
    return None


def limb_enum(skel, file_el):
    none, mx, ename = skel.get("LimbNone"), skel.get("LimbMax"), skel.get("EnumName")
    if not (none and mx and ename):
        return ""
    prefix = none[: -len("NONE")] if none.endswith("NONE") else None
    limbs = []
    for el in file_el:
        if el.tag == "Limb" and el.get("EnumName"):
            en = el.get("EnumName")
            if prefix is None or en.startswith(prefix):
                limbs.append(en)
    # ZAPD lists the limbs in limb-table order; the XML lists them by offset, which is the same order for
    # every skeleton the decomp ships (placeholder headers only: a real extraction regenerates these).
    seen = set()
    out = "typedef enum %s {\n    /* 0x00 */ %s,\n" % (ename, none)
    i = 1
    for en in limbs:
        if en in seen:
            continue
        seen.add(en)
        out += "    /* 0x%02X */ %s,\n" % (i, en)
        i += 1
    out += "    /* 0x%02X */ %s\n} %s;\n" % (i, mx, ename)
    return out


def out_dir_for(xml_path, out_root):
    parts = os.path.normpath(xml_path).split(os.sep)
    # assets/xml/<...>/<name>.xml
    rel = parts[2:]
    name = os.path.splitext(rel[-1])[0]
    if "scenes" in parts:
        return os.path.join(out_root, "assets", *rel[:-1])
    return os.path.join(out_root, "assets", *rel[:-1], name)


def c_ident_ok(name):
    return re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name) is not None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_root", help="extracted/<version>")
    ap.add_argument("--build-root", help="build/<version> (for the .inc.c stubs)")
    ap.add_argument("--symbols", help="write the asset symbol table (.S) here")
    ap.add_argument("--stubs", action="store_true", help="also write code-embedded asset stubs")
    args = ap.parse_args()

    sym_lines = []
    nh = 0
    for dirpath, _, files in os.walk(os.path.join("assets", "xml")):
        if os.sep + "audio" in dirpath or dirpath.endswith("audio"):
            continue
        for fn in sorted(files):
            if not fn.endswith(".xml"):
                continue
            xml_path = os.path.join(dirpath, fn)
            root = ET.parse(xml_path).getroot()
            odir = out_dir_for(xml_path, args.out_root)
            os.makedirs(odir, exist_ok=True)
            for file_el in root.iter("File"):
                fname = file_el.get("OutName") or file_el.get("Name")
                seg = file_el.get("Segment")
                base = file_el.get("BaseAddress")
                guard = re.sub(r"[^A-Z0-9_]", "_", fname.upper())
                h = ["#ifndef %s_H\n#define %s_H 1\n\n" % (guard, guard)]
                enums = []
                externs = []
                defines = []
                defs = []
                for el in file_el:
                    if not isinstance(el.tag, str):
                        continue
                    d = decl_of(el)
                    if d is None:
                        continue
                    t, name, is_arr, count, size = d
                    if not c_ident_ok(name):
                        continue
                    static = el.get("Static") == "On"
                    if el.tag == "Skeleton":
                        e = limb_enum(el, file_el)
                        if e:
                            enums.append(e)
                    if el.tag == "Texture":
                        p = snake(name)
                        defines.append("#define %s_WIDTH %d\n#define %s_HEIGHT %d\n#define %s_SIZE 0x%X\n" % (
                            p, ival(el.get("Width")), p, ival(el.get("Height")), p, size))
                    if not static:
                        if is_arr:
                            externs.append("extern %s %s[%s];\n" % (t, name, str(count) if count and el.tag == "Array" and base else ""))
                        else:
                            externs.append("extern %s %s;\n" % (t, name))
                    if seg is not None and not static:
                        off = ival(el.get("Offset"))
                        sym_lines.append("%s = 0x%08X;" % (name, (int(seg, 0) << 24) | off))
                    if base is not None:
                        st = "static " if static else ""
                        if el.tag == "Texture":
                            defs.append("%su64 %s[%d] = { 0 };\n" % (st, name, max(1, (size + 7) // 8)))
                        elif el.tag == "DList":
                            defs.append("%sGfx %s[1] = { gsSPEndDisplayList() };\n" % (st, name))
                        elif is_arr:
                            defs.append("%s%s %s[%d] = { 0 };\n" % (st, t, name, count if count else max(1, size)))
                        else:
                            defs.append("%s%s %s = { 0 };\n" % (st, t, name))
                    if args.build_root and (el.tag == "Texture" or el.tag == "Blob"):
                        outn = el.get("OutName") or name
                        sfx = (el.get("Format") if el.tag == "Texture" else "bin") + ".inc.c"
                        bdir = odir.replace(args.out_root, args.build_root, 1)
                        os.makedirs(bdir, exist_ok=True)
                        with open(os.path.join(bdir, "%s.%s" % (outn, sfx)), "w") as f:
                            f.write("0\n")
                    if args.stubs and el.tag == "Array" and base is not None:
                        with open(os.path.join(odir, name + ".vtx.inc"), "w") as f:
                            f.write("{ 0 }\n")
                h += enums
                if enums:
                    h.append("\n")
                for dfn in defines:
                    h.append(dfn)
                h += externs
                h.append("\n#endif\n")
                with open(os.path.join(odir, fname + ".h"), "w") as f:
                    f.write("".join(h))
                nh += 1
                if args.stubs and base is not None:
                    c = ['/* placeholder (no ROM): zero-filled definitions, replaced by a real extraction */\n',
                         '#include "ultra64.h"\n#include "z64.h"\n#include "macros.h"\n',
                         '#include "%s.h"\n\n' % fname]
                    with open(os.path.join(odir, fname + ".c"), "w") as f:
                        f.write("".join(c + defs))
    if args.symbols:
        with open(args.symbols, "w") as f:
            f.write("/* generated from the asset XMLs: each asset symbol = its segmented address */\n")
            seen = set()
            for l in sym_lines:
                n = l.split(" ")[0]
                if n in seen:
                    continue
                seen.add(n)
                f.write(l + "\n")
    print("headers: %d, symbols: %d" % (nh, len(sym_lines)), file=sys.stderr)


if __name__ == "__main__":
    main()
