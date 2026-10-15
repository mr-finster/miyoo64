#!/usr/bin/env bash
# qemu smoke run of build/miyoo/pd.arm (no display, no audio): PPM frame dumps + PERF lines.
#   tools/qemu_run.sh <your Perfect Dark (USA) Rev 1 .z64> <run dir> [seconds=120] [dump every=60]
# The ROM is symlinked into <run dir> (keep that outside the repo); nothing is copied.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM="$(readlink -f "${1:?rom}")"; RUN="${2:?run dir}"; T="${3:-120}"; E="${4:-60}"
SYSROOT=${SYSROOT:-/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot}
mkdir -p "$RUN/dump" "$RUN/save"
ln -sf "$ROM" "$RUN/pd.ntsc-final.z64"
# SIGKILL: the game answers SIGTERM with the kit's exit save and keeps running under qemu
SDL_VIDEODRIVER=dummy PD_NOAUDIO=1 GE_THREADS=1 GE_DUMP="$RUN/dump" GE_DUMP_EVERY="$E" \
  timeout -s KILL "$T" qemu-arm -L "$SYSROOT" "$ROOT/build/miyoo/pd.arm" \
  --basedir "$RUN" --savedir "$RUN/save" > "$RUN/log.txt" 2>&1 || true
echo "$(ls "$RUN/dump" | wc -l) frames dumped to $RUN/dump; log $RUN/log.txt"
grep PERF "$RUN/log.txt" | tail -5 || true
