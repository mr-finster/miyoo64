#!/bin/sh
# Build tools/handheld/halfwater_test.c for ARM (the Miyoo toolchain's gcc 8.3 when it is there, else the distro's
# cross gcc) and run it under qemu-arm: the half-resolution water layer (OOT_HALFWATER, 2026-10-02, a test switch).
#   sh tools/handheld/halfwater_test.sh [frames]          the invariants (check) + the scenes' stats
#   HW_SHOTS=<dir> sh tools/handheld/halfwater_test.sh    also write the scenes' PPMs (before/after sheets: halfwater_sheets.py)
#   HW_TREE=<tree> sh tools/handheld/halfwater_test.sh    also build the harness against that tree's gfx_soft.c and compare
#                                                       the random frames' and scenes' hashes, switch off and on
# Exit 0 when every check passed. The switch-off pixels against the old renderer: raster_test.sh / water_test.sh /
# dl_test.sh / tpe_test.sh.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-200}
O=${HW_OUT:-/tmp/halfwater_test}; mkdir -p "$O"
flags() { echo "-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w \
 -include $1/src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 \
 -DOOT_REVISION=0 -DOOT_REGION=REGION_US -DLIBULTRA_VERSION=LIBULTRA_VERSION_L -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 \
 -DPLATFORM_N64=0 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE \
 -I$1/src/port/hh/gfx -I$1/kit/platform -iquote $1/include -iquote $1/include/libc -iquote $1/src -iquote $1 \
 -I$1/src/port/hh/shim -I$1/src/port/hh"; }
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DTARGET_HANDHELD=1 -DHANDHELD=1"
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
if [ -x $SDK/usr/bin/arm-linux-gnueabihf-gcc ]; then
        CC="$SDK/usr/bin/arm-linux-gnueabihf-gcc"; ST=; QEMU="qemu-arm -L $SR"
else
    CC=arm-linux-gnueabihf-gcc; ST=-static; QEMU=qemu-arm
fi
$CC $ARMF $(flags $P) $ST -o $O/hw_arm $P/tools/handheld/halfwater_test.c -lm
if [ -n "$HW_TREE" ]; then
    $CC $ARMF $(flags $HW_TREE) $ST -o $O/hw_tree $P/tools/handheld/halfwater_test.c -lm
fi
fail=0
$QEMU $O/hw_arm check 1 $N || fail=1
$QEMU $O/hw_arm check 7 $N || fail=1
D=${HW_SHOTS:-$O/shots}; mkdir -p $D
for s in water flat; do
    $QEMU $O/hw_arm scene $s $D off water waterpx || fail=1
    if [ -n "$HW_TREE" ]; then
        for v in off water; do
            a=$($QEMU $O/hw_arm scene $s $O $v | sed 's/.*hash \([0-9a-f]*\).*/\1/'); b=$($QEMU $O/hw_tree scene $s $O $v | sed 's/.*hash \([0-9a-f]*\).*/\1/')
            if [ "$a" = "$b" ]; then echo "$s $v: identical to $HW_TREE (hash $a)"; else echo "$s $v: DIFFERS from $HW_TREE ($a vs $b)"; fail=1; fi
        done
    fi
done
if [ -n "$HW_TREE" ]; then
    $QEMU $O/hw_arm hashes 3 $N > $O/hashes_a.txt; $QEMU $O/hw_tree hashes 3 $N > $O/hashes_b.txt
    if cmp -s $O/hashes_a.txt $O/hashes_b.txt; then echo "hashes: $(wc -l < $O/hashes_a.txt) random frames (off and water) identical to $HW_TREE"
    else echo "hashes: DIFFER from $HW_TREE"; diff $O/hashes_a.txt $O/hashes_b.txt | head; fail=1; fi
fi
[ $fail = 0 ] && echo "halfwater_test: PASS" || { echo "halfwater_test: FAIL"; exit 1; }
