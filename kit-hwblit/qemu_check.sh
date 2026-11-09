#!/bin/sh
# kit-hwblit/qemu_check.sh [work dir]: miyoo_hwblit.c's logic under qemu-arm against mock_mi.c (a software stand-in
# for the engine): hwblit_test with an exact scaler, a smoothing one (MOCK_FILTER=1: only the two-quarter-turn way
# may be exact) and no ROTATE_180 (MOCK_ROTATE180=0: MIRROR_BOTH instead), the panel's buffer found through
# mock_sdl.c. A game's whole path: run it the same way (LD_LIBRARY_PATH=<work>/lib LD_PRELOAD=<work>/lib/mock_sdl.so,
# <GAME>_HWBLIT=1) and compare its <GAME>_PANEL_DUMP frames with the switch off. The real engine: hwblit_test on a
# device.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-${TMPDIR:-/tmp}/hwblit_qemu}
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SYSROOT=$TC/usr/arm-linux-gnueabihf/sysroot
CC="$TC/usr/bin/arm-linux-gnueabihf-gcc"
mkdir -p "$W/lib"
"$CC" -O2 -shared -fPIC "$HERE/mock_mi.c" -o "$W/lib/libmi_sys.so"
cp "$W/lib/libmi_sys.so" "$W/lib/libmi_gfx.so"
"$CC" -O2 -shared -fPIC -I"$SYSROOT/usr/include" "$HERE/mock_sdl.c" -o "$W/lib/mock_sdl.so" -ldl
sh "$HERE/build_test.sh" "$W" >/dev/null
for env in "" "MOCK_FILTER=1" "MOCK_ROTATE180=0"; do
    echo "=== mock engine: ${env:-exact scaler}"
    env $env SDL_VIDEODRIVER=dummy LD_LIBRARY_PATH="$W/lib" LD_PRELOAD="$W/lib/mock_sdl.so" qemu-arm -L "$SYSROOT" "$W/hwblit_test" | grep -v -E '^(system|fb0):'
done
