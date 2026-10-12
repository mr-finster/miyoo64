#!/bin/sh
# qprof.sh -- ARM instruction profile of the bench under qemu-arm: one deterministic pass over the
# m1 scenes (-s 0: the warm-up pass only), counted per function.
#   sh tools/qprof.sh <tag> [ENV=val ...]        e.g.  sh tools/qprof.sh r9_scalar PM64_CMNEON=0
# Needs `sh tools/setup_env.sh` (qprof + hhroot steps): a plugin-enabled qemu-arm and ocarina-of-time's
# insnprof.c plugin in refs/qprof/ -- stock Ubuntu qemu-user has plugins compiled out. QEMU,
# PLUGIN, SYSROOT override. Output: refs/qprof/<tag>.txt (total + per-function counts). Compare
# the "total" of two tags; qemu WALL time is useless for NEON (emulated through helpers).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=$1; shift
QEMU=${QEMU:-$ROOT/refs/qprof/qemu-arm}; PLUGIN=${PLUGIN:-$ROOT/refs/qprof/insnprof.so}
SYSROOT=${SYSROOT:-$ROOT/refs/hhroot}
SYMPROF=${SYMPROF:-$(dirname "$PLUGIN")/symprof.py}
make -s -C "$ROOT/bench" PLAT=miyoo build/miyoo/pm64bench.sym
O=$ROOT/refs/qprof; mkdir -p "$O"
cd "$ROOT/refs/cap"
env SDL_VIDEODRIVER=dummy "$@" "$QEMU" -L "$SYSROOT" -plugin "$PLUGIN,out=$O/$TAG.raw" \
    "$ROOT/bench/build/miyoo/pm64bench.sym" -s 0 -d "$O/log_$TAG" m1/*.pms >/dev/null 2>&1
${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-nm -n --defined-only "$ROOT/bench/build/miyoo/pm64bench.sym" > "$O/$TAG.syms"
{ grep -i '^total' "$O/$TAG.raw"; python3 "$SYMPROF" "$O/$TAG.syms" "$O/$TAG.raw" 2>/dev/null | head -40; } > "$O/$TAG.txt"
cat "$O/$TAG.txt"
