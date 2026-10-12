#!/usr/bin/env bash
# Headless sweep over levels (one container run): Mario wanders in each, frames
# dumped into refs/<tag>/L<level>/. Repeatable (SM64_AUDIO_SYNC), so two builds
# compare frame for frame with tools/sweepdiff.sh.
# usage: [SWEEP_ENV='VAR=value ...'] tools/sweep.sh <tag> [frames=600] [every=100] [levels...]
#   SWEEP_ENV: extra variables for every run (an A/B switch, e.g. SM64_LOOKAT=0)
#   SWEEP_BIN: the binary (default build/us_pc/sm64; build/us_pc_ubsan/sm64 for UBSAN=1)
#   default levels: castle grounds, castle, BoB, WF, JRB, CCM, BBH, HMC, LLL,
#   SSL, DDD, TTM (the debug level select's numbers, = the LEVEL_* enum)
set -euo pipefail
tag=$1; frames=${2:-600}; every=${3:-100}; shift $(( $# < 3 ? $# : 3 ))
levels=${*:-16 6 9 24 12 5 4 7 22 8 23 36}
cd "$(dirname "$0")/.."
rm -rf "refs/$tag"; mkdir -p "refs/$tag"
./dev.sh "cd /work/port && for l in $levels; do mkdir -p /work/refs/$tag/L\$l; \
  env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SM64_DATA=/tmp/sw\$l SM64_AUTO=2 SM64_AUDIO_SYNC=1 SM64_LEVEL=\$l \
  SM64_W=240 SM64_H=240 SM64_DUMP=/work/refs/$tag/L\$l SM64_DUMP_EVERY=$every SM64_DUMP_FROM=$every SM64_EXIT_AT=$frames ${SWEEP_ENV:-} \
  timeout 600 ./${SWEEP_BIN:-build/us_pc/sm64} > /work/refs/$tag/L\$l/run.log 2>&1; echo L\$l exit=\$?; done"
