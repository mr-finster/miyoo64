#!/bin/bash
# Make a build tree: the Banjo-Kazooie decompilation at the commit this port is based on, with the port's changes and
# its hh/ layer.
#   tools/setup.sh <dir> <your Banjo-Kazooie (USA 1.0) .z64>
# The ROM is only used by the release gates (tools/build_miyoo.sh: the leak scan); the game itself reads the player's
# ROM on the device. Everything ROM-derived stays in <dir>.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR=${1:?dir}; ROM=$(readlink -f "${2:?rom}")
BK_COMMIT=e8d31fa5
git clone https://gitlab.com/banjo.decomp/banjo-kazooie "$DIR"
cd "$DIR" && git checkout -q $BK_COMMIT
git submodule update --init --recursive -q lib/ultralib tools/bk_rom_compressor
git apply "$HERE/decomp.patch"
cp -r "$HERE/hh" .
cp "$ROM" baserom.us.v10.z64
# the decompressed ROM (core1, core2 and the level code inflated), for the leak scan
(cd tools/bk_rom_compressor && cargo build -q --release --bin bk_rom_decompress)
tools/bk_rom_compressor/target/release/bk_rom_decompress baserom.us.v10.z64 decompressed.us.v10.z64
# then, back here: tools/build_miyoo.sh <dir>  ->  BanjoKazooie-miyoo.zip
