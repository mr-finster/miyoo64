#!/bin/sh
# raster_test.sh: the 3D rows, variant against variant (tools/raster_test.c; 2026-10-04, the final speed sweep).
# Builds the harness (it includes port/sdl/gfx_soft.c) for ARM (Miyoo toolchain gcc 10.2, run under qemu-arm: the NEON rows,
# every variant) and x86 -m32 (the scalar loops, the desktop proxy's path), runs every kind of frame for a few seeds and
# checks: no variant differs from the defaults on ARM (colour and depth, byte for byte), and the ARM frame hash equals
# the x86 one for every run.
#   sh tools/raster_test.sh [frames]      (in the build image: ./dev.sh sh tools/raster_test.sh)
#   sh tools/raster_test.sh prof <variant> <kind> [frames]   ARM instructions per function, one variant only
#                                         (QPROF=<dir> holding the OoT port's port/tools/handheld/qprof/qlogcount.py)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/dkr_raster_test; mkdir -p "$out"
SDK=${SDK:-/opt/miyoomini-toolchain/usr}
SR="$SDK/arm-linux-gnueabihf/sysroot"
FL="-std=gnu99 -O2 -funsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off \
 -fsingle-precision-constant -D_LANGUAGE_C -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -D_FINALROM -DNDEBUG \
 -DF3DDKR_GBI -DVERSION_us_v80 -D_MIPS_SZLONG=32 -DCIC_ID=6103 -DPORT=1 -DMODERN_CC -w -DRT_LEAN=${RT_LEAN:-1} -DRT_XSPEED=${RT_XSPEED:-1} \
 -I$here/port -I$here/port/gfx -I$here/port/sdl -I$here/port/audio -I$here/port/ultra \
 -iquote $here/decomp/src -iquote $here/decomp/include -idirafter $here/decomp/include -idirafter $here/decomp/include/PR"
[ "${RT_LEAN:-1}" = 0 ] && FL=$(echo "$FL" | sed 's/-DRT_LEAN=0//')
[ "${RT_XSPEED:-1}" = 0 ] && FL=$(echo "$FL" | sed 's/-DRT_XSPEED=0//') # (a tree before the speed round of 2026-10-07)
ARMF="-march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DTARGET_HANDHELD=1 -DHANDHELD=1"
X86F="-m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1"
SRC=${RT_SRC:-$here/port/sdl}
[ "${1:-}" = build-arm-only ] && out=$2 # (raster_test_mut.sh)
"$SDK"/bin/arm-linux-gnueabihf-gcc $FL $ARMF -iquote "$SRC" -o "$out/rt_arm" "$here/tools/raster_test.c" -lm
[ "${1:-}" = build-arm-only ] && exit 0
if [ "${1:-}" = prof ]; then
  # this qemu has no TCG plugins: its block log (-d in_asm,exec,nochain) is streamed into the OoT port's qlogcount.py
  # (never written to disk); QPROF=<dir> holding qlogcount.py
  V=$2; K=$3; N=${4:-20}; Q=${QPROF:?QPROF=<dir with qlogcount.py>}
  RT_PROF=$V qemu-arm -L "$SR" -d in_asm,exec,nochain -D /dev/stderr "$out/rt_arm" 7 "$N" "$K" 2>&1 >"$out/prof_out.txt" | python3 "$Q/qlogcount.py" /dev/stdin ${FN:+"$out/rt_arm" $FN ${FNB:-8}}
  grep 'span px' "$out/prof_out.txt" || true
  exit 0
fi
gcc $FL $X86F -iquote "$SRC" -o "$out/rt_x86" "$here/tools/raster_test.c" -lm
N=${1:-60}
fail=0
for kind in 0 1 2 3 4 5 6 7; do
  for seed in 1 2; do
    a=$(qemu-arm -L "$SR" "$out/rt_arm" $seed $N $kind) || { echo "$a"; fail=1; }
    x=$("$out/rt_x86" $seed $N $kind) || fail=1
    ha=$(echo "$a" | tail -1 | sed -n 's/.*hash \([0-9a-f]*\).*/\1/p'); hx=$(echo "$x" | tail -1 | sed -n 's/.*hash \([0-9a-f]*\).*/\1/p')
    echo "ARM $(echo "$a" | tail -1)"
    [ "$ha" = "$hx" ] || { echo "  x86 hash $hx differs"; fail=1; }
  done
done
[ $fail = 0 ] && echo "raster_test: PASS" || { echo "raster_test: FAIL"; exit 1; }
