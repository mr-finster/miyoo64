#!/bin/sh
# Build port/hh/tools/rast_test.c for ARM (run under qemu-arm: the NEON paths) and x86 -m32 (the scalar loops, as the desktop
# proxy) and compare every variant of the rasterizer, byte for byte (2026-10-01; see the file's header).
#   sh port/hh/tools/rast_test.sh [frames]
# The ARM compiler: $HH_SDK (a Miyoo toolchain, the device's gcc 10.2) or /opt/miyoomini-toolchain/usr, else arm-linux-gnueabihf-gcc.
# Exit 0 when every variant drew the same pixels and the ARM and x86 frame hashes agree.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-60}
O=${RT_OUT:-/tmp/rast_test}; mkdir -p "$O"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -ffp-contract=off -w -D_LANGUAGE_C \
 -DHH_PORT_LAYER=1 -DTARGET_HANDHELD=1 -DPORT_GE_RAWCC=1 -I$P/hh/include -I$P/hh/gfx -I$P/kit/platform -I$P \
 -idirafter $P/include -idirafter $P/include/ultra64 -idirafter $P/include/PR -include $P/hh/include/hh_enum.h $RT_DEFS"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DHANDHELD=1"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
if [ -x $SDK/bin/arm-linux-gnueabihf-gcc ]; then
    SR=$SDK/arm-linux-gnueabihf/sysroot
    $SDK/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/rt_arm $P/hh/tools/rast_test.c -lm
    QEMU="qemu-arm -L $SR"
    echo "ARM: the Miyoo toolchain's gcc ($SDK)"
else
    arm-linux-gnueabihf-gcc $ARMF $FLAGS -static -o $O/rt_arm $P/hh/tools/rast_test.c -lm
    QEMU=qemu-arm
    echo "ARM: arm-linux-gnueabihf-gcc"
fi
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1 $FLAGS \
    -o $O/rt_x86 $P/hh/tools/rast_test.c -lm
fail=0
$O/rt_x86 mipx | tail -1 | tee /dev/stderr | grep -q ' 0 of' || fail=1
# GE_MIPLOD's level choice against the RDP's rule (2026-10-03): the tile function exactly, the span choice per pixel
# and mip_pick_fast (GE_LODFAST) against mip_pick_n64 (2026-10-04): both "0 differ" lines
# (2026-10-05: + GE_LODLEAN's n64_lod_tiles_lean, both builds, and mip_pick_lean, ARM: every "differ" line must be 0)
for b in "$QEMU $O/rt_arm" "$O/rt_x86"; do r=$($b lod 1 ${LOD_FRAMES:-100} | tee /dev/stderr | grep 'differ$');
    [ "$(echo "$r" | grep -c ' 0 differ$')" -ge 3 ] && [ "$(echo "$r" | grep -vc ' 0 differ$')" = 0 ] || fail=1; done
