#!/usr/bin/env bash
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (GoldenEye e79bebd, the ports lead's round 2). Builds a scratch copy of base-psp/ for the
# desktop with -flto, so the LTO link reports every function or object whose type differs
# between files (-Wlto-type-mismatch), then reads each first declaration and flags those with a
# float or a 64-bit value: on ARM hard-float those travel in other registers than an int (s0 vs
# r0), so the mismatch breaks only on the device. Int, pointer and narrow returns all come back
# in r0, and data used only by address does not care. Function pointers are not covered.
#   tools/abiscan.sh    -> bx/abiscan/{build.out,mismatch.txt}; the report on stdout
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="$ROOT/bx/abiscan"
rm -rf "$S" && mkdir -p "$S"
rsync -a --exclude build --exclude '*.core' "$ROOT/base-psp/" "$S/base-psp/"
python3 - "$S/base-psp/Makefile.handheld" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'LIBS    := $(SDL_LIBS) -lm -lpthread'
assert s.count(old) == 1
s = s.replace(old, old + '\nLDFLAGS += -flto=6 -Wlto-type-mismatch')
# the asset objects stay plain: the asset split renames their sections with objcopy, which
# can't touch LTO objects (and data used by address has no ABI to check)
old = '$(ASSET_OBJS): CFLAGS += -fno-single-precision-constant'
assert s.count(old) == 1
s = s.replace(old, old + '\n$(ASSET_OBJS): CFLAGS += -fno-lto')
open(p, 'w').write(s)
PY
# the link itself may fail (the scratch flags); the warnings come before it
"$ROOT/dev.sh" "cd /work/bx/abiscan/base-psp && make -f Makefile.handheld PLAT=pc -j6 \
    EXTRA_CFLAGS='-flto -Wno-implicit-function-declaration -Wno-int-conversion' \
    > /work/bx/abiscan/build.out 2>&1 || true"
python3 - "$S" <<'PY'
import os, re, sys
S = sys.argv[1]
lines = open(os.path.join(S, 'build.out'), errors='replace').read().splitlines()
blocks, i = [], 0
while i < len(lines):
    if 'does not match original declaration' in lines[i]:
        b, j = [lines[i]], i + 1
        while j < len(lines) and j < i + 14 and 'does not match original declaration' not in lines[j]:
            if 'note:' in lines[j] or re.match(r'\s+\d+ \|', lines[j]):
                b.append(lines[j])
            j += 1
        blocks.append(b)
        i = j
    else:
        i += 1
open(os.path.join(S, 'mismatch.txt'), 'w').write('\n\n'.join('\n'.join(b) for b in blocks))
flag = []
for b in blocks:
    text = '\n'.join(b)
    m0 = re.search(r"type of '([^']+)'", text)
    name = m0.group(1) if m0 else '?'
    m = re.search(r"^(\S+?):(\d+):\d+: note: '%s' was previously declared here" % re.escape(name), text, re.M)
    decl = ''
    if m:
        try:
            src = open(os.path.normpath(os.path.join(S, 'base-psp', m.group(1))), errors='replace').read().splitlines()
            decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
        except OSError:
            pass
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
failed = [l for l in lines if re.search(r'\*\*\* \[.*\] Error', l)]
if failed or not any('lto' in l.lower() and 'mismatch' in l.lower() for l in lines) and not blocks:
    print('NOTE: the LTO link did not complete (%s): the count below may be incomplete' % (failed[0].strip() if failed else 'no LTO output'))
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
print('(build.out: %d lines)' % len(lines))
PY
