#!/bin/sh
# Release gate 1: everything compiled into the executable -- every source and
# every header it includes, from the build's .d files -- is tracked in git.
# ROM-derived files (Torch output, extracted blobs) are all gitignored, so
# "tracked" means decomp or port source. Two untracked kinds are allowed:
#   build/<plat>/gen/*          made by tools/gen_link.py and gen_swapmap.py from
#                               the decomp's own build: symbol values and byte-order
#                               runs, numbers only (gate 3 scans the binary anyway)
#   include/assets/<ver>/*.h    Torch's asset headers: declarations only, checked
#                               here to hold no initialised data
#
#   sh tools/check_provenance.sh handheld|pc
set -u
PLAT=${1:-handheld}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME="$ROOT/game"
BUILD="$GAME/build/$PLAT"
[ -d "$BUILD" ] || { echo "ERROR: $BUILD missing - build PLAT=$PLAT first"; exit 1; }

deps=$(find "$BUILD" -name '*.d' -exec cat {} + | tr ' \\' '\n\n' | grep -v ':$' | grep -v '^$' | grep -v '^/' | sort -u)
n=0; bad=0; gen=0; hdr=0
for f in $deps; do
    n=$((n + 1))
    if git -C "$ROOT" ls-files --error-unmatch "game/$f" >/dev/null 2>&1; then
        continue
    fi
    case "$f" in
        build/$PLAT/gen/*)
            gen=$((gen + 1)) ;;
        include/assets/*.h)
            if grep -q -E '[{=]' "$GAME/$f"; then
                echo "DATA in a Torch header compiled into the executable: game/$f"; bad=$((bad + 1))
            else
                hdr=$((hdr + 1))
            fi ;;
        *)
            echo "UNTRACKED file compiled into the executable: game/$f"; bad=$((bad + 1)) ;;
    esac
done
echo "provenance: $n files checked ($gen generated tables, $hdr declaration-only Torch headers), $bad bad"
# ~400 sources and headers go into the port; a tiny list means the scan broke
[ "$n" -ge 150 ] || { echo "ERROR: only $n files listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
