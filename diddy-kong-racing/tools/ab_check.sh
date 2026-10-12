#!/bin/sh
# ab_check.sh [BASE_DIR] [OUT_DIR]: the standard bit-exact A/B for a change,
# run inside the container:  ./dev.sh sh tools/ab_check.sh
#
# BASE_DIR (default refs/base) holds dkr_pc and dkr_hh built from the previous
# commit (copy build/pc/dkr and build/miyoo/dkr there before the change); the
# new binaries are build/pc/dkr and build/miyoo/dkr. DATA (default refs/data)
# must hold dkr_assets.bin from the player's ROM (never commit it).
#
# Frames: x86 race at 100% (the player setting) and 85%, SHARP at 100%
# (DKR_BILINEAR=0), the benchmark tour (81 frames); qemu-arm race at 100%; and
# the new build x86 vs qemu-arm at 100%
# (they match pixel for pixel under DKR_FAST once the proxy renders the
# square size with DKR_W/DKR_H; its default is 320x240). Audio: the game
# audio under qemu-arm with the NEON kernels on and off (md5 1872f667...
# since round 7).
# "0/N differ" everywhere = bit-exact. On the user's Mac the x86 proxy runs
# emulated, so the full set takes a while; qemu timings are not device numbers.
set -u
cd "$(dirname "$0")/.."
B=${1:-refs/base}; S=${2:-refs/ab}; D=${DATA:-refs/data} # OUT_DIR is wiped first: keep other runs elsewhere
[ -x "$B/dkr_pc" ] && [ -x "$B/dkr_hh" ] || { echo "need $B/dkr_pc and $B/dkr_hh (the previous commit's builds)"; exit 1; }
[ -f "$D/dkr_assets.bin" ] || { echo "need $D/dkr_assets.bin (DKR_ROM=<rom> DKR_DATA=$D DKR_ASSETS_ONLY=1 build/pc/dkr)"; exit 1; }
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
rm -rf "$S"; mkdir -p "$S"
t0=$(date +%s)
BIN=$B/dkr_pc tools/sweep.sh x86 $S/x_b85 $D DKR_INTRES=85 DKR_EXIT_AT=4000 &
tools/sweep.sh x86 $S/x_n85 $D DKR_INTRES=85 DKR_EXIT_AT=4000 &
BIN=$B/dkr_pc tools/sweep.sh x86 $S/x_b100 $D DKR_INTRES=100 DKR_EXIT_AT=4000 &
tools/sweep.sh x86 $S/x_n100 $D DKR_INTRES=100 DKR_EXIT_AT=4000 &
BIN=$B/dkr_pc tools/sweep.sh x86 $S/x_bbench $D DKR_BENCH=1 DKR_DUMP_EVERY=250 DKR_EXIT_AT=-1 &
tools/sweep.sh x86 $S/x_nbench $D DKR_BENCH=1 DKR_DUMP_EVERY=250 DKR_EXIT_AT=-1 &
wait
BIN=$B/dkr_pc tools/sweep.sh x86 $S/x_bsharp $D DKR_BILINEAR=0 DKR_EXIT_AT=4000 &
tools/sweep.sh x86 $S/x_nsharp $D DKR_BILINEAR=0 DKR_EXIT_AT=4000 &
# a handheld build before round 14 takes its 3D rung and filter from the cfg (no cfg: 85%);
# this cfg gives it 100% SMOOTH. Round 14 and 15 read only magbilin from it; round 16 on read nothing.
FULL='display=0\ndynres=0\nfilter=1\nmagbilin=1\nintres=100\n'
CFG=$FULL BIN=$(pwd)/$B/dkr_hh tools/sweep.sh arm $S/a_b100 $D &
CFG=$FULL tools/sweep.sh arm $S/a_n100 $D &
tools/sweep.sh x86 $S/x_n100sq $D DKR_W=240 DKR_H=240 DKR_EXIT_AT=3200 &
tools/sweep.sh arm $S/a_aud1 $D DKR_EXIT_AT=2600 DKR_DUMP_EVERY=100000 DKR_AUDIO_DUMP=$(pwd)/$S/aud_neon1.raw &
tools/sweep.sh arm $S/a_aud0 $D DKR_EXIT_AT=2600 DKR_DUMP_EVERY=100000 DKR_AUDIO_DUMP=$(pwd)/$S/aud_neon0.raw DKR_AUDIONEON=0 &
wait
echo "--- results ($(( $(date +%s) - t0 )) s)"
{
for p in 85 100 bench sharp; do echo "x86 $p: $(tools/sweep_cmp.sh $S/x_b$p $S/x_n$p | tail -1)"; done
echo "arm 100: $(tools/sweep_cmp.sh $S/a_b100 $S/a_n100 | tail -1)"
echo "x86 vs arm (new, 100%, square): $(tools/sweep_cmp.sh $S/a_n100 $S/x_n100sq | tail -1)"
} | tee $S/summary.txt
md5sum $S/aud_neon1.raw $S/aud_neon0.raw || echo "ERROR: an audio dump is missing" | tee -a $S/summary.txt
if grep -q ERROR $S/summary.txt; then echo "*** A/B INVALID: a comparison had no frames or mismatched frame sets (see above) ***"; fi
for p in 85 100 bench sharp; do tools/sweep_cmp.sh $S/x_b$p $S/x_n$p | grep differs | head -5; done
tools/sweep_cmp.sh $S/a_b100 $S/a_n100 | grep differs | head -5
