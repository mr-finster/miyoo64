#!/bin/sh
# Bit-exact sweep of a handheld (ARM) build under qemu-arm: the scenes of
# sweep.sh, fixed RNG seed, PPM dumps into refs/sweep_arm_<tag>/. ARM-only code
# (NEON paths, the usat clamps, fill32) never runs in the desktop sweep, so
# changes to it are checked here, build against build:
#   ./dev.sh 'sh /work/port/tools/handheld/qsweep.sh <tag> <builddir>'   (builddir: oot + its embedded.bin)
#   tools/handheld/sweep.sh --cmp arm_<a> arm_<b>
set -u
TAG=$1; BD=/work/port/$2
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
OUT=/work/refs/sweep_arm_$TAG; rm -rf $OUT; mkdir -p $OUT
run() { # name, dump-every, exit-at, env...
    name=$1 every=$2 exitat=$3; shift 3
    A=/tmp/qs_$name; rm -rf $A; mkdir -p $A $A/od $OUT/$name
    ln -s /work/port/build/hh-gen/data $A/data; cp $BD/embedded.bin $A/
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_ASSETS=$A OOT_DATA=$A/od OOT_SEED=1234 "$@" \
        OOT_EXIT_AT=$exitat OOT_DUMP_EVERY=$every OOT_DUMP=$OUT/$name \
        timeout -s KILL 3000 qemu-arm -L $SR $BD/oot > $OUT/$name.log 2>&1
}
run kokiri  30 121 OOT_ENTRANCE=0xEE &
run deku    30 121 OOT_ENTRANCE=0x209 &
run field   30 121 OOT_ENTRANCE=0x17D &
run fieldcd 30 121 OOT_ENTRANCE=0xCD &
run market  30 121 OOT_ENTRANCE=0xB1 &
run title  100 601 OOT_AUTO=1 &
wait
run pause   40 241 OOT_ENTRANCE=0xEE OOT_AUTO=1
ls "$OUT"/*/ | grep -c ppm
