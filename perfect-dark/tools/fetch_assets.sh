#!/usr/bin/env bash
# Fetch the asset metadata the build's ID headers are made from (tools/gen_headers.sh), into
# src/assets/<romid>/ (gitignored, never committed or shipped).
#
# The game code here is fgsfdsfgs/perfect_dark (the PC port of the n64decomp/perfect_dark
# decompilation) with the Miyoo layer on top. That project keeps its per-ROM asset metadata
# (src/assets/<romid>/*.json: asset names and IDs, language tables, pads, tiles) in git; this
# repository does not. This makes a partial clone of it pinned to the commit the port is built from,
# checks out only those .json files, and copies them into place. Only names/IDs reach the code.
#
#   tools/fetch_assets.sh            # ROMID=ntsc-final (the USA Rev 1 ROM the Miyoo build uses)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN=32a1cb9f268dd3ac73016801025c6bbbfa20130f
R=${ROMID:-ntsc-final}
REF="${REF:-$ROOT/refs/perfect_dark}"
if [ ! -e "$REF" ]; then
    git clone --quiet --filter=blob:none --no-checkout https://github.com/fgsfdsfgs/perfect_dark.git "$REF"
    git -C "$REF" config core.sparseCheckout true
    git -C "$REF" config core.sparseCheckoutCone false
    mkdir -p "$REF/.git/info"
    printf '%s\n' "/src/assets/$R/*.json" "/src/assets/$R/lang/*.json" \
        "/src/assets/$R/pads/*.json" "/src/assets/$R/tiles/*.json" > "$REF/.git/info/sparse-checkout"
    git -C "$REF" checkout --quiet --detach "$PIN"
fi
[ "$(git -C "$REF" rev-parse HEAD)" = "$PIN" ] || { echo "$REF is not at $PIN; remove it first" >&2; exit 1; }
mkdir -p "$ROOT/src/assets/$R"
for d in . lang pads tiles; do
    mkdir -p "$ROOT/src/assets/$R/$d"
    cp "$REF/src/assets/$R/$d/"*.json "$ROOT/src/assets/$R/$d/"
done
echo "src/assets/$R from fgsfdsfgs/perfect_dark @ ${PIN:0:8}: $(find "$ROOT/src/assets/$R" -name '*.json' | wc -l) files"
