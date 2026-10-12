#!/bin/sh
# The save-state test (the kit's menu round), on the desktop proxy: run A enters a map, walks, saves slot 1 at game
# frame 300 (PM64_STATE_SAVE) and plays on to 420; run B, a fresh process, boots to the logos and loads slot 1 at
# frame 5 (PM64_STATE_LOAD). The frames B draws after its load must be A's frames after its save, byte for byte
# (tools/port/state_cmp.py). Frames and states stay in refs/ (ROM-derived).
#   tools/port/state_test.sh [map,entry,progress] [out dir]      (in ./dev.sh; default kmr_20,0,-128)
# ARM=1: the device build under qemu-arm (its region from assets.bin, built from the ROM on the first run, as on the
# card; PM64_ROM or "Paper Mario (USA).z64" at the repo root).
# Optional (2026-10-09): BIN_HH=<device binary> (with ARM=1; recipes.bin beside it), KEYS=<pad script> for run A,
# SAVE=<frame> (300), START=<env> for run A in place of PM64_MAP (e.g. START=PM64_BATTLE=0x0100), DIRTY=<n>
# (PM64_STATE_DIRTY: n more region pages in the state, checked by the region sums the two runs log).
set -e
MAP=${1:-kmr_20,0,-128}; OUT=${2:-refs/x28/st}; SAVE=${SAVE:-300}
rm -rf $OUT/a $OUT/b; rm -f $OUT/data/state_*; mkdir -p $OUT/a $OUT/b $OUT/data
BIN=./build/game/pc/pm64
if [ -n "$ARM" ]; then
    BIN="qemu-arm -L refs/hhroot ${BIN_HH:-./build/game/miyoo/pm64}"
    export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PM64_ROM="${PM64_ROM:-$PWD/Paper Mario (USA).z64}"
fi
KEYS=${KEYS:-"100:r+,250:r-,260:a+,263:a-,270:u+,290:u-"}
START=${START:-PM64_MAP=$MAP}
D=${DIRTY:+PM64_STATE_DIRTY=$DIRTY}
env $START PM64_PARTNER=1 PM64_KEYS=$KEYS PM64_STATE_SAVE=$SAVE,1 PM64_FRAMES=$((SAVE + 120)) PM64_SHOTS=$OUT/a \
    PM64_DATA=$OUT/data $D $BIN > $OUT/a.txt 2>&1
env PM64_STATE_LOAD=5,1 PM64_FRAMES=125 PM64_SHOTS=$OUT/b PM64_DATA=$OUT/data $D $BIN > $OUT/b.txt 2>&1
grep -E "^state|savestate" $OUT/a.txt $OUT/b.txt
ls -la $OUT/data | grep state_
if [ -n "$DIRTY" ]; then
    sa=$(sed -n 's/^state: region sum \([0-9a-f]*\) at the save.*/\1/p' $OUT/a.txt)
    sb=$(sed -n 's/^state: region sum \([0-9a-f]*\) after the load.*/\1/p' $OUT/b.txt)
    [ -n "$sa" ] && [ "$sa" = "$sb" ] && echo "region sums equal ($sa)" || { echo "region sums DIFFER (save ${sa:-none}, load ${sb:-none})"; exit 1; }
fi
python3 tools/port/state_cmp.py $OUT/a $OUT/b 100
