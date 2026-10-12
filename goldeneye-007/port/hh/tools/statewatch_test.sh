#!/bin/sh
# Build hh/hh_plat.c (the real file, host build) with hh/tools/statewatch_test.c and run it; "nohook" replays the
# old behaviour (no hh_plat_state_loaded call) for comparison. CC=<Miyoo toolchain gcc> for the device compiler.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
O=${SW_OUT:-/tmp/statewatch_test}; mkdir -p "$O"
# ARM (the device's ABI; the host's -m32 headers lack asm/errno.h here), run under qemu-arm
CC=${CC:-arm-linux-gnueabihf-gcc}
CF="-std=gnu89 -O1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -funsigned-char -fno-common -fwrapv -fno-strict-aliasing -fno-pie -DPORT=1 -D_LANGUAGE_C -DHH_PORT_LAYER=1 -DTARGET_PC=1
 -I$P/hh/include -I$P/kit/platform -I$P -I$P/src -I$P/src/game -I$P/src/inflate -I$P/src/libultra -I$P/src/libultrare -I$P/hh -idirafter $P/include -idirafter $P/include/ultra64 -idirafter $P/include/PR
 -include $P/hh/include/hh_enum.h -w"
$CC $CF -c -o $O/plat.o $P/hh/hh_plat.c
$CC -O1 -w -c -o $O/sw.o $P/hh/tools/statewatch_test.c
$CC -static -o $O/sw $O/plat.o $O/sw.o -Wl,--unresolved-symbols=ignore-all
qemu-arm $O/sw nohook
qemu-arm $O/sw
