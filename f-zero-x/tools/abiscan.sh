#!/usr/bin/env bash
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (after goldeneye-007 e79bebd: its Facility vent crash). The desktop build is 32-bit x86,
# where float and int arguments both travel on the stack, so a float parameter declared as an
# int elsewhere works there by luck; ARM hard-float passes floats in VFP registers and the
# callee reads the wrong ones. A float or 64-bit return, or a 64-bit argument (register pairs),
# breaks the same way.
# Builds the desktop target into build/pc-abiscan from a scratch copy of Makefile.handheld (in
# refs/abiscan/, untracked) with -flto and the game's warnings on, so the LTO link reports every
# function or object whose type differs between files (-Wlto-type-mismatch), then flags the
# ones with a float or a 64-bit value. Function pointers are not covered (casts hide them).
#   tools/abiscan.sh    (on the host; it calls ./dev.sh)
#   -> refs/abiscan/{build.out,mismatch.txt}; the report on stdout
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="$ROOT/refs/abiscan"
mkdir -p "$S"
python3 - "$ROOT/game/Makefile.handheld" "$S/Makefile.abiscan" <<'EOF'
import sys
s = open(sys.argv[1]).read()
def rep(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)
rep('-fexec-charset=EUC-JP -w\n', '-fexec-charset=EUC-JP\n')
rep('LIBS := $(SDL_LIBS) -lz -lm -lpthread', 'LIBS := $(SDL_LIBS) -lz -lm -lpthread\nLDFLAGS += -flto=6 -Wlto-type-mismatch')
open(sys.argv[2], 'w').write(s)
EOF
# fat objects keep nm (the layout check) and objcopy working; the link itself may fail on the
# scratch flags, and its warnings come before that
"$ROOT/dev.sh" "cd /work/game && B=build/pc-abiscan && M=/work/refs/abiscan/Makefile.abiscan && \
    X='-flto -ffat-lto-objects -Wno-implicit-function-declaration -Wno-int-conversion' && \
    { [ -d \$B/gen ] || make -f \$M PLAT=pc BUILD_DIR=\$B EXTRA_CFLAGS=\"\$X\" gen; } > /work/refs/abiscan/build.out 2>&1; \
    make -f \$M PLAT=pc BUILD_DIR=\$B EXTRA_CFLAGS=\"\$X\" -j6 >> /work/refs/abiscan/build.out 2>&1 || true"
python3 - "$S" "$ROOT/game" <<'EOF'
import os, re, sys
S, G = sys.argv[1], sys.argv[2]
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
    m = re.search(r"type of '([^']+)'", text)
    name = m.group(1) if m else '?'
    m = re.search(r"^(\S+?):(\d+):\d+: note: '%s' was previously declared here" % re.escape(name), text, re.M)
    decl = ''
    if m:
        p = os.path.normpath(os.path.join(G, m.group(1)))
        if os.path.exists(p):
            src = open(p, errors='replace').read().splitlines()
            decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime|long long)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
EOF
