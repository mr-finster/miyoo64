#!/usr/bin/env bash
# The public zip, BanjoKazooie-miyoo.zip: ONE zip that unzips onto the SD card's root and works on every Miyoo OS
# (miyoo/tools/miyoo_pack.py; miyoo/game.json). No ROM and nothing made from one: the game reads the player's own
# ROM, found on the card at the first start (hh/hh_card.c, miyoo_romfind.h).
#   tools/build_miyoo.sh <build tree>          (the tree tools/setup.sh made)
# OUT_DIR=<dir> puts the zip there (default: this folder, where git ignores it). HH_RELEASE=<x.y.z> overrides the
# release number in the version line (hh/Makefile.handheld's default otherwise).
# JOBS=<n>: make's parallel jobs (default: every core).
#
# The build: make with this repository's commit as the version, so console.log's first line names the build
# ("SESSION banjo Banjo-Kazooie <release> version <commit> build <stamp>", rewritten by every make).
# The release gates, all fatal, on the files that ship:
#   V. the executable carries this commit and the release number;
#   2. leak scan (tools/leak_scan.py): every 32-byte block of the ROM, of the decompressed ROM and of every packed
#      asset inflated, in N64 order and with 16- and 32-bit units swapped (the port's host order), against every
#      byte offset of every shipped file. The launchers, icon and config: none at all. The executable: at most 2048
#      windows at 8-byte steps (about 13 KB, every one of them a table the decompilation's own C sources compile to:
#      actor and animation id lists, render modes, collision and sound tables, display-list setups; no texture,
#      model, sound or level data, which the game reads from the player's ROM);
#   M. no authoring metadata or host paths in anything that ships;
#   3. exactly the files miyoo/game.json lists; then the zip's own listing (no ROM, no data, no text).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE=$(cd "${1:?build tree}" && pwd)
OUT_DIR="${OUT_DIR:-$HERE}"
OUT="$OUT_DIR/BanjoKazooie-miyoo.zip"
rm -f "$OUT" # first: a failed gate must not leave an older zip under this build's name
TC="${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-"
GAME="$HERE/miyoo/game.json"
ROM="$TREE/baserom.us.v10.z64"
DEC="$TREE/decompressed.us.v10.z64"
for f in "$ROM" "$DEC"; do
    test -f "$f" || { echo "ERROR: $f missing: run tools/setup.sh first"; exit 1; }
done
VER=$(git -C "$HERE" rev-parse --short HEAD)
REL=${HH_RELEASE:-$(sed -n 's/^HH_RELEASE ?= *//p' "$HERE/hh/Makefile.handheld")}

echo "build $REL ($VER)"
rm -rf "$TREE/hh" && cp -r "$HERE/hh" "$TREE/hh"
make -C "$TREE" -f hh/Makefile.handheld PLAT=miyoo HH_VERSION="$VER" HH_RELEASE="$REL" -j"${JOBS:-$(nproc)}" | tail -1

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/build"
"${TC}strip" -o "$STAGE/build/banjo" "$TREE/build/hh-miyoo/banjo"
EXE="$STAGE/build/banjo"

echo "gate V: the version line"
grep -a -q "Banjo-Kazooie $REL" "$EXE" && grep -a -q -x -F "$VER" <(strings -n 7 "$EXE") ||
    { echo "FAIL: the executable does not carry Banjo-Kazooie $REL / $VER"; exit 1; }
echo "  Banjo-Kazooie $REL, version $VER"

ICON="$(dirname "$GAME")/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["icon"])' "$GAME")"
SHIPPED=("$EXE" "$ICON" "$HERE"/miyoo/templates/* "$GAME")

echo "gate 2: leak scan against the ROM, the decompressed ROM and the inflated assets"
python3 "$HERE/tools/leak_scan.py" "$ROM" "$DEC" 2048 "${SHIPPED[@]}"

echo "gate M: no authoring metadata or host paths"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/|/dev/shm/' "${SHIPPED[@]}"; then
    echo "FAIL: identifying metadata in the files above"; exit 1
fi
echo "  clean"

echo "gate 3: only the game's files"
WANT=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$GAME")
HAVE=$(cd "$STAGE/build" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')
[ "$HAVE" = "$WANT" ] || { echo "FAIL: staged files '$HAVE' are not exactly '$WANT'"; exit 1; }
echo "  $HAVE"

echo "pack"
mkdir -p "$OUT_DIR"
python3 "$HERE/miyoo/tools/miyoo_pack.py" "$GAME" --build "$STAGE/build" -o "$OUT" | sed 's/^/  /'
python3 - "$OUT" <<'PY'
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
app = 'App/BanjoKazooie/'
want = set(app + f for f in ['banjo', 'launch.sh', 'config.json', 'icon.png', 'full_resolution'])
have = set(n for n in names if n.startswith(app))
bad = sorted(have ^ want)
bad += [n for n in names if n.lower().endswith(('.md', '.txt', '.z64', '.v64', '.n64', '.bin', '.rom', '.u64'))]
if bad:
    sys.exit('FAIL: the zip: ' + ', '.join(bad))
print('  %s: %d files, no ROM, no data' % (sys.argv[1].rsplit('/', 1)[-1], len(names)))
PY
ls -l "$OUT"
sha256sum "$OUT"
