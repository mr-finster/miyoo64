#!/bin/sh
# Bit-exact output sweep (after ocarina-of-time tools/handheld/sweep.sh). Runs the
# desktop build on the autopilot (title -> menus -> Mute City GP race), dumps
# the picture every 20 retraces from VI 60 to 2300 (113 frames: boot, menus,
# countdown, race, HUD), then compares against a saved sweep.
#
#   sh tools/sweep.sh save <name> [ENV=VAL ...]   # record a sweep (desktop build)
#   sh tools/sweep.sh save-arm <name> [ENV=VAL ...] # the same on the handheld build under qemu-arm
#                                                   # (NEON paths; QEMU=, ARMROOT= as tools/qprof.sh)
#   sh tools/sweep.sh cmp  <a> <b>                # frames that differ, px counts
#
# Extra ENV=VAL pairs are passed to the game (e.g. FZX_BILINEAR=0). PCBIN= / ARMBIN= run another
# build's binary (e.g. one built with BUILD_DIR=build/pc-x while the default build is in use).
# Sweeps live in $OUT (default refs/sweeps, untracked). Run it in ./dev.sh.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/refs/sweeps}
CMD=$1; shift
case $CMD in
save|save-arm)
    NAME=$1; shift
    D=$OUT/$NAME; rm -rf "$D"; mkdir -p "$D"
    [ -f "$ROOT/game/fzerox.sram.ref" ] && cp "$ROOT/game/fzerox.sram.ref" "$D/fzerox.sram"
    AT=$(seq -s, 60 20 2300)
    if [ "$CMD" = save-arm ]; then
        RUN="${QEMU:-qemu-arm} -L ${ARMROOT:-/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot} ${ARMBIN:-$ROOT/game/build/miyoo/fzerox}"
    else
        RUN=${PCBIN:-$ROOT/game/build/pc/fzerox}
    fi
    env "$@" FZX_ROM=$ROOT/game/baserom.us.rev0.z64 FZX_DATA=$D FZX_HEADLESS=1 FZX_AUTO=1 \
        FZX_DUMP_AT=$AT FZX_EXIT_AT=2310 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        $RUN > "$D/run.log" 2>&1
    echo "$NAME: $(ls "$D"/shot_*.ppm 2>/dev/null | wc -l) frames"
    ;;
cmp)
    python3 - "$OUT/$1" "$OUT/$2" <<'PY'
import sys, os, glob
a, b = sys.argv[1], sys.argv[2]
def load(p):
    d = open(p, 'rb').read()
    # binary PPM: P6\n<w> <h>\n255\n<rgb>
    parts = d.split(b'\n', 3)
    return d[len(parts[0]) + len(parts[1]) + len(parts[2]) + 3:]
names = sorted(os.path.basename(p) for p in glob.glob(a + '/shot_*.ppm'))
differ = 0
for n in names:
    pb = os.path.join(b, n)
    if not os.path.exists(pb): print(n, 'missing in', b); differ += 1; continue
    x, y = load(os.path.join(a, n)), load(pb)
    if x == y: continue
    px = sum(1 for i in range(0, min(len(x), len(y)), 3) if x[i:i+3] != y[i:i+3])
    print(n, px, 'px differ'); differ += 1
print(f'{len(names)} frames, {differ} differ')
PY
    ;;
esac
