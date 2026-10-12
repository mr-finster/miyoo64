#!/usr/bin/env bash
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (GoldenEye e79bebd, device round 10: the watch's item floats). A desktop build with -flto into
# its own build dir, so the LTO link reports every function or object whose type differs between
# files (-Wlto-type-mismatch); then each one's declaration is read and those with a float or a
# 64-bit value are flagged: on ARM hard-float those travel in other registers than an int. Int,
# pointer and narrow returns all come back in r0, and data used only by address does not care.
# Function pointers are not covered (casts hide them). The link itself may fail (the scratch
# LDFLAGS); the warnings come before it.
#   port/tools/handheld/abiscan.sh   -> refs/abiscan/{build.out,mismatch.txt}; the report on stdout
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="$ROOT/refs/abiscan"
rm -rf "$OUT" && mkdir -p "$OUT"
"$ROOT/dev.sh" "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; cd /work/port && \
    make -f Makefile.handheld PLAT=pc BUILD_DIR=build/hh-abiscan -j6 EXTRA_CFLAGS='-flto -ffat-lto-objects' \
    LDFLAGS='-m32 -no-pie -flto=6 -Wlto-type-mismatch' > /work/refs/abiscan/build.out 2>&1 || true"
python3 - "$ROOT" <<'EOF'
import os, re, sys
R = sys.argv[1]
S = os.path.join(R, 'refs', 'abiscan')
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
    if not m0:
        continue
    name = m0.group(1)
    m = re.search(r"^(\S+?):(\d+):\d+: note: '%s' was previously declared here" % re.escape(name), text, re.M)
    decl = ''
    if m:
        p = os.path.normpath(os.path.join(R, 'port', m.group(1)))
        if os.path.exists(p):
            src = open(p, errors='replace').read().splitlines()
            decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime|uint64_t|int64_t)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
EOF
