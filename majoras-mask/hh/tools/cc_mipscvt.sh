#!/bin/bash
# cc_mipscvt.sh <gcc> <gcc args...>: the game code's compiler. Compiles to assembly, makes every float -> unsigned
# conversion a signed one (vcvt.u32.f32/f64 -> vcvt.s32), then assembles. The N64 (IDO, MIPS trunc.w.s) converts
# a negative float to an unsigned char/short/int by wrapping its low bits (-81.6 -> (u8) 175); ARM's unsigned
# conversion saturates at 0. The game relies on the wrap in places (ActorShadow_DrawFeet's shadow alpha: the
# audit's UBSan run). Values in [0, 2^31) convert the same either way.
GCC=$1
shift
out=
args=()
asm=()
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift 2 ;;
        -c) shift ;;
        -march=* | -mtune=* | -mfpu=* | -mfloat-abi=* | -marm | -mthumb) args+=("$1"); asm+=("$1"); shift ;;
        *) args+=("$1"); shift ;;
    esac
done
[ -n "$out" ] || { echo "cc_mipscvt.sh: no -o" >&2; exit 2; }
s="$out.mipscvt.s"
"$GCC" -S "${args[@]}" -MT "$out" -o "$s" || { rm -f "$s"; exit 1; }
sed -i -E 's/\bvcvt\.u32\.f(32|64)\b/vcvt.s32.f\1/g' "$s"
"$GCC" -c "${asm[@]}" -o "$out" "$s"
rc=$?
rm -f "$s"
exit $rc
