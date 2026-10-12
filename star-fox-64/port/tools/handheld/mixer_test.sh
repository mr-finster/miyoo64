#!/bin/bash
# Bit-exact test of the mixer's NEON paths against its C loops (mixer_test.c),
# in the container, from port/:
#
#   tools/handheld/mixer_test.sh [trials per kernel] [mixer.c to test]
#
# Builds the mixer twice for the handheld (NEON as shipped; PORT_MIXER_SCALAR with
# its public functions renamed ref_*), links both into mixer_test and runs it
# under qemu-arm. Exit 0 = no mismatch. Point it at an older mixer.c to see the
# test catch a known difference (round 5: sm64-port's NEON mix and resample).
set -eu
cd "$(dirname "$0")/../.."
T=${1:-200000}
SRC=${2:-src/port/audio/mixer.c}
HH=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
S=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
O=build/mixer_test
mkdir -p $O
CF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -fsingle-precision-constant \
    -ffp-contract=off -fno-strict-aliasing -fwrapv -std=gnu99 \
    -Iinclude -Iinclude/libultra -I. -Isrc/port -Isrc/port/audio \
    -DF3DEX_GBI=1 -D_LANGUAGE_C=1 -DNON_MATCHING=1 -DAVOID_UB=1 -D_MIPS_SZLONG=32 -DTARGET_HANDHELD=1 -DHANDHELD=1"
${HH}gcc $CF -c "$SRC" -o $O/mixer_neon.o
${HH}gcc $CF -DPORT_MIXER_SCALAR -c "$SRC" -o $O/mixer_ref.o
${HH}nm --defined-only $O/mixer_ref.o | awk '$2 == "T" { print $3, "ref_" $3 }' > $O/rename.txt
${HH}objcopy --redefine-syms=$O/rename.txt $O/mixer_ref.o
${HH}gcc $CF tools/handheld/mixer_test.c $O/mixer_neon.o $O/mixer_ref.o -o $O/mixer_test
qemu-arm -L $S $O/mixer_test "$T"
