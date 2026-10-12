#!/bin/sh
# ARM instruction profile of a handheld build under qemu-arm (insnprof.c, a TCG
# plugin counting executed guest instructions per block). Two runs from boot
# exit at frames FROM and TO; their difference is exactly what frames FROM..TO
# cost, every thread included, with a fixed RNG seed (OOT_SEED) on a square output and the
# audio updates driven per game frame (OOT_AUDIO_SYNC: the wall-clock audio thread
# made the counts swing 10%+ between runs).
#   ./dev.sh 'sh /work/port/tools/qprof/run.sh <tag> <builddir> [entrance] [from] [to]'
# <builddir> (relative to port/) holds `oot` + the embedded.bin of the same link.
# Output: refs/qprof_<tag>/profile.txt (per function, symprof.py).
set -u
TAG=$1; BD=/work/port/$2; ENT=${3:-0xEE}; F1=${4:-60}; F2=${5:-120}
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
Q=/work/port/tools/qprof
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
gcc -shared -fPIC -O2 -I$Q $Q/insnprof.c -o /tmp/insnprof.so || exit 1
A=/tmp/qa; rm -rf $A; mkdir -p $A /tmp/od
ln -s /work/port/build/hh-gen/data $A/data
cp $BD/embedded.bin $A/
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_W=240 OOT_H=240 OOT_ASSETS=$A OOT_DATA=/tmp/od \
       OOT_SEED=1234 OOT_ENTRANCE=$ENT OOT_AUDIO_SYNC=1 ${EXTRA_ENV:-OOT_QPROF=1}
O=/work/refs/qprof_$TAG; rm -rf $O; mkdir -p $O
t0=$(date +%s)
for F in $F1 $F2; do
  OOT_EXIT_AT=$F timeout 5000 qemu-arm -L $SR -plugin /tmp/insnprof.so,out=$O/prof_$F.txt $BD/oot > $O/run_$F.log 2>&1
  echo "run to $F: exit=$? ($(( $(date +%s) - t0 ))s)"
done
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-nm -n --defined-only $BD/oot > $O/syms.txt
python3 $Q/symprof.py $O/syms.txt $O/prof_$F1.txt $O/prof_$F2.txt > $O/profile.txt
head -30 $O/profile.txt
