#!/usr/bin/env bash
# levelsweep.sh -- tools/sweep.sh across the 20 missions: the desktop proxy boots each from its
# -level_ token, skips the intro, walks, turns and fires (a fixed pad script) to VI
# 1600, dumping every 50th frame from 100. For renderer changes that may touch any level (the
# Frigate sea's palette rule, round 12): the frame count per mission, compared with
#   tools/levelsweep.sh <name> [VAR=value ...]   -> m2scratch/sweep/<name>/lv<id>/
#   tools/sweepdiff.py <before> <after>          (every scenario directory both runs have)
# Unpaced (GE_NOPACE=1), square output at 100% 3D, from port/build/pc (build it first). Six at a time,
# each capped at LEVELSWEEP_TIMEOUT seconds (default 5400: the shared Docker VM can be slow; a
# run cut short just has fewer frames, and tools/sweepdiff.py counts those as differing).
# SWEEP_BIN=<path under the repo> runs another build's binary (a copy, so port/ can be rebuilt
# while the sweep runs), as in tools/sweep.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:?usage: tools/levelsweep.sh <name> [VAR=value ...]}"
shift
EXTRA="$*"
TMO="${LEVELSWEEP_TIMEOUT:-5400}"
BIN="${SWEEP_BIN:+/work/$SWEEP_BIN}"
BIN="${BIN:-./build/pc/ge007}"
LEVELS="33 34 35 36 09 20 26 43 27 22 24 29 30 25 37 23 39 41 28 32"
rm -rf "$ROOT/m2scratch/sweep/$NAME"
for L in $LEVELS; do mkdir -p "$ROOT/m2scratch/sweep/$NAME/lv$L"; done
"$ROOT/dev.sh" "cd /work/port && ulimit -c 0; n=0;
  for L in $LEVELS; do
    env $EXTRA GE_NOPACE=1 GE_QUIET=1 GE_W=240 GE_H=240 GE_EXIT_AT=1600 GE_ARGS=-level_\$L GE_SAVE_DIR=/tmp/ls_${NAME}_\$L \
        GE_PADSCRIPT=600:b,606:,700:u,1000:ul,1100:u,1200:ub,1206:u,1250:ub,1256:u,1400:r,1600: \
        GE_DUMP=/work/m2scratch/sweep/$NAME/lv\$L GE_DUMP_FROM=100 GE_DUMP_EVERY=50 \
        timeout $TMO $BIN > /work/m2scratch/sweep/$NAME/lv\$L.out 2>&1 &
    n=\$((n + 1)); if [ \$((n % 6)) -eq 0 ]; then wait; fi
  done; wait"
for L in $LEVELS; do
  printf 'lv%-3s %3d frames  %s\n' "$L" "$(ls "$ROOT/m2scratch/sweep/$NAME/lv$L" | wc -l)" \
    "$(grep -a -m1 -e 'CRASH' -e 'GE_EXIT_AT' "$ROOT/m2scratch/sweep/$NAME/lv$L.out" | cut -c1-60)"
done
