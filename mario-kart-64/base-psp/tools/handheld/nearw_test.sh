#!/bin/sh
# nearw_test.sh: MK64_NEARW (the w plane at the projection's near plane where that is nearer than 2; 2026-10-04).
# Builds tools/handheld/nearw_test.c (it includes src/port/gfx/gfx_pc.c; the rasterizer's entry points it never reaches
# are left unresolved) for ARM (the Miyoo toolchain's gcc under qemu-arm) and x86 -m32 with Makefile.handheld's flags. No ROM.
#   sh tools/handheld/nearw_test.sh    (from base-psp/, in the build image; HH_SDK, NW_OUT)
# Pass: every projection's W_MIN as the switch says, on both; near 1 (Koopa Troopa Beach) with no disagreement beyond
# W_MIN; near 2 and 9 the same with the switch on and off; ARM = x86 line for line.
set -e
B=$(cd "$(dirname "$0")/../.." && pwd)
O=${NW_OUT:-/tmp/nearw_test}; mkdir -p "$O"
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
$CC_ARM $ARMF $FLAGS -o $O/nw_arm $B/tools/handheld/nearw_test.c -lm $U
gcc $X86 $FLAGS -o $O/nw_x86 $B/tools/handheld/nearw_test.c -lm $U
fail=0
for nw in 1 0; do
    MK64_NEARW=$nw $QEMU $O/nw_arm wmin > $O/wmin_arm_$nw.txt || fail=1
    MK64_NEARW=$nw $O/nw_x86 wmin > $O/wmin_x86_$nw.txt || fail=1
    tail -1 $O/wmin_arm_$nw.txt
    cmp -s $O/wmin_arm_$nw.txt $O/wmin_x86_$nw.txt || { echo "wmin MK64_NEARW=$nw: ARM and x86 differ"; fail=1; }
    grep -q ' 0 wrong' $O/wmin_arm_$nw.txt || { cat $O/wmin_arm_$nw.txt; fail=1; }
    for near in 1 2 9; do
        MK64_NEARW=$nw $QEMU $O/nw_arm clip $near 1 ${NW_TRIS:-100000} > $O/clip_arm_${nw}_$near.txt
        MK64_NEARW=$nw $O/nw_x86 clip $near 1 ${NW_TRIS:-100000} > $O/clip_x86_${nw}_$near.txt
        cat $O/clip_arm_${nw}_$near.txt
        cmp -s $O/clip_arm_${nw}_$near.txt $O/clip_x86_${nw}_$near.txt || { echo "  x86 differs"; fail=1; }
    done
done
# Koopa Troopa Beach (near 1): with the switch nothing beyond the w plane differs from the N64; near 2 and 9 (every
# other course): the switch changes nothing (their disagreements beyond W_MIN are the clipper's parked B1/B3: it fans
# the polygon before its -0.97w pass, so it draws what lies between w 2 and the near plane)
grep -q ' 0 beyond' $O/clip_arm_1_1.txt || { echo "near 1: points beyond W_MIN differ"; fail=1; }
for near in 2 9; do
    [ "$(sed 's/MK64_NEARW=[01]//' $O/clip_arm_1_$near.txt)" = "$(sed 's/MK64_NEARW=[01]//' $O/clip_arm_0_$near.txt)" ] || { echo "near $near: the switch changed it"; fail=1; }
done
[ $fail = 0 ] && echo "nearw_test: PASS" || { echo "nearw_test: FAIL"; exit 1; }
