#!/usr/bin/env bash
# The kit's checks, run in a port's build image (default sm64hh:build):
#  1. save-state tests on x86 -- the hole layout with and without a backstop swap,
#     and the include list -- built with HANDHELD so the player-facing paths run;
#  2. every platform file compiled for the device (the Miyoo toolchain's gcc, HANDHELD)
#     and for the desktop, once per port config in ports/, with -Wall -Wextra.
# The kit is mounted read-only; output goes to a temp dir in the container.
set -euo pipefail
KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-sm64hh:build}"
exec docker run --rm --platform linux/amd64 -v "$KIT":/kit:ro "$IMAGE" bash -c '
set -uo pipefail
cd /kit
out=$(mktemp -d)
fail=0
W="-std=gnu99 -O1 -g -Wall -Wextra"

mkdir -p "$out/hole" "$out/list"
printf "%s\n" "#define HH_GAME_ID \"test\"" "#define HH_ENV(n) \"TEST_\" n" "#define HH_SS_MAGIC 0x54535453u" > "$out/hole/hh_game.h"
cp "$out/hole/hh_game.h" "$out/list/hh_game.h"
echo "#define HH_SS_INCLUDE_LIST 1" >> "$out/list/hh_game.h"

run() { # name, extra flags...
    local name=$1; shift
    if gcc $W -DHANDHELD -DTEST_NAME="\"$name\"" "$@" -Iplatform tests/ss_test.c platform/savestate.c -o "$out/$name" 2> "$out/$name.log"; then
        [ -s "$out/$name.log" ] && { echo "$name: warnings"; cat "$out/$name.log"; fail=1; }
        "$out/$name" || fail=1
    else
        echo "$name: build failed"; cat "$out/$name.log"; fail=1
    fi
}
run ss_hole      -I"$out/hole"
run ss_hole_swap -I"$out/hole" -DTEST_BACKSTOP_SWAP
mkdir -p "$out/sparse" && cp "$out/hole/hh_game.h" "$out/sparse/hh_game.h" && echo "#define HH_SS_SPARSE 1" >> "$out/sparse/hh_game.h"
run ss_hole_sparse      -I"$out/sparse"
run ss_hole_sparse_swap -I"$out/sparse" -DTEST_BACKSTOP_SWAP
run ss_list_swap -I"$out/list" -DTEST_BACKSTOP_SWAP \
    -Wl,--defsym=__gamedata_start=__start_game_data,--defsym=__gamedata_end=__stop_game_data \
    -Wl,--defsym=__gamebss_start=__start_game_bss,--defsym=__gamebss_end=__stop_game_bss
run ss_hole_hooks -I"$out/hole" -DTEST_HOOKS
run ss_list_hooks -I"$out/list" -DTEST_BACKSTOP_SWAP -DTEST_HOOKS \
    -Wl,--defsym=__gamedata_start=__start_game_data,--defsym=__gamedata_end=__stop_game_data \
    -Wl,--defsym=__gamebss_start=__start_game_bss,--defsym=__gamebss_end=__stop_game_bss

# k1.8: hh_declick.h (fades, priming, flush), desktop and device compilers
if gcc $W -Iplatform tests/declick_test.c -lm -o "$out/declick" 2> "$out/declick.log" && [ ! -s "$out/declick.log" ]; then
    "$out/declick" || fail=1
else
    echo "declick: build failed or warned"; cat "$out/declick.log"; fail=1
fi
arm-linux-gnueabihf-gcc $W -O2 -Iplatform -c tests/declick_test.c -o "$out/declick.arm.o" 2> "$out/declick.arm.log" \
    && [ ! -s "$out/declick.arm.log" ] || { echo "declick: device build failed or warned"; cat "$out/declick.arm.log"; fail=1; }

for p in ports/*/; do
    p=${p%/}; id=${p#ports/}
    for f in platform/*.c; do
        o="$out/${id}_$(basename "$f" .c)"
        arm-linux-gnueabihf-gcc $W -O2 -DHANDHELD -I"$p" -Iplatform -c "$f" -o "$o.arm.o" 2> "$o.arm.log" || { echo "$id $f: device build failed"; fail=1; }
        [ -s "$o.arm.log" ] && { echo "$id $f: device warnings"; cat "$o.arm.log"; fail=1; }
        gcc $W -I"$p" -Iplatform -c "$f" -o "$o.pc.o" 2> "$o.pc.log" || { echo "$id $f: desktop build failed"; fail=1; }
        [ -s "$o.pc.log" ] && { echo "$id $f: desktop warnings"; cat "$o.pc.log"; fail=1; }
    done
    echo "$id: compiled (device + desktop)"
done
[ $fail -eq 0 ] && echo "ALL OK" || echo "SOMETHING FAILED"
exit $fail
'
