#!/bin/sh
# mixer_test.sh [runs]: NEON audio kernels vs the C loops under qemu-arm, then
# the same with planted mutations, which must be caught.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDK:-/opt/miyoomini-toolchain/usr}
CC=$(ls "$SDK"/bin/arm-linux-gnueabihf-gcc)
SYSROOT="$SDK/arm-linux-gnueabihf/sysroot"
out=${TMPDIR:-/tmp}/dkr_mixer_test
FLAGS="-O2 -std=gnu99 -fwrapv -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"  # the port's MARCH
$CC $FLAGS -o "$out" "$here/tools/mixer_test.c"
$CC $FLAGS -DACMD_MUTATE -o "$out.mut" "$here/tools/mixer_test.c"
echo "== kernels"; qemu-arm -L "$SYSROOT" "$out" "${1:-200000}"
echo "== planted mutations (must report mismatches)"
if qemu-arm -L "$SYSROOT" "$out.mut" "${1:-200000}"; then echo "MUTATION NOT CAUGHT"; exit 1; fi
echo "mutations caught"
