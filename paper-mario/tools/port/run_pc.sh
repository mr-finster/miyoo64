#!/bin/sh
# The native game on the desktop proxy (M2): build the game side on the Mac (clang), the host side and the link in
# Docker, then run it there. Frames go to refs/m2/shots (PPM, local only: they are the game's pictures).
#   tools/port/run_pc.sh [frames] [extra env assignments...]    e.g. tools/port/run_pc.sh 300 PM64_KEYS=200:start+
set -e
cd "$(dirname "$0")/../.."
FRAMES=${1:-60}; [ $# -gt 0 ] && shift
python3 tools/port/gen_build.py pc > /dev/null
make -f build/game/pc_game.mk -j12 -k 2>&1 | grep -E "error|FAILED" | head -20 || true
mkdir -p refs/m2/shots
rm -f refs/m2/shots/*.ppm
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work pm64hh:decomp bash -c "ninja -f build/game/pc.ninja -k 0 | grep -v '^\[' ; ulimit -c 0; env PM64_FRAMES=$FRAMES PM64_SHOTS=/work/refs/m2/shots PM64_SAVE=/work/refs/m2/pm64.sav PM64_DATA=/work/refs/m2/data $* timeout 600 ./build/game/pc/pm64 2>&1 | tail -80; echo \"exit \${PIPESTATUS[0]}\""
echo "frames: $(ls refs/m2/shots | wc -l)"
