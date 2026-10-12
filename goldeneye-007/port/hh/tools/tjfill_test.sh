#!/bin/sh
# Build port/hh/tools/tjfill_test.c for x86 -m32 (the scalar loops) and ARM (NEON rows, under qemu-arm) and run it (GE_TJFILL,
# 2026-10-04): every T-junction sliver filled, nothing else changed, GE_TJFILL=0 = the old frame, the row loops agree, and the
# ARM and x86 frame hashes agree.
#   sh port/hh/tools/tjfill_test.sh [frames]        HANDHELD_SDK=<dir> for the device compiler (else arm-linux-gnueabihf-gcc)
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-3000}
O=${TJ_OUT:-/tmp/tjfill_test}; mkdir -p "$O"
FL="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -ffp-contract=off -w -D_LANGUAGE_C -DHH_PORT_LAYER=1
 -DTARGET_HANDHELD=1 -DPORT_GE_RAWCC=1 -I$P/hh/include -I$P/kit/platform -I$P -idirafter $P/include
 -idirafter $P/include/ultra64 -idirafter $P/include/PR -include $P/hh/include/hh_enum.h"
X86="-O2 -m32 -msse2 -mfpmath=sse -malign-data=abi -fno-pie -no-pie -DTARGET_PC=1"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DHANDHELD=1"
gcc $X86 $FL -I$P/hh/gfx -o $O/tj_x86 $P/hh/tools/tjfill_test.c -lm
SDK=${HANDHELD_SDK:-/opt/miyoomini-toolchain/usr}
if [ -x $SDK/bin/arm-linux-gnueabihf-gcc ]; then
    $SDK/bin/arm-linux-gnueabihf-gcc $ARMF $FL -I$P/hh/gfx -static -o $O/tj_arm $P/hh/tools/tjfill_test.c -lm
else
    arm-linux-gnueabihf-gcc $ARMF $FL -I$P/hh/gfx -static -o $O/tj_arm $P/hh/tools/tjfill_test.c -lm
fi
fail=0
for seed in 1 2 3; do
    x=$($O/tj_x86 $N $seed) || fail=1; a=$(qemu-arm $O/tj_arm $N $seed) || fail=1
    echo "seed $seed x86: $(echo "$x" | tr '\n' ' ')"; echo "seed $seed ARM: $(echo "$a" | tr '\n' ' ')"
    hx=$(echo "$x" | sed -n "s/.*fill-only hash \([0-9a-f]*\).*/\1/p"); ha=$(echo "$a" | sed -n "s/.*fill-only hash \([0-9a-f]*\).*/\1/p")
    [ -n "$hx" ] && [ "$hx" = "$ha" ] || { echo "  x86 and ARM hashes differ"; fail=1; }
done
[ $fail = 0 ] && echo "tjfill_test.sh: PASS" || { echo "tjfill_test.sh: FAIL"; exit 1; }
