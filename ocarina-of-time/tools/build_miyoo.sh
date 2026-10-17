#!/usr/bin/env bash
# Package the Miyoo Mini build (PLAT=miyoo) as ONE zip that unzips onto the SD card's root and works
# on every Miyoo OS (miyoo/tools/miyoo_pack.py; miyoo/game.json):
#   tools/build_miyoo.sh                 the public zip: OcarinaOfTime-miyoo.zip (no ROM inside)
#   tools/build_miyoo.sh <your ROM>      a PERSONAL zip with your ROM inside as App/OcarinaOfTime/rom.z64
#   PERSONAL=1 tools/build_miyoo.sh      the same, with port/baseroms/ntsc-1.0/baserom.z64
# OUT_DIR=<dir> puts the zip there (default: the repo root, where git ignores it).
# Needs port/build/hh-miyoo from `make -C port -f Makefile.handheld PLAT=miyoo` (notes/miyoo.md) and
# the union toolchain (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain); runs on the host or in
# the build container, wherever that toolchain is.
#
# The release gates, all fatal, on the files that ship:
#   4. oot_recipes.bin belongs to this link (its stamp trailer = the executable's hh_build_stamp);
#   R. the recipes' literal budget: oot_recipes.bin is derived again from this link by
#      tools/handheld/hh_recipes.c (which fails above its fixed budget of 4096 non-pointer literals;
#      there is no override) and must come out byte for byte the same as the one that ships;
#   0. provenance: every C source compiled into the executable is tracked in git;
#   1. the embedded-ROM-data region (.hhassets) is NOBITS in the shipped binary;
#   1b. no shared-library variable copied into the save-state ranges (port/gamestate.ld);
#   2. no 32-byte window of the region's bytes (embedded.bin) in any shipped file (budget 16);
#   2b. leak scan of every shipped file against the whole decompressed ROM
#      (port/tools/handheld/leak_scan.py, 80 KB budget for decomp-source tables), including the
#      recipes' uncompressed op stream;
#   M. no authoring metadata or host paths in anything that ships;
#   3. exactly the files miyoo/game.json lists, no text; then the zip's own listing.
# A PERSONAL zip holds the player's ROM: never share it (git ignores *.zip).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
ROMARG="${1:-}"
PERSONAL="${PERSONAL:-0}"
[ -n "$ROMARG" ] && PERSONAL=1
OUT_DIR="${OUT_DIR:-$ROOT}"
OUT="$OUT_DIR/OcarinaOfTime-miyoo.zip"
[ "$PERSONAL" = 1 ] && OUT="$OUT_DIR/OcarinaOfTime-miyoo-PERSONAL.zip"
rm -f "$OUT" # first: a failed gate must not leave an older zip under this build's name
B=port/build/hh-miyoo
TC="${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-"
DROM=port/baseroms/ntsc-1.0/baserom-decompressed.z64
GAME=miyoo/game.json
for f in $B/oot $B/oot_embedded.elf $B/embedded.bin $B/oot_recipes.bin $B/oot_recipes.bin.ops $B/build_stamp.c; do
    test -f "$f" || { echo "ERROR: $f missing: build PLAT=miyoo first (notes/miyoo.md)"; exit 1; }
done
test -x "${TC}gcc" || { echo "ERROR: no Miyoo toolchain at ${TC}gcc (MIYOO_TOOLCHAIN)"; exit 1; }
test -f "$DROM" || { echo "ERROR: $DROM missing (the decomp's setup makes it from the player's ROM)"; exit 1; }

echo "gate 4: oot_recipes.bin is this link's"
RST=$(python3 -c "import sys; b=open(sys.argv[1],'rb').read(); print(b[-8:-4][::-1].hex() if b[-4:]==b'HHRC' else 'none')" $B/oot_recipes.bin)
BST=$(sed -n 's/.*hh_build_stamp = 0x\([0-9a-fA-F]*\);.*/\1/p' $B/build_stamp.c | tr A-F a-f)
[ "$RST" = "$BST" ] || { echo "FAIL: oot_recipes.bin stamp $RST, executable $BST"; exit 1; }
echo "  stamp $BST"

echo "gate R: the recipes' literal budget (derived again, compared)"
RT=$(mktemp -d)
gcc -O2 -Wall -Wno-unused-function -o "$RT/hh_recipes" port/tools/handheld/hh_recipes.c -lz
(cd port && env -u HH_LITERAL_BUDGET tools/handheld/recipes.sh "${TC}nm" "${TC}readelf" build/hh-miyoo/oot_embedded.elf \
    build/hh-miyoo/build_stamp.c build/hh-gen/oot_psp_asset_segments.c extracted/ntsc-1.0/baserom \
    build/hh-gen/data/segments/oot_psp_assets.bin build/hh-miyoo/embedded.bin "$RT/hh_recipes" "$RT/oot_recipes.bin") 2> "$RT/log" \
    || { cat "$RT/log"; echo "FAIL: the recipes do not derive within the budget"; exit 1; }
