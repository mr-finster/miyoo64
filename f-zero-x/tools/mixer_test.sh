#!/bin/bash
# Bit-exact test of the mixer's NEON paths against its C loops (tools/mixer_test.c, from
# star-fox-64 323f5a6), in ./dev.sh, from the repo root:
#
#   tools/mixer_test.sh [trials per kernel] [mixer.c to test]
#
# Builds the mixer twice for the handheld (NEON as shipped; PORT_MIXER_SCALAR with its
# public functions renamed ref_*), links both into mixer_test and runs it under
# qemu-arm. Exit 0 = no mismatch. Point it at an older mixer.c to see the test catch
# the known differences (sm64-port's NEON mix, resample and ADPCM shift).
set -eu
cd "$(dirname "$0")/../game"
T=${1:-200000}
SRC=${2:-src/port/audio/mixer.c}
HH=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
S=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
O=build/mixer_test
mkdir -p $O
# the game's own flags for src/port/audio/mixer.c (make -n, device build)
CF="-O2 -fno-math-errno -ffp-contract=off -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -marm \
    -std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-stack-protector \
    -D_LANGUAGE_C -DLANGUAGE_C -D_MIPS_SZLONG=32 -DF3DEX_GBI_2 -DNDEBUG -DVERSION_US=1 -DBUILD_REVISION=REVISION_B \
    -DLANGUAGE=LANGUAGE_ENG -DOS_TV_TYPE=OS_TV_TYPE_NTSC -DBUILD_VERSION=VERSION_I -DASSET_VERSION=us \
    -DASSET_REVISION=rev0 -DNON_MATCHING -DAVOID_UB -DHANDHELD=1 -DTARGET_HANDHELD=1 -DPORT_REAL_PTR_MIN=0x10000000u \
    -DHANDHELD_DEVICE=1 -Iinclude -Ibin/us/rev0 -I. -Isrc/port -Isrc/port/audio -D_OS_LIBC_H_ -include stdint.h -D_GNU_SOURCE=1"
${HH}gcc $CF -c "$SRC" -o $O/mixer_neon.o
${HH}gcc $CF -DPORT_MIXER_SCALAR -c "$SRC" -o $O/mixer_ref.o
${HH}nm --defined-only $O/mixer_ref.o | awk '$2 == "T" { print $3, "ref_" $3 }' > $O/rename.txt
${HH}objcopy --redefine-syms=$O/rename.txt $O/mixer_ref.o
${HH}gcc $CF ../tools/mixer_test.c $O/mixer_neon.o $O/mixer_ref.o -o $O/mixer_test
${QEMU:-qemu-arm} -L $S $O/mixer_test "$T"
