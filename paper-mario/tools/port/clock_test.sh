#!/bin/sh
# clock_test.sh: PM64_VDSOCLOCK. Builds tools/port/clock_test.c (it includes game/port/port_host.c; the game is left
# unresolved) for x86 -m32 (the host kernel's vDSO is found and used) and ARM under qemu-arm (qemu's own vDSO, which
# the 2x pick declines). CT_FORCE runs the agreement check through the vDSO where the pick declined it. The device's
# own pick is console.log's CLOCK line.
#   sh tools/port/clock_test.sh [reads]    (from the repo root, in the build image; DECOMP=<papermario>, CT_OUT)
set -e
ulimit -c 0
B=$(cd "$(dirname "$0")/../.." && pwd)
D=${DECOMP:-$B/refs/papermario}
O=${CT_OUT:-/tmp/clock_test}; mkdir -p "$O"
INC="-I$B/bench/kit -I$B/kit/platform -I$D/include -I$B/game/port"
DEFS="-DTARGET_HANDHELD=1 -DPM64=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DPORT_NO_FPS=1 -DNON_MATCHING=1 -DAVOID_UB=1"
FLAGS="-std=gnu11 -O2 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w"
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr
SR=$SDK/arm-linux-gnueabihf/sysroot
U="-Wl,--unresolved-symbols=ignore-all"
$SDK/bin/arm-linux-gnueabihf-gcc -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 \
    $INC $DEFS $FLAGS -DPM64_SDL12=1 -DHANDHELD=1 -o $O/ct_arm $B/tools/port/clock_test.c $U
gcc -m32 -fno-pie -no-pie -msse2 -mfpmath=sse $INC $DEFS $FLAGS -o $O/ct_x86 $B/tools/port/clock_test.c $U
fail=0
for v in 1 0; do
    PM64_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    [ $v = 0 ] || CT_FORCE=1 PM64_VDSOCLOCK=$v $O/ct_x86 ${1:-200000} 2>&1 || fail=1
    PM64_VDSOCLOCK=$v qemu-arm -L $SR $O/ct_arm ${1:-20000} 2>&1 || fail=1
done
[ $fail = 0 ] && echo "clock_test.sh: PASS" || { echo "clock_test.sh: FAIL"; exit 1; }
