#!/bin/sh
# Assemble the Miyoo Mini release zip, DiddyKongRacing-miyoo.zip in the repo root (run from anywhere
# after `make PLAT=miyoo`; the union toolchain at MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain):
#
#   sh tools/build_miyoo.sh [path/to/baserom.us.v80.z64]
#
# One zip, unzipped onto the SD card's root, for every Miyoo OS at once (miyoo/tools/miyoo_pack.py,
# from the shared miyoo/): the game in App/DiddyKongRacing/ (Onion, the stock OS, spruce: Apps),
# plus small launchers that start it from Onion's Games list, MinUI's home screen and Allium's Apps.
# The game itself is only the executable: the game data is copied from the player's
# ROM at first start (dkr_assets.bin in App/DiddyKongRacing/data/).
#
# The release gates, all fatal, run on what really ships: the zip is
# packed to a temporary file, unpacked, checked, and only then put in place.
#   1. provenance -- every source compiled into the executable is tracked in git, and so are the
#      packer, its templates and miyoo/game.json;
#   2. save-state region -- .game_bss is NOBITS, and no shared-library variable is copied
#      (R_ARM_COPY) into .game_data/.game_bss (glibc's stdin/stdout/stderr included: the region is
#      an include-list, so they must land outside it, and savestate.c keeps the live ones anyway);
#   3. only what the game needs to run -- the staged files are exactly game.json's "files", and the
#      game's folder in the zip holds exactly those plus the packer's launch.sh, config.json and
#      icon.png: no README, NOTICE, LICENSE or other text file;
#   4. leak scan -- no 32-byte window of any file in the zip matches the ROM's asset region, raw or
#      inflated, in any byte order (tools/leak_scan.py). NEEDS THE ROM (never shipped);
#   5. no authoring metadata (image XMP) or host paths in any file in the zip.
#
# NO_LEAK_SCAN=1 (no ROM at hand): gates 1-3 and 5 run, gate 4 is skipped, and the output is
# DiddyKongRacing-miyoo-UNSCANNED.zip -- not a release. Run the script again with the ROM before
# publishing anything.
#
# PERSONAL=1: a copy for your own card only. Your ROM goes in as App/DiddyKongRacing/rom.z64 (the
# game looks there first), so the zip is all the card needs. The output is
# DiddyKongRacing-miyoo-PERSONAL.zip, which contains the ROM: never share it.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PERSONAL=${PERSONAL:-0}
NO_LEAK_SCAN=${NO_LEAK_SCAN:-0}
BIN=build/miyoo/dkr
GAME=miyoo/game.json
FOLDER=DiddyKongRacing
OUT=DiddyKongRacing-miyoo.zip
if [ "$PERSONAL" = 1 ]; then OUT=DiddyKongRacing-miyoo-PERSONAL.zip
elif [ "$NO_LEAK_SCAN" = 1 ]; then OUT=DiddyKongRacing-miyoo-UNSCANNED.zip; fi
# The previous zip goes first: a failed gate must not leave it in place looking like this build.
rm -f "$OUT"
STAGE=build/miyoo_stage
TMPZIP=build/miyoo_pack.zip
UNPACK=build/miyoo_unpack
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf
ROM=${1:-refs/Diddy-Kong-Racing/baseroms/baserom.us.v80.z64}
test -x "$BIN" || { echo "ERROR: $BIN missing - run make PLAT=miyoo first"; exit 1; }
test -x "$TC-readelf" || { echo "ERROR: the Miyoo toolchain is not at $TC-* (set MIYOO_TOOLCHAIN)"; exit 1; }
if [ ! -f "$ROM" ]; then
    if [ "$NO_LEAK_SCAN" = 1 ] && [ "$PERSONAL" != 1 ]; then
        echo "WARNING: no ROM at $ROM: gate 4 (the leak scan) will be SKIPPED; $OUT is not a release"
    else
        echo "ERROR: ROM $ROM missing. The leak scan (gate 4) needs your Diddy Kong Racing US 1.1 ROM"
        echo "       (SHA-1 6d96743d46f8c0cd0edb0ec5600b003c89b93755; it is read, never shipped):"
        echo "           sh tools/build_miyoo.sh /path/to/baserom.us.v80.z64"
        echo "       NO_LEAK_SCAN=1 skips that gate and writes an UNSCANNED zip, which is not a release."
        exit 1
    fi
fi

echo "--- gate 1: provenance of the code ---"
bad=0; n=0
for f in $(make -s PLAT=miyoo print-sources); do
    n=$((n + 1))
    case "$f" in build/miyoo/gen/trig_tables.c) continue ;; esac
    git ls-files --error-unmatch "$f" >/dev/null 2>&1 || { echo "UNTRACKED source in the executable: $f"; bad=$((bad + 1)); }
