#!/bin/sh
# Frame-dump sweep for bit-exactness A/B (desktop or ARM under qemu-arm).
#   tools/sweep.sh desktop|arm <outdir> <binary> [VAR=value ...]
# Dumps frames 200..900 (every 100) of Dream Land (6), Sector Z (1) and Zebes
# (3) on a square output, 3D at 100% (the shipped resolution; pass SSB64_INTRES=85 to A/B a rung), FREE-RUN: with pacing on, a dropped render shifts
# which tick frame N shows and time-dependent effects "differ" (round 8).
# Compare two sweeps with tools/cmpframes.py <dirA> <dirB>.
# Needs SSB64_ROM (the user's ROM; never commit it) and, for arm, $ARMROOT
# (the Miyoo toolchain's sysroot, docker/Dockerfile). The NEON paths only exist
# in ARM builds: a desktop sweep proves nothing about them. A BENCH= build
# ignores SSB64_BENCH (always stage 6 first): sweep the plain `make PLAT=miyoo` binary.
set -eu
MODE=$1; OUT=$2; BIN=$(readlink -f "$3"); shift 3
: "${SSB64_ROM:?set SSB64_ROM to the US ROM}"
ARMROOT=${ARMROOT:-/tmp/armroot}
mkdir -p "$OUT"
for st in 6 1 3; do
    D=$OUT/st$st; rm -rf "$D"; mkdir -p "$D/dump"
    (
        cd "$D"
        export SSB64_DATA=$D SSB64_FREERUN=1 SSB64_INTRES=100 SSB64_W=240 SSB64_H=240 \
               SSB64_DUMP=$D/dump SSB64_DUMP_EVERY=100 SSB64_DUMP_FROM=200 SSB64_FRAMES=910 \
               SSB64_BENCH=$st SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
        for kv in "$@"; do export "$kv"; done
        if [ "$MODE" = arm ]; then qemu-arm -L "$ARMROOT" "$BIN" > run.log 2>&1
        else "$BIN" > run.log 2>&1; fi
        rm -f romdata.bin # the per-run extract of the ROM's data (see census.sh)
    ) &
done
wait
ls "$OUT"/st*/dump | grep -c ppm | sed 's/^/frames: /'
