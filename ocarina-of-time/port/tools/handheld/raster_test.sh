#!/bin/sh
# Build tools/handheld/raster_test.c for ARM (run under qemu-arm: the NEON rows) and x86 -m32 (the scalar loops) and
# compare every variant of the rasterizer's row kinds, byte for byte (2026-10-01; see the file's header).
#   sh tools/handheld/raster_test.sh [frames]   (HH_SDK=<Miyoo toolchain> picks the device compiler; else the distro's gcc)
# Exit 0 when every variant drew the same pixels and the ARM and x86 frame hashes agree.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-300}; [ "$1" = mut ] && N=${2:-150}
O=${RT_OUT:-/tmp/raster_test}; mkdir -p "$O"
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
        $SDK/usr/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/rt_arm $P/tools/handheld/raster_test.c -lm
    QEMU="qemu-arm -L $SR"
else
    arm-linux-gnueabihf-gcc $ARMF $FLAGS -static -o $O/rt_arm $P/tools/handheld/raster_test.c -lm
    QEMU=qemu-arm
fi
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS \
    -o $O/rt_x86 $P/tools/handheld/raster_test.c -lm
if [ "$1" = mut ]; then
    # Mutation runs: each perturbs one new path in a copy of gfx_soft.c (raster_test_mut.py); base against new on the
    # frames of the kind it needs must catch every one -- proof that the random frames reach it. ARM only (NEON rows).
    n=$(python3 $P/tools/handheld/raster_test_mut.py count)
    if [ -x $SDK/usr/bin/arm-linux-gnueabihf-gcc ]; then CC="$SDK/usr/bin/arm-linux-gnueabihf-gcc"; ST=; else CC=arm-linux-gnueabihf-gcc; ST=-static; fi
    export CC ST ARMF FLAGS O P
    seq ${MUT_FROM:-0} $((n - 1)) | xargs -P ${JOBS:-4} -I{} sh -c '
        M=$O/mut{}; mkdir -p $M; rm -f $M/rt_arm $M/res.txt
        python3 $P/tools/handheld/raster_test_mut.py apply {} $P/src/port/hh/gfx/gfx_soft.c $M/gfx_soft.c >$M/name.txt || { echo missing >$M/res.txt; exit 0; }
        $CC $ARMF -iquote $M $FLAGS $ST -o $M/rt_arm $P/tools/handheld/raster_test.c -lm || echo "no build" >$M/res.txt'
    seq ${MUT_FROM:-0} $((n - 1)) | xargs -P ${JOBS:-4} -I{} sh -c '
        M='"$O"'/mut{}; [ -f $M/res.txt ] && exit 0
        k=$(python3 '"$P"'/tools/handheld/raster_test_mut.py kind {})
        if env H_VARS=base,new ${k:+H_KIND=$k} '"$QEMU"' $M/rt_arm cmp 11 '"$N"' >$M/out.txt 2>&1; then echo "NOT caught" >$M/res.txt
        else echo "caught ($(grep -c differ $M/out.txt) frame diffs)" >$M/res.txt; fi'
    miss=0; i=${MUT_FROM:-0} # MUT_FROM=<i>: only the mutations from i on
    while [ $i -lt $n ]; do
        r=$(cat $O/mut$i/res.txt); echo "mut $i: $(cat $O/mut$i/name.txt): $r"
        case "$r" in caught*) ;; *) miss=1 ;; esac
        i=$((i + 1))
    done
    [ $miss = 0 ] && echo "raster_test mut: PASS (every mutation caught)" || { echo "raster_test mut: FAIL"; exit 1; }
    exit 0
