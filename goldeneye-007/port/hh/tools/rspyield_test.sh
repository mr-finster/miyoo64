#!/bin/sh
# The sound across slow frames (GE_RSPYIELD, hh/hh_os.c): hh/tools/rspyield_test.c with the real hh_os.c, hh_ctx.S,
# hh_hw.c, hh/audio/hh_audio_sdl.c and src/sched.c, built for the device (the Miyoo toolchain's gcc: $HH_SDK or
# /opt/miyoomini-toolchain/usr) and run under qemu-arm in real time, once with the old kernel (GE_RSPYIELD=0) and once with the
# default. Each run must keep the game's sound continuous (no AI sample dropped or repeated), never run the main
# thread during a draw, and draw every display list unchanged; the default must not run the card dry.
#   sh port/hh/tools/rspyield_test.sh [seconds]
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
S=${1:-12}
O=${RY_OUT:-/tmp/rspyield_test}; mkdir -p "$O"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
SR=$SDK/arm-linux-gnueabihf/sysroot
CC="$SDK/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
GAME="-std=gnu89 -O2 -funsigned-char -fplan9-extensions -fno-common -fwrapv -fno-strict-aliasing -fno-pie
 -ffp-contract=off -fno-math-errno -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DBUGFIX_R0 -DBYTEMATCH -DPORT=1
 -D_LANGUAGE_C -DTARGET_HANDHELD=1 -DHANDHELD=1 -I$P/hh/include -I$P/kit/platform -I$P -I$P/src -I$P/src/game
 -I$P/src/inflate -I$P/src/libultra -I$P/src/libultrare -idirafter $P/include -idirafter $P/include/ultra64
 -idirafter $P/include/PR -I$SR/usr/include/SDL -include $P/hh/include/hh_enum.h -w"
KIT="-std=gnu11 -O2 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -ffp-contract=off -fno-pie -D_LANGUAGE_C
 -DHH_PORT_LAYER=1 -DTARGET_HANDHELD=1 -DHANDHELD=1 -I$P/hh/include -I$P/hh/gfx -I$P/kit/platform -I$P -idirafter $P/include
 -idirafter $P/include/ultra64 -idirafter $P/include/PR -I$SR/usr/include/SDL -include $P/hh/include/hh_enum.h -w"
$CC $GAME -DHH_PORT_LAYER=1 -c -o $O/os.o $P/hh/hh_os.c
$CC $GAME -DHH_PORT_LAYER=1 -c -o $O/hw.o $P/hh/hh_hw.c
$CC $GAME -include $P/hh/include/ge_libc.h -c -o $O/sched.o $P/src/sched.c
$CC $KIT -c -o $O/ring.o $P/hh/audio/hh_audio_sdl.c
$CC -c -o $O/ctx.o $P/hh/hh_ctx.S
$CC $GAME -std=gnu11 -DHH_PORT_LAYER=1 -I$P/hh -c -o $O/t.o $P/hh/tools/rspyield_test.c
$CC -no-pie -o $O/rt $O/t.o $O/os.o $O/hw.o $O/sched.o $O/ring.o $O/ctx.o -lpthread -Wl,--unresolved-symbols=ignore-all
fail=0
GE_RSPYIELD=0 qemu-arm -L $SR $O/rt $S | tee $O/old.txt | grep rspyield_test || fail=1
qemu-arm -L $SR $O/rt $S | tee $O/new.txt | grep rspyield_test || fail=1
RY_PLAIN=1 qemu-arm -L $SR $O/rt $S | tee $O/plain.txt | grep rspyield_test || fail=1
grep -q "yields 0," $O/plain.txt || { echo "  45 ms frames yielded"; fail=1; }
u0=$(sed -n 's/.*underruns \([0-9]*\).*/\1/p' $O/old.txt); u1=$(sed -n 's/.*underruns \([0-9]*\).*/\1/p' $O/new.txt)
echo "underruns: GE_RSPYIELD=0 $u0, default $u1"
[ -n "$u1" ] && [ "$u1" -eq 0 ] || fail=1
[ $fail = 0 ] && echo "rspyield_test: PASS" || { echo "rspyield_test: FAIL"; exit 1; }
