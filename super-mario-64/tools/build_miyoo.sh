#!/bin/sh
# Assemble the Miyoo Mini release zip (SuperMario64-miyoo.zip) after `make PLAT=miyoo` in port/, on a host
# with the union toolchain (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain) and the ROM as
# port/baserom.us.z64 (the leak scan needs it):
#   sh tools/build_miyoo.sh            # the public zip: code and numbers only
#   PERSONAL=1 sh tools/build_miyoo.sh # SuperMario64-miyoo-PERSONAL.zip: your ROM inside as rom.z64. Never share it.
#
# One zip, unzipped onto the SD card's root, for every Miyoo OS at once (miyoo/tools/miyoo_pack.py, from
# the shared Miyoo kit): the game in App/SuperMario64/ (Onion, the stock OS, spruce: Apps), plus small launchers
# that start it from Onion's Games list, MinUI's home screen and Allium's Apps. The game builds
# its data from the player's own ROM on first start.
#
# The release gates, all fatal: 1 provenance, 1b untracked includes, 2 NOBITS asset region, 2c code
# pointers, 2b no shared-library variable copied into the save-state region (glibc's stdin/stdout/stderr always
# are, and the kit's savestate.c keeps the live ones across a load: those three are allowed), 3 leak scan against
# the ROM, 4 no authoring metadata or host paths, 5 only the listed files; then the zip's own listing.
set -eu
PERSONAL=${PERSONAL:-0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=$ROOT/port
BUILD=$PORT/build/us_miyoo
BIN=$BUILD/sm64
OUT=${OUT:-$ROOT/SuperMario64-miyoo.zip}
[ "$PERSONAL" = 1 ] && OUT=${OUT%.zip}-PERSONAL.zip
STAGE=${TMPDIR:-/tmp}/sm64_miyoo_stage
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf
ROM=$PORT/baserom.us.z64
GAME=$ROOT/miyoo/game.json
rm -f "$OUT"
test -x "$BIN" || { echo "ERROR: $BIN missing - build PLAT=miyoo first"; exit 1; }
test -f "$BUILD/recipes.bin" || { echo "ERROR: $BUILD/recipes.bin missing"; exit 1; }
if [ "$(head -c 8 "$BUILD/recipes.bin")" != "SM64RCP1" ]; then
    echo "ERROR: recipes.bin is a stub: run make PLAT=miyoo assets-recipes"; exit 1
fi
test -f "$ROM" || { echo "ERROR: $ROM missing (the leak scan needs it)"; exit 1; }

echo "--- gate 1: provenance of the code ---"
sh "$ROOT/tools/assets/check_provenance.sh" miyoo
echo "--- gate 1b: what the code includes from the decomp's untracked data ---"
(cd "$PORT" && python3 "$ROOT/tools/assets/check_includes.py" miyoo)

echo "--- gate 2: asset region is NOBITS ---"
$TC-readelf -SW "$BIN" | grep -E '\.assetdata +NOBITS' >/dev/null || { echo "ERROR: .assetdata missing or not NOBITS - the executable carries game data"; exit 1; }
echo "NOBITS"

echo "--- gate 2c: every code pointer in the game data lands on a function of this executable ---"
python3 "$ROOT/tools/assets/check_ptrs.py" "$BUILD/assets.bin" "$BIN"

echo "--- gate 2b: no libc variable copied into the save-state region (glibc's stdio aside) ---"
sym() { $TC-nm "$BIN" | awk -v s="$1" '$3 == s { print $1; exit }'; }
DS=$(sym __data_start); AS=$(sym __assets_state_end); HS=$(sym __start_dontsave); HE=$(sym __stop_dontsave)
[ -n "$DS" ] && [ -n "$AS" ] && [ -n "$HS" ] && [ -n "$HE" ] || { echo "ERROR: save-state region symbols missing"; exit 1; }
BAD=$($TC-readelf -rW "$BIN" | awk '$3 == "R_ARM_COPY" { print $1, $5 }' | while read -r a name; do
    case "$name" in stdin@*|stdout@*|stderr@*) continue ;; esac
    v=$((0x$a))
    if [ $v -ge $((0x$DS)) ] && [ $v -lt $((0x$AS)) ] && ! { [ $v -ge $((0x$HS)) ] && [ $v -lt $((0x$HE)) ]; }; then
        echo "  $name at 0x$a"
    fi
done)
[ -z "$BAD" ] || { echo "ERROR: copied into the save-state region:"; echo "$BAD"; exit 1; }
echo "none inside [0x$DS, 0x$AS) outside the dontsave hole (stdio allowed)"

rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$BIN" "$STAGE/sm64"
$TC-strip --strip-all --remove-section=.comment "$STAGE/sm64"
chmod +x "$STAGE/sm64"
cp "$BUILD/recipes.bin" "$STAGE/recipes.bin"

echo "--- gate 3: leak scan of every shipped file against the ROM ---"
python3 "$ROOT/tools/assets/leak_scan.py" --rom "$ROM" --assets "$BUILD/assets.bin" --symbols-from "$BIN" --budget-kb 24 \
    "$STAGE/sm64" "$STAGE/recipes.bin" "$ROOT/miyoo/icon.png" "$ROOT"/miyoo/templates/* "$GAME"

echo "--- gate 4: no authoring metadata or host paths in the shipped files ---"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/' "$STAGE"/* "$ROOT/miyoo/icon.png" "$ROOT"/miyoo/templates/* "$GAME"; then
    echo "ERROR: identifying metadata in the files above"; exit 1
fi
echo "clean"

echo "--- gate 5: only what the game needs to run ---"
want=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$GAME")
have=$(cd "$STAGE" && ls | sort | tr '\n' ' ' | sed 's/ $//')
[ "$have" = "$want" ] || { echo "ERROR: staged files '$have' are not exactly '$want'"; exit 1; }
echo "clean: $have"

echo "--- pack ---"
set -- "$GAME" --build "$STAGE" -o "$OUT" --check
[ "$PERSONAL" = 1 ] && set -- "$@" --rom "$ROM"
python3 "$ROOT/miyoo/tools/miyoo_pack.py" "$@"

echo "--- the zip's listing: App/SuperMario64 holds exactly the game's files (rom.z64 only in a PERSONAL zip) ---"
python3 - "$OUT" "$GAME" "$PERSONAL" <<'EOF'
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); game = json.load(open(sys.argv[2])); personal = sys.argv[3] == '1'
app = 'App/%s/' % game['folder']
names = [n for n in z.namelist() if not n.endswith('/')]
got = set(n for n in names if n.startswith(app))
want = set(app + f for f in game['files'] + ['launch.sh', 'config.json', 'icon.png', 'full_resolution'] + (['rom.z64'] if personal else []))
if got != want: sys.exit('ERROR: %s holds %s, want %s' % (app, sorted(got), sorted(want)))
roms = [n for n in names if n.lower().endswith(('.z64', '.n64', '.v64'))]
if roms != ([app + 'rom.z64'] if personal else []): sys.exit('ERROR: ROM files in the zip: %s' % roms)
print('%d entries; %s ok%s' % (len(z.namelist()), app, ' (rom.z64 inside: never share this zip)' if personal else ''))
EOF
rm -rf "$STAGE"
ls -la "$OUT"; md5sum "$OUT"
