#!/bin/sh
# Real game audio through the NEON mixer kernels and the C loops side by side
# (OOT_MIXER_VERIFY=1) under qemu-arm, audio driven per frame (OOT_AUDIO_SYNC):
#   ./dev.sh 'sh /work/port/tools/handheld/mixer_verify.sh <builddir> [entrance] [frames]'
# Prints the verify counters (calls, mismatches) from the log.
set -u
BD=/work/port/$1; ENT=${2:-0xEE}; FR=${3:-600}
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
A=/tmp/mv_$(basename $BD)_$ENT; rm -rf $A; mkdir -p $A/od
ln -s /work/port/build/hh-gen/data $A/data; cp $BD/embedded.bin $A/
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_ASSETS=$A OOT_DATA=$A/od OOT_SEED=1234 OOT_AUDIO_SYNC=1 \
    OOT_MIXER_VERIFY=1 OOT_ENTRANCE=$ENT OOT_EXIT_AT=$FR timeout -s KILL 3000 qemu-arm -L $SR $BD/oot > $A/run.log 2>&1
echo "exit $? ($ENT, $FR frames)"
L=$(cat $A/run.log $A/od/*.log 2>/dev/null | grep -a 'MIXER VERIFY')
echo "$L" | grep -v ' calls, ' | sort | uniq -c | head -8   # mismatch reports, if any
echo "$L" | grep ' calls, ' | tail -1                            # the last running total
