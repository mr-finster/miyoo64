#!/bin/sh
# vtx_test.sh: MK64_VTXFAST (2026-10-04, the final speed sweep) through the real renderer.
# Builds tools/handheld/vtx_test.c (it includes src/port/gfx/gfx_pc.c and links src/port/sdl/gfx_soft.c and
# src/port/gfx/gfx_cc.c) with Makefile.handheld's flags for ARM (the Miyoo toolchain's gcc, run under qemu-arm) and x86
# -m32, from this tree and from $OLD (the tree before the switches). No ROM.
#   sh tools/handheld/vtx_test.sh [frames]    (from base-psp/, in the build image; HH_SDK, VT_OUT, OLD, VT_OLDDIR)
# Pass: for every seed and screen, the draw hash (every float and colour handed to the rasterizer) and the frame
# hash (every finished frame's pixels) are the same with the switch off, on and ab, the same as $OLD's, and ARM =
# x86.
# Linked at 0x10000000 as the game is (Makefile.handheld): gfx_pc.c treats texels below 0x08000000 as an unresolved
# segment address and skips them.
set -e
B=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-60}
OLD=${OLD:-origin/sweep-2026-10-01}
O=${VT_OUT:-/tmp/vtx_test}; rm -rf "$O"; mkdir -p "$O/old"
FLAGS="-std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-toplevel-reorder -fno-math-errno \
 -ffp-contract=off -fsingle-precision-constant -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-sincosf -w \
 -DPORT_DRAW_DIST=3000 -DPORT_MAX_VOICES=18 -DVERSION_US=1 \
 -DPORT_PROFILE=1 -DF3DEX_GBI=1 -DF3D_OLD=1 -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1 -DNO_SEGMENTED_MEMORY=1 \
 -DPORT_TEX32=2 -DTARGET_HANDHELD=1 -I$B/include -I$B/src -I$B/src/port -I$B/src/port/gfx -I$B/src/port/audio \
 -I$B/src/port/sdl -I$B"
ARMF="-O3 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer -DHANDHELD=1"
X86="-O2 -m32 -fno-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
SR=$SDK/arm-linux-gnueabihf/sysroot
CC_ARM="$SDK/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
# $OLD's three files (VT_OLDDIR: a folder that already holds them, for a container without the repo's .git)
if [ -n "$VT_OLDDIR" ]; then cp $VT_OLDDIR/gfx_pc.c $VT_OLDDIR/gfx_cc.c $VT_OLDDIR/gfx_soft.c $O/old/
else for f in gfx/gfx_pc.c gfx/gfx_cc.c sdl/gfx_soft.c; do git -C $B show $OLD:base-psp/src/port/$f > $O/old/$(basename $f); done; fi
for v in new old; do
    if [ $v = new ]; then S=$B/src/port; I=""; else S=$O/old; I="-iquote $O/old"; fi
    sd=$S/sdl; gd=$S/gfx; [ $v = old ] && sd=$S && gd=$S
    $CC_ARM $ARMF $FLAGS -c $sd/gfx_soft.c -o $O/soft_arm_$v.o
    $CC_ARM $ARMF $FLAGS -c $gd/gfx_cc.c -o $O/cc_arm_$v.o
    gcc $X86 $FLAGS -c $sd/gfx_soft.c -o $O/soft_x86_$v.o
    gcc $X86 $FLAGS -c $gd/gfx_cc.c -o $O/cc_x86_$v.o
    $CC_ARM $ARMF $FLAGS $I -no-pie -Wl,-Ttext-segment=0x10000000 -o $O/vt_arm_$v $B/tools/handheld/vtx_test.c $O/soft_arm_$v.o $O/cc_arm_$v.o -lm
    gcc $X86 $FLAGS $I -no-pie -Wl,-Ttext-segment=0x10000000 -o $O/vt_x86_$v $B/tools/handheld/vtx_test.c $O/soft_x86_$v.o $O/cc_x86_$v.o -lm
done
hashes() { sed -n 's/.*: \(.*\)$/\1/p'; }
fail=0
for screen in "240 240" "240 240 204 204" "320 240"; do
    for seed in 1 2 3; do
        ref=$($QEMU $O/vt_arm_old $seed $N $screen)
        echo "ARM $OLD: $ref"
        want=$(echo "$ref" | hashes)
        for sw in 0 1 ab; do
            for a in arm x86; do
                if [ $a = arm ]; then r=$(MK64_VTXFAST=$sw VT_ABEVERY=7 $QEMU $O/vt_arm_new $seed $N $screen)
                else r=$(MK64_VTXFAST=$sw VT_ABEVERY=7 $O/vt_x86_new $seed $N $screen); fi
                got=$(echo "$r" | hashes)
                [ "$got" = "$want" ] && st=same || { st=DIFFERS; fail=1; }
                echo "  $a vtxfast=$sw: $got ($st)"
            done
        done
        r=$($O/vt_x86_old $seed $N $screen | hashes)
        [ "$r" = "$want" ] || { echo "  x86 $OLD differs from ARM $OLD: $r"; fail=1; }
    done
done
[ $fail = 0 ] && echo "vtx_test: PASS" || { echo "vtx_test: FAIL"; exit 1; }
