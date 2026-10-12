#!/bin/sh
# sweep.sh arm|x86 OUTDIR DATADIR [ENV=VAL...]
# Deterministic frame-dump run for bit-exact A/B checks. DATADIR must hold a
# dkr_assets.bin extracted from the player's ROM (never commit it); it is
# copied so saves and cfg stay per-run. Defaults: race autopilot, dumps every
# 100 iterations, 3200 iterations, 3D at 100% (the only player setting since
# round 14; lower rungs and SHARP are developer switches). Override with env, e.g.
#   tools/sweep.sh arm /tmp/a data DKR_CMNEON=0
#   tools/sweep.sh x86 /tmp/b data DKR_BENCH=1 DKR_DUMP_EVERY=250 DKR_EXIT_AT=-1   (the benchmark exits itself)
#   tools/sweep.sh arm /tmp/c data DKR_INTRES=85 DKR_BILINEAR=0   (a lower rung, SHARP: developer A/B only)
# Compare two runs with tools/sweep_cmp.sh A B. qemu timings are not device numbers.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
plat=$1; out=$2; data=$3; shift 3
SYSROOT=${SYSROOT:-/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot}
rm -rf "$out" "$out.data"; mkdir -p "$out"; cp -r "$data" "$out.data"; rm -f "$out.data"/state_*.sav
[ -n "${CFG:-}" ] && printf "$CFG" > "$out.data/dkr_display.cfg"
case $plat in
  arm) run="qemu-arm -L $SYSROOT ${BIN:-$here/build/miyoo/dkr}" ;;
  x86) run="${BIN:-$here/build/pc/dkr}" ;;
  *) echo "arm|x86"; exit 1 ;;
esac
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy DKR_DATA="$out.data" DKR_FAST=1 DKR_AUTO=race DKR_INTRES=100 \
    DKR_DUMP="$out" DKR_DUMP_EVERY=100 DKR_EXIT_AT=3200 "$@" $run > "$out.log" 2>&1 || true
echo "$out: $(ls "$out" | wc -l) frames, $(tail -1 "$out.log")"
