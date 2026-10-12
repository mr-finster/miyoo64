#!/bin/bash
# ARM sweep (after ocarina-of-time's qsweep.sh): the handheld binary under qemu-arm
# with the virtual clock, every frame rendered, frames dumped. Compare two
# sweeps with `sweep.sh diff`. NEON paths only run here, not in the x86 build.
#
#   qsweep.sh <out dir> [binary dir] [frames] [every]
#
# The binary dir holds sf64, recipes.bin and embedded.bin (default
# build/miyoo). Extra environment (e.g. SF64_CMNEON=0) passes through; the
# data dir (SF64_DATA, default ./qsweep_data) must be prepared by one run
# before parallel sweeps of the same binary.
set -eu
out=${1:?out dir}; dir=${2:-build/miyoo}; frames=${3:-2400}; every=${4:-100}
S=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/arm-linux-gnueabihf/sysroot
mkdir -p "$out"
find "$out" -maxdepth 1 -name '*.ppm' -delete
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SF64_VCLOCK=1 SF64_FAST=1 SF64_DYNRES=0 SF64_AUTO=${SF64_AUTO:-2} \
    SF64_EXIT_AT="$frames" SF64_DUMP="$out" SF64_DUMP_EVERY="$every" \
    SF64_BASEROM=${SF64_BASEROM:-./baserom.us.rev1.z64} SF64_DATA=${SF64_DATA:-./qsweep_data} \
    qemu-arm -L "$S" "$dir/sf64" > "$out/run.log" 2>&1
echo "qsweep: $(ls "$out"/frame_*.ppm | wc -l) frames in $out"
