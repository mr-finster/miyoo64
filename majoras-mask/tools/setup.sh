#!/bin/bash
# Make a build tree: zeldaret/mm at the commit this port is based on, with the port's changes and its hh/ layer.
#   tools/setup.sh <dir> <your Majora's Mask (USA) .z64>
# Then, in <dir>: the decomp's own setup and matching build (generated headers, extracted assets, the N64 ELF),
# the data pack, and the Miyoo build. Everything ROM-derived stays in <dir> (git-ignored there).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR=${1:?dir}; ROM=$(readlink -f "${2:?rom}")
MM_COMMIT=bf6bd9d0d3a90fe2f8960e034fbfd328e8bdef22
git clone https://github.com/zeldaret/mm.git "$DIR"
cd "$DIR" && git checkout -q $MM_COMMIT
git apply "$HERE/decomp.patch"
cp -r "$HERE/hh" .
cp "$ROM" baseroms/n64-us/baserom.z64
python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt
make -C tools -j4
P=.venv/bin/python3; E=extracted/n64-us
$P tools/decompress_baserom.py -v n64-us
$P tools/extract_baserom.py baseroms/n64-us/baserom-decompressed.z64 $E/baserom -v n64-us
$P tools/extract_incbins.py $E/baserom $E/incbin -v n64-us
$P tools/extract_yars.py $E/baserom -v n64-us
make assets
# the matching N64 build: its generated files, and the ELF whose symbols give every asset/ROM address
make -j4 rom
make -f hh/Makefile.handheld pack          # build/hh-pack.bin (ROM-derived, local only)
make -f hh/Makefile.handheld PLAT=miyoo -j4 # build/hh-miyoo/mm
# the first-start recipe the public zip ships (op numbers only): how the device rebuilds the pack from the ROM
.venv/bin/python3 hh/tools/mm_recipes.py baseroms/n64-us/baserom.z64 build/hh-pack.bin build/mm_recipes.bin
# then, back here: tools/build_miyoo.sh <dir>  ->  MajorasMask-miyoo.zip
