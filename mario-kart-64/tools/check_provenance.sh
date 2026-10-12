#!/bin/sh
# Release gate 1: every source file whose bytes stay in the executable (i.e.
# every compiled file that is NOT an asset object of the split) must be
# tracked in git. The ROM-derived files (extracted textures, torch-emitted
# C, .bin blobs) are all gitignored, so "tracked" == "decomp/port source",
# and an asset that slipped out of ASSET_OBJS shows up here as UNTRACKED.
#
#   sh tools/check_provenance.sh miyoo|pc         (from the repo root or /work)
set -u
PLAT=${1:-miyoo}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT/base-psp" || exit 1
bad=0; n=0
list=$(make -f Makefile.handheld PLAT=$PLAT -s print-code-sources) || { echo "ERROR: make print-code-sources failed"; exit 1; }
for f in $list; do
    n=$((n + 1))
    if ! git -C "$ROOT" ls-files --error-unmatch "base-psp/$f" >/dev/null 2>&1; then
        echo "UNTRACKED source compiled into the executable: base-psp/$f"; bad=$((bad + 1))
    fi
done
# the generated segment tables (port_seg_tables.c): pointers only, built from tracked headers/yamls
echo "provenance: $n code sources checked, $bad untracked"
# the port has ~90 code sources; an empty or tiny list means the listing broke, not that all is well
[ "$n" -ge 60 ] || { echo "ERROR: only $n code sources listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