fi
fail=0
# OOT_NEARW's span depth against the exact depth (2026-10-04); OOT_NEARW=0 shows the old path's misses (not a failure)
for b in "$QEMU $O/rt_arm" "$O/rt_x86"; do r=$($b nearz 1 ${NEARZ_SPANS:-1000000} | tail -1); echo "$r"; case "$r" in *" 0 differ"*) ;; *) fail=1 ;; esac; done
OOT_NEARW=0 $O/rt_x86 nearz 1 ${NEARZ_SPANS:-1000000} | tail -1
for run in "1:" "2:H_KIND=alpha" "3:H_KIND=sky" "4:H_KIND=ui" "5:H_KIND=opq" "6:H_KIND=2d" "7:H_PSHADE0=1" "8:H_ZPASS=1" "9:H_KIND=seq" "10:H_KIND=lerp" "11:H_KIND=lerp H_PSHADE0=1" \
           "12:H_EDGENOTE=1 H_KIND=alpha" "13:H_EDGENOTE=1" "14:H_EDGENOTE=1 H_KIND=lerp" \
           "15:H_NEARZ=1" "16:H_NEARZ=1 H_KIND=opq" "17:H_NEARZ=1 H_KIND=alpha" "18:H_NEARZ=1 H_KIND=lerp" "19:H_NEARZ=1 H_ZPASS=1" \
           "20:H_NEARZ=1 OOT_NEARW=0" \
           "21:H_KIND=opq H_DETC=1" "22:H_KIND=alpha H_DETC=1" "23:H_KIND=2d H_MAG=1" "24:H_KIND=seq H_DETC=1" \
           "25:H_KIND=lerp H_DETL=1" "26:H_KIND=lerp H_DETL=1 H_PSHADE0=1" \
           "27:H_KIND=alpha H_NOZ=1" "28:H_KIND=alpha H_NOZ=1 H_PSHADE0=1" "29:H_KIND=alpha H_NOZ=1 H_EDGENOTE=1" \
           "30:H_INSIDE=1" "31:H_INSIDE=1 H_KIND=lerp" "32:H_INSIDE=1 H_KIND=alpha" "33:H_INSIDE=1 H_KIND=sky" "34:H_INSIDE=1 H_KIND=seq" \
           "35:H_INSIDE=1 H_KIND=alpha H_NOZ=1" "36:H_INSIDE=1 H_KIND=alpha H_DETC=1" "37:H_INSIDE=1 H_KIND=lerp H_ZPASS=1" "38:H_INSIDE=1 H_KIND=alpha H_EDGENOTE=1"; do
    seed=${run%%:*}; envs=${run#*:}
    a=$(env $envs $QEMU $O/rt_arm cmp $seed $N | tail -1) || { fail=1; env $envs $QEMU $O/rt_arm cmp $seed $N | head -5; }
    x=$(env $envs $O/rt_x86 cmp $seed $N | tail -1) || fail=1
    echo "ARM ${envs:-mixed}: $a"; echo "x86 ${envs:-mixed}: $x"
    # (the "|| fail" above sees tail's status, not the test's: check the count itself, 2026-10-02)
    for r in "$a" "$x"; do case "$r" in *" 0 px differ"*) ;; *) echo "  pixels differ (or no result)"; fail=1 ;; esac; done
    ha=$(echo "$a" | sed 's/.*hash \([0-9a-f]*\).*/\1/'); hx=$(echo "$x" | sed 's/.*hash \([0-9a-f]*\).*/\1/')
    [ "$ha" = "$hx" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
done
# OLD=<git rev> (2026-10-04, OOT_NEARW): the same harness built against that rev's gfx_soft.c draws runs 1-14 with the
# same hashes as this tree with OOT_NEARW=0 and with it on (the standard frames' depths stay in [-1, 2] at span starts)
if [ -n "${OLD:-}" ]; then
    mkdir -p $O/old; git -C $P show $OLD:port/src/port/hh/gfx/gfx_soft.c > $O/old/gfx_soft.c
    gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 -DRT_OLDTREE \
        $(echo "$FLAGS" | sed "s|-I$P/src/port/hh/gfx|-I$O/old -I$P/src/port/hh/gfx|") -o $O/rt_x86_old $P/tools/handheld/raster_test.c -lm
    for run in "1:" "2:H_KIND=alpha" "3:H_KIND=sky" "4:H_KIND=ui" "5:H_KIND=opq" "6:H_KIND=2d" "7:H_PSHADE0=1" "8:H_ZPASS=1" "9:H_KIND=seq" "10:H_KIND=lerp" "11:H_KIND=lerp H_PSHADE0=1" \
               "12:H_EDGENOTE=1 H_KIND=alpha" "13:H_EDGENOTE=1" "14:H_EDGENOTE=1 H_KIND=lerp"; do
        seed=${run%%:*}; envs=${run#*:}
        h0=$(env $envs $O/rt_x86_old cmp $seed $N | tail -1 | sed 's/.*hash //')
        for nw in 0 1; do
            h=$(env $envs OOT_NEARW=$nw $O/rt_x86 cmp $seed $N | tail -1 | sed 's/.*hash //')
            [ "$h" = "$h0" ] || { echo "OLD $OLD run $seed (OOT_NEARW=$nw): hash $h, $OLD's $h0"; fail=1; }
        done
    done
    echo "OLD $OLD: runs 1-14 hashed against OOT_NEARW=0 and 1"
fi
[ $fail = 0 ] && echo "raster_test: PASS" || { echo "raster_test: FAIL"; exit 1; }
