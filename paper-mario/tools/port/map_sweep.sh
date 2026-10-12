#!/bin/sh
# The map sweep (M2): the native game on the desktop proxy enters every map of the game directly (PM64_MAP, a new
# game's data with the story at that area's chapter, Goombario following) and runs a few seconds there, to find what
# crashes before a player does. Per map: refs/m2/sweep/<map>/out.txt (the port's log) and a frame every 60 (PPM, local
# only); the summary is refs/m2/sweep/summary.txt (ok / CRASH / TIMEOUT per map: a hang gets a SIGSEGV after 2 minutes,
# so its log ends with the backtrace of where it spun).
#   tools/port/map_sweep.sh [frames] [jobs] [map regex]      e.g. tools/port/map_sweep.sh 240 5 '^(kmr|mac)_'
set -e
cd "$(dirname "$0")/../.."
FRAMES=${1:-240}; JOBS=${2:-5}; FILTER=${3:-.}
python3 tools/port/gen_build.py pc > /dev/null
make -f build/game/pc_game.mk -j12 -k 2>&1 | grep -E "error|FAILED" | head -20 || true
mkdir -p refs/m2/sweep
# the maps in the game's order (src/world/world.c), each with the story progress of its chapter (include/enums.h)
python3 - "$FILTER" > refs/m2/sweep/maps.txt <<'EOF'
import re, sys
src = open('refs/papermario/src/world/world.c').read()
progress = {'kmr': -128, 'mac': -107, 'tik': -107, 'kgr': 8, 'iwa': -72, 'sbk': -72, 'dro': -72, 'isk': -72,
            'nok': -102, 'trd': -102, 'hos': -107, 'mim': -51, 'obk': -51, 'arn': -51, 'dgb': -51, 'omo': -12,
            'jan': 8, 'kzn': 8, 'flo': 42, 'sam': 61, 'pra': 61, 'kpa': 90, 'osr': -107, 'kkj': -107, 'end': 96,
            'mgm': -107, 'gv': -107, 'tst': -128}
area = None; seen = set()  # kkj_26 is in the table twice (world.c: the second is unreachable)
for line in src.splitlines():
    m = re.match(r'MapConfig (\w+)_maps\[\] = \{', line)
    if m: area = m.group(1); continue
    m = re.search(r'MAP(?:_WITH_INIT)?\((\w+)\)', line)
    if m and area and m.group(1) != 'map' and m.group(1) not in seen and re.search(sys.argv[1], m.group(1)):
        # no partner in the ending: its parade fills the sprite heap to the last few KB without one
        seen.add(m.group(1)); print(m.group(1), progress.get(area, -107), 0 if area == 'end' else 1)
EOF
echo "$(wc -l < refs/m2/sweep/maps.txt | tr -d ' ') maps, $FRAMES frames each, $JOBS at a time"
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work pm64hh:decomp bash -c "
ninja -f build/game/pc.ninja -k 0 | grep -v '^\[' ; ulimit -c 0
run() {
    d=refs/m2/sweep/\$1; rm -rf \$d; mkdir -p \$d
    env PM64_MAP=\$1,0,\$2 PM64_PARTNER=\$3 PM64_FRAMES=$FRAMES PM64_SHOTS=/work/\$d PM64_SHOTS_EVERY=60 PM64_DATA=/work/\$d \
        timeout -s SEGV 120 ./build/game/pc/pm64 > \$d/out.txt 2>&1
    st=\$?
    if grep -q '^port: $FRAMES game frames' \$d/out.txt; then r=ok; elif [ \$st = 124 ]; then r=TIMEOUT; else r=CRASH; fi
    echo \"\$1 \$r\"
}
export -f run
xargs -P $JOBS -L 1 bash -c 'run \$0 \$1 \$2' < refs/m2/sweep/maps.txt > refs/m2/sweep/summary.txt
"
sort -k2 refs/m2/sweep/summary.txt | awk '{n[$2]++} END {for (k in n) printf "%s %d\n", k, n[k]}'
grep -v " ok$" refs/m2/sweep/summary.txt | head -40 || true
