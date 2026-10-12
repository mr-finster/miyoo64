#!/bin/sh
# The 2026-10-07 speed round's switches (SSB64_FASTROWS, SSB64_SPANVEC, SSB64_ZSKIP; tools/speed_test.c's header): build
# the harness for ARM (the Miyoo toolchain's gcc 8.3 under qemu-arm: the NEON rows, where the three live) and x86 -m32 (the
# scalar loops: the switches change nothing there, a check of the harness), then
#  - rand: every frame drawn with the switches all off, all on and each off must be equal, colour and depth; the
#    all-on hash must be the same on ARM and x86;
#  - OLD=<git rev> (or OLD=<label> OLDFILE=<file>): the same harness against that tree's gfx_soft.c must give the same hash on both;
#  - mut: each mutation of the new code (applied to a copy of gfx_soft.c) must make rand fail on ARM.
#   sh tools/speed_test.sh [frames] [mut]    MIYOO_TOOLCHAIN=<dir> (default /opt/miyoomini-toolchain), SPEED_OUT=<dir> (default /tmp/speed_test)
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
P=$R/port
N=${1:-200}
O=${SPEED_OUT:-/tmp/speed_test}; mkdir -p "$O/old" "$O/mut"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w \
 -DPORT=1 -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -DREGION_US=1 -DVERSION_US=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C \
 -DN_MICRO=1 -DHANDHELD_PORT=1 -DTARGET_HANDHELD=1 -I$R/tools -I$P/hh -I$P/hh/include -idirafter $P/decomp/include -idirafter $P/decomp/src"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -fno-PIC -DHANDHELD=1"
X86="-O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1"
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr
SR=$SDK/arm-linux-gnueabihf/sysroot
CC_ARM="$SDK/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
hash_of() { echo "$1" | sed 's/.*hash \([0-9a-f]*\).*/\1/'; }
$CC_ARM $ARMF $FLAGS -I$P/hh/gfx -o $O/sp_arm $R/tools/speed_test.c -lm
gcc $X86 $FLAGS -I$P/hh/gfx -o $O/sp_x86 $R/tools/speed_test.c -lm
fail=0
ra=$($QEMU $O/sp_arm rand $N) || fail=1; echo "ARM $ra"
rx=$($O/sp_x86 rand $N) || fail=1; echo "x86 $rx"
[ "$(hash_of "$ra")" = "$(hash_of "$rx")" ] || { echo "  ARM and x86 differ"; fail=1; }
if [ -n "$OLD" ]; then
    # OLD=<rev>, or OLD=<label> OLDFILE=<that tree's gfx_soft.c> (where git can't reach the repo, e.g. a worktree in a container)
    if [ -n "$OLDFILE" ]; then cp "$OLDFILE" $O/old/gfx_soft.c; else git -C $R show $OLD:port/hh/gfx/gfx_soft.c > $O/old/gfx_soft.c; fi
    $CC_ARM $ARMF $FLAGS -DSPEED_OLDFILE -I$O/old -I$P/hh/gfx -o $O/sp_old_arm $R/tools/speed_test.c -lm
    gcc $X86 $FLAGS -DSPEED_OLDFILE -I$O/old -I$P/hh/gfx -o $O/sp_old_x86 $R/tools/speed_test.c -lm
    oa=$($QEMU $O/sp_old_arm rand $N) || true; ox=$($O/sp_old_x86 rand $N) || true
    echo "ARM $OLD: $oa"; echo "x86 $OLD: $ox"
    [ "$(hash_of "$ra")" = "$(hash_of "$oa")" ] || { echo "  ARM differs from $OLD"; fail=1; }
    [ "$(hash_of "$rx")" = "$(hash_of "$ox")" ] || { echo "  x86 differs from $OLD"; fail=1; }
    for b in arm x86; do
        if [ $b = arm ]; then n1=$($QEMU $O/sp_arm scene 4); o1=$($QEMU $O/sp_old_arm scene 4); else n1=$($O/sp_x86 scene 4); o1=$($O/sp_old_x86 scene 4); fi
        echo "$b scene: $(hash_of "$n1") ($OLD: $(hash_of "$o1"))"
        [ "$(hash_of "$n1")" = "$(hash_of "$o1")" ] || { echo "  scene differs"; fail=1; }
    done
fi
if [ "$2" = mut ]; then
    set -- \
      's/(uz0 < uz1 ? uz0 : uz1) > (int)vget_lane_u16(m4, 0) + zbias/(uz0 > uz1 ? uz0 : uz1) > (int)vget_lane_u16(m4, 0) + zbias/' \
      's/(uz0 < uz1 ? uz0 : uz1) > (int)vget_lane_u16(m4, 0) + zbias/(uz0 < uz1 ? uz0 : uz1) > (int)vget_lane_u16(m4, 0)/' \
      's/vld1q_u16(zp + span - 8)/vld1q_u16(zp)/' \
      's/(float32x4_t){ zf, cb, cg, cr }/(float32x4_t){ zf, cr, cg, cb }/' \
      's/Bn = vmlaq_f32(B, DB, fsv)/Bn = vmlaq_f32(B, DA, fsv)/' \
      's/vget_lane_f32(nuv, 1) \/ fspan/vget_lane_f32(nuv, 0) \/ fspan/' \
      's/ma\[4\] = { 0, ~0u, ~0u, ~0u }/ma[4] = { ~0u, ~0u, ~0u, ~0u }/' \
      's/if (vget_lane_u32(g2, 0) \& vget_lane_u32(g2, 1)) break;/if (vget_lane_u32(g2, 0)) break;/' \
      's/W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx))); \/\* w_i/W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx + 1))); \/* w_i/' \
      's/TRI_EDGE(ek2, b2, w2dx)/TRI_EDGE(ek2, b2, w1dx)/' \
      's/trr.vb1 = (float32x4_t){ a1, uw1, vw1, iw1 }/trr.vb1 = (float32x4_t){ a1, uw1, vw1, iw0 }/'
    i=0; caught=0
    for m in "$@"; do
        i=$((i + 1))
        sed "$m" $P/hh/gfx/gfx_soft.c > $O/mut/gfx_soft.c
        if cmp -s $O/mut/gfx_soft.c $P/hh/gfx/gfx_soft.c; then echo "  mutation $i: did not apply ($m)"; fail=1; continue; fi
        $CC_ARM $ARMF $FLAGS -I$O/mut -I$P/hh/gfx -o $O/sp_mut $R/tools/speed_test.c -lm
        if r=$($QEMU $O/sp_mut rand $N); then echo "  mutation $i NOT caught: $m ($r)"; fail=1
        else caught=$((caught + 1)); echo "  mutation $i caught: $(echo "$r" | sed 's/.*against all off: //')"; fi
    done
    echo "mutations: $caught of $i caught"
fi
[ $fail = 0 ] && echo "speed_test: PASS" || { echo "speed_test: FAIL"; exit 1; }
