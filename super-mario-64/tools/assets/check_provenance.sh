#!/bin/sh
# Release gate 1 (MK64 port's tools/check_provenance.sh): every source
# file whose bytes stay in the executable -- every compiled file that is NOT an
# asset object of the split -- must be tracked in git. The decomp's ROM-derived
# files are all untracked (restored by tools/decomp_data.sh), so "tracked" ==
# "decomp/port code", and an asset that slipped out of ASSET_OBJS shows up here.
#
#   sh tools/assets/check_provenance.sh miyoo|pc     (repo root or /work)
set -u
PLAT=${1:-miyoo}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT/port" || exit 1
bad=0; n=0
list=$(make PLAT=$PLAT -s print-code-sources | grep -E "^[A-Za-z0-9_./-]+\.(c|s|cpp)$") || { echo "ERROR: make print-code-sources failed"; exit 1; }
for f in $list; do
    n=$((n + 1))
    if ! git -C "$ROOT" ls-files --error-unmatch "port/$f" >/dev/null 2>&1; then
        echo "UNTRACKED source compiled into the executable: port/$f"; bad=$((bad + 1))
    fi
done
echo "provenance: $n code sources checked, $bad untracked"
# ~130 code sources: an empty or tiny list means the listing broke, not that all is well
[ "$n" -ge 90 ] || { echo "ERROR: only $n code sources listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
