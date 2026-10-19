#!/bin/bash
# PUBLIC card zip: no ROM and nothing made from one. The data pack is built on the player's Mini at first start from
# their ROM (hh/src/hh_convert.c: App/HarvestMoon64/rom.z64, the game folder, or the SD card's Roms/N64; SHA-1
# checked, any byte order) by the recipe (hm64_recipes.bin: op numbers, lengths and the build's own addresses, no ROM
# bytes; hh/tools/hm64_recipe.py).
#   tools/package_public.sh <build tree> <out.zip>
# <build tree>: the tree tools/setup.sh made, built with `make -f hh/Makefile.handheld recipe` (build/hh/hm64 and
# build/hh/hm64_recipes.bin of the same link).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TREE=$(cd "${1:?build tree}" && pwd); OUT=$(readlink -f "${2:?out zip}")
STAGE=$(mktemp -d)
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-strip -o $STAGE/hm64 $TREE/build/hh/hm64
cp $TREE/build/hh/hm64_recipes.bin $STAGE/
head -c 4 $STAGE/hm64_recipes.bin | grep -q HMR1
rm -f "$OUT"
python3 $HERE/miyoo/tools/miyoo_pack.py $HERE/miyoo/game.json --build $STAGE -o "$OUT"
rm -rf $STAGE
unzip -l "$OUT"
