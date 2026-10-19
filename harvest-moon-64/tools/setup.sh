#!/bin/bash
# Make a build tree: harvestwhisperer/hm64-decomp at the commit this port is based on, with the port's changes and
# its hh/ layer, then the decomp's own setup and matching build (the transpiled bytecode and text, the N64 ELF and
# map), the Miyoo build and the data pack. Everything ROM-derived stays in <dir> (git-ignored there).
#   tools/setup.sh <dir> <your Harvest Moon 64 (USA) .z64>   # SHA-1 90631460f1876a14849df0541d534012b410a34c
# Needs: the Miyoo toolchain (/opt/miyoomini-toolchain), python3 + venv, binutils-mips-linux-gnu, gcc, curl.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DIR=${1:?dir}; ROM=$(readlink -f "${2:?rom}")
HM64_COMMIT=9824b88
[ "$(sha1sum "$ROM" | cut -d' ' -f1)" = 90631460f1876a14849df0541d534012b410a34c ] || { echo "not Harvest Moon 64 (USA) .z64"; exit 1; }
git clone https://github.com/harvestwhisperer/hm64-decomp "$DIR"
cd "$DIR" && git checkout -q $HM64_COMMIT
git apply "$HERE/decomp.patch"
cp -r "$HERE/hh" .
cp "$ROM" baserom.us.z64
python3 -m venv .venv && .venv/bin/pip install -q -r tools/requirements.txt 'splat64[mips]==0.36.3'
mkdir -p tools/gcc-2.7.2
curl -sSL https://github.com/decompals/mips-gcc-2.7.2/releases/download/main/gcc-2.7.2-linux.tar.gz | tar -xz -C tools/gcc-2.7.2
curl -sSL https://github.com/decompals/mips-binutils-2.6/releases/latest/download/binutils-2.6-linux.tar.gz | tar -xz -C tools/gcc-2.7.2
gcc -o tools/build/mkldscript tools/build/mkldscript.c tools/build/spec.c tools/build/util.c
export PATH="$PWD/.venv/bin:$PATH"
# the matching N64 build (decomp.patch changes only TARGET_HANDHELD code): bytecode/text assembly, hm64.elf/.map
make setup
make -j4
make -f hh/Makefile.handheld -j4      # build/hh/hm64
make -f hh/Makefile.handheld pack     # build/hh/hm64-pack.bin (ROM-derived, local only; rebuild after every link)
