#!/usr/bin/env python3
"""Byte-swap the hex literals of texture .inc.c files so a little-endian compile
stores them in N64 (big-endian) byte order.

The handheld renderer reads texel data in N64 byte order everywhere (notes/m2-design.md).
Textures embedded in code (actor overlays, transitions) are compiled from these
.inc.c files as u64/u32/u16 arrays, which a little-endian compiler would store
byte-reversed per element. Each literal is reversed according to its width
(16 hex digits = u64, 8 = u32, 4 = u16).

usage: texbe.py <src_root> <dst_root> [file ...]   (paths relative to src_root)
"""
import re, sys
from pathlib import Path

LIT = re.compile(r"0x([0-9A-Fa-f]{16}|[0-9A-Fa-f]{8}|[0-9A-Fa-f]{4})\b")

def swap(m):
    h = m.group(1)
    return "0x" + "".join(reversed([h[i:i + 2] for i in range(0, len(h), 2)]))

def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    for rel in sys.argv[3:]:
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        text = (src / rel).read_text()
        new = LIT.sub(swap, text)
        if not out.exists() or out.read_text() != new:
            out.write_text(new)

if __name__ == "__main__":
    main()
