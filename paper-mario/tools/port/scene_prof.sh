#!/bin/sh
# The game's ARM instruction profile at a scene (the speed round): the device build under a plugin-enabled qemu-arm
# (refs/qprof), run to game frame N; two runs' difference is one window of the scene (tools: refs/qprof/symprof.py
# <syms> <run to N1> <run to N2>). Frames stay local.
#   tools/port/scene_prof.sh <tag> <frames> ENV=val ...        (in ./dev.sh; PM64_DATA defaults to refs/speed/hhdata)
set -e
TAG=$1; N=$2; shift 2
Q=refs/qprof; mkdir -p refs/speed/hhdata
${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-nm -n --defined-only build/game/miyoo/pm64 > $Q/game.syms
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "PM64_ROM=$PWD/Paper Mario (USA).z64" PM64_DATA=refs/speed/hhdata PM64_SEED=1 \
    PM64_FRAMES=$N "$@" $Q/qemu-arm -L refs/hhroot -plugin $Q/insnprof.so,out=$Q/$TAG.raw ./build/game/miyoo/pm64 \
    > refs/speed/$TAG.log 2>&1
grep -i '^total' $Q/$TAG.raw
