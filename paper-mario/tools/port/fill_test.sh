#!/bin/sh
# fill_test.sh: PM64_FILLRULE (owned edges for blended draws without AA, 2026-10-03; ocarina-of-time a7c4909). Builds
# tools/port/fill_test.c (it includes bench/kit/gfx_soft.c, the renderer of the bench and the game) for ARM (the
# Miyoo toolchain's gcc 8.3 under qemu-arm: the NEON rows) and x86 -m32 (the scalar loops), with the bench's flags, and the
# same harness against gfx_soft.c of git $OLD (default sweep-2026-10-01, the tree before the rule) with only the old
# rule's frames. Then tools/port/fps_test.c (hash 6, matrix 3) on the same two trees: the switch is off there, so equal. Needs no
# ROM; the decomp headers at refs/papermario/include (tools/setup_local.sh).
#   sh tools/port/fill_test.sh [frames]    (in the build image: ./dev.sh sh tools/port/fill_test.sh; MIYOO_TOOLCHAIN, FT_OUT)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-2000}
OLD=${OLD:-sweep-2026-10-01}
O=${FT_OUT:-/tmp/fill_test}; mkdir -p "$O/old"
DEF="-DTARGET_HANDHELD=1 -DPM64=1 -DPM64_REPLAY=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DPORT_NO_FPS=1 -DNON_MATCHING=1 -DAVOID_UB=1"
COMMON="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common $DEF -I$R/bench/kit -I$R/refs/papermario/include -I$R/tools/port -w"
ARMF="-march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fsingle-precision-constant -O3 -fno-math-errno -fomit-frame-pointer -ffp-contract=off"
X86="-m32 -fno-pie -no-pie -malign-data=abi -fsingle-precision-constant -O2 -fno-math-errno -msse2 -mfpmath=sse -ffp-contract=off"
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr
SR=$SDK/arm-linux-gnueabihf/sysroot
CC_ARM="$SDK/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
git -C $R show $OLD:bench/kit/gfx_soft.c > $O/old/gfx_soft.c
for v in new old; do
    if [ $v = new ]; then I="-iquote $R/bench/kit"; D=""; else I="-iquote $O/old"; D="-DFT_OLDTREE"; fi
    $CC_ARM $ARMF $COMMON $D $I -o $O/ft_arm_$v $R/tools/port/fill_test.c -lm
    gcc $X86 $COMMON $D $I -o $O/ft_x86_$v $R/tools/port/fill_test.c -lm
    F2=""; [ $v = old ] && F2=-DFPS_OLDTREE
    $CC_ARM $ARMF $COMMON $F2 $I -o $O/fps_arm_$v $R/tools/port/fps_test.c -lm
    gcc $X86 $COMMON $F2 $I -o $O/fps_x86_$v $R/tools/port/fps_test.c -lm
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
for m in "hash 6" "matrix 3"; do
    ref=""
    for b in "arm_new $QEMU" "arm_old $QEMU" "x86_new" "x86_old"; do
        set -- $b; t=$1; shift
        h=$("$@" $O/fps_$t $m | tail -1)
        echo "fps_test $m $t: $h"
        hh=$(hash "$h" hash); [ -z "$ref" ] && ref=$hh
        [ -n "$hh" ] && [ "$hh" = "$ref" ] || { echo "  fps_test $m $t differs"; fail=1; }
    done
done
[ $fail = 0 ] && echo "fill_test: PASS" || { echo "fill_test: FAIL"; exit 1; }
