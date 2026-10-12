#!/bin/sh
# Symbolize the crash backtrace the native game printed: tools/port/bt.sh < log (or bt.sh logfile)
cd "$(dirname "$0")/../.."
grep -oE '#[0-9]+ 0x[0-9a-f]+' "${1:-/dev/stdin}" | awk '{print $2}' > build/game/bt.txt
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work pm64hh:decomp bash -c 'addr2line -f -i -C -e build/game/pc/pm64 $(cat build/game/bt.txt) | paste - -'
