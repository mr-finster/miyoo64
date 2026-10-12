#!/bin/sh
# Build port/hh/tools/fill_test.c for ARM (run under qemu-arm: the NEON rows, the lean blended rows, GE_LEANTRI's row loop)
# and x86 -m32 (the scalar loops, as the desktop proxy): GE_FILLRULE's owned edges -- no pixel drawn twice, no crack, every
# row loop the same pixels -- on meshes whose triangles share edges (2026-10-03; see the file's header).
#   sh port/hh/tools/fill_test.sh [frames]
# The ARM compiler: $HH_SDK (a Miyoo toolchain, the device's gcc 10.2) or /opt/miyoomini-toolchain/usr, else arm-linux-gnueabihf-gcc.
# Exit 0 when every seed passes on ARM and x86 and their hashes agree.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-2000}
O=${FT_OUT:-/tmp/fill_test}; mkdir -p "$O"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -ffp-contract=off -w -D_LANGUAGE_C \
 -DHH_PORT_LAYER=1 -DTARGET_HANDHELD=1 -DPORT_GE_RAWCC=1 -I$P/hh/include -I$P/hh/gfx -I$P/kit/platform -I$P \
 -idirafter $P/include -idirafter $P/include/ultra64 -idirafter $P/include/PR -include $P/hh/include/hh_enum.h"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DHANDHELD=1"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
if [ -x $SDK/bin/arm-linux-gnueabihf-gcc ]; then
    SR=$SDK/arm-linux-gnueabihf/sysroot
    $SDK/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/ft_arm $P/hh/tools/fill_test.c -lm
    QEMU="qemu-arm -L $SR"
    echo "ARM: the Miyoo toolchain's gcc ($SDK)"
else
    arm-linux-gnueabihf-gcc $ARMF $FLAGS -static -o $O/ft_arm $P/hh/tools/fill_test.c -lm
    QEMU=qemu-arm
    echo "ARM: arm-linux-gnueabihf-gcc"
fi
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1 $FLAGS \
    -o $O/ft_x86 $P/hh/tools/fill_test.c -lm
fail=0
for seed in 1 2 3; do
    a=$($QEMU $O/ft_arm $seed $N | tail -1) || fail=1
    x=$($O/ft_x86 $seed $N | tail -1) || fail=1
    echo "ARM seed $seed: $a"; echo "x86 seed $seed: $x"
    for r in "$a" "$x"; do case "$r" in *" 0 twice, 0 cracks, 0 outside, 0 px differ"*) ;; *) echo "  owned edges failed (or no result)"; fail=1 ;; esac; done
    ha=$(echo "$a" | sed 's/.*hash \([0-9a-f]*\).*/\1/'); hx=$(echo "$x" | sed 's/.*hash \([0-9a-f]*\).*/\1/')
    [ "$ha" = "$hx" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
done
[ $fail = 0 ] && echo "fill_test: PASS" || { echo "fill_test: FAIL"; exit 1; }
