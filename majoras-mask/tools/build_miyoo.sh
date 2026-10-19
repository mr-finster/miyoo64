#!/usr/bin/env bash
# The public zip, MajorasMask-miyoo.zip: ONE zip that unzips onto the SD card's root and works on every Miyoo OS
# (miyoo/tools/miyoo_pack.py; miyoo/game.json). No ROM and nothing made from one: the first start builds the data
# pack on the device from the player's own ROM with mm_recipes.bin.
#   tools/build_miyoo.sh <build tree>          (the tree tools/setup.sh made, built)
# OUT_DIR=<dir> puts the zip there (default: this folder, where git ignores it).
#
# The release gates, all fatal, on the files that ship:
#   R. mm_recipes.bin belongs to this build: derived again from the tree's pack and ROM (hh/tools/mm_recipes.py),
#      byte for byte;
#   2. leak scan: every 32-byte block of the ROM and of the decompressed ROM (blocks with fewer than 8 distinct
#      byte values skipped), against every byte offset of every shipped file. mm_recipes.bin: none at all (op
#      numbers only). The executable: at most 256 (8 KB: tables the decompilation's own sources compile to, as
#      in the other ports);
#   M. no authoring metadata or host paths in anything that ships;
#   3. exactly the files miyoo/game.json lists; then the zip's own listing (no ROM, no pack, no text).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE=$(cd "${1:?build tree}" && pwd)
OUT_DIR="${OUT_DIR:-$HERE}"
OUT="$OUT_DIR/MajorasMask-miyoo.zip"
rm -f "$OUT" # first: a failed gate must not leave an older zip under this build's name
TC="${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf-"
GAME="$HERE/miyoo/game.json"
ROM="$TREE/baseroms/n64-us/baserom.z64"
for f in "$TREE/build/hh-miyoo/mm" "$TREE/build/hh-pack.bin" "$TREE/build/mm_recipes.bin" "$ROM"; do
    test -f "$f" || { echo "ERROR: $f missing: run tools/setup.sh first"; exit 1; }
done

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
"${TC}strip" -o "$STAGE/mm" "$TREE/build/hh-miyoo/mm"
chmod +x "$STAGE/mm"
cp "$TREE/build/mm_recipes.bin" "$STAGE/mm_recipes.bin"

echo "gate R: mm_recipes.bin is this pack's"
python3 "$HERE/hh/tools/mm_recipes.py" "$ROM" "$TREE/build/hh-pack.bin" "$STAGE/derived.bin" | sed 's/^/  /'
cmp -s "$STAGE/derived.bin" "$STAGE/mm_recipes.bin" || { echo "FAIL: the shipped mm_recipes.bin is not this pack's"; exit 1; }
rm "$STAGE/derived.bin"
echo "  derived again, byte for byte"

ICON="$(dirname "$GAME")/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["icon"])' "$GAME")"
SHIPPED=("$STAGE/mm" "$STAGE/mm_recipes.bin" "$ICON" "$HERE"/miyoo/templates/* "$GAME")

echo "gate 2: leak scan against the ROM and the decompressed ROM"
python3 - "$ROM" "$TREE/baseroms/n64-us/baserom-decompressed.z64" "${SHIPPED[@]}" <<'PY'
import sys, zlib
import numpy as np
M = np.uint64(0x9E3779B97F4A7C15)
def h4(q):
    x = q[:, 0] * M; x ^= q[:, 1]; x *= M; x ^= q[:, 2]; x *= M; x ^= q[:, 3]; x *= M
    return x
hs = []
for p in sys.argv[1:3]:
    raw = open(p, 'rb').read()
    n = len(raw) // 32 * 32
    b = np.frombuffer(raw[:n], np.uint8).reshape(-1, 32)
    srt = np.sort(b, axis=1)
    distinct = 1 + (np.diff(srt.astype(np.int16), axis=1) != 0).sum(1)
    hs.append(h4(np.frombuffer(raw[:n], '<u8').reshape(-1, 4)[distinct >= 8]))
H = np.unique(np.concatenate(hs))
bad = False
for f in sys.argv[3:]:
    d = open(f, 'rb').read()
    if f.endswith('mm_recipes.bin'):
        d = d[:64] + zlib.decompress(d[64:])  # the op stream too (header: "MMR1", 5 u32, sha1[40])
    hits = 0
    for o in range(8):
        m = (len(d) - o) // 8
        if m < 4:
            continue
        w = np.frombuffer(d[o:o + m * 8], '<u8')
        q = np.stack([w[i:len(w) - 3 + i] for i in range(4)], 1)
        hits += int(np.isin(h4(q), H).sum())
    budget = 0 if not f.endswith('/mm') else 256
    print('  %s: %d ROM blocks (budget %d)' % (f.rsplit('/', 1)[-1], hits, budget))
    bad |= hits > budget
sys.exit(1 if bad else 0)
PY

echo "gate M: no authoring metadata or host paths"
if grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/' "${SHIPPED[@]}"; then
    echo "FAIL: identifying metadata in the files above"; exit 1
fi
echo "  clean"

echo "gate 3: only the game's files"
WANT=$(python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["files"] + ["mm_recipes.bin"])))' "$GAME")
HAVE=$(cd "$STAGE" && ls -A | sort | tr '\n' ' ' | sed 's/ $//')
[ "$HAVE" = "$WANT" ] || { echo "FAIL: staged files '$HAVE' are not exactly '$WANT'"; exit 1; }
echo "  $HAVE"

echo "pack"
mkdir -p "$OUT_DIR"
mkdir "$STAGE/build" && mv "$STAGE/mm" "$STAGE/build/mm"
python3 "$HERE/miyoo/tools/miyoo_pack.py" "$GAME" --build "$STAGE/build" -o "$OUT" | sed 's/^/  /'
python3 - "$OUT" "$STAGE/mm_recipes.bin" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], 'a', zipfile.ZIP_DEFLATED)
zi = zipfile.ZipInfo('App/MajorasMask/mm_recipes.bin', (2020, 1, 1, 0, 0, 0))
zi.compress_type = zipfile.ZIP_DEFLATED
zi.external_attr = 0o644 << 16
z.writestr(zi, open(sys.argv[2], 'rb').read())
z.close()
PY
python3 - "$OUT" <<'PY'
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
app = 'App/MajorasMask/'
want = set(app + f for f in ['mm', 'mm_recipes.bin', 'launch.sh', 'config.json', 'icon.png', 'full_resolution'])
have = set(n for n in names if n.startswith(app))
bad = sorted(have ^ want)
bad += [n for n in names if n.lower().endswith(('.md', '.txt', '.z64', '.v64', '.n64', '.bin')) and n != app + 'mm_recipes.bin']
bad += [n for n in names if 'mm_pack' in n]
if bad:
    sys.exit('FAIL: the zip: ' + ', '.join(bad))
print('  %s: %d files, no ROM, no pack' % (sys.argv[1].rsplit('/', 1)[-1], len(names)))
PY
ls -l "$OUT"
