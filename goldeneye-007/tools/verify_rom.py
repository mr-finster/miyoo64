#!/usr/bin/env python3
"""Identify a GoldenEye 007 dump without copying it anywhere.

Usage: tools/verify_rom.py <rom> [--write-z64 <out.z64>]

Detects the byte order from the first word (z64 80371240 big-endian, v64 37804012
byte-swapped, n64 40123780 word-swapped), normalises to z64 in memory, and checks it
against the three retail versions the n64decomp/007 decomp builds (sha1 from
ge007.{u,j,e}.sha1, md5 from scripts/extract_cdata.sh). Prints the version and exits 0
only for a known retail dump. --write-z64 writes the normalised big-endian image (only
needed if the dump is v64/n64; the decomp wants baserom.u.z64).
"""
import hashlib, sys

KNOWN = {
    "abe01e4aeb033b6c0836819f549c791b26cfde83": ("US (NTSC-U) 1.0", "70c525880240c1e838b8b1be35666c3b"),
    "2a5dade32f7fad6c73c659d2026994632c1b3174": ("JP (NTSC-J)", "1880da358f875c0740d4a6731e110109"),
    "167c3c433dec1f1eb921736f7d53fac8cb45ee31": ("EU (PAL)", "cff69b70a8ad674a0efe5558765855c9"),
}

def to_z64(b: bytes) -> tuple[bytes, str]:
    head = b[:4]
    if head == b"\x80\x37\x12\x40":
        return b, "z64"
    if head == b"\x37\x80\x40\x12":   # v64: 16-bit byte swap
        a = bytearray(b); a[0::2], a[1::2] = b[1::2], b[0::2]
        return bytes(a), "v64"
    if head == b"\x40\x12\x37\x80":   # n64: 32-bit little-endian words
        a = bytearray(len(b))
        for i in range(0, len(b) - 3, 4):
            a[i:i + 4] = b[i:i + 4][::-1]
        return bytes(a), "n64"
    raise SystemExit(f"not an N64 ROM (first word {head.hex()})")

def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__); return 2
    raw = open(sys.argv[1], "rb").read()
    z, order = to_z64(raw)
    sha1 = hashlib.sha1(z).hexdigest(); md5 = hashlib.md5(z).hexdigest()
    name = z[0x20:0x34].decode("ascii", "replace").rstrip()
    print(f"size {len(z)} bytes, byte order {order}, name '{name}', id '{z[0x3B:0x3E].decode('ascii','replace')}', "
          f"country '{chr(z[0x3E])}', version {z[0x3F]}, crc {z[0x10:0x14].hex()}/{z[0x14:0x18].hex()}")
    print(f"sha1 {sha1}\nmd5  {md5}")
    known = KNOWN.get(sha1)
    if "--write-z64" in sys.argv:
        out = sys.argv[sys.argv.index("--write-z64") + 1]
        open(out, "wb").write(z); print(f"wrote {out}")
    if not known:
        print("UNKNOWN dump: not one of the three retail versions the decomp builds"); return 1
    if known[1] != md5:
        print("sha1 matches but md5 does not (?)"); return 1
    print(f"OK: GoldenEye 007 {known[0]} retail dump")
    return 0

if __name__ == "__main__":
    sys.exit(main())
