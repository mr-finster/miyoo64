#!/bin/bash
# Visual tour: every scene in a list (default tour.txt; LIST=<file>) on the desktop build (square output, fixed
# seed, clock held), frames FROM, FROM+EVERY, ... up to EXIT (defaults 150/150/451) into
# refs/tour_<tag>/<name>/. A 5th field is a cutscene layer (OOT_CUTSCENE, e.g. 0xFFF0; empty = none), a 6th
# an OOT_INPUT script with ';' for ',' (e.g. 100:;60:u;30:;2:a to walk a boss room's intro in), a 7th
# OOT_WARPROOM warps (';' for ',', '/' between warps: into a boss's trigger box), an 8th this stop's EXIT.
# Then tour_sheets.py makes the native-240 sheets.
#   ./dev.sh 'cd /work/port && tools/handheld/tour.sh <tag> [jobs]'
set -u
TAG=$1; J=${2:-6}; OUT=/work/refs/tour_$TAG; rm -rf $OUT; mkdir -p $OUT
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_W=240 OOT_H=240 OOT_ASSETS=build/hh-gen OOT_SEED=1234 OOT_QUIET=1
LIST=${LIST:-tools/handheld/tour.txt}; FROM=${FROM:-150}; EVERY=${EVERY:-150}; EXIT=${EXIT:-451}
one() { # name entrance adult daytime [cutscene] [input] [warps] [exit]
    local d="$OUT/$1" w="/tmp/tour_$2_$3_$4_${5:-x}"; mkdir -p "$d" "$w"
    env OOT_DATA="$w" OOT_ENTRANCE=$2 OOT_ADULT=$3 OOT_DAYTIME=$4 OOT_TIMELOCK=$4 ${5:+OOT_CUTSCENE=$5} ${6:+OOT_INPUT=${6//;/,}} ${7:+OOT_WARPROOM=${7//;/,}} \
        OOT_EXIT_AT=${8:-$EXIT} OOT_DUMP_FROM=$FROM OOT_DUMP_EVERY=$EVERY OOT_DUMP="$d" \
        timeout -s KILL 900 ./build/hh-pc/oot > "$d.log" 2>&1
}
n=0
grep -v '^#' "$LIST" | { while IFS=, read -r name e a t cs in wr ex; do
    one "$name" "$e" "$a" "$t" "$cs" "$in" "$wr" "$ex" &
    n=$((n + 1)); [ $((n % J)) -eq 0 ] && wait
done; wait; }
ls "$OUT" | grep -vc '\.log$'
