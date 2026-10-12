#!/usr/bin/env bash
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (goldeneye-007 e79bebd, the Facility vent crash: a callback declared with s32s but defined
# with floats; on ARM hard-float the floats travel in VFP registers, so the caller's ints never
# arrive). Builds the desktop proxy with -flto into build/pc-abi so the LTO link reports every
# function or object whose type differs between files (-Wlto-type-mismatch), then reads each
# declaration and flags those with a float or a 64-bit value. Int, pointer and narrow returns
# all come back in r0, and data used only by address does not care. Function pointers are not
# covered (casts hide them). The link itself may fail; the warnings come before it.
#   port/tools/handheld/abiscan.sh    -> port/build/pc-abi/{abiscan.out,mismatch.txt}; report on stdout
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
S="$ROOT/port/build/pc-abi"
mkdir -p "$S"
"$ROOT/dev.sh" "cd /work/port && make -f Makefile.handheld PLAT=pc BUILD_DIR=build/pc-abi -j6 \
    EXTRA_CFLAGS='-flto -Wno-implicit-function-declaration -Wno-int-conversion' \
    EXTRA_LDFLAGS='-flto=6 -Wlto-type-mismatch' > /work/port/build/pc-abi/abiscan.out 2>&1 || true"
python3 - "$S" "$ROOT/port" <<'PY'
import os, re, sys
S, P = sys.argv[1], sys.argv[2]
lines = open(os.path.join(S, 'abiscan.out'), errors='replace').read().splitlines()
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
            src = open(os.path.normpath(os.path.join(P, m.group(1))), errors='replace').read().splitlines()
            decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
        except OSError:
            pass
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
PY
