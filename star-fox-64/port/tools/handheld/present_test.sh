#!/bin/sh
# present_test.sh [old gfx_sdl12.c] [new gfx_sdl12.c]: the present's conversion (round 2026-10-03, SF64_PRESFAST),
# old against new, bit for bit: x86 (scalar), and the Miyoo toolchain's gcc (HH_SDK=<sdk dir>, else arm-linux-gnueabihf-gcc)
# under qemu-arm with NEON, the new file both with SF64_PRESFAST=1 and =0. Default old: git HEAD's file (then it
# checks on = off); for the 2026-10-03 change pass the file before it: git show 21803b5:port/src/port/sdl/gfx_sdl12.c.
# PT_COUNT=<n>: also qemu instruction counts for n presents (piped through qcount.py).
# Needs no ROM. Exit 0 = identical everywhere.
set -e
here=$(cd "$(dirname "$0")" && pwd); port=$(cd "$here/../.." && pwd)
new=${2:-$port/src/port/sdl/gfx_sdl12.c}
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
if [ -n "$1" ]; then cp "$1" "$w/old.c"; else git -C "$port" show HEAD:port/src/port/sdl/gfx_sdl12.c > "$w/old.c"; fi
cp "$new" "$w/new.c"
sed -n '/^static unsigned char sGamma8/,/^void osViSwapBuffer/p' "$port/src/port/ultra_shim.c" | sed '$d' > "$w/pt_gamma.inc"
for v in old new; do
    mkdir -p "$w/$v"; cp "$w/pt_gamma.inc" "$w/$v/"
    sed -n '/^\/\* ARGB8888 -> RGB565 for one contiguous row/,/^\/\* ARGB8888 (render res) -> RGB565 staging/p' "$w/$v.c" | sed '$d' > "$w/$v/pt_rows.inc"
    sed -n '/^\/\* Border fill/,/^\/\* Take the screen for direct drawing/p' "$w/$v.c" | sed '$d' > "$w/$v/pt_present.inc"
    [ -s "$w/$v/pt_rows.inc" ] && [ -s "$w/$v/pt_present.inc" ] || { echo "present_test: can't find the present code in $v"; exit 1; }
done
if [ -n "$HH_SDK" ]; then ACC="$HH_SDK/usr/bin/arm-linux-gnueabihf-gcc"; SR="$HH_SDK/usr/arm-linux-gnueabihf/sysroot"
else ACC=arm-linux-gnueabihf-gcc; SR=/usr/arm-linux-gnueabihf; fi
AFL="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fsingle-precision-constant -fno-math-errno -fomit-frame-pointer -ffp-contract=off -std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -static"
XFL="-O2 -fno-math-errno -ffp-contract=off -std=gnu99 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common"
for v in old new; do
    gcc $XFL -I"$w/$v" -o "$w/x86_$v" "$here/present_test.c" -lm
    BR_NO_CCACHE=1 $ACC $AFL -I"$w/$v" -o "$w/arm_$v" "$here/present_test.c" -lm
done
rc=0
run() { "$@" > "$w/out" || { echo "FAIL: $* exited non-zero"; tail -1 "$w/out"; rc=1; }; cat "$w/out"; }
run "$w/x86_old" > "$w/x_old"; run "$w/x86_new" > "$w/x_new"
run qemu-arm -L "$SR" "$w/arm_old" > "$w/a_old"
SF64_PRESFAST=1 run qemu-arm -L "$SR" "$w/arm_new" > "$w/a_new1"
SF64_PRESFAST=0 run qemu-arm -L "$SR" "$w/arm_new" > "$w/a_new0"
n=$(grep -c '^case' "$w/a_old")
for p in "x_old x_new" "a_old a_new1" "a_old a_new0"; do
    set -- $p
    if cmp -s "$w/$1" "$w/$2"; then echo "$1 = $2: $n cases identical; $(tail -1 "$w/$2")"
    else echo "$1 != $2:"; diff "$w/$1" "$w/$2" | head; rc=1; fi
done
# x86 (scalar) = ARM where the arithmetic is the same: the scalar ARM cases (neon 0)
grep 'neon 0' "$w/x_new" > "$w/xs"; grep 'neon 0' "$w/a_new1" > "$w/as"
if cmp -s "$w/xs" "$w/as"; then echo "x86 = ARM on the $(wc -l < "$w/xs") scalar cases"; else echo "x86 != ARM (scalar cases)"; rc=1; fi
grep 'neon 1' "$w/x_new" | sed 's/neon 1/neon 0/' > "$w/xn"; grep 'neon 1' "$w/a_new1" | sed 's/neon 1/neon 0/' > "$w/an"
if cmp -s "$w/xn" "$w/an"; then echo "x86 scalar = ARM NEON on the $(wc -l < "$w/xn") NEON cases"; else echo "note: ARM NEON cases differ from x86 scalar (expected only where the NEON path itself differs from scalar)"; fi
if [ -n "$PT_COUNT" ]; then # qemu instructions for PT_COUNT presents of the default frame (a hint only), piped, never on disk
    qc=$port/tools/handheld/qcount.py
    for v in "old -" "new 1" "new 0"; do set -- $v
        n=$(PT_BENCH=$PT_COUNT SF64_PRESFAST=$2 qemu-arm -L "$SR" -d in_asm,exec,nochain "$w/arm_$1" 2>&1 >/dev/null | python3 "$qc")
        echo "qemu instructions, $PT_COUNT presents, $1 (presfast $2): $n"
    done
fi
[ $rc = 0 ] && echo "present_test: PASS" || echo "present_test: FAIL"
exit $rc
