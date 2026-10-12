#!/bin/sh
# First-start converter check (M3b): the game builds its data from the ROM into an
# empty folder, then both files are compared with the host-built ones.
#   ./dev.sh 'sh /work/port/tools/handheld/qconvert.sh <handheld|pc> [rom] [order]'
# handheld: the device binary under qemu-arm; pc: the 32-bit x86 proxy. [order]: z64
# (default), v64 or n64 -- a byte-swapped copy of the ROM is made in /tmp for the run.
# Then: a second start must build nothing, and a changed pack id must rebuild.
set -eu
PLAT=$1; ROM=${2:-/work/port/baseroms/ntsc-1.0/baserom.z64}; ORDER=${3:-z64}
BD=/work/port/build/hh-$PLAT
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
if [ "$PLAT" = handheld ]; then
    command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
        RUN="qemu-arm -L $SR $BD/oot"
else
    RUN="$BD/oot"
fi
A=/tmp/qconv_$PLAT; rm -rf $A; mkdir -p $A
if [ "$ORDER" != z64 ]; then
    python3 - "$ROM" $A/rom.$ORDER $ORDER <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
if sys.argv[3] == 'v64': d[0::2], d[1::2] = d[1::2], d[0::2]
else: d[0::4], d[1::4], d[2::4], d[3::4] = d[3::4], d[2::4], d[1::4], d[0::4]
open(sys.argv[2], 'wb').write(d)
PY
    ROM=$A/rom.$ORDER
fi
run() { env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_ASSETS=$A/root OOT_DATA=$A/od OOT_ROM=$ROM \
        OOT_RECIPES=$BD/oot_recipes.bin OOT_CONVERT_ONLY=1 "$@" $RUN; }
mkdir -p $A/root $A/od
T0=$(date +%s.%N); run > $A/first.log 2>&1 || { echo "FAIL: first start exit $?"; tail -5 $A/first.log; exit 1; }
T1=$(date +%s.%N)
grep '^hh: ' $A/first.log | grep -v '^hh: .*|' | sed 's/^/  /'
echo "  first start: $(python3 -c "print(round($T1 - $T0, 1))") s wall ($PLAT, $ORDER)"
cmp $A/root/data/segments/oot_psp_assets.bin /work/port/build/hh-gen/data/segments/oot_psp_assets.bin && echo "  oot_psp_assets.bin: byte-identical ($(stat -c %s $A/root/data/segments/oot_psp_assets.bin) bytes)"
cmp $A/root/embedded.bin $BD/embedded.bin && echo "  embedded.bin: byte-identical ($(stat -c %s $A/root/embedded.bin) bytes)"
run > $A/second.log 2>&1
grep -q 'nothing to build' $A/second.log && echo "  second start: builds nothing"
printf 'XXXXHHPI' > $A/root/data/segments/oot_psp_assets.id   # another recipe's pack
run > $A/third.log 2>&1
grep -q 'building the game data' $A/third.log && cmp -s $A/root/data/segments/oot_psp_assets.bin /work/port/build/hh-gen/data/segments/oot_psp_assets.bin \
    && echo "  changed pack id: rebuilt, byte-identical"
printf '\0\0\0\0HHEB' | dd of=$A/root/embedded.bin bs=1 seek=$(( $(stat -c %s $A/root/embedded.bin) - 8 )) conv=notrunc 2>/dev/null  # another link's stamp
run > $A/fourth.log 2>&1
grep -q 'building embedded.bin' $A/fourth.log && ! grep -q 'SHA-1' $A/fourth.log && cmp -s $A/root/embedded.bin $BD/embedded.bin \
    && echo "  another build's embedded.bin: rebuilt from the pack alone (no ROM read), byte-identical"
