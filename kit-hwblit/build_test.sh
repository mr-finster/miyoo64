#!/bin/sh
# kit-hwblit/build_test.sh [out dir]: build hwblit_test (the device check of the 2D engine) with the union toolchain
# (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain), from this folder's miyoo_hwblit.c, the file the games build.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE}
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SYSROOT=$TC/usr/arm-linux-gnueabihf/sysroot
"$TC/usr/bin/arm-linux-gnueabihf-gcc" -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DMIYOO_HWBLIT_TEST \
    -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -I"$HERE" -I"$SYSROOT/usr/include" "$HERE/hwblit_test.c" "$HERE/miyoo_hwblit.c" \
    -L"$SYSROOT/usr/lib" -lSDL -ldl -lpthread -o "$OUT/hwblit_test"
"$TC/usr/bin/arm-linux-gnueabihf-strip" --strip-all --remove-section=.comment "$OUT/hwblit_test"
echo "$OUT/hwblit_test"
