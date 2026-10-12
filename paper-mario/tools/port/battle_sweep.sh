#!/bin/sh
# The battle sweep (M2): the native game on the desktop proxy starts each of the game's 563 battle formations
# directly (PM64_BATTLE, the attract demo's level-3 Mario with Goombario) and plays it with A pressed every 12 frames
# (Jump, the first target, the action commands and guards at random timings), a few at a time, to find what crashes
# before a player does. Per battle: refs/m2/bsweep/<id>/out.txt and a frame every 100 (PPM, local only); the summary is
# refs/m2/bsweep/summary.txt: ok (ran its frames), END (won or lost, then failed going back to a world it never came
# from: the switch starts from nothing, so that step is not tested here), SKIP (battle_names.h names more formations
# than some areas list), CRASH, TIMEOUT (a hang: a SIGSEGV after 3 minutes, its log ends with where it spun).
#   tools/port/battle_sweep.sh [frames] [jobs] [name regex]      e.g. tools/port/battle_sweep.sh 600 5 '^BTL_KMR'
set -e
cd "$(dirname "$0")/../.."
FRAMES=${1:-600}; JOBS=${2:-5}; FILTER=${3:-.}
python3 tools/port/gen_build.py pc > /dev/null
make -f build/game/pc_game.mk -j12 -k 2>&1 | grep -E "error|FAILED" | head -20 || true
mkdir -p refs/m2/bsweep
python3 - "$FILTER" > refs/m2/bsweep/battles.txt <<'EOF'
import re, sys
for m in re.finditer(r'(BTL_\w+_FORMATION_\w+)\s*=\s*(0x[0-9A-Fa-f]+)', open('refs/papermario/src/battle/battle_names.h').read()):
    if re.search(sys.argv[1], m.group(1)): print(m.group(2), m.group(1))
EOF
KEYS=$(python3 -c "print(','.join(f'{f}:a+,{f+3}:a-' for f in range(60, $FRAMES, 12)))")
echo "$(wc -l < refs/m2/bsweep/battles.txt | tr -d ' ') battles, $FRAMES frames each, $JOBS at a time"
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work pm64hh:decomp bash -c "
ninja -f build/game/pc.ninja -k 0 | grep -v '^\[' ; ulimit -c 0
run() {
    d=refs/m2/bsweep/\$1; rm -rf \$d; mkdir -p \$d
    env PM64_BATTLE=\$1 PM64_FRAMES=$FRAMES PM64_KEYS=$KEYS PM64_SHOTS=/work/\$d PM64_SHOTS_EVERY=100 PM64_DATA=/work/\$d \
        timeout -s SEGV 180 ./build/game/pc/pm64 > \$d/out.txt 2>&1
    st=\$?
    if grep -q '^port: $FRAMES game frames' \$d/out.txt; then r=ok
    elif grep -q '^PM64_BATTLE: no battle' \$d/out.txt; then r=SKIP
    elif [ \$st = 124 ]; then r=TIMEOUT
    elif grep -q 'state_step_end_battle\|post_battle' \$d/bt.txt 2>/dev/null; then r=END
    else r=CRASH; fi
    echo \"\$1 \$2 \$r\"
}
export -f run
xargs -P $JOBS -L 1 bash -c 'run \$0 \$1' < refs/m2/bsweep/battles.txt > refs/m2/bsweep/summary.txt
"
# name the crashes' frames (END is told from CRASH by the backtrace: addr2line on the desktop build)
for d in $(awk '$3 == "CRASH" {print $1}' refs/m2/bsweep/summary.txt); do
    grep -E '^  #[0-9]+ ' refs/m2/bsweep/$d/out.txt | head -12 > refs/m2/bsweep/$d/bt_raw.txt
    sh tools/port/bt.sh refs/m2/bsweep/$d/bt_raw.txt > refs/m2/bsweep/$d/bt.txt 2>/dev/null || true
    if grep -q 'state_step_end_battle\|post_battle\|update_encounters' refs/m2/bsweep/$d/bt.txt; then
        sed -i '' "s/^$d \(.*\) CRASH$/$d \1 END/" refs/m2/bsweep/summary.txt
    fi
done
awk '{n[$3]++} END {for (k in n) printf "%s %d\n", k, n[k]}' refs/m2/bsweep/summary.txt
grep -E " (CRASH|TIMEOUT)$" refs/m2/bsweep/summary.txt | head -40 || true
