#!/bin/sh
# Build port/hh/tools/cvgalpha_test.c for x86 -m32 (the scalar loops) and ARM (NEON rows, under qemu-arm) and run it;
# with OLD=<git rev> also against that tree's gfx_soft.c: its frames must equal GE_CVGALPHA=0's (the A/B path).
#   sh port/hh/tools/cvgalpha_test.sh [frames]        HANDHELD_SDK=<dir> for the device compiler (else arm-linux-gnueabihf-gcc)
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-5000}
O=${CV_OUT:-/tmp/cvgalpha_test}; mkdir -p "$O/old"
FL="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -ffp-contract=off -w -D_LANGUAGE_C -DHH_PORT_LAYER=1
 -DTARGET_HANDHELD=1 -DPORT_GE_RAWCC=1 -I$P/hh/include -I$P/kit/platform -I$P -idirafter $P/include
 -idirafter $P/include/ultra64 -idirafter $P/include/PR -include $P/hh/include/hh_enum.h"
X86="-O2 -m32 -msse2 -mfpmath=sse -malign-data=abi -fno-pie -no-pie -DTARGET_PC=1"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DHANDHELD=1"
gcc $X86 $FL -I$P/hh/gfx -o $O/cv_x86 $P/hh/tools/cvgalpha_test.c -lm
SDK=${HANDHELD_SDK:-/opt/miyoomini-toolchain/usr}
if [ -x $SDK/bin/arm-linux-gnueabihf-gcc ]; then
    SR=$SDK/arm-linux-gnueabihf/sysroot
    $SDK/bin/arm-linux-gnueabihf-gcc $ARMF $FL -I$P/hh/gfx -static -o $O/cv_arm $P/hh/tools/cvgalpha_test.c -lm
else
    arm-linux-gnueabihf-gcc $ARMF $FL -I$P/hh/gfx -static -o $O/cv_arm $P/hh/tools/cvgalpha_test.c -lm
fi
fail=0
for seed in 1 2 3; do
    x=$($O/cv_x86 $N $seed) || fail=1; a=$(qemu-arm $O/cv_arm $N $seed) || fail=1
    echo "seed $seed x86: $(echo "$x" | tr '\n' ' ')"; echo "seed $seed ARM: $(echo "$a" | tr '\n' ' ')"
    [ "$(echo "$x" | grep hash)" = "$(echo "$a" | grep hash)" ] || { echo "  x86 and ARM old-path hashes differ"; fail=1; }
    if [ -n "$OLD" ]; then
        git -C $P show $OLD:port/hh/gfx/gfx_soft.c > $O/old/gfx_soft.c
        gcc $X86 $FL -I$O/old -I$P/hh/gfx -DCV_OLDFILE -o $O/cv_old $P/hh/tools/cvgalpha_test.c -lm
        h=$($O/cv_old $N $seed | grep hash || true)
        [ "$h" = "$(echo "$x" | grep hash)" ] && echo "  GE_CVGALPHA=0 == $OLD" || { echo "  GE_CVGALPHA=0 differs from $OLD"; fail=1; }
    fi
done
[ $fail = 0 ] && echo "cvgalpha_test.sh: PASS" || { echo "cvgalpha_test.sh: FAIL"; exit 1; }
