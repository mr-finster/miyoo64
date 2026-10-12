#!/usr/bin/env bash
# Assemble the Miyoo Mini release zip from the PLAT=miyoo build (notes/miyoo.md, README_MIYOO.md).
#   make -C port -f Makefile.handheld PLAT=miyoo -j6     # MIYOO_TOOLCHAIN, default /opt/miyoomini-toolchain
#   tools/m1_build.sh <dump>                            # once: the map the table scrub needs
#   tools/build_miyoo.sh <your GoldenEye 007 (USA) dump> [out dir]
#
# One zip, unzipped onto the SD card's root, for every Miyoo OS at once (miyoo/tools/miyoo_pack.py,
# from the shared miyoo/): the game in App/GoldenEye/ (Onion, the stock OS, spruce: Apps), plus small
# launchers that hand over to it from Onion's Games list, MinUI's home screen and Allium's Apps. It is
# CODE ONLY: the executable with the ROM's data tables zeroed (tools/romtables.py),
# recipes.bin (numbers only: where those tables come from in the player's ROM), the launch scripts and
# the icon (miyoo/icon.png, drawn). The game finds the player's ROM (App/GoldenEye/rom.z64 first, else
# anywhere on the card up to two folders deep, hh_card.c) and fills the tables from it at every start.
#
# The release gates, all fatal:
#   5. the testing unlocks: the executable's marker says GE_TEST_UNLOCKS=0 (RELEASE=1 refuses a 1);
#   1. provenance: every file compiled into the executable is tracked in git, except the untracked
#      decomp tables under port/assets -- the ones romtables.py scrubs; the packer, its templates,
#      game.json and the icon are tracked too;
#   2. leak scan: no shipped 16-byte window matches the ROM inside a scrubbed table, and the decomp's
#      own tracked source tables stay under the 48 KB cap (tools/leak_scan.py) -- over the executable,
#      recipes.bin, the icon and the launch script templates;
#   3. no host paths or authoring metadata in anything shipped (the packer checks every zip entry too);
#   4. only what the game needs: the staged files are exactly miyoo/game.json's "files"; the zip's
#      App/GoldenEye/ holds those plus launch.sh, config.json and icon.png; no README, *.md or *.txt;
#      a public zip has no ROM.
#
# PERSONAL=1: a copy for your own card only. The dump goes in as App/GoldenEye/rom.z64 (the game looks
# there first), so the zip is all the card needs. Output GoldenEye-miyoo-PERSONAL.zip: it holds your
# ROM, never share it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROM="${1:?usage: tools/build_miyoo.sh <your GoldenEye 007 (USA) dump> [out dir]}"
OUTDIR="${2:-$ROOT}"
PERSONAL="${PERSONAL:-0}"
OUT="$OUTDIR/GoldenEye-miyoo.zip"
[ "$PERSONAL" = 1 ] && OUT="$OUTDIR/GoldenEye-miyoo-PERSONAL.zip"
BIN="$ROOT/port/build/miyoo/ge007"
MAP="$ROOT/m1/ge007/build/u/ge007.u.map"
STAGE="$ROOT/m2scratch/miyoo_stage"
WORK="$ROOT/m2scratch/miyoo_work"
GAME="$ROOT/miyoo/game.json"
TC="${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf"
rm -f "$OUT" # a failed gate must not leave an older zip looking like this build's
test -x "$BIN" || { echo "ERROR: $BIN missing: build PLAT=miyoo first"; exit 1; }
test -f "$MAP" || { echo "ERROR: $MAP missing: run tools/m1_build.sh first"; exit 1; }
test -x "$TC-strip" || { echo "ERROR: $TC-strip missing: set MIYOO_TOOLCHAIN"; exit 1; }
rm -rf "$STAGE" "$WORK"; mkdir -p "$STAGE" "$WORK"
ICON="$ROOT/miyoo/icon.png"
TEMPLATES=("$ROOT"/miyoo/templates/*)

echo "--- gate 5: the testing unlocks ---"
UNL=$(LC_ALL=C grep -a -o 'GE_TEST_UNLOCKS=[01]' "$BIN" | head -1 || true)
case "$UNL" in
    GE_TEST_UNLOCKS=0) echo "unlocks: off by default (release)" ;;
    GE_TEST_UNLOCKS=1) if [ "${RELEASE:-0}" = 1 ]; then
                           echo "ERROR: RELEASE=1 but every mission and cheat is unlocked by default (GE_TEST_UNLOCKS=1)."
                           echo "       Set GE_TEST_UNLOCKS to 0 in port/src/game/front.c and rebuild."; exit 1
                       fi
                       echo "unlocks: ON by default (test build: every mission and cheat open)" ;;
    *) echo "ERROR: no GE_TEST_UNLOCKS marker in $BIN"; exit 1 ;;
esac

echo "--- gate 1: provenance of the code ---"
python3 - "$ROOT" <<'PY'
import glob, os, subprocess, sys
root = sys.argv[1]
port = os.path.join(root, 'port')
tracked = set(subprocess.run(['git', '-C', root, 'ls-files'], capture_output=True, text=True, check=True).stdout.split('\n'))
deps = set()
for d in glob.glob(os.path.join(port, 'build/miyoo/**/*.d'), recursive=True):
    text = open(d).read().replace('\\\n', ' ')
    for line in text.split('\n'):
        if ':' not in line:
            continue
        for p in line.split(':', 1)[1].split():
            if not p.startswith('/'):
                p = os.path.normpath(os.path.join(port, p))
            if p.startswith(port + '/'):
                deps.add(os.path.relpath(p, root))
if not deps:
    print('ERROR: no dependency files under port/build/miyoo'); sys.exit(1)
bad = sorted(p for p in deps if p not in tracked and not p.startswith('port/assets/') and not p.startswith('port/build/'))
if bad:
    print('ERROR: compiled from files git does not track:'); [print('  ' + p) for p in bad]; sys.exit(1)
print('%d source files, all tracked (port/assets tables: scrubbed below)' % len(deps))
PY
for f in miyoo/game.json miyoo/tools/miyoo_pack.py miyoo/icon.png "${TEMPLATES[@]#$ROOT/}"; do
    git -C "$ROOT" ls-files --error-unmatch "$f" >/dev/null 2>&1 || { echo "ERROR: $f is shipped or packs the zip but git does not track it"; exit 1; }
done
echo "packer, templates, game.json, icon: tracked"

echo "--- scrub the ROM tables, strip ---"
python3 "$ROOT/tools/romtables.py" --elf "$BIN" --map "$MAP" --rom "$ROM" --src "$ROOT/port" \
    --out-elf "$WORK/ge007_scrubbed" --out-recipes "$STAGE/recipes.bin" --report "$WORK/romtables.txt"
"$TC-strip" --strip-all --remove-section=.comment -o "$STAGE/ge007" "$WORK/ge007_scrubbed"
chmod 755 "$STAGE/ge007"

echo "--- gate 2: leak scan of every shipped file against the ROM ---"
python3 "$ROOT/tools/leak_scan.py" --rom "$ROM" --map "$MAP" --src "$ROOT/port" --elf "$WORK/ge007_scrubbed" \
    --cap $((48 * 1024)) "$STAGE"/* "$ICON" "${TEMPLATES[@]}" | tee "$WORK/leak.txt" | grep -v '^    ' || true
grep -q '^leak scan: pass' "$WORK/leak.txt" || { echo "ERROR: leak scan failed (m2scratch/miyoo_work/leak.txt)"; exit 1; }

echo "--- gate 3: no host paths or authoring metadata ---"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/Volumes/' "$STAGE"/* "$ICON" "${TEMPLATES[@]}" "$GAME"; then
    echo "ERROR: identifying metadata in the files above"; exit 1
fi
echo clean

echo "--- gate 4: only what the game needs ---"
python3 - "$STAGE" "$GAME" <<'PY'
import json, os, sys
stage, game = sys.argv[1], json.load(open(sys.argv[2]))
want, have = sorted(game['files']), sorted(os.listdir(stage))
bad = [n for n in have if n.lower().endswith(('.md', '.txt')) or n.upper().startswith(('README', 'NOTICE'))]
if have != want or bad:
    print('ERROR: staged %s, game.json "files" %s, text files %s' % (have, want, bad)); sys.exit(1)
print('%d files, exactly game.json\'s: %s' % (len(have), ' '.join(have)))
PY

echo "--- pack ---"
set -- "$GAME" --build "$STAGE" -o "$WORK/out.zip" --check
[ "$PERSONAL" = 1 ] && set -- "$@" --rom "$ROM"
python3 "$ROOT/miyoo/tools/miyoo_pack.py" "$@"

echo "--- gate 4 (the zip): App/GoldenEye holds only the game's files ---"
python3 - "$WORK/out.zip" "$GAME" "$PERSONAL" <<'PY'
import json, sys, zipfile
z, game, personal = zipfile.ZipFile(sys.argv[1]), json.load(open(sys.argv[2])), sys.argv[3] == '1'
app = 'App/%s/' % game['folder']
want = sorted(game['files'] + ['launch.sh', 'config.json', 'icon.png'] + (['rom.z64'] if personal else []))
have = sorted(n[len(app):] for n in z.namelist() if n.startswith(app))
rom = [n for n in z.namelist() if n.lower().endswith(('.z64', '.n64', '.v64'))]
bad = [n for n in z.namelist() if n.lower().endswith(('.md', '.txt')) or n.split('/')[-1].upper().startswith(('README', 'NOTICE'))]
if have != want or bad or (rom and not personal) or (personal and rom != [app + 'rom.z64']):
    print('ERROR: %s holds %s, want %s; ROM entries %s; text files %s' % (app, have, want, rom, bad)); sys.exit(1)
print('%s%s; %d entries in all' % (app, ' '.join(have), len(z.namelist())))
PY
mv "$WORK/out.zip" "$OUT"
ls -la "$OUT"
[ "$PERSONAL" = 1 ] && echo "$OUT holds your ROM: keep it to your own card, never share it."
exit 0
