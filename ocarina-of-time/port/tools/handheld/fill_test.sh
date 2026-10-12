#!/bin/sh
# Build tools/handheld/fill_test.c for ARM (run under qemu-arm: the NEON rows) and x86 -m32 (the scalar loops): OOT_FILLRULE's
# owned edges (no pixel drawn twice, no crack, on meshes whose triangles share edges) and OOT_SHADELERP's rows
# (2026-10-03; see the file's header).
#   sh tools/handheld/fill_test.sh [frames]   (HH_SDK=<Miyoo toolchain> picks the device compiler; else the distro's gcc)
# Exit 0 when both pass on ARM and x86 and their hashes agree.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-2000}
O=${FT_OUT:-/tmp/fill_test}; mkdir -p "$O"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w \
 -include $P/src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 \
 -DOOT_REVISION=0 -DOOT_REGION=REGION_US -DLIBULTRA_VERSION=LIBULTRA_VERSION_L -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 \
 -DPLATFORM_N64=0 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE \
 -I$P/src/port/hh/gfx -I$P/kit/platform -iquote $P/include -iquote $P/include/libc -iquote $P/src -iquote $P \
 -I$P/src/port/hh/shim -I$P/src/port/hh"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DTARGET_HANDHELD=1 -DHANDHELD=1"
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
if [ -x $SDK/usr/bin/arm-linux-gnueabihf-gcc ]; then
        $SDK/usr/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/ft_arm $P/tools/handheld/fill_test.c -lm
    QEMU="qemu-arm -L $SR"
else
    arm-linux-gnueabihf-gcc $ARMF $FLAGS -static -o $O/ft_arm $P/tools/handheld/fill_test.c -lm
    QEMU=qemu-arm
fi
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS \
    -o $O/ft_x86 $P/tools/handheld/fill_test.c -lm
fail=0
for seed in 1 2 3; do
    a=$($QEMU $O/ft_arm $seed $N | tail -1) || fail=1
    x=$($O/ft_x86 $seed $N | tail -1) || fail=1
    echo "ARM seed $seed: $a"; echo "x86 seed $seed: $x"
    ha=$(echo "$a" | sed 's/.*hash \([0-9a-f]*\).*/\1/'); hx=$(echo "$x" | sed 's/.*hash \([0-9a-f]*\).*/\1/')
    [ "$ha" = "$hx" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
done
[ $fail = 0 ] && echo "fill_test: PASS" || { echo "fill_test: FAIL"; exit 1; }
