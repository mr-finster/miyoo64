#!/bin/sh
# Build tools/handheld/pace_test.c (the real hh_sched.c pacer on a simulated clock) and run it over a few frame-time
# spreads, without and with OOT_PACECARRY (2026-10-01, the FPS hunt). Exit 0 when every invariant held.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
O=${PT_OUT:-/tmp/pace_test}; mkdir -p "$O"
gcc -O1 -m32 -fno-pie -no-pie -std=gnu11 -fsigned-char -fwrapv -w -include $P/src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING \
    -DAVOID_UB -DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 -DOOT_REVISION=0 -DOOT_REGION=REGION_US \
    -DLIBULTRA_VERSION=LIBULTRA_VERSION_L -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 -DPLATFORM_N64=0 -DPLATFORM_GC=0 \
    -DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE -DTARGET_HANDHELD=1 -DTARGET_PC=1 \
    -I$P/src/port/hh/gfx -I$P/kit/platform -iquote $P/include -iquote $P/include/libc -iquote $P/src -iquote $P \
    -I$P/src/port/hh/shim -I$P/src/port/hh -iquote $P/src/port/psp -o $O/pace_test $P/tools/handheld/pace_test.c -lm
fail=0
# mean sd tail% tail-extra: a scene at ~46 ms with a slow tail (Kokiri-like), one at ~49 ms (Morpha-like),
# one always over (the basement), one always under (Hyrule)
for spread in "47 3 16 12" "49.5 3 20 15" "57 4 10 15" "38 3 5 10"; do
    for c in 0 8 17 25 50; do
        OOT_PACECARRY=$c $O/pace_test $spread 6000 7 || fail=1
    done
done
[ $fail = 0 ] && echo "pace_test: PASS" || { echo "pace_test: FAIL"; exit 1; }
