#!/bin/sh
# clock_test.sh: FZX_VDSOCLOCK (goldeneye-007's X37). Builds tools/clock_test.c for x86 -m32 (the host kernel's vDSO is found)
# and ARM under qemu-arm (qemu's own vDSO, which the 2x pick declines). CT_FORCE runs the agreement check through the
# vDSO where the pick declined it. The device's own pick is console.log's CLOCK line.
#   sh <this> [reads]    (in the build image; HH_SDK, CT_OUT)
set -e
ulimit -c 0
B=$(cd "$(dirname "$0")/.." && pwd)
O=${CT_OUT:-/tmp/clock_test}; mkdir -p "$O"
INC="-I$B/game/include -I$B/game -I$B/game/src/port -I$B/game/src/port/gfx -I$B/game/src/port/ultra -I$B/game/src/port/audio -I$B/game/src/port/sdl"
DEFS="-D_LANGUAGE_C -DLANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI_2 -DNDEBUG -DVERSION_US=1 -DNON_MATCHING -DAVOID_UB -DHANDHELD=1 -DTARGET_HANDHELD=1 -DPORT_REAL_PTR_MIN=0x10000000u -D_OS_LIBC_H_ -include stdint.h"
FLAGS="-std=gnu11 -O2 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w"
SDK=${HH_SDK:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
[ -e $SR/lib/ld-musl-armhf.so.1 ] || U="-Wl,--unresolved-symbols=ignore-all"
$SDK/usr/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 \
    $INC $DEFS $FLAGS -DHANDHELD=1 -DHANDHELD_DEVICE=1 -o $O/ct_arm $B/tools/clock_test.c $U
gcc -m32 -fno-pie -no-pie -msse2 -mfpmath=sse $INC $DEFS $FLAGS -o $O/ct_x86 $B/tools/clock_test.c $U
fail=0
for v in 1 0; do
    FZX_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    [ $v = 0 ] || CT_FORCE=1 FZX_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    FZX_VDSOCLOCK=$v qemu-arm -L $SR $O/ct_arm ${1:-20000} 2>&1 || fail=1
done
[ $fail = 0 ] && echo "clock_test.sh: PASS" || { echo "clock_test.sh: FAIL"; exit 1; }
