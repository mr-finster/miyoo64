#!/bin/sh
# Build src/game/vtxstore.c with the game's flags (x86 -m32, as PLAT=pc) and hh/tools/vtxstore_test.c, and run it.
# OLD=<git rev>: also that tree's vtxstore.c (expected to FAIL before the halfword fix).
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
O=${VS_OUT:-/tmp/vtxstore_test}; mkdir -p "$O"
CF="-std=gnu89 -m32 -msse2 -mfpmath=sse -O1 -funsigned-char -fplan9-extensions -fno-common -fwrapv -fno-strict-aliasing
 -fno-pie -ffp-contract=off -DVERSION_US -DLANG_US -DREFRESH_NTSC -DBUGFIX_R0 -DBYTEMATCH -DPORT=1 -D_LANGUAGE_C -DTARGET_PC=1
 -I$P/hh/include -I$P/kit/platform -I$P -I$P/src -I$P/src/game -I$P/src/libultra -I$P/src/libultrare -idirafter $P/include
 -idirafter $P/include/ultra64 -idirafter $P/include/PR -include $P/hh/include/hh_enum.h -w"
# Without port/assets (tools/port_assets.sh: the decomp's tables, untracked) the headers' asset lists are stubbed
# empty: vtxstore.c uses none of them, only the struct layouts.
[ -d $P/assets ] || CF="$CF -I$O/stub"
stub() { # create each missing assets/ include as an empty file
    for k in $(seq 1 60); do
        m=$(gcc $CF -E -o /dev/null "$1" 2>&1 | sed -n 's/.*fatal error: \(assets\/[^:]*\): No such file.*/\1/p' | head -1)
        [ -n "$m" ] || return 0
        mkdir -p "$O/stub/$(dirname "$m")"; : > "$O/stub/$m"
    done
}
run() { # $1 = vtxstore.c
    [ -d $P/assets ] || { stub "$1"; stub $P/hh/tools/vtxstore_test.c; }
    gcc $CF -c -o $O/vs.o "$1" && gcc $CF -c -o $O/vt.o $P/hh/tools/vtxstore_test.c && gcc -m32 -no-pie -o $O/vt $O/vs.o $O/vt.o && $O/vt
}
r=0
if [ -n "$OLD" ]; then
    git -C $P show $OLD:port/src/game/vtxstore.c > $O/vtxstore_old.c
    echo "== $OLD"; (cd $P/src/game && run $O/vtxstore_old.c) || true
fi
echo "== working tree"; run $P/src/game/vtxstore.c || r=1
exit $r
