#!/usr/bin/env python3
"""Rewrite GAS data directives so the emitted bytes are big-endian.

The N64 is big-endian and the game reads the sound tables (sequences.s,
instrument_sets.s) as raw ROM data.  The PSP assembler emits .word/.hword
little-endian, so those directives are turned into equivalent .byte lists.
Everything else (.incbin, labels, .balign, .byte) passes through unchanged.

usage: asm_be.py <in.s> <out.s>
"""
import re
import sys

DIRECTIVE_RE = re.compile(r"^(\s*)\.(word|hword|half|short|dword)\s+(.*)$")
# .align N means 2^N bytes on MIPS/ARM (what the decomp's .s files intend) but
# N bytes on x86: spell it as .balign 2^N, which every target reads the same.
# The layout matters -- sequences.s computes ROM-format offsets from labels.
ALIGN_RE = re.compile(r"^(\s*)\.align\s+(\d+)\s*(,.*)?$")


def split_operands(text):
    """Split on top-level commas (ignore commas inside parentheses)."""
    out, depth, cur = [], 0, []
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def convert_line(line):
    # Drop '#' comments entirely: x86 GAS accepts them but ARM GAS errors
    # ("junk at end of line") on trailing ones, and '@' (ARM's comment char)
    # is not portable the other way.  n64split's headers and per-byte offset
    # annotations are all comments; nothing semantic is lost.
    code, _, comment = line.partition("#")
    am = ALIGN_RE.match(code)
    if am:
        indent, n, fill = am.groups()
        return f"{indent}.balign {1 << int(n)}{fill or ''}\n"
    m = DIRECTIVE_RE.match(code)
    if not m:
        if comment:
            stripped = code.rstrip()
            return (stripped + "\n") if stripped else ""
        return line
    indent, kind, operands = m.groups()
    size = {"word": 4, "hword": 2, "half": 2, "short": 2, "dword": 8}[kind]
    bytes_out = []
    for op in split_operands(operands):
        for i in range(size - 1, -1, -1):
            shift = i * 8
            bytes_out.append(f"(({op}) >> {shift}) & 0xff")
    return f"{indent}.byte " + ", ".join(bytes_out) + "\n"


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        lines = f.readlines()
    with open(dst, "w") as f:
        for line in lines:
            f.write(convert_line(line))


if __name__ == "__main__":
    main()
