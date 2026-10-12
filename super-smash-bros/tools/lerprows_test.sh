#!/bin/sh
# The texel-lerp NEON rows (SSB64_LERPROWS; tools/lerprows_test.c's header): build the harness for ARM (the Miyoo
# toolchain's gcc 8.3 under qemu-arm: the NEON rows) and x86 -m32 (the scalar loops), then
#  - rand: every frame drawn with the rows off, on, and with the NEON rows off (the scalar loops) must be equal, colour
#    and depth; the hash with the rows on must be the same on ARM and x86;
#  - OLD=<git rev>: the same harness against that tree's gfx_soft.c must give the same hash on both (nothing else moved);
#  - mut: each mutation of the new rows (MUT=1..n, applied to a copy of gfx_soft.c) must make rand fail on ARM.
#   sh tools/lerprows_test.sh [frames] [mut]    MIYOO_TOOLCHAIN=<dir> (default /opt/miyoomini-toolchain), LR_OUT=<dir> (default /tmp/lerprows_test)
# Nothing big is written: a few small binaries.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
P=$R/port
N=${1:-200}
O=${LR_OUT:-/tmp/lerprows_test}; mkdir -p "$O/old" "$O/mut"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w \
 -DPORT=1 -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -DREGION_US=1 -DVERSION_US=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C \
 -DN_MICRO=1 -DHANDHELD_PORT=1 -DTARGET_HANDHELD=1 -I$P/hh -I$P/hh/include -idirafter $P/decomp/include -idirafter $P/decomp/src"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -fno-PIC -DHANDHELD=1"
X86="-O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1"
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr
if [ -x $SDK/bin/arm-linux-gnueabihf-gcc ]; then
    SR=$SDK/arm-linux-gnueabihf/sysroot
    CC_ARM="$SDK/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
    echo "ARM: the Miyoo toolchain's gcc ($SDK)"
else
    CC_ARM="arm-linux-gnueabihf-gcc -static"; QEMU=qemu-arm
    echo "ARM: arm-linux-gnueabihf-gcc"
fi
hash_of() { echo "$1" | sed 's/.*hash \([0-9a-f]*\).*/\1/'; }
$CC_ARM $ARMF $FLAGS -I$P/hh/gfx -o $O/lr_arm $R/tools/lerprows_test.c -lm
gcc $X86 $FLAGS -I$P/hh/gfx -o $O/lr_x86 $R/tools/lerprows_test.c -lm
fail=0
ra=$($QEMU $O/lr_arm rand $N) || fail=1; echo "ARM $ra"
rx=$($O/lr_x86 rand $N) || fail=1; echo "x86 $rx"
[ "$(hash_of "$ra")" = "$(hash_of "$rx")" ] || { echo "  ARM and x86 differ"; fail=1; }
if [ -n "$OLD" ]; then
    git -C $R show $OLD:port/hh/gfx/gfx_soft.c > $O/old/gfx_soft.c
    $CC_ARM $ARMF $FLAGS -DLR_OLDFILE -I$O/old -I$P/hh/gfx -o $O/lr_old_arm $R/tools/lerprows_test.c -lm
    gcc $X86 $FLAGS -DLR_OLDFILE -I$O/old -I$P/hh/gfx -o $O/lr_old_x86 $R/tools/lerprows_test.c -lm
    oa=$($QEMU $O/lr_old_arm rand $N) || true; ox=$($O/lr_old_x86 rand $N) || true
    echo "ARM $OLD: $oa"; echo "x86 $OLD: $ox"
    [ "$(hash_of "$ra")" = "$(hash_of "$oa")" ] || { echo "  ARM differs from $OLD"; fail=1; }
    [ "$(hash_of "$rx")" = "$(hash_of "$ox")" ] || { echo "  x86 differs from $OLD"; fail=1; }
    for b in arm x86; do
        if [ $b = arm ]; then n1=$($QEMU $O/lr_arm scene 4); o1=$($QEMU $O/lr_old_arm scene 4); else n1=$($O/lr_x86 scene 4); o1=$($O/lr_old_x86 scene 4); fi
        echo "$b scene: $(hash_of "$n1") ($OLD: $(hash_of "$o1"))"
        [ "$(hash_of "$n1")" = "$(hash_of "$o1")" ] || { echo "  scene differs"; fail=1; }
    done
fi
if [ "$2" = mut ]; then
    # one mutation each: the tint's two directions, K's lanes, the alpha lane, the z-tested and no-z selection, the
    # alpha variant, the rgb_texa / rgb_in / fog exclusions
    set -- \
      's/vqsub_u16(S, kl), Tx)/vqsub_u16(S, kl), S)/' \
      's/cm_div255(vmul_u16(vqsub_u16(kl, S), Tx))/cm_div255(vmul_u16(vqsub_u16(kl, S), S))/' \
      's/(uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0/(uint16_t)lkr, (uint16_t)lkg, (uint16_t)lkb, 0/' \
      's/(uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0/(uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 1/' \
      's/rgb_tex \&\& rgb_in \&\& texlerp \&\& !rgb_texa;/rgb_tex \&\& rgb_in \&\& texlerp;/' \
      's/rgb_tex \&\& rgb_in \&\& texlerp \&\& !rgb_texa;/rgb_tex \&\& texlerp \&\& !rgb_texa;/' \
      's/cmt.noz = !ztest \&\& (cm_mod || cm_tint);/cmt.noz = !ztest \&\& cm_mod;/' \
      's/{ CM_VARIANTS(CM_BILIN, 0, 1, 1) }/{ CM_VARIANTS(CM_BILIN, 0, 0, 1) }/' \
      's/{ CM_VARIANTS(CM_BILIN, 2, 1, 1) }/{ CM_VARIANTS(CM_BILIN, 2, 1, 0) }/' \
      's/cmt.texonly = !ztest \&\& !cm_mod \&\& !cm_tint;/cmt.texonly = !ztest \&\& !cm_mod;/' \
      's/const bool cm_tint = gfx_soft_lerp_rows \&\& use_bilin \&\&/const bool cm_tint = gfx_soft_lerp_rows \&\&/'
    i=0; caught=0
    for m in "$@"; do
        i=$((i + 1))
        sed "$m" $P/hh/gfx/gfx_soft.c > $O/mut/gfx_soft.c
        if cmp -s $O/mut/gfx_soft.c $P/hh/gfx/gfx_soft.c; then echo "  mutation $i: did not apply ($m)"; fail=1; continue; fi
        $CC_ARM $ARMF $FLAGS -I$O/mut -I$P/hh/gfx -o $O/lr_mut $R/tools/lerprows_test.c -lm
        if r=$($QEMU $O/lr_mut rand $N); then echo "  mutation $i NOT caught: $m ($r)"; fail=1
        else caught=$((caught + 1)); echo "  mutation $i caught: $(echo "$r" | sed 's/.*rows on vs off: //')"; fi
    done
    echo "mutations: $caught of $i caught"
fi
[ $fail = 0 ] && echo "lerprows_test: PASS" || { echo "lerprows_test: FAIL"; exit 1; }
