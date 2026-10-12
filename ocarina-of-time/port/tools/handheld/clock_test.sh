#!/bin/sh
# Build tools/handheld/clock_test.c (hh_port.c's port_time_us64: the vDSO clock against the syscall) for x86 -m32 and
# ARM (qemu-arm; on the device run the ARM binary itself: it is the device's clock that counts) and run both.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
O=${CT_OUT:-/tmp/clock_test}; mkdir -p "$O"
FLAGS="-std=gnu11 -fsigned-char -fwrapv -w -include $P/src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB \
 -DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 -DOOT_REVISION=0 -DOOT_REGION=REGION_US -DLIBULTRA_VERSION=LIBULTRA_VERSION_L \
 -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 -DPLATFORM_N64=0 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL \
 -DGBI_DOWHILE -I$P/src/port/hh/gfx -I$P/kit/platform -iquote $P/include -iquote $P/include/libc -iquote $P/src -iquote $P \
 -I$P/src/port/hh/shim -I$P/src/port/hh -iquote $P/src/port/psp"
fail=0
# (a host without the i386 kernel headers: the x86_64 multiarch ones dispatch on __i386__)
gcc -O2 -m32 -fno-pie -no-pie -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS -o $O/clock_x86 $P/tools/handheld/clock_test.c 2>/dev/null ||
    gcc -O2 -m32 -fno-pie -no-pie -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS -idirafter /usr/include/x86_64-linux-gnu \
        -o $O/clock_x86 $P/tools/handheld/clock_test.c
$O/clock_x86 || fail=1
OOT_VDSOCLOCK=0 $O/clock_x86 20000 | head -1
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
if [ -x $SDK/usr/bin/arm-linux-gnueabihf-gcc ]; then
    $SDK/usr/bin/arm-linux-gnueabihf-gcc -O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 \
        -DTARGET_HANDHELD=1 -DHANDHELD=1 $FLAGS -o $O/clock_arm $P/tools/handheld/clock_test.c
    qemu-arm -L $SDK/usr/arm-linux-gnueabihf/sysroot $O/clock_arm 20000 || fail=1
else
    arm-linux-gnueabihf-gcc -O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
        -DTARGET_HANDHELD=1 -DHANDHELD=1 $FLAGS -static -o $O/clock_arm $P/tools/handheld/clock_test.c
    qemu-arm $O/clock_arm 20000 || fail=1
fi
[ $fail = 0 ] && echo "clock_test: PASS" || { echo "clock_test: FAIL"; exit 1; }