grep -E 'literal\)|literals .* other|round trip' "$RT/log" | sed 's/^/  /'
cmp -s "$RT/oot_recipes.bin" $B/oot_recipes.bin || { echo "FAIL: the shipped oot_recipes.bin is not what this link derives"; exit 1; }
cmp -s "$RT/oot_recipes.bin.ops" $B/oot_recipes.bin.ops || { echo "FAIL: oot_recipes.bin.ops is not this link's"; exit 1; }
echo "  the shipped oot_recipes.bin is the one derived here, within the fixed budget"
rm -rf "$RT"

echo "gate 0: provenance"
sh port/tools/handheld/check_provenance.sh miyoo

echo "gate 1: .hhassets NOBITS"
"${TC}readelf" -SW $B/oot | grep -E '\.hhassets +NOBITS' >/dev/null || { echo "FAIL: .hhassets is not NOBITS"; exit 1; }

echo "gate 1b: no libc variable copied into the save-state ranges"
sym() { "${TC}nm" $B/oot | awk -v s="$1" '$3 == s && !f { print $1; f = 1 }'; }
GDS=$(sym __gamedata_start); GDE=$(sym __gamedata_end); GBS=$(sym __gamebss_start); GBE=$(sym __gamebss_end)
[ -n "$GDS" ] && [ -n "$GDE" ] && [ -n "$GBS" ] && [ -n "$GBE" ] || { echo "FAIL: save-state range symbols missing"; exit 1; }
BAD=$("${TC}readelf" -rW $B/oot | awk '$3 == "R_ARM_COPY" { print $1, $5 }' | while read -r a name; do
    v=$((0x$a))
    if { [ $v -ge $((0x$GDS)) ] && [ $v -lt $((0x$GDE)) ]; } || { [ $v -ge $((0x$GBS)) ] && [ $v -lt $((0x$GBE)) ]; }; then
        echo "  $name at 0x$a"
    fi
done)
[ -z "$BAD" ] || { echo "FAIL: copied into the save-state ranges:"; echo "$BAD"; exit 1; }
echo "  none in [0x$GDS, 0x$GDE) or [0x$GBS, 0x$GBE); $("${TC}readelf" -rW $B/oot | grep -c R_ARM_COPY) copies elsewhere"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp $B/oot "$STAGE/oot"
"${TC}strip" --strip-unneeded -R .comment "$STAGE/oot"
chmod +x "$STAGE/oot"
cp $B/oot_recipes.bin "$STAGE/"
# what the packer adds: the launch scripts (from the templates), the icon (scaled up, image chunks only)
ICON="$(dirname "$GAME")/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["icon"])' "$GAME")"
SHIPPED=("$STAGE/oot" "$STAGE/oot_recipes.bin" "$ICON" miyoo/templates/* "$GAME")

echo "gate 2: embedded ROM data absent from shipped files"
python3 - "$B/embedded.bin" "${SHIPPED[@]}" $B/oot_recipes.bin.ops <<'PY'
import sys
blob = open(sys.argv[1], 'rb').read()[:-8]
wins = set()
for i in range(0, len(blob) - 32, 4):
    w = blob[i:i + 32]
    if len(set(w)) >= 6:
        wins.add(w)
bad = 0
for f in sys.argv[2:]:
    d = open(f, 'rb').read()
    hits = sum(1 for i in range(0, len(d) - 32) if d[i:i + 32] in wins)
    print(f"  {f.split('/')[-1]}: {hits} windows")
    bad += hits
print(f'  total {bad} (budget 16)')
sys.exit(1 if bad > 16 else 0)
PY

echo "gate 2b: leak scan against the whole (decompressed) ROM"
LEAK_SCAN_TC="$TC" python3 port/tools/handheld/leak_scan.py --rom $DROM --elf $B/oot --repo "$ROOT" --budget-kb 80 \
    "${SHIPPED[@]}" $B/oot_recipes.bin.ops | sed 's/^/  /'
test "${PIPESTATUS[0]}" -eq 0

echo "gate M: no authoring metadata or host paths"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/' "${SHIPPED[@]}"; then
    echo "FAIL: identifying metadata in the files above"; exit 1
fi
echo "  clean"

echo "gate 3: only the game's files, no text"
WANT=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$GAME")
HAVE=$(cd "$STAGE" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')
[ "$HAVE" = "$WANT" ] || { echo "FAIL: staged files '$HAVE' are not exactly '$WANT'"; exit 1; }
echo "  $HAVE"

echo "pack"
set -- "$GAME" --build "$STAGE" -o "$OUT" --check
if [ "$PERSONAL" = 1 ]; then
    ROM="${ROMARG:-port/baseroms/ntsc-1.0/baserom.z64}"
    test -f "$ROM" || { echo "FAIL: no ROM at $ROM"; exit 1; }
    set -- "$@" --rom "$ROM"
fi
mkdir -p "$OUT_DIR"
python3 miyoo/tools/miyoo_pack.py "$@" | sed 's/^/  /'
# the zip's own listing: the game's folder holds exactly the files, the scripts and the icon (and
# rom.z64 only in a PERSONAL zip); nothing else anywhere is text beyond the launchers' configs
python3 - "$OUT" "$GAME" "$PERSONAL" <<'PY'
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
    sys.exit('FAIL: the zip: ' + ', '.join(bad))
print('  %s: %d files, the game folder as listed%s' % (out.rsplit('/', 1)[-1], len(names), ', WITH YOUR ROM: never share it' if personal else ''))
PY
ls -l "$OUT"
