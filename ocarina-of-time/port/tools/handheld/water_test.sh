#!/bin/sh
# Build tools/handheld/water_test.c for ARM (run under qemu-arm: the NEON rows) and x86 -m32 (the scalar loops) and
# compare every variant of the basement water's draws, byte for byte (2026-10-01; see the file's header).
#   sh tools/handheld/water_test.sh [frames]      (in the build container: ./dev.sh 'sh /work/port/tools/handheld/water_test.sh')
# Exit 0 when every variant drew the same pixels and the ARM and x86 frame hashes agree.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-100}
O=${WT_OUT:-/tmp/water_test}; mkdir -p "$O"
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
        $SDK/usr/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/wt_arm $P/tools/handheld/water_test.c -lm
    QEMU="qemu-arm -L $SR"
else
    arm-linux-gnueabihf-gcc $ARMF $FLAGS -static -o $O/wt_arm $P/tools/handheld/water_test.c -lm
    QEMU=qemu-arm
fi
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS \
    -o $O/wt_x86 $P/tools/handheld/water_test.c -lm
fail=0
$QEMU $O/wt_arm ops || fail=1
for run in "1:" "2:H_PURE=1" "3:H_OPAQUE=1" "4:H_PSHADE0=1"; do
    seed=${run%%:*}; envs=${run#*:}
    a=$(env $envs $QEMU $O/wt_arm cmp $seed $N | tail -1) || fail=1
    x=$(env $envs $O/wt_x86 cmp $seed $N | tail -1) || fail=1
    echo "ARM ${envs:-mixed}: $a"; echo "x86 ${envs:-mixed}: $x"
    # (the "|| fail" above sees tail's status, not the test's: check the count itself, 2026-10-02)
    for r in "$a" "$x"; do case "$r" in *" 0 px differ"*) ;; *) echo "  pixels differ (or no result)"; fail=1 ;; esac; done
    ha=$(echo "$a" | sed 's/.*hash \([0-9a-f]*\).*/\1/'); hx=$(echo "$x" | sed 's/.*hash \([0-9a-f]*\).*/\1/')
    [ "$ha" = "$hx" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
done
a=$(H_FAST=1 H_FWTINT=0 $QEMU $O/wt_arm cmp 5 $N | tail -1) || fail=1; echo "ARM fastwater, no tint: $a"
case "$a" in *" 0 px differ"*) ;; *) echo "  pixels differ (or no result)"; fail=1 ;; esac
[ $fail = 0 ] && echo "water_test: PASS" || { echo "water_test: FAIL"; exit 1; }