# GE_OWNFAST: own_pxf / own_row against own_px (2026-10-04)
for b in "$QEMU $O/rt_arm" "$O/rt_x86"; do $b own 1 ${OWN_TRIS:-1000000} | tail -1 | tee /dev/stderr | grep -q ' 0 differ' || fail=1; done
# GE_EDGESKIP: every skipped right-hand solve is one that changes nothing (2026-10-05)
for b in "$QEMU $O/rt_arm" "$O/rt_x86"; do $b edge 1 ${EDGE_TRIS:-300000} | tail -1 | tee /dev/stderr | grep -q ' 0 wrong' || fail=1; done
for b in "$QEMU $O/rt_arm" "$O/rt_x86"; do $b 2d 1 $N | tail -1 | tee /dev/stderr | grep -q ' 0 px differ' || fail=1; done
# GE_NEARW's span depth against the exact depth (2026-10-04); GE_NEARW=0 shows the old path's misses (not a failure)
for b in "$QEMU $O/rt_arm" "$O/rt_x86"; do $b nearz 1 ${NEARZ_SPANS:-1000000} | tail -1 | tee /dev/stderr | grep -q ' 0 differ' || fail=1; done
GE_NEARW=0 $O/rt_x86 nearz 1 ${NEARZ_SPANS:-1000000} | tail -1
o=$($QEMU $O/rt_arm ops | tail -1)
case "$o" in ops:*) echo "$o"; case "$o" in *" 0 differ"*) ;; *) fail=1 ;; esac ;; esac
for run in "1:" "2:R_KIND=0" "3:R_KIND=1" "4:R_KIND=2" "5:R_KIND=4" "6:R_PSHADE0=1" "7:R_TRILIN0=1" "8:R_ZPASS=1 R_KIND=0" "9:R_ZPASS=1 R_KIND=1" "10:R_T16=1" "11:R_T16=1 R_KIND=0" "12:R_KIND=5" "13:R_DEFER=1" "14:R_DEFER=1 R_KIND=4" "15:R_DEFER=1 R_KIND=5" "16:R_KIND=6" "17:R_KIND=6 R_ZPASS=1" "18:R_KIND=7" "19:R_KIND=7 R_ZPASS=1" "20:R_KIND=8" "21:R_KIND=8 R_ZPASS=1" \
           "22:R_OWN=1" "23:R_OWN=1 R_KIND=2" "24:R_OWN=1 R_KIND=6" "25:R_OWN=1 R_KIND=7" "26:R_OWN=1 R_KIND=8 R_ZPASS=1" \
           "27:R_OWN=1 R_DEFER=1 R_ZPASS=1" "28:R_OWN=1 R_DEFER=1 R_ZPASS=1 R_KIND=6" "29:R_OWN=1 R_DEFER=1" \
           "30:R_LOD=1" "31:R_LOD=1 R_KIND=0" "32:R_LOD=1 R_KIND=1" "33:R_LOD=1 R_KIND=2" "34:R_LOD=1 R_KIND=4" "35:R_LOD=1 R_KIND=6" \
           "36:R_LOD=1 R_T16=1" "37:R_LOD=1 R_MIPLOD=1" "38:R_LOD=1 R_MIPLOD=3" "39:R_LOD=1 R_MIPLOD=0" "40:R_LOD=1 R_DEFER=1" \
           "41:R_LOD=1 R_KIND=5" "42:R_LOD=1 R_ZPASS=1 R_KIND=1" "43:R_LOD=1 R_OWN=1 R_KIND=2" "44:R_LOD=1 R_TRILIN0=1" \
           "45:R_NEARZ=1" "46:R_NEARZ=1 R_KIND=0" "47:R_NEARZ=1 R_KIND=1" "48:R_NEARZ=1 R_KIND=2" "49:R_NEARZ=1 R_KIND=4" \
           "50:R_NEARZ=1 R_LOD=1" "51:R_NEARZ=1 GE_NEARW=0" \
           "52:R_CLAMP=1" "53:R_CLAMP=1 R_KIND=0" "54:R_CLAMP=1 R_KIND=1" "55:R_CLAMP=1 R_KIND=2" "56:R_CLAMP=1 R_KIND=6" \
           "57:R_CLAMP=1 R_LOD=1" "58:R_CLAMP=1 R_LOD=1 R_KIND=0" "59:R_CLAMP=1 R_LOD=1 R_KIND=1" "60:R_CLAMP=1 R_LOD=1 R_KIND=2" \
           "61:R_CLAMP=1 R_LOD=1 R_TRILIN0=1" "62:R_CLAMP=1 R_LOD=1 R_MIPLOD=0" "63:R_CLAMP=1 R_T16=1 R_KIND=0" \
           "64:R_CLAMP=1 R_ZPASS=1 R_KIND=1" "65:R_CLAMP=1 R_LOD=1 R_KIND=5" \
           "66:R_ZPLANE=1" "67:R_ZPLANE=1 R_KIND=0" "68:R_ZPLANE=1 R_KIND=1" "69:R_ZPLANE=1 R_KIND=2" "70:R_ZPLANE=1 R_KIND=5" \
           "71:R_ZPLANE=1 R_LOD=1" "72:R_ZPLANE=1 R_NEARZ=1" "73:R_ZPLANE=1 R_OWN=1 R_KIND=2" "74:R_ZPLANE=1 R_CLAMP=1 R_LOD=1" \
           "75:R_ZPLANE=1 R_KIND=4" "76:R_ZPLANE=1 R_DEFER=1" "77:R_ZPLANE=1 R_KIND=6" "78:R_ZPLANE=1 R_T16=1"; do
    seed=${run%%:*}; envs=${run#*:}
    a=$(env $envs $QEMU $O/rt_arm cmp $seed $N | tail -1) || fail=1
    x=$(env $envs $O/rt_x86 cmp $seed $N | tail -1) || fail=1
    echo "ARM ${envs:-mixed}: $a"; echo "x86 ${envs:-mixed}: $x"
    # (the "|| fail" above sees tail's status, not the test's: check the count itself, 2026-10-02)
    for r in "$a" "$x"; do case "$r" in *" 0 px differ"*) ;; *) echo "  pixels differ (or no result)"; fail=1 ;; esac; done
    ha=$(echo "$a" | sed 's/.*hash \([0-9a-f]*\).*/\1/'); hx=$(echo "$x" | sed 's/.*hash \([0-9a-f]*\).*/\1/')
    [ "$ha" = "$hx" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
done
[ $fail = 0 ] && echo "rast_test: PASS" || { echo "rast_test: FAIL"; exit 1; }
