#!/bin/sh
# spancvt_test.sh [step]: MK64_SPANCVT's conversions against the old float clamps, every <step>th float bit pattern
# plus the bounds' neighbourhoods (step 1 = all 2^32). ARM only (the switch is ARM only). From base-psp/, in the image.
set -e
ulimit -c 0
B=$(cd "$(dirname "$0")/../.." && pwd)
O=${SC_OUT:-/tmp/spancvt_test}; mkdir -p "$O"
FLAGS="-std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-toplevel-reorder -fno-math-errno \
 -ffp-contract=off -fsingle-precision-constant -w -DPORT_DRAW_DIST=3000 -DPORT_MAX_VOICES=18 -DVERSION_US=1 \
 -DPORT_PROFILE=1 -DF3DEX_GBI=1 -DF3D_OLD=1 -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C=1 -DNO_SEGMENTED_MEMORY=1 \
 -DPORT_TEX32=2 -DTARGET_HANDHELD=1 -I$B/include -I$B/src -I$B/src/port -I$B/src/port/gfx -I$B/src/port/audio \
 -I$B/src/port/sdl -I$B"
ARMF="-O3 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer -DHANDHELD=1"
SDK=${HH_SDK:-/opt/miyoomini-toolchain/usr}
SR=$SDK/arm-linux-gnueabihf/sysroot
$SDK/bin/arm-linux-gnueabihf-gcc $ARMF $FLAGS -o $O/sc_arm $B/tools/handheld/spancvt_test.c -lm -Wl,--unresolved-symbols=ignore-all
qemu-arm -L $SR $O/sc_arm ${1:-13}
