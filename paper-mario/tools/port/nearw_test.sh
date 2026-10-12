#!/bin/sh
# nearw_test.sh: PM64_NEARW (the eye's clip at w >= 1/64 with no near plane, and the span depth that needs; 2026-10-04,
# goldeneye-007 c20b9ee). Builds tools/port/nearw_test.c (it includes bench/kit/gfx_pc.c) and tools/port/fps_test.c
# (it includes bench/kit/gfx_soft.c) for ARM (the Miyoo toolchain's gcc 8.3 under qemu-arm: the NEON rows) and x86 -m32
# (the scalar loops), with the bench's flags. Needs no ROM; the decomp headers at refs/papermario/include.
#   sh tools/port/nearw_test.sh    (in the build image: ./dev.sh sh tools/port/nearw_test.sh; MIYOO_TOOLCHAIN, NW_OUT)
# Pass: fps_test nearz 0 px more than 1 from the exact depth (ARM and x86); fps_test matrix with near-eye depths
# (FPS_NEARZ=1) ARM hash = x86 hash with PM64_NEARW on; nearw_test nearw 0 points beyond W_MIN against the N64's NoN
# rule, ARM = x86. The switch-off identity with the tree before is tools/port/fill_test.sh's (run it with PM64_NEARW=0).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${NW_OUT:-/tmp/nearw_test}; mkdir -p "$O"
DEF="-DTARGET_HANDHELD=1 -DPM64=1 -DPM64_REPLAY=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DPORT_NO_FPS=1 -DNON_MATCHING=1 -DAVOID_UB=1"
COMMON="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common $DEF -I$R/bench/kit -I$R/refs/papermario/include -I$R/tools/port -w"
ARMF="-march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fsingle-precision-constant -O3 -fno-math-errno -fomit-frame-pointer -ffp-contract=off"
X86="-m32 -fno-pie -no-pie -malign-data=abi -fsingle-precision-constant -O2 -fno-math-errno -msse2 -mfpmath=sse -ffp-contract=off"
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr
SR=$SDK/arm-linux-gnueabihf/sysroot
CC_ARM="$SDK/bin/arm-linux-gnueabihf-gcc"; QEMU="qemu-arm -L $SR"
U="-Wl,--unresolved-symbols=ignore-all"
$CC_ARM $ARMF $COMMON -iquote $R/bench/kit -o $O/nw_arm $R/tools/port/nearw_test.c -lm $U
gcc $X86 $COMMON -iquote $R/bench/kit -o $O/nw_x86 $R/tools/port/nearw_test.c -lm $U
$CC_ARM $ARMF $COMMON -iquote $R/bench/kit -o $O/fps_arm $R/tools/port/fps_test.c -lm
gcc $X86 $COMMON -iquote $R/bench/kit -o $O/fps_x86 $R/tools/port/fps_test.c -lm
hash() { echo "$1" | sed -n "s/.* $2 \([0-9a-f]*\).*/\1/p"; }
fail=0
for b in "$QEMU $O/fps_arm" "$O/fps_x86"; do r=$($b nearz 1 ${NEARZ_SPANS:-1000000} | tail -1); echo "$r"; case "$r" in *" 0 differ"*) ;; *) fail=1 ;; esac; done
PM64_NEARW=0 $O/fps_x86 nearz 1 ${NEARZ_SPANS:-1000000} | tail -1
a=$(FPS_NEARZ=1 $QEMU $O/fps_arm matrix 3 | tail -1); x=$(FPS_NEARZ=1 $O/fps_x86 matrix 3 | tail -1)
echo "matrix, near-eye depths, ARM: $a"; echo "matrix, near-eye depths, x86: $x"
[ "$(hash "$a" hash)" = "$(hash "$x" hash)" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
for s in 1 2; do
    a=$($QEMU $O/nw_arm nearw $s ${NW_TRIS:-200000} | tail -1) || fail=1
    x=$($O/nw_x86 nearw $s ${NW_TRIS:-200000} | tail -1) || fail=1
    echo "$a"; [ "$a" = "$x" ] || { echo "  x86: $x (differs)"; fail=1; }
    PM64_NEARW=0 $O/nw_x86 nearw $s ${NW_TRIS:-200000} | tail -1
done
[ $fail = 0 ] && echo "nearw_test: PASS" || { echo "nearw_test: FAIL"; exit 1; }
