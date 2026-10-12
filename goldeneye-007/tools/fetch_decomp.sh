#!/usr/bin/env bash
# Recreate refs/007: a CODE-ONLY partial clone of n64decomp/007 pinned to one commit.
#
# n64decomp/007 commits ~35 MB of ROM-derived data (assets/obseg bg/stan/setup C, animation
# tables, fonts, the Rareware logo ...). None of that is ever fetched here: the clone is
# --filter=blob:none --no-checkout, and the sparse checkout is the EXACT path list in
# tools/decomp_sparse_paths.txt:
#   - code, docs, build scripts, the IDO/IRIX compiler (tools/irix) and metadata CSVs;
#   - the ~1,060 small assets/ headers/tables the game code #includes (names, enums,
#     model-file header/record rows, weapon/explosion tables, skeleton joint lists,
#     .s glue) -- approved by the user 2026-09-24; kept untracked, never shipped.
# The real asset content is extracted from the user's own ROM at build time (M1).
# Afterwards origin points nowhere, so no later git command can lazily fetch a skipped blob.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN=c4356466796c697dfd298010b9bed261f9ed8c6a
DEST="${DEST:-$ROOT/refs/007}"
[ -e "$DEST" ] && { echo "$DEST exists; remove it first" >&2; exit 1; }
git clone --quiet --filter=blob:none --no-checkout https://github.com/n64decomp/007.git "$DEST"
cd "$DEST"
git config core.sparseCheckout true
git config core.sparseCheckoutCone false
mkdir -p .git/info
sed 's|^|/|' "$ROOT/tools/decomp_sparse_paths.txt" > .git/info/sparse-checkout
git checkout --quiet --detach "$PIN"
git remote set-url origin /nonexistent/do-not-lazy-fetch
# Guard: no asset data (.c other than .inc.c, .bin) and no media may be checked out.
bad=$(git ls-files -t | awk '$1=="H"{print $2}' | grep -E '^assets/.*\.(c|bin)$|\.(png|jpg|gif|ogg)$' | grep -v '\.inc\.c$' || true)
[ -z "$bad" ] || { echo "UNEXPECTED asset/media files checked out:" >&2; echo "$bad" >&2; exit 1; }
echo "refs/007 @ $(git rev-parse --short HEAD): $(git ls-files -t | grep -c '^H') files checked out"
