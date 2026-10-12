#!/bin/sh
# clock_test.sh: DKR_VDSOCLOCK (the GoldenEye port's X37). Builds tools/clock_test.c for x86 -m32 (the host kernel's vDSO is found)
# and ARM under qemu-arm (qemu's own vDSO, which the 2x pick declines). CT_FORCE runs the agreement check through the
# vDSO where the pick declined it. The device's own pick is console.log's CLOCK line.
#   sh <this> [reads]    (in the build image; HH_SDK, CT_OUT)
set -e
ulimit -c 0
B=$(cd "$(dirname "$0")/.." && pwd)
O=${CT_OUT:-/tmp/clock_test}; mkdir -p "$O"
INC="-I$B/port -I$B/port/gfx -I$B/port/sdl -I$B/port/audio -I$B/port/ultra -iquote $B/decomp/src -iquote $B/decomp/include -idirafter $B/decomp/include -idirafter $B/decomp/include/PR"
DEFS="-D_LANGUAGE_C -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -D_FINALROM -DNDEBUG -DF3DDKR_GBI -DVERSION_us_v80 -D_MIPS_SZLONG=32 -DCIC_ID=6103 -DPORT=1 -DMODERN_CC -DTARGET_HANDHELD=1"
FLAGS="-std=gnu11 -O2 -funsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
SR=$SDK/arm-linux-gnueabihf/sysroot
U="-Wl,--unresolved-symbols=ignore-all"
$SDK/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 \
    $INC $DEFS $FLAGS -DHANDHELD=1  -o $O/ct_arm $B/tools/clock_test.c $U
gcc -m32 -fno-pie -no-pie -msse2 -mfpmath=sse $INC $DEFS $FLAGS -o $O/ct_x86 $B/tools/clock_test.c $U
fail=0
for v in 1 0; do
    DKR_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    [ $v = 0 ] || CT_FORCE=1 DKR_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    DKR_VDSOCLOCK=$v qemu-arm -L $SR $O/ct_arm ${1:-20000} 2>&1 || fail=1
done
[ $fail = 0 ] && echo "clock_test.sh: PASS" || { echo "clock_test.sh: FAIL"; exit 1; }
