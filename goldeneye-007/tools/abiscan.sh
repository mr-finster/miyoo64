#!/usr/bin/env bash
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (device round 10: the watch's item floats). Builds a scratch copy of port/ for the desktop
# with -flto and warnings on, so the LTO link reports every function or object whose type
# differs between files (-Wlto-type-mismatch), then reads each definition and flags those
# with a float or a 64-bit value: on ARM hard-float those travel in other registers than an
# int. Int, pointer and narrow returns all come back in r0 (the callee widens), and data
# used only by address does not care. Function pointers are not covered (casts hide them).
#   tools/abiscan.sh    -> m2scratch/abiscan/{build.out,mismatch.txt}; the report on stdout
# ABISCAN_SRC=<dir> scans another tree's port/ (e.g. an older commit's, from git archive).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="$ROOT/m2scratch/abiscan"
rm -rf "$S" && mkdir -p "$S"
rsync -a --exclude build --exclude core --exclude '*.core' "${ABISCAN_SRC:-$ROOT/port}/" "$S/port/"
python3 - "$S/port/Makefile.handheld" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('-include hh/include/hh_enum.h -w $(EXTRA_CFLAGS)', '-include hh/include/hh_enum.h $(EXTRA_CFLAGS)')
s = s.replace('-MMD -MP -w $(EXTRA_CFLAGS)', '-MMD -MP $(EXTRA_CFLAGS)')
s = s.replace('LIBS      := $(SDL_LIBS) -lm -lpthread',
              'LIBS      := $(SDL_LIBS) -lm -lpthread\nLDFLAGS   += -flto=6 -Wlto-type-mismatch')
open(p, 'w').write(s)
EOF
# the link itself may fail (the scratch flags); the warnings come before it
"$ROOT/dev.sh" "cd /work/m2scratch/abiscan/port && make -f Makefile.handheld PLAT=pc -j6 \
    EXTRA_CFLAGS='-flto -Wno-implicit-function-declaration -Wno-int-conversion' \
    > /work/m2scratch/abiscan/build.out 2>&1 || true"
python3 - "$S" <<'EOF'
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
    name = re.search(r"type of '([^']+)'", text).group(1)
    m = re.search(r"^(\S+?):(\d+):\d+: note: '%s' was previously declared here" % re.escape(name), text, re.M)
    decl = ''
    if m:
        src = open(os.path.normpath(os.path.join(S, 'port', m.group(1))), errors='replace').read().splitlines()
        decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
EOF
