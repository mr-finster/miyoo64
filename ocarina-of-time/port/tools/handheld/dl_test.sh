#!/bin/sh
# Build tools/handheld/dl_test.c (gfx_pc.c + gfx_cc.c + gfx_soft.c + hh_trig.c on synthetic display lists) for ARM
# (qemu-arm: the device's NEON paths) and x86 -m32 (the desktop proxy's), then compare the front end's old and new
# paths byte for byte (2026-10-01, the FPS hunt; see the file's header).
#   sh tools/handheld/dl_test.sh [loads] [frames]
# Exit 0 when every comparison matched.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
NV=${1:-20000}
NF=${2:-60}
O=${DT_OUT:-/tmp/dl_test}; mkdir -p "$O"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w \
 -fno-builtin-sinf -fno-builtin-cosf \
 -include $P/src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 \
 -DOOT_REVISION=0 -DOOT_REGION=REGION_US -DLIBULTRA_VERSION=LIBULTRA_VERSION_L -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 \
 -DPLATFORM_N64=0 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE \
 -I$P/src/port/hh/gfx -I$P/kit/platform -iquote $P/include -iquote $P/include/libc -iquote $P/src -iquote $P \
 -I$P/src/port/hh/shim -I$P/src/port/hh"
SRCS="$P/tools/handheld/dl_test.c $P/src/port/hh/gfx/gfx_cc.c $P/src/port/hh/gfx/gfx_soft.c $P/src/port/hh/hh_trig.c"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DTARGET_HANDHELD=1 -DHANDHELD=1"
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
if [ -x $SDK/usr/bin/arm-linux-gnueabihf-gcc ]; then
        $SDK/usr/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/dt_arm $SRCS -lm -ldl
    QEMU="qemu-arm -L $SR"
else
    arm-linux-gnueabihf-gcc $ARMF $FLAGS -static -o $O/dt_arm $SRCS -lm -ldl
    QEMU=qemu-arm
fi
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS \
    -o $O/dt_x86 $SRCS -lm -ldl
fail=0
for s in 1 2 3; do
    $QEMU $O/dt_arm vtx $s $NV || fail=1
    $O/dt_x86 vtx $s $NV || fail=1
done
$QEMU $O/dt_arm vc 1 $NV || fail=1   # the vertex combiner, scalar vs NEON (OOT_VCNEON)
$QEMU $O/dt_arm flags 1 1000000 || fail=1   # the clip flags, float vs integer keys (OOT_VTXFLAGS)
$O/dt_x86 flags 1 1000000 || fail=1
$O/dt_x86 vc 1 1 || fail=1           # (x86: the division identity only)
# the near-eye clip against the N64's NoN rule (OOT_NEARW, 2026-10-04): 0 points beyond W_MIN may differ, ARM = x86;
# with OOT_NEARW=0 the points the old w >= 2 cut lost are counted (reported, not a failure)
for s in 1 2; do
    a=$($QEMU $O/dt_arm nearw $s ${NEARW_TRIS:-200000} | tail -1); x=$($O/dt_x86 nearw $s ${NEARW_TRIS:-200000} | tail -1)
    echo "$a"; case "$a" in *" 0 beyond"*) ;; *) fail=1 ;; esac
    [ "$a" = "$x" ] || { echo "  x86: $x"; fail=1; }
    OOT_NEARW=0 $O/dt_x86 nearw $s ${NEARW_TRIS:-200000} | tail -1
done
# whole frames: each switch off against on, per architecture (the frame hashes of a run, compared)
for sw in ${DT_SWITCHES:-OOT_VTXFAST OOT_VCNEON OOT_VTXFLAGS}; do
    for s in 11 12; do
        for arch in arm x86; do
            if [ $arch = arm ]; then R="$QEMU $O/dt_arm"; else R="$O/dt_x86"; fi
            env $sw=0 $R dl $s $NF > $O/dl_${sw}_0_${arch}_$s.txt
            env $sw=1 $R dl $s $NF > $O/dl_${sw}_1_${arch}_$s.txt
            if cmp -s $O/dl_${sw}_0_${arch}_$s.txt $O/dl_${sw}_1_${arch}_$s.txt; then
                echo "dl $sw $arch seed $s: $NF frames identical ($(awk '{t+=$6} END {print t}' $O/dl_${sw}_1_${arch}_$s.txt) triangles drawn)"
            else
                echo "dl $sw $arch seed $s: DIFFERENT"; diff $O/dl_${sw}_0_${arch}_$s.txt $O/dl_${sw}_1_${arch}_$s.txt | head -5; fail=1
            fi
        done
    done
done
# 2026-10-02: shared floor edges near the eye (seam: OOT_CRACKFILL) and cut-out rims (edge: OOT_EDGETINT) through the
# whole pipeline -- ARM's NEON rows against x86's scalar loops, and no hole left on a floor of shared edges
for sky in 0 1; do
    a=$(SEAM_SKY=$sky $QEMU $O/dt_arm seam 3 300 | tail -1); x=$(SEAM_SKY=$sky $O/dt_x86 seam 3 300 | tail -1)
    echo "seam ARM: $a"; [ "$a" = "$x" ] || { echo "seam x86: $x (differs)"; fail=1; }
    case "$a" in *"0 with holes"*) ;; *) echo "  holes left on shared edges"; fail=1 ;; esac
done
a=$(SEAM_TJ=1 $QEMU $O/dt_arm seam 5 100 | tail -1); x=$(SEAM_TJ=1 $O/dt_x86 seam 5 100 | tail -1)
echo "seam T-junctions ARM: $a"; [ "$a" = "$x" ] || { echo "seam T-junctions x86: $x (differs)"; fail=1; }
mkdir -p $O/edge_arm $O/edge_x86
$QEMU $O/dt_arm edge $O/edge_arm >/dev/null; $O/dt_x86 edge $O/edge_x86 >/dev/null
for f in edge_0 edge_1 n64_c n64_a n64_g; do cmp -s $O/edge_arm/$f.ppm $O/edge_x86/$f.ppm || { echo "edge $f: ARM and x86 differ"; fail=1; }; done
echo "edge: ARM and x86 views compared"
[ $fail = 0 ] && echo "dl_test: PASS" || { echo "dl_test: FAIL"; exit 1; }
