#!/bin/sh
# split_test.sh: the Miyoo Mini's second core in the renderer (gfx_soft.c raster_tri, MIYOO): tools/handheld/split_test.c
# (it includes src/port/sdl/gfx_soft.c and miyoo_worker.c) built with the Miyoo toolchain and run under qemu-arm with
# its sysroot, one core (SF64_THREADS=1), the default split, and every triangle of 2+ rows split. Needs no ROM.
#   sh tools/handheld/split_test.sh [frames]    (from port/; MIYOO_TOOLCHAIN=<dir> default /opt/miyoomini-toolchain,
#                                              ST_OUT=<dir> default /tmp/split_test)
# Pass: the three runs' hashes (every frame's colour and depth) and replay counts agree, and the split runs shared
# triangles.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-300}
O=${ST_OUT:-/tmp/split_test}; mkdir -p "$O"
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SR=$TC/usr/arm-linux-gnueabihf/sysroot
FLAGS="-std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off \
 -fsingle-precision-constant -w -DF3DEX_GBI=1 -D_LANGUAGE_C=1 -DVERSION_US=1 -DNON_MATCHING=1 -DAVOID_UB=1 -DNDEBUG=1 \
 -D_MIPS_SZLONG=32 -DPORT_PROFILE=1 -DTARGET_HANDHELD=1 -DHANDHELD=1 -DTARGET_MIYOO=1 -DMIYOO=1 -D_FILE_OFFSET_BITS=64 \
 -I$P/include -I$P/include/libultra -I$P -I$P/src/port -I$P/src/port/gfx -I$P/src/port/sdl -I$P/src/port/audio"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer"
$TC/usr/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -iquote $P/src/port/sdl -o $O/split_test $P/tools/handheld/split_test.c -lm -lpthread
run() { env "$@" qemu-arm -L $SR $O/split_test 1 $N; }
fail=0
for seed in 1 2 3; do
    one=$(env SF64_THREADS=1 qemu-arm -L $SR $O/split_test $seed $N)
    def=$(qemu-arm -L $SR $O/split_test $seed $N)
    all=$(env SF64_SPLITPX=0 SF64_SPLITROWS=2 qemu-arm -L $SR $O/split_test $seed $N)
    echo "seed $seed one core:   $one"; echo "seed $seed default:    $def"; echo "seed $seed all split:  $all"
    h() { echo "$1" | sed -n 's/^hash \([0-9a-f]*\) split [0-9]* tris \([0-9]*\) live \([0-9]*\).*/\1 \2 \3/p'; }
    [ -n "$(h "$one")" ] && [ "$(h "$one")" = "$(h "$def")" ] && [ "$(h "$one")" = "$(h "$all")" ] || { echo "  DIFFER"; fail=1; }
    case "$def" in *"split 0 "*) echo "  the default run shared nothing"; fail=1;; esac
done
[ $fail = 0 ] && echo "split_test: PASS" || { echo "split_test: FAIL"; exit 1; }
