#!/bin/sh
# Bit-exact regression sweep for rasterizer changes (device round 12/13).
# Runs INSIDE the container:
#   ./dev.sh 'sh /work/bitexact_sweep.sh <label> [magbilin]'
#   ./dev.sh 'sh /work/bitexact_sweep.sh cmp <a> <b>'
#
# Builds PLAT=pc with STRICT 32-bit float (the Makefile now sets -mfpmath=sse
# and -ffp-contract=off for the proxy, matching the device's VFP: x87's 80-bit
# intermediates produce phantom +/-1 rounding under harmless code motion that
# never happens on ARM, and the device build has contraction off too), then
# autopilots a varied set of courses under both texture filters and dumps two
# in-race frames each. Desktop has no frameskip (gfx_sdl.c), so rendered frame
# N is always game tick N -- runs are deterministic and may be run in parallel.
#
# [magbilin] (default 0): 0 = always-bilinear, the path an OLD build without the
# magnified-only gate takes (that build ignores the unknown env var), so a
# baseline label is comparable; 1 = the SHIPPED default. Compare both: base vs
# candidate at 0 proves every bit-exact change; candidate(0) vs candidate(1)
# shows what the gate itself changes.
#
# Exit status: non-zero if any run crashed/timed out or produced no frames --
# a sweep that cannot fail is not a sweep (round 13).
set -u
ROOT=/work/bx
COURSES="18 8 13 5 3 6 10 4"   # DK, Luigi, Rainbow, Frappe, Banshee, Koopa Beach, Turnpike, Yoshi Valley
PAR=6

if [ "${1:-}" = "cmp" ]; then
    A="$ROOT/$2"; B="$ROOT/$3"; bad=0; n=0
    for d in "$A"/*/; do
        k=$(basename "$d")
        for f in "$d"/frame_*.ppm; do
            [ -f "$f" ] || continue
            g="$B/$k/$(basename "$f")"; n=$((n+1))
            if [ ! -f "$g" ]; then echo "MISSING $k/$(basename "$f") in $3"; bad=$((bad+1)); continue; fi
            if ! cmp -s "$f" "$g"; then
                px=$(python3 - "$f" "$g" <<'PY'
import sys
def px(p):
    b=open(p,'rb').read(); i=b.index(b'255\n')+4; return b[i:]
a=px(sys.argv[1]); c=px(sys.argv[2])
print(sum(1 for k in range(0,min(len(a),len(c)),3) if a[k:k+3]!=c[k:k+3]))
PY
)
                echo "DIFF $k/$(basename "$f"): $px px"; bad=$((bad+1))
            fi
        done
    done
    echo "=== $n frames compared, $bad differ ==="
    if [ "$n" -eq 0 ]; then echo "==> NO FRAMES: nothing was compared (both labels empty?)"; exit 2; fi
    [ "$bad" -eq 0 ] && echo "==> BIT-EXACT: $2 == $3"
    exit "$bad"
fi

LABEL="${1:?label}"; MAGBILIN="${2:-0}"
OUT="$ROOT/$LABEL"
cd /work/base-psp
make -f Makefile.handheld PLAT=pc clean >/dev/null 2>&1
if ! make -f Makefile.handheld PLAT=pc -j6 >/tmp/bx_build.log 2>&1; then
    grep -E "error:|Error:|undefined reference" /tmp/bx_build.log | head -10
    echo "BUILD FAILED"; exit 1
fi
test -x build/pc/mk64 || { echo "BUILD FAILED (no binary)"; exit 1; }
echo "build ok ($LABEL, magbilin=$MAGBILIN)"
# M6: the executable carries no game data. Build the asset cache once from the
# ROM (build/pc/data/assets.bin) so the 16 runs below all load it instead of
# each extracting; a failed extraction (CRC) fails the sweep here.
export MK64_ROM=/work/base-psp/baserom.us.z64
if ! MK64_ASSETS_ONLY=1 MK64_QUIET=1 /work/base-psp/build/pc/mk64 > /tmp/bx_assets.log 2>&1; then
    tail -5 /tmp/bx_assets.log; echo "ASSET EXTRACTION FAILED"; exit 1
fi
grep -E "assets: (generated|.*loaded)" /tmp/bx_assets.log | tail -1
rm -rf "$OUT"; mkdir -p "$OUT"

run() { # course filter(sharp|smooth)  -> writes $D/status
    D="$OUT/c$(printf %02d "$1")_$2"; mkdir -p "$D"
    if [ "$2" = sharp ]; then BL=0; else BL=1; fi
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MK64_AUTO=1 MK64_QUIET=1 \
      MK64_COURSE=$1 MK64_W=240 MK64_H=240 MK64_BILINEAR=$BL MK64_MAGBILIN=$MAGBILIN \
      MK64_DUMP="$D" MK64_DUMP_EVERY=150 MK64_DUMP_FROM=900 MK64_EXIT_AT=1051 \
      timeout 240 /work/base-psp/build/pc/mk64 > "$D/run.log" 2>&1
    rc=$?   # captured BEFORE any command substitution can clobber it
    nf=$(ls "$D"/*.ppm 2>/dev/null | wc -l)
    echo "$rc $nf" > "$D/status"
    echo "  c$1/$2: $nf frames, exit $rc"
}
i=0
for C in $COURSES; do for F in sharp smooth; do
    run "$C" "$F" &
    i=$((i+1)); [ $((i % PAR)) -eq 0 ] && wait
done; done
wait
fail=0
for st in "$OUT"/*/status; do
    read -r rc nf < "$st"
    # 0 = clean exit; MK64_EXIT_AT ends the process via exit(0). Anything else,
    # or a run with no frames, fails the sweep.
    if [ "$rc" -ne 0 ] || [ "$nf" -lt 2 ]; then echo "FAILED RUN: $(dirname "$st") (exit $rc, $nf frames)"; fail=$((fail+1)); fi
done
echo "SWEEP $LABEL COMPLETE: $(find "$OUT" -name 'frame_*.ppm' | wc -l) frames, $fail failed runs"
[ "$fail" -eq 0 ] || exit 1
