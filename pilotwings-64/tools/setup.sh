#!/bin/bash
# setup.sh <dir>: a build tree: the Pilotwings 64 decompilation (n64decomp/pw64, MIT) at the commit this port is
# based on, with the port's changes (decomp.patch, every change behind TARGET_HANDHELD) and its hh/ layer. No ROM is
# needed to build: the game reads the player's ROM at run time.
#   then: tools/build_miyoo.sh <dir> <your Pilotwings 64 (USA) .z64>  ->  Pilotwings64-miyoo.zip
#   (or by hand: make -f hh/Makefile.handheld PLAT=miyoo -j4; PLAT=pc: the 32-bit x86 proxy for tests)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR=${1:?dir}
git clone -q https://github.com/n64decomp/pw64 "$DIR"
git -C "$DIR" checkout -q 11a76263f4a6fdbecdc86d2701a35075b18ba511
git -C "$DIR" apply "$HERE/decomp.patch"
cp -r "$HERE/hh" "$DIR/hh"
printf 'hh/\nbuild/\n' >> "$DIR/.git/info/exclude"
echo "tree ready: $DIR"
