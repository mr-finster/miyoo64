#!/bin/sh
# Make the M1 bench scenes (*.pms) from the player's own ROM. Local only: the output is
# ROM-derived and must never be committed, uploaded or put in a zip.
#   sh tools/cap/make_m1_scenes.sh /path/to/baserom.z64 [outdir]
# Needs mupen64plus (ui-console, rsp-hle) and its dev headers: apt install
# mupen64plus-ui-console mupen64plus-rsp-hle libmupen64plus-dev
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
ROM=${1:?usage: make_m1_scenes.sh /path/to/baserom.z64 [outdir]}
OUT=${2:-$ROOT/refs/cap/m1}
WORK=$ROOT/refs/cap/work
sh "$ROOT/tools/check_rom.sh" "$ROM" | grep -q "Paper Mario us" || { echo "need the US ROM"; exit 1; }
make -C "$HERE" >/dev/null
mkdir -p "$OUT" "$WORK"
cc -O2 -o "$WORK.pms_convert" "$ROOT/tools/pms/pms_convert.c"
for script in m1_newgame m1_demo; do
    rm -f "$WORK"/*.pmc
    PMCAP_SCRIPT="$HERE/$script.txt" PMCAP_OUT="$WORK" /usr/games/mupen64plus --nospeedlimit --noosd \
        --configdir "$WORK" --gfx "$HERE/pmcap-video.so" --audio dummy --rsp mupen64plus-rsp-hle \
        --input "$HERE/pmcap-input.so" "$ROM" >"$WORK/$script.log" 2>&1
    for name in $(awk '$2 == "cap" { print $4 }' "$HERE/$script.txt"); do
        "$WORK.pms_convert" "$OUT/$name.pms" "$WORK/${name}"_*.pmc 2>&1 | tail -1 | sed "s/^/$name: /"
    done
done
rm -rf "$WORK" "$WORK.pms_convert"
ls -la "$OUT"
