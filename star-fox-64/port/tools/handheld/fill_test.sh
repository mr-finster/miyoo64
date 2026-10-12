#!/bin/sh
# fill_test.sh: SF64_FILLRULE (owned edges for blended draws without AA, 2026-10-03; ocarina-of-time a7c4909). Builds
# tools/handheld/fill_test.c (it includes src/port/sdl/gfx_soft.c) for ARM (the Miyoo toolchain's gcc 8.3 under qemu-arm:
# the NEON rows) and x86 -m32 (the scalar loops), and the same harness against gfx_soft.c of git $OLD (default
# sweep-2026-10-01, the tree before the rule) with only the old rule's frames. Needs no ROM.
#   sh tools/handheld/fill_test.sh [frames]    (from port/; HH_SDK=<dir> default /opt/miyoomini-toolchain, FT_OUT=<dir>)
# Pass: with the rule no pixel twice, no crack, nothing outside, the deferred backdrop's replay under an owned-edge
# draw = the direct draw, ARM hash = x86 hash; the old rule's frames (the switch off) hash the same as $OLD's on both.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-2000}
OLD=${OLD:-sweep-2026-10-01}
O=${FT_OUT:-/tmp/fill_test}; mkdir -p "$O/old"
FLAGS="-std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off \
 -fsingle-precision-constant -w -DF3DEX_GBI=1 -D_LANGUAGE_C=1 -DVERSION_US=1 -DNON_MATCHING=1 -DAVOID_UB=1 -DNDEBUG=1 \
 -D_MIPS_SZLONG=32 -DPORT_PROFILE=1 -DTARGET_HANDHELD=1 -I$P/include -I$P/include/libultra -I$P -I$P/src/port \
 -I$P/src/port/gfx -I$P/src/port/sdl -I$P/src/port/audio"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer -DHANDHELD=1"
X86="-O2 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_PC=1"
SDK=${HH_SDK:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
[ -e $SR/lib/ld-musl-armhf.so.1 ] || CC_ARM="$SDK/usr/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
git -C $P show $OLD:port/src/port/sdl/gfx_soft.c > $O/old/gfx_soft.c
$CC_ARM $ARMF $FLAGS -iquote $P/src/port/sdl -o $O/ft_arm $P/tools/handheld/fill_test.c -lm
gcc $X86 $FLAGS -iquote $P/src/port/sdl -o $O/ft_x86 $P/tools/handheld/fill_test.c -lm
$CC_ARM $ARMF $FLAGS -DFT_OLDTREE -iquote $O/old -o $O/ft_old_arm $P/tools/handheld/fill_test.c -lm
gcc $X86 $FLAGS -DFT_OLDTREE -iquote $O/old -o $O/ft_old_x86 $P/tools/handheld/fill_test.c -lm
hash() { echo "$1" | sed -n "s/.* $2 \([0-9a-f]*\).*/\1/p"; }
fail=0
for seed in 1 2 3; do
    a=$($QEMU $O/ft_arm $seed $N | tail -1) || fail=1
    x=$($O/ft_x86 $seed $N | tail -1) || fail=1
    ao=$(FT_OLDONLY=1 $QEMU $O/ft_old_arm $seed $N | tail -1)
    xo=$(FT_OLDONLY=1 $O/ft_old_x86 $seed $N | tail -1)
    echo "ARM seed $seed: $a"; echo "x86 seed $seed: $x"
    echo "ARM seed $seed, $OLD: $ao"; echo "x86 seed $seed, $OLD: $xo"
    [ "$(hash "$a" hash)" = "$(hash "$x" hash)" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
    [ "$(hash "$a" old)" = "$(hash "$ao" old)" ] || { echo "  ARM: the switch-off frames differ from $OLD"; fail=1; }
    [ "$(hash "$x" old)" = "$(hash "$xo" old)" ] || { echo "  x86: the switch-off frames differ from $OLD"; fail=1; }
done
[ $fail = 0 ] && echo "fill_test: PASS" || { echo "fill_test: FAIL"; exit 1; }
