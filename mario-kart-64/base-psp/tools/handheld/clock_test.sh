#!/bin/sh
# clock_test.sh: MK64_VDSOCLOCK (2026-10-04). Builds tools/handheld/clock_test.c (it includes src/port/sdl/main.c; the
# game is left unresolved) with Makefile.handheld's flags for x86 -m32 (the host kernel's vDSO is found and used) and
# ARM under qemu-arm (qemu 8's own vDSO). CT_FORCE runs the agreement check through the vDSO where the pick
# (faster by 2x) declined it. The device's own pick is console.log's CLOCK line.
#   sh tools/handheld/clock_test.sh [reads]    (from base-psp/, in the build image; HH_SDK, CT_OUT)
set -e
ulimit -c 0 # no qemu core files
B=$(cd "$(dirname "$0")/../.." && pwd)
O=${CT_OUT:-/tmp/clock_test}; mkdir -p "$O"
FLAGS="-std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-toplevel-reorder -fno-math-errno \
 -ffp-contract=off -fsingle-precision-constant -w -DPORT_DRAW_DIST=3000 -DPORT_MAX_VOICES=18 -DVERSION_US=1 \
 -DPORT_PROFILE=1 -DF3DEX_GBI=1 -DF3D_OLD=1 -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1 -DNO_SEGMENTED_MEMORY=1 \
 -DPORT_TEX32=2 -DTARGET_HANDHELD=1 -I$B/include -I$B/src -I$B/src/port -I$B/src/port/gfx -I$B/src/port/audio \
 -I$B/src/port/sdl -I$B"
ARMF="-O3 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer -DHANDHELD=1"
X86="-O2 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
SR=$SDK/arm-linux-gnueabihf/sysroot
CC_ARM="$SDK/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
U="-Wl,--unresolved-symbols=ignore-all"
SDLI=$($SDK/bin/sdl-config --cflags 2>/dev/null || true)
$CC_ARM $ARMF $FLAGS $SDLI -o $O/ct_arm $B/tools/handheld/clock_test.c $U
gcc $X86 $FLAGS -o $O/ct_x86 $B/tools/handheld/clock_test.c $U
fail=0
for v in 1 0 ab; do
    MK64_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    [ $v = 0 ] || CT_FORCE=1 MK64_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    MK64_VDSOCLOCK=$v $QEMU $O/ct_arm ${1:-20000} 2>&1 || fail=1
done
[ $fail = 0 ] && echo "clock_test.sh: PASS" || { echo "clock_test.sh: FAIL"; exit 1; }
