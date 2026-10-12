#!/bin/sh
# Build tools/handheld/crack_test.c for ARM (run under qemu-arm: the NEON prefilter) and x86 -m32 (the per-pixel
# prefilter) and compare crack_fill's old scan with OOT_CRACKSCAN's on random scenes (2026-10-03; see the file).
#   sh tools/handheld/crack_test.sh [scenes]        (HH_SDK=<Miyoo toolchain> picks the device compiler)
#   sh tools/handheld/crack_test.sh mut [scenes]    each mutation of the new scan must be caught (ARM)
# Exit 0 when every scene matched on both and the ARM and x86 hashes agree (the old scan's output, seed for seed).
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-3000}; [ "$1" = mut ] && N=${2:-1500}
O=${CT_OUT:-/tmp/crack_test}; mkdir -p "$O"
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
        CC=$SDK/usr/bin/arm-linux-gnueabihf-gcc; ST=; QEMU="qemu-arm -L $SR"
else
    CC=arm-linux-gnueabihf-gcc; ST=-static; QEMU=qemu-arm
fi
if [ "$1" = mut ]; then
    # one perturbation of the new scan each (in a copy of gfx_soft.c); the old scan against it must differ
    G=$P/src/port/hh/gfx/gfx_soft.c; miss=0; k=0
    for m in 's/vandq_u16(vorrq_u16(cu, cd), vorrq_u16(cl, cr))/vandq_u16(vorrq_u16(cu, cd), vdupq_n_u16(0xFFFF))/' \
             's/vandq_u16(vorrq_u16(cu, cd), vorrq_u16(cl, cr))/vandq_u16(vdupq_n_u16(0xFFFF), vorrq_u16(cl, cr))/' \
             's/vld1_u8(cv + i0), vdup_n_u8(0)/vld1_u8(cv + i0 + 1), vdup_n_u8(0)/' \
             's/const int i = i0 + k;/const int i = i0 + (k ^ 1);/' \
             's/for (; x < w - 1 \&\& nf < CF_MAX; x++) {/for (; x < w - 2 \&\& nf < CF_MAX; x++) {/' \
             's/const int xg = 1 + ((w - 2) \/ 8) \* 8;/const int xg = 1 + ((w - 2) \/ 8) * 8 + 8;/' \
             's/vld1q_u16(zb + i0 + w), ff)/vld1q_u16(zb + i0 + w + 1), ff)/'; do
        M=$O/mut$k; mkdir -p $M; sed "$m" $G > $M/gfx_soft.c
        if cmp -s $G $M/gfx_soft.c; then echo "mut $k: did not apply ($m)"; miss=1; k=$((k + 1)); continue; fi
        $CC $ARMF -iquote $M $FLAGS $ST -o $M/ct_arm $P/tools/handheld/crack_test.c -lm
        if $QEMU $M/ct_arm 1 $N >$M/out.txt 2>&1; then echo "mut $k: NOT caught ($m)"; miss=1
        else echo "mut $k: caught ($(tail -1 $M/out.txt))"; fi
        k=$((k + 1))
    done
    [ $miss = 0 ] && echo "crack_test mut: PASS (every mutation caught)" || { echo "crack_test mut: FAIL"; exit 1; }
    exit 0
fi
$CC $ARMF $FLAGS $ST -o $O/ct_arm $P/tools/handheld/crack_test.c -lm
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS \
    -o $O/ct_x86 $P/tools/handheld/crack_test.c -lm
fail=0
for seed in 1 2 3; do
    a=$($QEMU $O/ct_arm $seed $N | tail -1) || fail=1
    x=$($O/ct_x86 $seed $N | tail -1) || fail=1
    echo "ARM seed $seed: $a"; echo "x86 seed $seed: $x"
    for r in "$a" "$x"; do case "$r" in *" 0 differ"*) ;; *) echo "  scenes differ (or no result)"; fail=1 ;; esac; done
    [ "${a##*hash }" = "${x##*hash }" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
done
[ $fail = 0 ] && echo "crack_test: PASS" || { echo "crack_test: FAIL"; exit 1; }
