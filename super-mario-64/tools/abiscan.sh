#!/usr/bin/env bash
# abiscan.sh -- declarations that disagree between files, which only the device's ABI punishes
# (GoldenEye e79bebd, the lead's round 2: its Facility vent crash). Builds the desktop proxy with
# -flto into build/abiscan/ (the normal build is untouched), so the LTO link reports every
# function or object whose type differs between files (-Wlto-type-mismatch), then flags those
# with a float or a 64-bit value: on ARM hard-float those travel in other registers than an
# int (the x86 proxy never notices). Int, pointer and narrow returns all come back in r0, and
# data used only by address does not care. Function pointers are not covered (casts hide them).
# One LTO partition: a parallel LTRANS runs its own make, which lost the jobserver here.
#   tools/abiscan.sh    -> refs/abiscan/{build.out,mismatch.txt}; the report on stdout
set -euo pipefail
cd "$(dirname "$0")/.."
out=refs/abiscan
rm -rf "$out" port/build/abiscan; mkdir -p "$out"  # a clean build: make does not track flags
# The asset objects stay out of LTO: objcopy renames their sections (the asset region), which
# it can't do to an LTO object. They are data; the ABI risk is in the code's function types.
printf 'include Makefile\n$(ASSET_OBJS): CFLAGS += -fno-lto\n' > "$out/Makefile.lto"
./dev.sh "cd /work/port && make -f /work/$out/Makefile.lto PLAT=pc -j6 BUILD_DIR_BASE=build/abiscan HH_OPT='-O1 -flto' \
  HH_LDEXTRA='-flto -flto-partition=one -Wlto-type-mismatch' > /work/$out/build.out 2>&1; echo make exit=\$? >> /work/$out/build.out"
python3 - "$out" port <<'EOF'
import os, re, sys
out, port = sys.argv[1], sys.argv[2]
lines = open(os.path.join(out, 'build.out'), errors='replace').read().splitlines()
blocks, i = [], 0
while i < len(lines):
    if 'does not match original declaration' in lines[i] or 'type of' in lines[i] and 'does not match' in lines[i]:
        b, j = [lines[i]], i + 1
        while j < len(lines) and j < i + 14 and 'does not match' not in lines[j]:
            if 'note:' in lines[j] or re.match(r'\s+\d+ \|', lines[j]):
                b.append(lines[j])
            j += 1
        blocks.append(b)
        i = j
    else:
        i += 1
open(os.path.join(out, 'mismatch.txt'), 'w').write('\n\n'.join('\n'.join(b) for b in blocks))
flag = []
for b in blocks:
    text = '\n'.join(b)
    m = re.search(r"type of '([^']+)'", text)
    name = m.group(1) if m else '?'
    m = re.search(r"^(\S+?):(\d+):\d+: note: '%s' was previously declared here" % re.escape(name), text, re.M)
    decl = ''
    if m:
        try:
            src = open(os.path.normpath(os.path.join(port, m.group(1))), errors='replace').read().splitlines()
            decl = ' '.join(src[max(0, int(m.group(2)) - 3):int(m.group(2))])
        except OSError:
            pass
    if re.search(r'\b(f32|f64|float|double|u64|s64|OSTime)\b', decl + text):
        flag.append((name, m.group(1) + ':' + m.group(2) if m else '?'))
print(lines[-1] if lines else 'no build output')
if not lines or lines[-1] != 'make exit=0':
    print('ERROR: the build did not finish, so the link never checked the types (see build.out)')
    sys.exit(1)
print('%d declarations differ between files; %d involve a float or a 64-bit value:' % (len(blocks), len(flag)))
for name, where in flag:
    print('  %-40s %s' % (name, where))
EOF
