#!/bin/sh
# 1P game "DK VS Metal Mario" (spgame_stage 10, DK), 3D at 100%, every 20 ticks from 3140 to 8240
# (= z64 reference frames 3600..8700 in refs/m64ref/magnify1: ref = port + 460).
#   ./dev.sh sh tools/sp_route.sh <set name> <desktop binary> [VAR=value ...]
# FROM/EVERY/UNTIL override the dump start, step and last tick. Compare with tools/refcloser.py
# (in step with z64 to about port tick 3560, round 15).
set -u
SET=$1; BIN=$2; shift 2
R=/work/refs/ssb-decomp-re/baserom.us.z64; D=/work/refs/seams/$SET
rm -rf $D; mkdir -p $D/dump
cd $D && env SSB64_DATA=$D SSB64_ROM=$R SSB64_FREERUN=1 SSB64_INTRES=100 SSB64_W=240 SSB64_H=240 \
    SSB64_DUMP=$D/dump SSB64_DUMP_EVERY=${EVERY:-20} SSB64_DUMP_FROM=${FROM:-3140} SSB64_FRAMES=${UNTIL:-8245} \
    SSB64_START_SCENE=52 SSB64_SPGAME_STAGE=10 SSB64_SPGAME_FKIND=2 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$@" $BIN > run.log 2>&1
echo "$SET: $(ls $D/dump | wc -l) frames"
