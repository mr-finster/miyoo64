#!/bin/sh
# Assemble the Miyoo Mini release zip. Runs inside the build container (after
# make -f Makefile.handheld PLAT=miyoo):
#   ./dev.sh 'sh /work/build_miyoo.sh'
# or anywhere the union toolchain lives (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain).
#
# One zip, unzipped onto the SD card's root, for every Miyoo OS at once (miyoo/tools/miyoo_pack.py,
# from the shared miyoo/): the game in App/MarioKart64/ (Onion, the stock OS, spruce: Apps), plus
# small launchers that start it from Onion's Games list, MinUI's home screen and Allium's Apps. The
# zip holds the executable (asset region NOLOAD, no game data), recipes.bin, the launch scripts and
# the icon. The game builds its data from the player's own ROM on first start.
#
# The release gates, all fatal: 1 provenance, 2 NOBITS asset region, 2b no shared-library
# variable copied into the save-state region (glibc's stdin/stdout/stderr always are, and
# savestate.c keeps the live ones across a load: those three are allowed), 3 leak scan against
# the ROM, 4 no authoring metadata or host paths, 5 only the listed files.
#
# PERSONAL=1: a copy for your own card only. Your ROM goes in as App/MarioKart64/rom.z64 (the game
# looks there first), so the zip is all the card needs. The output is
# MarioKart64-miyoo-PERSONAL.zip, which contains the ROM: never share it.
set -eu
PERSONAL=${PERSONAL:-0}
ROOT=$(cd "$(dirname "$0")" && pwd)
BASE=$ROOT/base-psp
BUILD=$BASE/build/miyoo
BIN=$BUILD/mk64
OUT=$ROOT/MarioKart64-miyoo.zip
[ "$PERSONAL" = 1 ] && OUT=${OUT%.zip}-PERSONAL.zip
STAGE=${TMPDIR:-/tmp}/miyoo_stage
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf
ROM=$BASE/baserom.us.z64
rm -f "$OUT"
test -x "$BIN" || { echo "ERROR: $BIN missing - build PLAT=miyoo first"; exit 1; }
test -f "$BUILD/recipes.bin" || { echo "ERROR: $BUILD/recipes.bin missing"; exit 1; }
if [ "$(head -c 8 "$BUILD/recipes.bin")" != "MK64RCP1" ]; then
    echo "ERROR: recipes.bin is a stub: run make -f Makefile.handheld PLAT=miyoo assets-recipes"; exit 1
fi

echo "--- gate 1: provenance of the code ---"
sh "$ROOT/tools/check_provenance.sh" miyoo

echo "--- gate 2: asset region is NOBITS ---"
$TC-readelf -SW "$BIN" | grep -E '\.assetdata +NOBITS' >/dev/null || { echo "ERROR: .assetdata missing or not NOBITS - the executable carries game data"; exit 1; }

echo "--- gate 2b: no libc variable copied into the save-state region (stdio aside) ---"
sym() { $TC-nm "$BIN" | awk -v s="$1" '$3 == s { print $1; exit }'; }
DS=$(sym __data_start); AS=$(sym __assets_start); HS=$(sym __start_dontsave); HE=$(sym __stop_dontsave)
[ -n "$DS" ] && [ -n "$AS" ] && [ -n "$HS" ] && [ -n "$HE" ] || { echo "ERROR: save-state region symbols missing"; exit 1; }
BAD=$($TC-readelf -rW "$BIN" | awk '$3 == "R_ARM_COPY" { print $1, $5 }' | while read -r a name; do
    case "$name" in stdin@*|stdout@*|stderr@*) continue ;; esac
    v=$((0x$a))
    if [ $v -ge $((0x$DS)) ] && [ $v -lt $((0x$AS)) ] && ! { [ $v -ge $((0x$HS)) ] && [ $v -lt $((0x$HE)) ]; }; then
        echo "  $name at 0x$a"
    fi
done)
[ -z "$BAD" ] || { echo "ERROR: copied into the save-state region:"; echo "$BAD"; exit 1; }
echo "clean"

rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$BIN" "$STAGE/mk64"
$TC-strip --remove-section=.comment "$STAGE/mk64"
chmod +x "$STAGE/mk64"
cp "$BUILD/recipes.bin" "$STAGE/recipes.bin"

echo "--- gate 3: leak scan of every shipped file against the ROM ---"
python3 "$ROOT/tools/leak_scan.py" --rom "$ROM" --assets "$BUILD/assets.bin" "$BIN" "$STAGE/recipes.bin" "$ROOT/miyoo/icon_release.png" "$ROOT"/miyoo/templates/*

echo "--- gate 4: no authoring metadata or host paths in the shipped files ---"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/' "$STAGE/mk64" "$STAGE/recipes.bin" "$ROOT/miyoo/icon_release.png" "$ROOT"/miyoo/templates/* "$ROOT/miyoo/game.json"; then
    echo "ERROR: identifying metadata in the files above"; exit 1
fi
echo "clean"

echo "--- gate 5: only what the game needs to run ---"
want=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$ROOT/miyoo/game.json")
have=$(cd "$STAGE" && ls | sort | tr '\n' ' ' | sed 's/ $//')
[ "$have" = "$want" ] || { echo "ERROR: staged files '$have' are not exactly '$want'"; exit 1; }
echo "clean: $have"

echo "--- pack ---"
set -- "$ROOT/miyoo/game.json" --build "$STAGE" -o "$OUT" --check
if [ "$PERSONAL" = 1 ]; then
    test -f "$ROM" || { echo "ERROR: PERSONAL=1 needs $ROM"; exit 1; }
    set -- "$@" --rom "$ROM"
fi
python3 "$ROOT/miyoo/tools/miyoo_pack.py" "$@"
ls -la "$OUT"
