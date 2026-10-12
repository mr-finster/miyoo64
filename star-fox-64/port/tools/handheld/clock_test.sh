#!/bin/sh
# clock_test.sh: SF64_VDSOCLOCK (goldeneye-007's X37). Builds tools/handheld/clock_test.c for x86 -m32 (the host kernel's vDSO is found)
# and ARM under qemu-arm (qemu's own vDSO, which the 2x pick declines). CT_FORCE runs the agreement check through the
# vDSO where the pick declined it. The device's own pick is console.log's CLOCK line.
#   sh <this> [reads]    (in the build image; HH_SDK, CT_OUT)
set -e
ulimit -c 0
B=$(cd "$(dirname "$0")/../.." && pwd)
O=${CT_OUT:-/tmp/clock_test}; mkdir -p "$O"
INC="-I$B/include -I$B/include/libultra -I$B -I$B/src/port -I$B/src/port/gfx -I$B/src/port/sdl -I$B/src/port/audio"
DEFS="-DF3DEX_GBI=1 -D_LANGUAGE_C=1 -DVERSION_US=1 -DNON_MATCHING=1 -DAVOID_UB=1 -DNDEBUG=1 -D_MIPS_SZLONG=32 -DPORT_PROFILE=1 -DTARGET_HANDHELD=1"
FLAGS="-std=gnu11 -O2 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w"
SDK=${HH_SDK:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
[ -e $SR/lib/ld-musl-armhf.so.1 ] || U="-Wl,--unresolved-symbols=ignore-all"
$SDK/usr/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 \
    $INC $DEFS $FLAGS -DHANDHELD=1  -o $O/ct_arm $B/tools/handheld/clock_test.c $U
gcc -m32 -fno-pie -no-pie -msse2 -mfpmath=sse $INC $DEFS $FLAGS -o $O/ct_x86 $B/tools/handheld/clock_test.c $U
fail=0
for v in 1 0; do
    SF64_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    [ $v = 0 ] || CT_FORCE=1 SF64_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    SF64_VDSOCLOCK=$v qemu-arm -L $SR $O/ct_arm ${1:-20000} 2>&1 || fail=1
done
[ $fail = 0 ] && echo "clock_test.sh: PASS" || { echo "clock_test.sh: FAIL"; exit 1; }
