#!/bin/sh
# fill_test.sh: MK64_FILLRULE (owned edges for blended draws without AA, 2026-10-03; the OoT port a7c4909). Builds
# tools/handheld/fill_test.c (it includes src/port/sdl/gfx_soft.c) for ARM (the Miyoo toolchain's gcc under qemu-arm: the
# NEON rows) and x86 -m32 (the scalar loops) with Makefile.handheld's flags, and the same harness against gfx_soft.c of
# git $OLD (default sweep-2026-10-01, the tree before the rule) with only the old rule's frames. Needs no ROM.
#   sh tools/handheld/fill_test.sh [frames]    (from base-psp/, in the build image: ./dev.sh; HH_SDK, FT_OUT)
# Pass: with the rule no pixel twice, no crack, nothing outside, ARM hash = x86 hash; the old rule's frames (the switch
# off) hash the same as $OLD's on both.
set -e
B=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-2000}
OLD=${OLD:-sweep-2026-10-01}
O=${FT_OUT:-/tmp/fill_test}; mkdir -p "$O/old"
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
git -C $B show $OLD:base-psp/src/port/sdl/gfx_soft.c > $O/old/gfx_soft.c
for v in new old; do
    if [ $v = new ]; then I="-iquote $B/src/port/sdl"; D=""; else I="-iquote $O/old"; D="-DFT_OLDTREE"; fi
    $CC_ARM $ARMF $FLAGS $D $I -o $O/ft_arm_$v $B/tools/handheld/fill_test.c -lm
    gcc $X86 $FLAGS $D $I -o $O/ft_x86_$v $B/tools/handheld/fill_test.c -lm
done
hash() { echo "$1" | sed -n "s/.* $2 \([0-9a-f]*\).*/\1/p"; }
fail=0
for seed in 1 2 3; do
    a=$($QEMU $O/ft_arm_new $seed $N | tail -1) || fail=1
    x=$($O/ft_x86_new $seed $N | tail -1) || fail=1
    ao=$(FT_OLDONLY=1 $QEMU $O/ft_arm_old $seed $N | tail -1)
    xo=$(FT_OLDONLY=1 $O/ft_x86_old $seed $N | tail -1)
    echo "ARM seed $seed: $a"; echo "x86 seed $seed: $x"
    echo "ARM seed $seed, $OLD: $ao"; echo "x86 seed $seed, $OLD: $xo"
    [ "$(hash "$a" hash)" = "$(hash "$x" hash)" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
    [ "$(hash "$a" old)" = "$(hash "$ao" old)" ] || { echo "  ARM: the switch-off frames differ from $OLD"; fail=1; }
    [ "$(hash "$x" old)" = "$(hash "$xo" old)" ] || { echo "  x86: the switch-off frames differ from $OLD"; fail=1; }
done
[ $fail = 0 ] && echo "fill_test: PASS" || { echo "fill_test: FAIL"; exit 1; }
