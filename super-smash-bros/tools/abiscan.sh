#!/bin/sh
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (GoldenEye e79bebd, the lead's round-2 item 7: a callback declared with s32s where the caller
# passed floats left the device reading a stale register, the Facility vent crash). Builds the
# desktop port with LTO in build/desktop-lto, so the link reports every function or object whose
# type differs between files (-Wlto-type-mismatch), then flags those with a float or a 64-bit
# value: on ARM hard-float those travel in other registers than an int. Int, pointer and narrow
# returns all come back in r0, and data used only by address does not care. Function pointers
# are not covered (casts hide them). The desktop build is -m32, so sizes match the device's.
#   ./dev.sh sh tools/abiscan.sh    -> refs/abiscan/{build.out,mismatch.txt}; the report on stdout
set -eu
cd "$(dirname "$0")/.."
S=refs/abiscan
rm -rf "$S" port/build/desktop-lto && mkdir -p "$S"
# the link may fail under the scratch flags; the warnings come before it
make -C port PLAT=desktop BUILD=build/desktop-lto -j6 \
    ARCH='-m32 -fno-pie -malign-data=abi -msse2 -mfpmath=sse -flto=6 -Wlto-type-mismatch' \
    > "$S/build.out" 2>&1 || true
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
    m0 = re.search(r"type of '([^']+)'", text)
    name = m0.group(1) if m0 else '?'
    m = re.search(r"^(\S+?):(\d+):\d+: note: '%s' was previously declared here" % re.escape(name), text, re.M)
    decl = ''
    if m:
        p = os.path.normpath(os.path.join('port', m.group(1)))
        if os.path.exists(p):
            src = open(p, errors='replace').read().splitlines()
            decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime|long long)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
EOF
