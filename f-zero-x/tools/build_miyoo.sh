#!/bin/sh
# Package the Miyoo Mini build (PLAT=miyoo) as ONE zip that unzips onto the SD card's root and works
# on every Miyoo OS (miyoo/tools/miyoo_pack.py; miyoo/game.json):
#   sh tools/build_miyoo.sh               the public zip: FZeroX-miyoo.zip (no ROM inside)
#   PERSONAL=1 sh tools/build_miyoo.sh    FZeroX-miyoo-PERSONAL.zip: the player's ROM inside as
#                                         App/FZeroX/rom.z64 (their card only: never share it)
# FZX_ROM=<path> names the ROM (default game/baserom.us.rev0.z64): the leak scan needs it either way.
# OUT_DIR=<dir> puts the zip there (default: the repo root, where git ignores it).
# Needs game/build/miyoo/fzerox from `make -C game -f Makefile.handheld PLAT=miyoo` (notes/miyoo.md) and
# the union toolchain (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain).
#
# The release gates, all fatal, on the files that ship:
#   1. provenance: every file compiled in is tracked in git (tools/check_provenance.sh miyoo);
#   2. the N64 RAM image (.n64_rdram) is NOBITS;
#   5. save states hold game memory only (tools/check_savestate.py);
#   2b. no shared-library variable copied into the executable (R_ARM_COPY);
#   3. leak scan of every file in the zip against the ROM (tools/leak_scan.py; the executable
#      unstripped, so its matches are attributed);
#   4. no authoring metadata or host paths in anything that ships;
#   6. the zip holds exactly the game's files, the launchers and the icon (rom.z64 only when PERSONAL).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME=$ROOT/game
BUILD=$GAME/build/miyoo
BIN=$BUILD/fzerox
ROM=${FZX_ROM:-$GAME/baserom.us.rev0.z64}
PERSONAL=${PERSONAL:-0}
OUT_DIR=${OUT_DIR:-$ROOT}
OUT=$OUT_DIR/FZeroX-miyoo.zip
[ "$PERSONAL" = 1 ] && OUT=$OUT_DIR/FZeroX-miyoo-PERSONAL.zip
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf
PY=${PYTHON:-python3}
GJ=$ROOT/miyoo/game.json

rm -f "$OUT" # first: a gate that fails below must not leave an older zip under this build's name
test -x "$BIN" || { echo "ERROR: $BIN missing - make -f Makefile.handheld PLAT=miyoo first"; exit 1; }
test -x "$TC-gcc" || { echo "ERROR: no Miyoo toolchain at $TC-gcc (MIYOO_TOOLCHAIN)"; exit 1; }
test -f "$ROM" || { echo "ERROR: $ROM missing (the leak scan needs the player's ROM; set FZX_ROM)"; exit 1; }

echo "--- gate 1: provenance ---"
sh "$ROOT/tools/check_provenance.sh" miyoo

echo "--- gate 2: the N64 RAM image is NOBITS ---"
$TC-readelf -SW "$BIN" | grep -E ' \.n64_rdram +NOBITS' >/dev/null || { echo "ERROR: .n64_rdram is not NOBITS"; exit 1; }
echo "ok"

echo "--- gate 5: save states hold game memory only ---"
$PY "$ROOT/tools/check_savestate.py" "$BUILD/fzerox.map"

echo "--- gate 2b: no shared-library variable copied into the executable ---"
COPIES=$($TC-readelf -rW "$BIN" | awk '$3 == "R_ARM_COPY" { print "  " $5 " at 0x" $1 }')
[ -z "$COPIES" ] || { echo "ERROR: R_ARM_COPY relocations:"; echo "$COPIES"; exit 1; }
echo "none"

STAGE=$(mktemp -d)
UNZ=$(mktemp -d)
trap 'rm -rf "$STAGE" "$UNZ"' EXIT
cp "$BIN" "$STAGE/fzerox"
$TC-strip --strip-all --remove-section=.comment "$STAGE/fzerox"
chmod +x "$STAGE/fzerox"

echo "--- pack ---"
mkdir -p "$OUT_DIR"
if [ "$PERSONAL" = 1 ]; then
    $PY "$ROOT/miyoo/tools/miyoo_pack.py" "$GJ" --build "$STAGE" -o "$OUT" --check --rom "$ROM" | sed 's/^/  /'
else
    $PY "$ROOT/miyoo/tools/miyoo_pack.py" "$GJ" --build "$STAGE" -o "$OUT" --check | sed 's/^/  /'
fi
(cd "$UNZ" && $PY -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(".")' "$OUT")

echo "--- gate 3: leak scan of every shipped file against the ROM ---"
# everything in the zip but the player's own rom.z64 (PERSONAL), and the executable unstripped: the
# same section bytes as the stripped one, plus the symbol names that attribute each match
# (names with spaces: Apps/F-Zero X.pak, Roms/F-Zero X (FZX))
(cd "$UNZ" && find . -type f ! -path './App/FZeroX/fzerox' ! -path './App/FZeroX/rom.z64' -print0 | sort -z |
    xargs -0 $PY "$ROOT/tools/leak_scan.py" --rom "$ROM" --gen-object "$BUILD/gen/swapmap_us.o" --map "$BUILD/fzerox.map" "$BIN")
cmp -s "$STAGE/fzerox" "$UNZ/App/FZeroX/fzerox" || { echo "ERROR: the zip's executable is not the one staged"; exit 1; }

echo "--- gate 4: no authoring metadata or host paths ---"
if (cd "$UNZ" && find . -type f ! -name rom.z64 -exec grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/' {} +); then
    echo "ERROR: identifying metadata or host paths in the files above"
    exit 1
fi
echo "clean"

echo "--- gate 6: only the game's files, the launchers and the icon ---"
$PY - "$OUT" "$GJ" "$PERSONAL" <<'PY'
import json, sys, zipfile
out, game, personal = sys.argv[1], json.load(open(sys.argv[2])), sys.argv[3] == '1'
app = 'App/%s/' % game['folder']
names = zipfile.ZipFile(out).namelist()
want = set(app + f for f in game['files'] + ['launch.sh', 'config.json', 'icon.png', 'full_resolution'] + (['rom.z64'] if personal else []))
have = set(n for n in names if n.startswith(app))
bad = sorted(have ^ want)
bad += [n for n in names if n.lower().endswith(('.md', '.txt')) or n.rsplit('/', 1)[-1].upper().startswith(('README', 'NOTICE'))]
bad += [n for n in names if n.endswith('/rom.z64') and not personal]
if bad:
    sys.exit('ERROR: the zip: ' + ', '.join(bad))
print('ok: %s, %d files, the game folder as listed%s' % (out.rsplit('/', 1)[-1], len(names),
      ', WITH YOUR ROM: never share it' if personal else ''))
PY
ls -l "$OUT"
md5sum "$OUT"
