#!/bin/sh
# Assemble the Miyoo Mini release zip, SuperSmashBros-miyoo.zip in the repo root (notes/miyoo.md):
#
#   sh tools/build_miyoo.sh [path/to/the US ROM]      (default $SSB64_ROM, else refs/ssb-decomp-re/baserom.us.z64)
#
# Builds port/build/miyoo/ssb64 with the union toolchain (MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain), then
# packs ONE zip, unzipped onto the SD card's root, for every Miyoo OS at once (miyoo/tools/miyoo_pack.py, from
# the shared Miyoo kit): the game in App/SuperSmashBros/ (Onion, the stock OS, spruce: Apps), plus small launchers
# that start it from Onion's Games list, MinUI's home screen and Allium's Apps. The game itself is only the
# executable: the game data is copied from the player's ROM at first start
# (App/SuperSmashBros/data/romdata.bin).
#
# The release gates, all fatal, run on what really ships: the zip is packed to a temporary
# file, unpacked, checked, and only then put in place.
#   1. provenance: every source compiled into the executable is tracked in git, none from relocData or an
#      extracted asset (tools/check_provenance.sh miyoo); the packer, its templates, miyoo/game.json and the
#      icon are tracked too;
#   2. save-state region: no shared-library variable is copied (R_ARM_COPY) into the executable except the stdio
#      streams, and those lie outside the include list (port/gamestate.ld's two ranges);
#   3. leak scan: no 32-byte window of any file in the zip matches the ROM or any decompressed reloc file in any
#      byte order, and the executable's matches (the decomp's own tracked C tables) stay under the 144 KB cap
#      (tools/leak_scan.py). NEEDS THE ROM (read, never shipped);
#   4. contents: the staged files are exactly game.json's "files", App/SuperSmashBros holds exactly those plus the
#      packer's launch.sh, config.json, icon.png and full_resolution, and no README, NOTICE, LICENSE or other text file is in the zip;
#   5. no authoring metadata (image XMP/Photoshop) or host path in any file in the zip.
#
# NO_LEAK_SCAN=1 (no ROM at hand): gates 1, 2, 4 and 5 run, gate 3 is skipped, and the output is
# SuperSmashBros-miyoo-UNSCANNED.zip -- not a release.
# PERSONAL=1: a copy for your own card only. Your ROM goes in as App/SuperSmashBros/rom.z64 (the game looks there
# first), so the zip is all the card needs. The output is SuperSmashBros-miyoo-PERSONAL.zip, which contains the
# ROM: never share it.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PERSONAL=${PERSONAL:-0}
NO_LEAK_SCAN=${NO_LEAK_SCAN:-0}
BIN=port/build/miyoo/ssb64
GAME=miyoo/game.json
FOLDER=SuperSmashBros
OUT=SuperSmashBros-miyoo.zip
if [ "$PERSONAL" = 1 ]; then OUT=SuperSmashBros-miyoo-PERSONAL.zip
elif [ "$NO_LEAK_SCAN" = 1 ]; then OUT=SuperSmashBros-miyoo-UNSCANNED.zip; fi
# The previous zip goes first: a failed gate must not leave it in place looking like this build.
rm -f "$OUT"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STAGE=$WORK/stage
TMPZIP=$WORK/pack.zip
UNPACK=$WORK/unpack
TC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf
ROM=${1:-${SSB64_ROM:-refs/ssb-decomp-re/baserom.us.z64}}
test -x "$TC-readelf" || { echo "ERROR: the Miyoo toolchain is not at $TC-* (set MIYOO_TOOLCHAIN)"; exit 1; }
if [ ! -f "$ROM" ]; then
    if [ "$NO_LEAK_SCAN" = 1 ] && [ "$PERSONAL" != 1 ]; then
        echo "WARNING: no ROM at $ROM: gate 3 (the leak scan) will be SKIPPED; $OUT is not a release"
    else
        echo "ERROR: ROM $ROM missing. The leak scan (gate 3) needs your Super Smash Bros. (USA) ROM"
        echo "       (SHA-1 e2929e10fccc0aa84e5776227e798abc07cedabf; it is read, never shipped):"
        echo "           sh tools/build_miyoo.sh /path/to/the/rom"
        echo "       NO_LEAK_SCAN=1 skips that gate and writes an UNSCANNED zip, which is not a release."
        exit 1
    fi
fi

echo "--- build ---"
make -C port PLAT=miyoo -j"$(nproc)" > "$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }

