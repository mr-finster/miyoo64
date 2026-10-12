#!/bin/sh
# fill_test.sh: DKR_FILLRULE (owned edges for blended draws without AA, 2026-10-03; the OoT port a7c4909). Builds
# tools/fill_test.c (it includes port/sdl/gfx_soft.c) for ARM (Miyoo toolchain gcc, run under qemu-arm: the NEON rows) and
# x86 -m32 (the scalar loops), and the same harness against gfx_soft.c of git $BASE (default sweep-2026-10-01, the tree
# before the rule) with only the old rule's frames.
#   sh tools/fill_test.sh [frames]      (in the build image: ./dev.sh sh tools/fill_test.sh)
# Pass: with the rule no pixel twice, no crack, nothing outside, on ARM and x86 alike (the same hash); the old rule's
# frames (the switch off) hash the same as the tree before, on both.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
N=${1:-2000}
BASE=${BASE:-sweep-2026-10-01}
out=${TMPDIR:-/tmp}/dkr_fill_test; mkdir -p "$out/old"
git -C "$here" show "$BASE:port/sdl/gfx_soft.c" > "$out/old/gfx_soft.c"
SDK=${SDK:-/opt/miyoomini-toolchain/usr}
SR="$SDK/arm-linux-gnueabihf/sysroot"
FL="-std=gnu99 -O2 -funsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off \
 -fsingle-precision-constant -D_LANGUAGE_C -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -D_FINALROM -DNDEBUG \
 -DF3DDKR_GBI -DVERSION_us_v80 -D_MIPS_SZLONG=32 -DCIC_ID=6103 -DPORT=1 -DMODERN_CC -w \
 -I$here/port -I$here/port/gfx -I$here/port/sdl -I$here/port/audio -I$here/port/ultra \
 -iquote $here/decomp/src -iquote $here/decomp/include -idirafter $here/decomp/include -idirafter $here/decomp/include/PR"
ARMF="-march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DTARGET_HANDHELD=1 -DHANDHELD=1"
X86F="-m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1"
for v in new old; do
  if [ $v = new ]; then S="-iquote $here/port/sdl"; D=""; else S="-iquote $out/old"; D="-DFT_OLDTREE"; fi
  "$SDK"/bin/arm-linux-gnueabihf-gcc $FL $ARMF $D $S -o "$out/ft_arm_$v" "$here/tools/fill_test.c" -lm
  gcc $FL $X86F $D $S -o "$out/ft_x86_$v" "$here/tools/fill_test.c" -lm
done
fail=0
hash() { echo "$1" | sed -n "s/.* $2 \([0-9a-f]*\).*/\1/p"; }
for seed in 1 2 3; do
  a=$(qemu-arm -L "$SR" "$out/ft_arm_new" $seed $N | tail -1) || fail=1
  x=$("$out/ft_x86_new" $seed $N | tail -1) || fail=1
  ao=$(FT_OLDONLY=1 qemu-arm -L "$SR" "$out/ft_arm_old" $seed $N | tail -1)
  xo=$(FT_OLDONLY=1 "$out/ft_x86_old" $seed $N | tail -1)
  echo "ARM seed $seed: $a"; echo "x86 seed $seed: $x"
  echo "ARM seed $seed, $BASE: $ao"; echo "x86 seed $seed, $BASE: $xo"
  [ "$(hash "$a" hash)" = "$(hash "$x" hash)" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
  [ "$(hash "$a" old)" = "$(hash "$ao" old)" ] || { echo "  ARM: the switch-off frames differ from $BASE"; fail=1; }
  [ "$(hash "$x" old)" = "$(hash "$xo" old)" ] || { echo "  x86: the switch-off frames differ from $BASE"; fail=1; }
done
[ $fail = 0 ] && echo "fill_test: PASS" || { echo "fill_test: FAIL"; exit 1; }
