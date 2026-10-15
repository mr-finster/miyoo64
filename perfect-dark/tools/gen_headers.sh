#!/usr/bin/env bash
# Generate the asset ID headers (src/generated/<romid>/) from the decomp's asset metadata
# (src/assets/<romid>/, gitignored, ROM-derived, local only). Names/IDs only.
set -euo pipefail
cd "$(dirname "$0")/.."
R=${ROMID:-ntsc-final}; A=src/assets/$R; T=tools/assetmgr
export ROMID=$R
for f in $A/lang/*.json; do python3 $T/mklang "$f" en --headers-only --romid=$R; done
for f in $A/pads/*.json; do python3 $T/mkpads "$f" "" --headers-only --romid=$R; done
for f in $A/tiles/*.json; do python3 $T/mktiles "$f" "" --headers-only --romid=$R; done
python3 $T/mkanims $A/animations.json "" --headers-only --romid=$R
python3 $T/mksequences $A/sequences.json "" --headers-only --romid=$R
python3 $T/mktextures $A/textures.json "" --headers-only --romid=$R
