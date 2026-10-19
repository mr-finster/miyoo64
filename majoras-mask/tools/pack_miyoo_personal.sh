#!/bin/bash
# PERSONAL card zip (holds the data pack built from the owner's ROM: never public).
#   tools/package_personal.sh <build tree> <out.zip>
# <build tree>: the tree tools/setup.sh made, built (build/hh-miyoo/mm, build/hh-pack.bin). The card layout is the
# Miyoo kit's (miyoo/tools/miyoo_pack.py: App/MajorasMask + the Onion Ports shortcut, MinUI pak, Apps entry), plus
# App/MajorasMask/data/mm_pack.bin.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TREE=$(cd "${1:?build tree}" && pwd); OUT=$(readlink -f "${2:?out zip}")
STAGE=$(mktemp -d)
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-strip -o $STAGE/mm $TREE/build/hh-miyoo/mm
python3 $HERE/miyoo/tools/miyoo_pack.py $HERE/miyoo/game.json --build $STAGE -o "$OUT"
# + the first-start recipe (op numbers only, hh/tools/mm_recipes.py) next to mm, and data/mm_pack.id ("MMPI" + the
# recipe's CRC) so the shipped pack counts as current and a ROM on the card does not trigger a rebuild
python3 - "$OUT" $TREE/build/hh-pack.bin $TREE/build/mm_recipes.bin <<'PY'
import struct, sys, zipfile
z = zipfile.ZipFile(sys.argv[1], 'a', zipfile.ZIP_DEFLATED)
def add(name, data):
    zi = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    z.writestr(zi, data)
rec = open(sys.argv[3], 'rb').read()
assert rec[:4] == b'MMR1'
crc = struct.unpack('<I', rec[16:20])[0]
add('App/MajorasMask/data/mm_pack.bin', open(sys.argv[2], 'rb').read())
add('App/MajorasMask/mm_recipes.bin', rec)
add('App/MajorasMask/data/mm_pack.id', b'MMPI' + struct.pack('<I', crc))
z.close()
PY
rm -rf $STAGE
ls -l "$OUT"
