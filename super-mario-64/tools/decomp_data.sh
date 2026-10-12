#!/bin/sh
# Restore the decomp's ROM-derived game data into port/ from the pinned upstream,
# sm64-port @ 2b17d08 (the user approved fetching it, 2026-09-24). These paths are
# never tracked (.gitignore) and never shipped: they compile only into the NOLOAD
# asset region, and the release gates scan every shipped byte against the ROM.
#   sh tools/decomp_data.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PIN=2b17d081c9798b31b91dc71f37994b0da28cffc9
REF=$ROOT/refs/sm64-port
# keep in step with the "port/" block of .gitignore
PATHS="actors levels assets bin data text sound assets.json src/goddard/dynlists
include/trig_tables.inc.c include/text_strings.h.in include/text_menu_strings.h.in
asm rsp lib/PR tools/ido5.3_compiler enhancements doxygen"
[ -d "$REF/.git" ] || git clone -q https://github.com/sm64-port/sm64-port.git "$REF"
git -C "$REF" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$REF" fetch -q origin
# exactly the tracked content at the pinned commit (git archive), nothing stray
for p in $PATHS; do rm -rf "$ROOT/port/$p"; done
git -C "$REF" archive "$PIN" -- $PATHS | tar -x -C "$ROOT/port"
n=$(git -C "$REF" ls-tree -r --name-only "$PIN" -- $PATHS | wc -l | tr -d ' ')
echo "decomp data: $n files restored into port/ from sm64-port ${PIN%${PIN#???????}}"
