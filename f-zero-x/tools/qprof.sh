#!/bin/sh
# ARM instruction profile of the handheld build under qemu-arm (after ocarina-of-time
# tools/qprof). The game is deterministic under qemu (virtual clock), so
# two runs from boot that exit at retraces FROM and TO give exactly what frames
# FROM..TO cost, every thread included.
#
#   sh tools/qprof.sh <tag> [from] [to] ["<FZX_KEYS script>"]
#
# Default window: VI 1200-1500 of the autopilot run (Mute City GP, 30 machines).
# Needs a qemu-arm with plugin support (QEMU=path) and the matching qemu-plugin.h
# under $QEMU_SRC/include/qemu; ./dev.sh's image has both (docker/Dockerfile).
# Output: $OUT/qprof_<tag>/profile.txt (default refs/, untracked). ARMBIN= profiles another build's
# binary. TOP=n: the functions listed, in the file and on screen (qprof_sym.py reads it too).
set -u
TAG=$1; F1=${2:-1200}; F2=${3:-1500}; KEYS=${4:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-qemu-arm}
QEMU_SRC=${QEMU_SRC:-/opt/qemu-plugin}
SR=${ARMROOT:-/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot}
OUT=${OUT:-$ROOT/refs}/qprof_$TAG
BIN=${ARMBIN:-$ROOT/game/build/miyoo/fzerox} # ARMBIN=: another build's binary (as tools/sweep.sh)
PY=${PYTHON:-python3}
rm -rf "$OUT"; mkdir -p "$OUT/data"
gcc -shared -fPIC -O2 -I"$QEMU_SRC/include/qemu" "$ROOT/tools/qprof_insn.c" -o "$OUT/insnprof.so" || exit 1
[ -f "$ROOT/game/fzerox.sram.ref" ] && cp "$ROOT/game/fzerox.sram.ref" "$OUT/data/fzerox.sram"
for F in $F1 $F2; do
    # both runs start alike: the first launch hashes the ROM (~0.8 G instructions) and
    # records it in romcheck.txt, which the second would otherwise skip
    rm -f "$OUT/data/console.log" "$OUT/data/romcheck.txt"
    if [ -n "$KEYS" ]; then AUTO=; else AUTO=1; fi
    FZX_ROM=$ROOT/game/baserom.us.rev0.z64 FZX_DATA=$OUT/data FZX_HEADLESS=1 FZX_AUTO=$AUTO FZX_KEYS=$KEYS \
        FZX_EXIT_AT=$F "$QEMU" -L "$SR" -plugin "$OUT/insnprof.so,out=$OUT/prof_$F.txt" "$BIN" > "$OUT/run_$F.log" 2>&1
done
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-nm -n --defined-only "$BIN" > "$OUT/syms.txt"
$PY "$ROOT/tools/qprof_sym.py" "$OUT/syms.txt" "$OUT/prof_$F1.txt" "$OUT/prof_$F2.txt" > "$OUT/profile.txt"
head -${TOP:-25} "$OUT/profile.txt"