done
for f in "$GAME" miyoo/tools/miyoo_pack.py miyoo/templates/* miyoo/icon.png; do
    git ls-files --error-unmatch "$f" >/dev/null 2>&1 || { echo "UNTRACKED packer input: $f"; bad=$((bad + 1)); }
done
echo "provenance: $n sources, $bad untracked"
[ "$n" -ge 100 ] || { echo "ERROR: only $n sources listed - the check did not run"; exit 1; }
[ "$bad" -eq 0 ] || exit 1

echo "--- gate 2: save-state region ---"
$TC-readelf -SW "$BIN" | grep -E '\.game_bss +NOBITS' >/dev/null || { echo "ERROR: .game_bss missing or not NOBITS"; exit 1; }
sym() { $TC-nm "$BIN" | awk -v s="$1" '$3 == s { print $1; exit }'; }
DS=$(sym __gamedata_start); DE=$(sym __gamedata_end); BS=$(sym __gamebss_start); BE=$(sym __gamebss_end)
[ -n "$DS" ] && [ -n "$DE" ] && [ -n "$BS" ] && [ -n "$BE" ] || { echo "ERROR: save-state region symbols missing"; exit 1; }
BAD=$($TC-readelf -rW "$BIN" | awk '$3 == "R_ARM_COPY" { print $1, $5 }' | while read -r a name; do
    v=$((0x$a))
    if { [ $v -ge $((0x$DS)) ] && [ $v -lt $((0x$DE)) ]; } || { [ $v -ge $((0x$BS)) ] && [ $v -lt $((0x$BE)) ]; }; then
        echo "  $name at 0x$a"
    fi
done)
[ -z "$BAD" ] || { echo "ERROR: copied into the save-state region:"; echo "$BAD"; exit 1; }
COPIES=$($TC-readelf -rW "$BIN" | awk '$3 == "R_ARM_COPY" { printf "%s ", $5 }')
echo "region .game_data 0x$DS-0x$DE, .game_bss 0x$BS-0x$BE; no R_ARM_COPY inside (copies elsewhere: ${COPIES:-none})"

rm -rf "$STAGE" "$UNPACK" "$TMPZIP"; mkdir -p "$STAGE" "$UNPACK"
cp "$BIN" "$STAGE/dkr"
$TC-strip --remove-section=.comment "$STAGE/dkr"
chmod +x "$STAGE/dkr"

echo "--- pack (to a temporary zip, checked below) ---"
set -- "$GAME" --build "$STAGE" -o "$TMPZIP"
if [ "$PERSONAL" = 1 ]; then set -- "$@" --rom "$ROM"; fi
python3 miyoo/tools/miyoo_pack.py "$@"
python3 - "$TMPZIP" "$UNPACK" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        n = os.path.normpath(i.filename)
        if n.startswith('..') or os.path.isabs(n):
            sys.exit('zip entry outside the card root: %s' % i.filename)
    z.extractall(sys.argv[2])
PY

echo "--- gate 3: only what the game needs to run ---"
want=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$GAME")
have=$(cd "$STAGE" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')
[ "$have" = "$want" ] || { echo "ERROR: staged files '$have' are not exactly '$want'"; exit 1; }
extra=""; [ "$PERSONAL" = 1 ] && extra=rom.z64
WANT_APP=$(printf '%s\n' $want launch.sh config.json icon.png $extra | sort | tr '\n' ' ')
GOT_APP=$(cd "$UNPACK/App/$FOLDER" && ls -A | sort | tr '\n' ' ')
[ "$GOT_APP" = "$WANT_APP" ] || { echo "ERROR: App/$FOLDER holds [$GOT_APP], not [$WANT_APP]"; exit 1; }
TEXT=$(cd "$UNPACK" && find . -type f | grep -i -E '(\.md|\.txt|/readme[^/]*|/notice[^/]*|/licen[cs]e[^/]*)$' || true)
[ -z "$TEXT" ] || { echo "ERROR: text files in the zip:"; echo "$TEXT"; exit 1; }
echo "App/$FOLDER: $GOT_APP"

echo "--- gate 4: leak scan of every file in the zip against the ROM ---"
if [ -f "$ROM" ]; then
    # a PERSONAL zip carries the ROM itself on purpose: everything else is scanned
    find "$UNPACK" -type f ! -path "*/App/$FOLDER/rom.z64" -print0 | xargs -0 python3 tools/leak_scan.py --rom "$ROM"
else
    echo "SKIPPED (NO_LEAK_SCAN=1, no ROM): $OUT is not a release"
fi

echo "--- gate 5: no authoring metadata or host paths in the zip ---"
if find "$UNPACK" -type f ! -path "*/App/$FOLDER/rom.z64" -print0 | xargs -0 grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/'; then
    echo "ERROR: identifying metadata in the files above"; exit 1
fi
echo "clean"

mv "$TMPZIP" "$OUT"
echo "--- contents ---"
python3 - "$OUT" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        print('%10d  %o  %s' % (i.file_size, i.external_attr >> 16 & 0o777, i.filename))
PY
ls -la "$OUT"
if [ "$PERSONAL" = 1 ]; then echo "$OUT CONTAINS YOUR ROM: keep it to yourself"; fi
if [ ! -f "$ROM" ]; then echo "NOT A RELEASE: the leak scan did not run (no ROM)"; fi
