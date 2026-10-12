#!/bin/sh
# The release icon (the user's call, 2026-09-26): the game's own gold balloon, the HUD's balloon
# counter sprite (28x32, texture 85 in the hub), cut from the player's ROM into miyoo/icon_game.png
# (untracked: the .gitignore keeps it out of git) at 1:1, centred on a transparent 32x32.
# Unused since 2026-10-06 (the release icon is miyoo/icon.png, drawn). Run in the container after `make PLAT=pc`:
#   ./dev.sh sh tools/make_icon.sh
# DATA (default refs/data) holds dkr_assets.bin from the player's ROM.
set -eu
cd "$(dirname "$0")/.."
D=${DATA:-refs/data}; T=build/icon_tex
[ -f "$D/dkr_assets.bin" ] || { echo "need $D/dkr_assets.bin"; exit 1; }
rm -rf "$T" "$T.data"; mkdir -p "$T"; cp -r "$D" "$T.data"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy DKR_DATA="$T.data" DKR_FAST=1 DKR_W=240 DKR_H=240 \
    DKR_LEVEL=0,1,0 DKR_TEXDUMP="$T" DKR_EXIT_AT=200 build/pc/dkr > /dev/null 2>&1
python3 - "$T" miyoo/icon_game.png <<'PY'
import glob, hashlib, struct, sys, zlib
src = glob.glob(sys.argv[1] + '/tex_*_id85_28x32.ppm')
assert len(src) == 1, 'the balloon texture (id 85, 28x32) was not uploaded'
def pnm(path):
    d = open(path, 'rb').read(); f = d.split(maxsplit=4)
    return int(f[1]), int(f[2]), f[4]
w, h, rgb = pnm(src[0]); _, _, a = pnm(src[0][:-4] + '_a.pgm')
px = [(rgb[3*i], rgb[3*i+1], rgb[3*i+2], a[i]) for i in range(w * h)]
cols = [x for x in range(w) if any(px[y*w+x][3] for y in range(h))]
rows = [y for y in range(h) if any(px[y*w+x][3] for x in range(w))]
x0, x1, y0, y1 = cols[0], cols[-1] + 1, rows[0], rows[-1] + 1
ox, oy = (32 - (x1 - x0)) // 2, (32 - (y1 - y0)) // 2
out = [[(0, 0, 0, 0)] * 32 for _ in range(32)]
for y in range(y0, y1):
    for x in range(x0, x1):
        out[oy + y - y0][ox + x - x0] = px[y*w+x]
raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in out)
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 6, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
# the pixels of the icon the user saw and picked (2026-09-26), as a hash
print('icon pixels sha256', hashlib.sha256(raw).hexdigest()[:16])
PY
rm -rf "$T" "$T.data"
echo "wrote miyoo/icon_game.png"
