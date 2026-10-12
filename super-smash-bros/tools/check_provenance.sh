#!/bin/sh
# Release gate 1: every source compiled into the executable is tracked in git,
# and none of it comes from the decomp's relocData (the ROM's game data as C)
# or any extracted/generated asset. The only generated inputs are the credits
# tables, built from the decomp's tracked text files.
#   sh tools/check_provenance.sh [miyoo|desktop]
set -u
PLAT=${1:-miyoo}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT/port" || exit 1
bad=0; n=0
list=$(make -s PLAT=$PLAT print-sources) || { echo "ERROR: make print-sources failed"; exit 1; }
for f in $list; do
    n=$((n + 1))
    case "$f" in
        *relocData*|*assets/*|*.inc.c) echo "FORBIDDEN source compiled in: port/$f"; bad=$((bad + 1)); continue ;;
    esac
    if ! git -C "$ROOT" ls-files --error-unmatch "port/$f" >/dev/null 2>&1; then
        echo "UNTRACKED source compiled into the executable: port/$f"; bad=$((bad + 1))
    fi
done
# generated headers the build pulls in: only the credits tables from tracked text
gen=$(find "build/$PLAT/gen" -type f 2>/dev/null | grep -v '/credits/' || true)
[ -z "$gen" ] || { echo "UNEXPECTED generated input: $gen"; bad=$((bad + 1)); }
echo "provenance: $n code sources checked, $bad problems"
[ "$n" -ge 400 ] || { echo "ERROR: only $n sources listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