echo "--- gate 1: provenance ---"
sh tools/check_provenance.sh miyoo
bad=0
for f in "$GAME" miyoo/tools/miyoo_pack.py miyoo/templates/* miyoo/icon.png; do
    git ls-files --error-unmatch "$f" >/dev/null 2>&1 || { echo "UNTRACKED packer input: $f"; bad=$((bad + 1)); }
done
[ "$bad" -eq 0 ] || exit 1
echo "packer inputs tracked"

echo "--- gate 2: save-state region ---"
BAD=$($TC-readelf -rW "$BIN" | awk '$3 == "R_ARM_COPY" { print $5 }' | grep -vxE 'stdin(@.*)?|stdout(@.*)?|stderr(@.*)?' || true)
[ -z "$BAD" ] || { echo "FAIL: R_ARM_COPY of: $BAD"; exit 1; }
$TC-nm "$BIN" | python3 -c '
import sys
s = {}
for l in sys.stdin:
    f = l.split()
    if len(f) == 3: s[f[2].split("@")[0]] = int(f[0], 16)
r = [(s.get("__gamedata_start"), s.get("__gamedata_end")), (s.get("__gamebss_start"), s.get("__gamebss_end"))]
if None in r[0] + r[1]: sys.exit("FAIL: no save-state range symbols")
for k in ("stdin", "stdout", "stderr", "environ", "__environ"):
    if k in s and any(a <= s[k] < b for a, b in r): sys.exit("FAIL: %s inside a save-state range" % k)
print("ok: stdio copies only, outside the include list (data %d KB, bss %d KB)" % ((r[0][1] - r[0][0]) // 1024, (r[1][1] - r[1][0]) // 1024))
'

mkdir -p "$STAGE" "$UNPACK"
cp "$BIN" "$STAGE/ssb64"
$TC-strip --strip-unneeded -R .comment "$STAGE/ssb64"
chmod +x "$STAGE/ssb64"

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

echo "--- gate 4: only what the game needs to run ---"
want=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"])))' "$GAME")
have=$(cd "$STAGE" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')
[ "$have" = "$want" ] || { echo "ERROR: staged files '$have' are not exactly '$want'"; exit 1; }
extra=""; [ "$PERSONAL" = 1 ] && extra=rom.z64
WANT_APP=$(printf '%s\n' $want launch.sh config.json icon.png full_resolution $extra | sort | tr '\n' ' ')
GOT_APP=$(cd "$UNPACK/App/$FOLDER" && ls -A | sort | tr '\n' ' ')
[ "$GOT_APP" = "$WANT_APP" ] || { echo "ERROR: App/$FOLDER holds [$GOT_APP], not [$WANT_APP]"; exit 1; }
TEXT=$(cd "$UNPACK" && find . -type f | grep -i -E '(\.md|\.txt|/readme[^/]*|/notice[^/]*|/licen[cs]e[^/]*)$' || true)
[ -z "$TEXT" ] || { echo "ERROR: text files in the zip:"; echo "$TEXT"; exit 1; }
echo "App/$FOLDER: $GOT_APP"

echo "--- gate 5: no authoring metadata or host paths in the zip ---"
if find "$UNPACK" -type f ! -path "*/App/$FOLDER/rom.z64" -print0 | xargs -0 grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/|/Volumes/'; then
    echo "ERROR: identifying metadata or a host path in the files above"; exit 1
fi
echo "clean"

echo "--- gate 3: leak scan of every file in the zip against the ROM and its reloc files ---"
if [ -f "$ROM" ]; then
    cc -O2 -Iport/hh -o "$WORK/reloc_dump" tools/reloc_dump.c port/hh/romdata.c port/hh/bs/vpk0.c
    mkdir -p "$WORK/reloc"
    SSB64_ROM="$ROM" "$WORK/reloc_dump" "$WORK/reloc" >/dev/null
    # the scan takes a .z64: normalise the dump the same way the port does
    python3 - "$ROM" "$WORK/reloc/rom.z64" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
m = bytes(d[:4])
if m == b'\x37\x80\x40\x12': d[0::2], d[1::2] = d[1::2], d[0::2]
elif m == b'\x40\x12\x37\x80': d[0::4], d[1::4], d[2::4], d[3::4] = d[3::4], d[2::4], d[1::4], d[0::4]
open(sys.argv[2], 'wb').write(d)
PY
    # a PERSONAL zip carries the ROM itself on purpose: everything else is scanned, the shipped (stripped)
    # executable included; the budget counts every ELF's matches together, so the unstripped one isn't added
    find "$UNPACK" -type f ! -path "*/App/$FOLDER/rom.z64" -print0 | xargs -0 python3 tools/leak_scan.py --budget-kb 144 \
        --rom "$WORK/reloc/rom.z64" --reloc-dir "$WORK/reloc" --readelf "$TC-readelf"
else
    echo "SKIPPED (NO_LEAK_SCAN=1, no ROM): $OUT is not a release"
fi

mv "$TMPZIP" "$OUT"
echo "--- contents ---"
python3 - "$OUT" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        print('%10d  %o  %s' % (i.file_size, i.external_attr >> 16 & 0o777, i.filename))
PY
ls -la "$OUT"
md5sum "$OUT"
if [ "$PERSONAL" = 1 ]; then echo "$OUT CONTAINS YOUR ROM: keep it to yourself"; fi
if [ ! -f "$ROM" ]; then echo "NOT A RELEASE: the leak scan did not run (no ROM)"; fi
