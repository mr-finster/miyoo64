#!/usr/bin/env python3
"""Give every top-level array in Torch's ast_radio.c the N64's 4-byte alignment.

usage: align_radio.py <src ast_radio.c> <dst .c>

The message bank is linked into RAM on the N64 and IDO aligned each of its
arrays to 4 bytes; GCC aligns u16 arrays to 2, which shifts every symbol
after the first odd-length array. The port rebuilds this bank at boot from
the ROM (derive_recipes.py --radio), which needs the N64's layout exactly,
so each definition gets __attribute__((aligned(4))). The output is a build
product (ROM-derived, git-ignored), never committed.
"""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
pat = re.compile(r'^((?:static\s+)?(?:const\s+)?[A-Za-z_]\w*\*?\s+[A-Za-z_]\w*(?:\[[^\]]*\])+)\s*=', re.M)
out, n = pat.subn(lambda m: m.group(1) + ' __attribute__((aligned(4))) =', text)
open(dst, 'w').write(out)
print(f'align_radio: {n} arrays aligned to 4')
