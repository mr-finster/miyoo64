#!/bin/sh
# Reference frames from the player's ROM in mupen64plus (Rice video, HLE RSP),
# driven by the same key script as the port's FZX_KEYS, for side-by-side checks.
# Local only: nothing leaves the machine, and nothing here is committed.
#
#   sh tools/ref_shots.sh <rom.z64> <outdir> "<vi,vi,...>" ["<FZX_KEYS script>"]
#
# Needs: mupen64plus-ui-console, mupen64plus-video-rice, mupen64plus-rsp-hle,
# xvfb. The script plugin (tools/m64_script_input.c) is built on first use; it
# counts controller polls, which run within a few frames of the port's VIs.
set -eu
ROM=$1; OUT=$2; SHOTS=$3; KEYS=${4:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${TMPDIR:-/tmp}/fzx_ref
mkdir -p "$WORK/home" "$OUT"
[ -f "$WORK/mupen64plus-input-script.so" ] ||
    gcc -shared -fPIC -O2 -o "$WORK/mupen64plus-input-script.so" "$ROOT/tools/m64_script_input.c"
rm -rf "$WORK/home/.local/share/mupen64plus/screenshot"
HOME=$WORK/home SDL_AUDIODRIVER=dummy FZX_KEYS=$KEYS xvfb-run -a -s "-screen 0 640x480x24" \
    /usr/games/mupen64plus --noosd --windowed --resolution 320x240 --gfx mupen64plus-video-rice.so \
    --rsp mupen64plus-rsp-hle.so --input "$WORK/mupen64plus-input-script.so" --testshots "$SHOTS" "$ROM" \
    > "$WORK/run.log" 2>&1
i=0
for f in $(ls "$WORK/home/.local/share/mupen64plus/screenshot/"*.png | sort); do
    v=$(echo "$SHOTS" | cut -d, -f$((i + 1)))
    cp "$f" "$OUT/ref_$(printf %05d "$v").png"
    i=$((i + 1))
done
echo "$i reference frames in $OUT"
