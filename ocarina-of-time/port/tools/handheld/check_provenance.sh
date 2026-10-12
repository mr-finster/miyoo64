#!/bin/sh
# Release gate 0 (mario-kart-64's tools/check_provenance.sh, for OoT): every C source compiled
# into the executable is tracked in git, so nothing made from the ROM can have slipped into
# the code as a source file. ROM-derived files (extracted/, build/, baseroms/) are all
# gitignored, so "tracked" == decomp or port source. Two kinds of input are not tracked and
# are allowed by name:
#   - the generated tables (build/hh-gen: the asset table, the object/scene and audio
#     tables): numbers -- addresses, sizes -- written by tracked tools, whose bytes the
#     leak scan (tools/handheld/leak_scan.py) checks against the ROM;
#   - the build stamp (random per link).
# Asset DATA #included by game sources goes to the NOLOAD .hhassets region (gate 1), and the
# leak scan catches any the split missed.
#   sh port/tools/handheld/check_provenance.sh [handheld|pc]      (in the build container)
set -u
PLAT=${1:-handheld}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT/port" || exit 1
list=$(make -f Makefile.handheld PLAT=$PLAT -s print-C_FILES | sed 's/^C_FILES=//') || { echo "ERROR: make print-C_FILES failed"; exit 1; }
tracked=$(git -c safe.directory='*' -C "$ROOT" ls-files port) || { echo "ERROR: git ls-files failed"; exit 1; }
bad=0; n=0
for f in $list; do
    n=$((n + 1))
    if ! printf '%s\n' "$tracked" | grep -qxF "port/$f"; then
        echo "UNTRACKED source compiled into the executable: port/$f"; bad=$((bad + 1))
    fi
done
gen=$(make -f Makefile.handheld PLAT=$PLAT -s print-GEN_C | sed 's/^GEN_C=//')
for f in $gen; do
    case "$f" in
        build/hh-gen/oot_psp_asset_segments.c|build/hh-gen/oot_psp_asset_tables.c|build/hh-gen/oot_psp_audio_tables.c) ;;
        *) echo "UNEXPECTED generated source: port/$f"; bad=$((bad + 1)) ;;
    esac
done
echo "provenance: $n code sources checked, $bad untracked; generated tables: $(echo $gen | wc -w) (allowed, leak-scanned)"
# ~900 sources; a tiny list means the listing broke, not that all is well
[ "$n" -ge 500 ] || { echo "ERROR: only $n code sources listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ]
