#!/bin/sh
# Make the M1 scenes AND an emulator state at each scene's first poll, from one capture run, so the real game
# can be rendered at the same instant (m64ref.sh ... -- --savestate <state>) for the user's fix sheets.
# Local only: scenes and states are ROM-derived and must never be committed, uploaded or put in a zip.
#   sh tools/cap/make_m1_states.sh /path/to/baserom.z64 [scene outdir] [state outdir] [script.txt ...]
# The scripts default to m1_newgame.txt and m1_demo.txt. Poll counts differ between emulator builds (the canonical
# m1 scenes came from the cloud's mupen64plus): find a scene's polls on this machine with a sparse scan first
# (BUILDLOG round 15e).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
ROM=${1:?usage: make_m1_states.sh /path/to/baserom.z64 [scene outdir] [state outdir]}
OUT=${2:-$ROOT/refs/cap/m1_same}
STATES=${3:-$ROOT/refs/m64ref/states}
WORK=$ROOT/refs/cap/work_states
sh "$ROOT/tools/check_rom.sh" "$ROM" | grep -q "Paper Mario us" || { echo "need the US ROM"; exit 1; }
make -C "$HERE" >/dev/null
mkdir -p "$OUT" "$STATES" "$WORK"
cc -O2 -o "$WORK.pms_convert" "$ROOT/tools/pms/pms_convert.c"
shift $(( $# < 3 ? $# : 3 ))
[ $# -gt 0 ] || set -- "$HERE/m1_newgame.txt" "$HERE/m1_demo.txt"
for path in "$@"; do
    script=$(basename "$path" .txt)
    rm -f "$WORK"/*.pmc
    # the same script, plus a state saved at each capture's poll
    awk -v d="$STATES" '{ print } $2 == "cap" { print $1 " save " d "/" $4 ".st" }' "$path" > "$WORK/$script.txt"
    PMCAP_SCRIPT="$WORK/$script.txt" PMCAP_OUT="$WORK" /usr/games/mupen64plus --nospeedlimit --noosd \
        --configdir "$WORK" --gfx "$HERE/pmcap-video.so" --audio dummy --rsp mupen64plus-rsp-hle \
        --input "$HERE/pmcap-input.so" "$ROM" >"$WORK/$script.log" 2>&1
    for name in $(awk '$2 == "cap" { print $4 }' "$path"); do
        "$WORK.pms_convert" "$OUT/$name.pms" "$WORK/${name}"_*.pmc 2>&1 | tail -1 | sed "s/^/$name: /"
    done
    grep "pmcap-input: save" "$WORK/$script.log" || true
done
rm -rf "$WORK" "$WORK.pms_convert"
ls -la "$OUT" "$STATES"
