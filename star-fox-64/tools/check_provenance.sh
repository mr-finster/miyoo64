#!/bin/sh
# Release gate 1 (from the MK64 handheld port): every source compiled into the
# executable must be tracked in git. Everything generated from the ROM (Torch
# asset sources, the aligned message bank, build/) is git-ignored, so
# "tracked" means decomp or port source, and a ROM-derived file that slipped
# into the code list shows up here as UNTRACKED.
#
#   sh tools/check_provenance.sh handheld|pc
set -u
PLAT=${1:-handheld}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT/port" || exit 1
bad=0; n=0
list=$(make -f Makefile.handheld PLAT=$PLAT -s print-code-sources) || { echo "ERROR: make print-code-sources failed"; exit 1; }
for f in $list; do
    n=$((n + 1))
    if ! git -C "$ROOT" ls-files --error-unmatch "port/$f" >/dev/null 2>&1; then
        echo "UNTRACKED source compiled into the executable: port/$f"; bad=$((bad + 1))
    fi
done
echo "provenance: $n code sources checked, $bad untracked"
# 116 game + port sources (round 1); a tiny list means the listing broke, not that all is well
[ "$n" -ge 100 ] || { echo "ERROR: only $n code sources listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
