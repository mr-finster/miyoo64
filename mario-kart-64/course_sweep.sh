#!/bin/sh
# All-course render sweep (device round 3: "check all courses for TT-like
# issues"). Builds PLAT=pc, then autopilots into every race course at the square
# CONVERT size with MK64_COURSE forcing, dumping frames 900/1050/1200 (in-race).
# Output: /work/sweep/cNN_frame_*.ppm + per-course logs.
set -u
cd /work/base-psp
make -f Makefile.handheld PLAT=pc -j6 2>&1 | grep -E "error:|Error:|fatal error:" | head -5
test -x build/pc/mk64 || { echo "BUILD FAILED"; exit 1; }
echo "build ok"
OUT=/work/sweep
mkdir -p "$OUT"
rm -f "$OUT"/*.ppm
for C in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 18; do
    D="$OUT/run_$C"
    mkdir -p "$D"; rm -f "$D"/*.ppm
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MK64_AUTO=1 MK64_QUIET=1 \
      MK64_COURSE=$C MK64_W=240 MK64_H=240 MK64_DUMP="$D" MK64_DUMP_EVERY=150 \
      timeout 100 /work/base-psp/build/pc/mk64 > "$D/run.log" 2>&1
    for F in 000900 001050 001200; do
        test -f "$D/frame_$F.ppm" && cp "$D/frame_$F.ppm" "$OUT/c$(printf %02d $C)_$F.ppm"
    done
    echo "course $C done: $(ls "$D"/*.ppm 2>/dev/null | wc -l) frames"
done
echo "SWEEP COMPLETE"
ls "$OUT"/c*.ppm | wc -l
