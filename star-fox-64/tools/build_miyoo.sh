#!/usr/bin/env bash
# Package the Miyoo Mini build (PLAT=miyoo) as ONE zip that unzips onto the SD card's root and works on every
# Miyoo OS (miyoo/tools/miyoo_pack.py; miyoo/game.json; notes/miyoo.md):
#   tools/build_miyoo.sh                 the public zip: StarFox64-miyoo.zip (no ROM inside)
#   tools/build_miyoo.sh <your ROM>      a PERSONAL zip with your ROM inside as App/StarFox64/rom.z64
#   PERSONAL=1 tools/build_miyoo.sh      the same, with port/baserom.us.rev1.z64
# OUT_DIR=<dir> puts the zip there (default: the repo root, where git ignores it).
# Needs port/build/miyoo from `make -C port -f Makefile.handheld PLAT=miyoo all` and the union toolchain
# (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain). The leak scan needs the player's ROM locally
# (port/baserom.us.rev1.z64).
#
# The release gates, all fatal, on the files that ship:
#   0. the files are this tree's build (make -q), recipes.bin SF64RCP2, embedded.bin SF64EMB1;
#   1. provenance: every source compiled into the executable is tracked in git;
#   2. the data regions (.hhasset, .ast_radio) are NOBITS: no game data in the executable;
#   2b. no shared-library variable copied into the save-state region, except glibc's stdin/stdout/stderr
#       (savestate.c keeps the live ones across a load: the desktop build's rule, now the Miyoo's);
#   3. leak scan of every shipped file against the ROM (tools/leak_scan.py, zero tolerance: --budget-kb 0),
#      the packer's scripts and icon included;
#   4. no authoring metadata or host paths in anything that ships;
#   5. no co-author or generated-with lines in any commit message;
#   6. exactly the files miyoo/game.json lists, no text; then the zip's own listing.
# A PERSONAL zip holds the player's ROM: never share it (git ignores *.zip).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
ROMARG="${1:-}"
PERSONAL="${PERSONAL:-0}"
[ -n "$ROMARG" ] && PERSONAL=1
OUT_DIR="${OUT_DIR:-$ROOT}"
OUT="$OUT_DIR/StarFox64-miyoo.zip"
[ "$PERSONAL" = 1 ] && OUT="$OUT_DIR/StarFox64-miyoo-PERSONAL.zip"
rm -f "$OUT" # first: a failed gate must not leave an older zip under this build's name
PORT=$ROOT/port
B=$PORT/build/miyoo
BIN=$B/sf64
TC="${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-"
ROM=$PORT/baserom.us.rev1.z64
GAME=miyoo/game.json
test -x "${TC}gcc" || { echo "ERROR: no Miyoo toolchain at ${TC}gcc (MIYOO_TOOLCHAIN)"; exit 1; }
test -x "$BIN" || { echo "ERROR: $BIN missing - make -C port -f Makefile.handheld PLAT=miyoo all first"; exit 1; }
test -f "$ROM" || { echo "ERROR: $ROM missing (the leak scan needs the player's ROM, locally)"; exit 1; }

echo "gate 0: this tree's build"
make -C "$PORT" -f Makefile.handheld PLAT=miyoo -q all >/dev/null 2>&1 || { echo "FAIL: the Miyoo build is out of date - make PLAT=miyoo all first"; exit 1; }
[ "$(head -c 8 "$B/recipes.bin")" = "SF64RCP2" ] || { echo "FAIL: recipes.bin is not SF64RCP2"; exit 1; }
[ "$(head -c 8 "$B/embedded.bin")" = "SF64EMB1" ] || { echo "FAIL: embedded.bin is not SF64EMB1"; exit 1; }
echo "  build $(sed -n 's/.*0x\([0-9a-fA-F]*\);.*/\1/p' "$B/build_stamp.c" | tr a-f A-F)"

echo "gate 1: provenance"
sh tools/check_provenance.sh miyoo | sed 's/^/  /'
test "${PIPESTATUS[0]}" -eq 0

echo "gate 2: data regions are NOBITS"
for sec in .hhasset .ast_radio; do
    "${TC}readelf" -SW "$BIN" | grep -E " $sec +NOBITS" >/dev/null || { echo "FAIL: $sec is not NOBITS - the executable carries game data"; exit 1; }
done
echo "  .hhasset, .ast_radio NOBITS"

