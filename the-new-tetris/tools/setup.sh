#!/bin/bash
# setup.sh <dir>: a build tree: tnt-splat at the pinned commit + decomp.patch + hh/, libultra (decompals/ultralib)
# headers and C in tools/ultralib. No ROM is needed to build: the game reads the player's ROM at run time.
#   then: make -f hh/Makefile.handheld PLAT=miyoo -j4   (PLAT=pc: the 32-bit x86 proxy for tests)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR=${1:?dir}
git clone -q https://github.com/chris-gilmore/tnt-splat "$DIR"
git -C "$DIR" checkout -q bbbded9f6837e26f9c4b0007b6911e96d4585074
git clone -q https://github.com/decompals/ultralib "$DIR/tools/ultralib"
git -C "$DIR/tools/ultralib" checkout -q e24c836796df4bf520ff8b11a5c9d2cea3a66cbd
git -C "$DIR" apply "$HERE/decomp.patch"
cp -r "$HERE/hh" "$DIR/hh"
printf 'hh/\nbuild/\ntools/ultralib/\n' >> "$DIR/.git/info/exclude"
echo "tree ready: $DIR"
