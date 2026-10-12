#!/usr/bin/env bash
# Compare two sweeps level by level: changed frames and pixels.
# usage: tools/sweepdiff.sh <tagA> <tagB> [outdir]  (outdir: diff strips per level)
# Exits 1 when a sweep has no levels or a level compared no frames or lost some
# (tools/framediff.py): an empty match must never read as "no differences".
set -uo pipefail
cd "$(dirname "$0")/.."
a=$1; b=$2; out=${3:-}
fail=0
shopt -s nullglob
dirs=(refs/$a/L*)
[ ${#dirs[@]} -gt 0 ] || { echo "ERROR: no levels in refs/$a"; exit 1; }
for d in "${dirs[@]}"; do
  l=$(basename "$d")
  if [ -n "$out" ]; then r=$(python3 tools/framediff.py "$d" "refs/$b/$l" "$out/$l" 2>/dev/null); rc=$?
  else r=$(python3 tools/framediff.py "$d" "refs/$b/$l" 2>/dev/null); rc=$?; fi
  n=$(echo "$r" | grep -c 'changed px')
  echo "$l: $n changed; $(echo "$r" | grep -v 'changed px' | tail -2 | tr '\n' ' ')"
  [ $rc -eq 0 ] || fail=1
done
[ $fail -eq 0 ] || echo "ERROR: some levels failed to compare (above)"
exit $fail
