#!/bin/sh
# Frame set for renderer A/Bs (runs in the container: ./dev.sh sh tools/census.sh ...), 3D at 100%.
#   sh tools/census.sh <set name> <desktop binary> [VAR=value ...]   (the extras go to every run)
# Frames land in refs/seams/<set>/<scene>/dump; compare two sets with tools/cmpframes.py.
# The title (menus frames 100 and 150) differs run to run: mntitle.c picks colours with
# syUtilsRandTimeUCharRange, seeded from the clock. Ignore those two.
# All nine VS stages plus Meta Crystal with Metal Mario, the menus to the 1P character
# select, pause and results. Six at a time (the container has 6 CPUs).
set -u
SET=$1; BIN=$2; shift 2; EXTRA="$*"
R=/work/refs/ssb-decomp-re/baserom.us.z64; OUT=/work/refs/seams/$SET
rm -rf $OUT; mkdir -p $OUT
run() { # name env...
    name=$1; shift
    D=$OUT/$name; mkdir -p $D/dump
    ( cd $D && env SSB64_DATA=$D SSB64_ROM=$R SSB64_FREERUN=1 SSB64_INTRES=100 SSB64_W=240 SSB64_H=240 \
        SSB64_DUMP=$D/dump SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$@" $EXTRA $BIN > run.log 2>&1
      rm -f romdata.bin ) # the per-run extract of the ROM's data (14 MB): 595 of them filled 8.5 GB by round 24
}
F="SSB64_FRAMES=910 SSB64_DUMP_EVERY=100 SSB64_DUMP_FROM=200"
for st in 0 1 2 3 4 5; do run fight$st SSB64_BENCH=$st $F & done; wait
for st in 6 7 8; do run fight$st SSB64_BENCH=$st $F & done
run metal SSB64_BENCH=13 SSB64_BENCH_FIGHTERS=13,0,1,2 SSB64_FRAMES=1210 SSB64_DUMP_EVERY=50 SSB64_DUMP_FROM=200 &
run menus SSB64_AUTO=1 SSB64_FRAMES=1500 SSB64_DUMP_EVERY=50 SSB64_DUMP_FROM=0 &
run pause SSB64_BENCH=6 SSB64_AUTO=1 SSB64_FRAMES=900 SSB64_DUMP_EVERY=25 SSB64_DUMP_FROM=450 &
wait
run results SSB64_BENCH=6 SSB64_FRAMES=4800 SSB64_DUMP_EVERY=100 SSB64_DUMP_FROM=3900
echo "$SET: $(find $OUT -name '*.ppm' | wc -l) frames"
