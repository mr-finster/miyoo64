#!/bin/sh
# qprof_tree.sh <bench dir> <tag>: ARM instruction profile of any bench tree (a worktree of an old round under refs/, say), one -s 0 pass over m1, inside ./dev.sh. Output: refs/qprof/<tag>.txt.
set -eu
B=$1; TAG=$2; O=/work/refs/qprof
make -s -C "$B" PLAT=miyoo PM_INC=/work/refs/papermario/include build/miyoo/pm64bench.sym
cd /work/refs/cap
env SDL_VIDEODRIVER=dummy /work/refs/qprof/qemu-arm -L /work/refs/hhroot -plugin "/work/refs/qprof/insnprof.so,out=$O/$TAG.raw" "$B/build/miyoo/pm64bench.sym" -s 0 -d "$O/log_$TAG" m1/*.pms >/dev/null 2>&1
${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-nm -n --defined-only "$B/build/miyoo/pm64bench.sym" > "$O/$TAG.syms"
{ grep -i '^total' "$O/$TAG.raw"; /usr/bin/python3 /work/refs/qprof/symprof.py "$O/$TAG.syms" "$O/$TAG.raw" 2>/dev/null | head -25; } > "$O/$TAG.txt"
cat "$O/$TAG.txt"