echo "gate 2b: no libc variable copied into the save-state region (glibc's stdio handles excepted)"
sym() { "${TC}nm" "$BIN" | awk -v s="$1" '$3 == s && !f { print $1; f = 1 }'; }
DS=$(sym __data_start); AS=$(sym __assets_start); HS=$(sym __start_dontsave); HE=$(sym __stop_dontsave)
[ -n "$DS" ] && [ -n "$AS" ] && [ -n "$HS" ] && [ -n "$HE" ] || { echo "FAIL: save-state region symbols missing (DS=$DS AS=$AS HS=$HS HE=$HE)"; exit 1; }
COPIES=$("${TC}readelf" -rW "$BIN" | awk '$3 == "R_ARM_COPY" { print $1, $5 }')
BAD=$(echo "$COPIES" | while read -r a name; do
    [ -n "$a" ] || continue
    v=$((0x$a))
    if [ $v -ge $((0x$DS)) ] && [ $v -lt $((0x$AS)) ] && ! { [ $v -ge $((0x$HS)) ] && [ $v -lt $((0x$HE)) ]; }; then
        case "${name%%@*}" in stdin|stdout|stderr) ;; *) echo "  $name at 0x$a";; esac
    fi
done)
[ -z "$BAD" ] || { echo "FAIL: copied into the save-state region:"; echo "$BAD"; exit 1; }
echo "  none inside [0x$DS, 0x$AS) outside the dontsave hole but the stdio handles; copies: $(echo "$COPIES" | awk '{ print $2 }' | tr '\n' ' ')"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp "$BIN" "$STAGE/sf64"
"${TC}strip" --strip-unneeded -R .comment "$STAGE/sf64"
chmod +x "$STAGE/sf64"
cp "$B/recipes.bin" "$B/embedded.bin" "$STAGE/"
ICON="$(dirname "$GAME")/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["icon"])' "$GAME")"
SHIPPED=("$STAGE/sf64" "$STAGE/recipes.bin" "$STAGE/embedded.bin" "$ICON" miyoo/templates/* "$GAME")

echo "gate 3: leak scan of every shipped file against the ROM (zero tolerance)"
{ nm "$B/ast_radio.o"; awk '{ sub(/^\.(data|rodata)(\.rel(\.ro)?(\.local)?)?\./, "", $2); print "00000000 D " $2 }' "$B/gen/hhasset.txt"; } > "$STAGE/.radio_syms"
# the unstripped binary (symbol names for the report): the same bytes as the stripped one, plus symbols
python3 tools/leak_scan.py --budget-kb 0 --rom "$ROM" --image "$B/sf64.rom" --asset-syms "$B/gen/asset_syms.ld,$STAGE/.radio_syms" \
    "$BIN" "${SHIPPED[@]}" | sed 's/^/  /'
test "${PIPESTATUS[0]}" -eq 0
rm -f "$STAGE/.radio_syms"

echo "gate 4: no authoring metadata or host paths"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/' "${SHIPPED[@]}"; then
    echo "FAIL: identifying metadata in the files above"; exit 1
fi
echo "  clean"

echo "gate 5: no co-author or generated-with lines in any commit message"
if git log --format=%B | grep -i -E 'co-authored-by|generated with'; then
    echo "FAIL: attribution in the commit history (lines above)"; exit 1
fi
echo "  clean ($(git rev-list --count HEAD) commits)"

echo "gate 6: only the game's files, no text"
WANT=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$GAME")
HAVE=$(cd "$STAGE" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')
[ "$HAVE" = "$WANT" ] || { echo "FAIL: staged files '$HAVE' are not exactly '$WANT'"; exit 1; }
echo "  $HAVE"

echo "pack"
set -- "$GAME" --build "$STAGE" -o "$OUT" --check
if [ "$PERSONAL" = 1 ]; then
    PROM="${ROMARG:-$ROM}"
    test -f "$PROM" || { echo "FAIL: no ROM at $PROM"; exit 1; }
    set -- "$@" --rom "$PROM"
fi
mkdir -p "$OUT_DIR"
python3 miyoo/tools/miyoo_pack.py "$@" | sed 's/^/  /'
# the zip's own listing: the game's folder holds exactly the files, the scripts and the icon (and rom.z64 only in
# a PERSONAL zip); nothing else anywhere is text beyond the launchers' configs
python3 - "$OUT" "$GAME" "$PERSONAL" <<'PY'
import json, sys, zipfile
out, game, personal = sys.argv[1], json.load(open(sys.argv[2])), sys.argv[3] == '1'
app = 'App/%s/' % game['folder']
names = zipfile.ZipFile(out).namelist()
want = set(app + f for f in game['files'] + ['launch.sh', 'config.json', 'icon.png'] + (['rom.z64'] if personal else []))
have = set(n for n in names if n.startswith(app))
bad = sorted(have ^ want)
bad += [n for n in names if n.lower().endswith(('.md', '.txt')) or n.rsplit('/', 1)[-1].upper().startswith(('README', 'NOTICE'))]
bad += [n for n in names if n.endswith('/rom.z64') and not personal]
if bad:
    sys.exit('FAIL: the zip: ' + ', '.join(bad))
print('  %s: %d files, the game folder as listed%s' % (out.rsplit('/', 1)[-1], len(names), ', WITH YOUR ROM: never share it' if personal else ''))
PY
ls -l "$OUT"
md5sum "$OUT"
