#!/bin/sh
# Device samples per source line of one gfx_soft.c function (tools/sprof.py's --lines needs -g,
# which the shipped build lacks). Recompiles gfx_soft.c as it was at <commit> (the build the
# samples came from) with -g and the handheld flags, then charges each sample in <function> to the
# OUTERMOST inlined frame of its offset (as tools/qprof_lines.sh), i.e. the line of the macro or
# helper call that is hot. Run it in ./dev.sh.
#
#   sh tools/sprof_lines.sh <bench_prof.bin> <unstripped fzerox> <commit> <function> [N]
set -e
P=$1; BIN=$2; C=$3; F=$4; N=${5:-20}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
O=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf
T=$ROOT/game/src/port/gfx/.sprof_gfx_soft.c # beside the real one, for its relative includes (untracked)
OBJ=$ROOT/refs/sprof_gfx_soft_g.o
cd "$ROOT/game"
git show "$C:game/src/port/gfx/gfx_soft.c" > "$T"
CMD=$(make -f Makefile.handheld PLAT=miyoo -n -B build/miyoo/src/port/gfx/gfx_soft.o | grep " -c " | tail -1)
eval "$(echo "$CMD" | sed "s| -o [^ ]*| -g -o $OBJ|; s|src/port/gfx/gfx_soft.c|$T|; s|^[@ ]*||")"
rm -f "$T"
S=$($O-nm "$BIN" | awk -v f=$F '$3==f{print $1}'); E=$($O-nm -n "$BIN" | awk -v f=$F 'p{print $1;exit} $3==f{p=1}')
SO=$($O-nm "$OBJ" | awk -v f=$F '$3==f{print $1}')
python3 - "$P" $S $E $SO $N "$OBJ" "$O-addr2line" <<'PY'
import sys, struct, subprocess, collections
path, s, e, so, n, obj, a2l = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16), int(sys.argv[5]), sys.argv[6], sys.argv[7]
data = open(path, 'rb').read(); pos = 0; cnt = collections.Counter(); total = 0
while pos < len(data):
    nl = data.index(b'\n', pos); head = data[pos:nl]; pos = nl + 1
    if not head.startswith(b'FZXSPROF1'): break
    nl = data.index(b'\n', pos); k, lost = map(int, data[pos:nl].split()); pos = nl + 1
    for i in range(k):
        pc, lr = struct.unpack_from('<II', data, pos + 8 * i)
        total += 1
        if s <= pc < e: cnt[pc] += 1
    pos += 8 * k
    pos = data.index(b'END\n', pos) + 4
rows = list(cnt.items())
out = subprocess.run([a2l, '-i', '-a', '-e', obj] + [hex(pc - s + so) for pc, _ in rows], capture_output=True, text=True).stdout
agg = collections.Counter()
for (pc, c), blk in zip(rows, out.split('0x')[1:]):
    ls = [x for x in blk.strip().split('\n')[1:] if x]
    agg[ls[-1].split('/')[-1].split(' ')[0] if ls else '?'] += c
tot = sum(agg.values())
print('%d samples in the function, %d in all (%.1f%%)' % (tot, total, 100.0 * tot / max(total, 1)))
for k, v in agg.most_common(n): print('%7d %5.1f%% of it  %5.2f%% of all  %s' % (v, 100.0 * v / tot, 100.0 * v / total, k))
PY
