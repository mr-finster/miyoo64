#!/bin/sh
# The native port's changes to the Paper Mario decomp (M2), kept as one patch against a pinned commit.
#   tools/port/decomp_patch.sh save    refs/papermario's working changes -> game/patches/decomp.patch
#   tools/port/decomp_patch.sh apply   a fresh clone at the pinned commit + the patch
#   tools/port/decomp_patch.sh check   the patch still applies to the pinned commit
# The decomp's own build (./configure, asset splitting) runs in the pm64hh:decomp image (docker/Dockerfile.decomp).
set -e
cd "$(dirname "$0")/../.."
PIN=ee321921d
DECOMP=refs/papermario
PATCH=game/patches/decomp.patch
case "$1" in
save)
    git -C "$DECOMP" diff "$PIN" -- . > "$PATCH"
    echo "saved $(grep -c '^diff --git' "$PATCH") files to $PATCH"
    ;;
apply)
    [ -d "$DECOMP/.git" ] || git clone https://github.com/pmret/papermario "$DECOMP"
    git -C "$DECOMP" checkout -q "$PIN"
    git -C "$DECOMP" apply --whitespace=nowarn "$(pwd)/$PATCH"
    echo "applied $PATCH at $PIN"
    ;;
check)
    tmp=$(mktemp -d)
    git -C "$DECOMP" worktree add -q --detach "$tmp" "$PIN"
    git -C "$tmp" apply --check "$(pwd)/$PATCH" && echo "applies cleanly at $PIN"
    git -C "$DECOMP" worktree remove --force "$tmp"
    ;;
*) echo "usage: $0 save|apply|check"; exit 2 ;;
esac
