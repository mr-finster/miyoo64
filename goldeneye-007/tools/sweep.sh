#!/usr/bin/env bash
# sweep.sh -- a fixed set of desktop-proxy runs whose frame dumps are compared between two
# builds or two knob settings (tools/sweepdiff.py): the frame count every screen-changing fix
# reports, and the frames its before/after sheets are cut from.
#   tools/sweep.sh <name> [VAR=value ...]    -> m2scratch/sweep/<name>/{play,menu,intro}/
# The runs are unpaced (GE_NOPACE=1: deterministic), square output at 100% 3D, from the build in
# port/build/pc (build it first). The VAR=value pairs go into every run's environment, e.g.
#   tools/sweep.sh before GE_NEARCLIP=1 ; tools/sweep.sh after ; tools/sweepdiff.py before after
# Scenarios:
#   play   the Dam from a stage boot, the watch opened and closed (VI 300-2100, every 8th frame)
#   menu   boot, logos, title, file select, mission select, difficulty, briefing (every 6th)
#   intro  the menu path into the Dam intro with its truck camera (GE_INTROCAM=5), every 4th
#   snow_game, snow_split, walk_game, walk_split  Surface 2's near snow (2026-10-04): Bench 1's two
#          Surface 2 views (game fog, fog split) and a straight walk with each, every frame 320-607;
#          tools/snowcheck.py holds their brightness flicker to limits (exit 1 past them)
# SWEEP_BIN=<path under the repo> runs another build's binary (an older commit's, built in a
# scratch copy) from port/, e.g. SWEEP_BIN=m2scratch/bt_head/port/build/pc/ge007.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${1:?usage: tools/sweep.sh <name> [VAR=value ...]}"
shift
EXTRA="$*"
OUT="/work/m2scratch/sweep/$NAME"
rm -rf "$ROOT/m2scratch/sweep/$NAME"
mkdir -p "$ROOT/m2scratch/sweep/$NAME"/{play,menu,intro,snow_game,snow_split,walk_game,walk_split}
BENCHPAD='600:b,606:,700:u,1000:ul,1100:u,1200:ub,1206:u,1250:ub,1256:u' # hh_bench.c's child
MENU='1500:s,1506:,1800:a,1806:,2000:a,2006:,2200:a,2206:,2400:a,2406:'
BIN="${SWEEP_BIN:+/work/$SWEEP_BIN}"
BIN="${BIN:-./build/pc/ge007}"
"$ROOT/dev.sh" "cd /work/port && ulimit -c 0;
  env $EXTRA GE_NOPACE=1 GE_QUIET=1 GE_W=240 GE_H=240 GE_EXIT_AT=2100 GE_ARGS=-level_33 GE_SAVE_DIR=/tmp/sw_${NAME}_p \
      GE_PADSCRIPT=1500:s,1506:,1700:r,1703:,1800:r,1803:,1950:s,1956: GE_DUMP=$OUT/play GE_DUMP_FROM=300 GE_DUMP_EVERY=8 \
      timeout 2400 $BIN > $OUT/play.out 2>&1 &
  env $EXTRA GE_NOPACE=1 GE_QUIET=1 GE_W=240 GE_H=240 GE_EXIT_AT=3000 GE_SAVE_DIR=/tmp/sw_${NAME}_m \
      GE_PADSCRIPT=$MENU GE_DUMP=$OUT/menu GE_DUMP_FROM=700 GE_DUMP_EVERY=6 \
      timeout 2400 $BIN > $OUT/menu.out 2>&1 &
  env $EXTRA GE_NOPACE=1 GE_QUIET=1 GE_W=240 GE_H=240 GE_EXIT_AT=3480 GE_INTROCAM=5 GE_SAVE_DIR=/tmp/sw_${NAME}_i \
      GE_PADSCRIPT=$MENU,2800:s,2806: GE_DUMP=$OUT/intro GE_DUMP_FROM=1360 GE_DUMP_EVERY=4 \
      timeout 2400 $BIN > $OUT/intro.out 2>&1 &
  wait
  for s in snow_game:0:$BENCHPAD snow_split:1:$BENCHPAD walk_game:0:600:b,606:,700:u walk_split:1:600:b,606:,700:u; do
    n=\${s%%:*}; r=\${s#*:}; fp=\${r%%:*}; pad=\${r#*:}
    env $EXTRA GE_NOPACE=1 GE_QUIET=1 GE_AUDIO=0 GE_W=240 GE_H=240 GE_EXIT_AT=1300 GE_ARGS=-level_43 GE_FOGPX=\$fp \
        GE_SAVE_DIR=/tmp/sw_${NAME}_\$n GE_PADSCRIPT=\$pad GE_DUMP=$OUT/\$n GE_DUMP_FROM=320 GE_DUMP_EVERY=1 \
        timeout 2400 $BIN > $OUT/\$n.out 2>&1 &
  done
  wait"
for s in play menu intro snow_game snow_split walk_game walk_split; do
    printf '%-10s %4d frames\n' "$s" "$(ls "$ROOT/m2scratch/sweep/$NAME/$s" | wc -l)"
done
# Surface 2's snow flicker (2026-10-04): past its limits the sweep fails
python3 "$ROOT/tools/snowcheck.py" "$ROOT/m2scratch/sweep/$NAME"
