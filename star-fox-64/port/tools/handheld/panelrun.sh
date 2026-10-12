#!/bin/bash
# Device panels for a before/after sheet (the owner's rule: every visible fix gets before | after |
# real game). In the container: one device binary under qemu with SF64_AUTO=2 (or SF64_KEYS, passed
# through), dumping the square panel the owner sees (panel_*.ppm) and the render (frame_*.ppm).
#
#   tools/handheld/panelrun.sh <tag> <bin dir> <every> <exit at>     -> /work/refs/panels/<tag>/
#
# The bin dir holds sf64, recipes.bin and embedded.bin (keep a copy of each build you may want as
# "before", e.g. refs/sweeps/bin_<tag>). The device backend numbers dumps by rendered frame from 0;
# SF64_DUMP_FROM (passed through) skips the ones before a moment (builds from round 11 on; older
# binaries dump from 0, so pick <every> to land on the frames you want and delete the rest).
# Each binary gets its own data dir (its image is built on the first start). The real game at the
# same frames: m64ref.sh with the SF64_AUTO=2 pad converted to polls (poll = 2 * read - 73), then
# tools/handheld/abzsheet.py (BUILDLOG round 10).
set -u
cd /work/port
T=$1; B=$2; E=$3; N=$4
S=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
O=/work/refs/panels/$T; D=/work/refs/panels/data_$(basename "$B")
rm -rf "$O"; mkdir -p "$O" "$D"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SF64_EXIT_AT=2 SF64_BASEROM=/work/port/baserom.us.rev1.z64 SF64_DATA="$D" \
  timeout 300 qemu-arm -L $S "$B/sf64" > /dev/null 2>&1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SF64_VCLOCK=1 SF64_FAST=1 SF64_AUTO=${SF64_AUTO:-2} SF64_EXIT_AT="$N" \
  SF64_DUMP="$O" SF64_DUMP_EVERY="$E" SF64_DUMP_FROM="${SF64_DUMP_FROM:-0}" SF64_BASEROM=/work/port/baserom.us.rev1.z64 SF64_DATA="$D" \
  timeout 3000 qemu-arm -L $S "$B/sf64" > "$O/run.log" 2>&1
echo "$T: exit $?, $(ls "$O"/panel_*.ppm 2>/dev/null | wc -l) panels, $(grep 'gamestate 7' "$O/run.log" | head -1)"
