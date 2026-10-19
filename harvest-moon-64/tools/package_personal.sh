#!/bin/bash
# PERSONAL card zip (holds the data pack built from the owner's ROM: never public).
#   tools/package_personal.sh <build tree> <out.zip>
# The public layout (tools/package_public.sh) plus App/HarvestMoon64/data/hm64-pack.bin and data/hm64-pack.id ("HMPI"
# + the recipe's CRC), so the shipped pack counts as current and a ROM on the card does not trigger a rebuild.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TREE=$(cd "${1:?build tree}" && pwd); OUT=$(readlink -f "${2:?out zip}")
STAGE=$(mktemp -d)
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-strip -o $STAGE/hm64 $TREE/build/hh/hm64
cp $TREE/build/hh/hm64_recipes.bin $STAGE/
rm -f "$OUT"
python3 $HERE/miyoo/tools/miyoo_pack.py $HERE/miyoo/game.json --build $STAGE -o "$OUT"
python3 - "$OUT" $TREE/build/hh/hm64-pack.bin $TREE/build/hh/hm64_recipes.bin <<'PY'
import struct, sys, zipfile, zlib
z = zipfile.ZipFile(sys.argv[1], 'a', zipfile.ZIP_DEFLATED)
def add(name, data):
    zi = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    z.writestr(zi, data)
rec = open(sys.argv[3], 'rb').read()
assert rec[:4] == b'HMR1'
crc = struct.unpack('<I', rec[12:16])[0]
pack = open(sys.argv[2], 'rb').read()
assert zlib.crc32(pack) & 0xFFFFFFFF == crc, 'the pack is not the recipe\'s'
add('App/HarvestMoon64/data/hm64-pack.bin', pack)
add('App/HarvestMoon64/data/hm64-pack.id', b'HMPI' + struct.pack('<I', crc))
z.close()
PY
rm -rf $STAGE
ls -l "$OUT"
