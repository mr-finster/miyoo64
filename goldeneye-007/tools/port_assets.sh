#!/usr/bin/env bash
# Copy the approved decomp metadata (the 1,063 assets/ headers and table rows the game
# code #includes; see tools/decomp_sparse_paths.txt) from refs/007 into port/assets.
# Untracked (.gitignore): our repo never holds ROM-derived tables; the M6 gates keep their
# bytes out of every zip.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/refs/007"
grep '^assets/' "$ROOT/tools/decomp_sparse_paths.txt" | while read -r p; do
  [ -f "$p" ] && { mkdir -p "$ROOT/port/$(dirname "$p")"; cp -p "$p" "$ROOT/port/$p"; }
done
# IDO enum semantics (port/hh/include/hh_enum.h): the asset headers' enums become int.
find "$ROOT/port/assets" -name '*.h' -o -name '*.c' | sort | xargs python3 "$ROOT/port/hh/tools/enum_int.py" | tail -1
echo "port/assets: $(find "$ROOT/port/assets" -type f | wc -l | tr -d ' ') files"
