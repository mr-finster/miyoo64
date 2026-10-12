#!/bin/bash
# Bit-exact sweep: dump frames from a fixed set of scenes on the desktop proxy
# (square output, fixed RNG seed) into refs/sweep_<tag>/.
#   ./dev.sh 'cd /work/port && tools/handheld/sweep.sh <tag>'
#   tools/handheld/sweep.sh --cmp <tagA> <tagB>    (host or container: pixel diff)
# Renderer changes meant to be output-identical must show 0 differing pixels.
set -u
REFS=/work/refs
if [ "${1:-}" = "--cmp" ]; then
    [ -d "$REFS" ] || REFS="$(cd "$(dirname "$0")/../../../refs" && pwd)"
    python3 - "$REFS/sweep_$2" "$REFS/sweep_$3" <<'PY'
import os, sys
a, b = sys.argv[1], sys.argv[2]
tot = bad = 0
for scene in sorted(d for d in os.listdir(a) if os.path.isdir(os.path.join(a, d))):
    for f in sorted(os.listdir(os.path.join(a, scene))):
        pa, pb = os.path.join(a, scene, f), os.path.join(b, scene, f)
        if not os.path.exists(pb):
            print("missing", scene, f); bad += 1; continue
        da, db = open(pa, 'rb').read(), open(pb, 'rb').read()
        ha, hb = da.split(b'\n', 3), db.split(b'\n', 3)
        n = sum(1 for i in range(0, min(len(ha[3]), len(hb[3])), 3) if ha[3][i:i+3] != hb[3][i:i+3])
        tot += 1
        if n:
            bad += 1
            print("%-10s %s: %d px differ" % (scene, f, n))
print("%d frames, %d differ" % (tot, bad))
PY
    exit 0
fi
TAG=${1:?usage: sweep.sh <tag> | --cmp <a> <b>}
OUT=$REFS/sweep_$TAG
rm -rf "$OUT"; mkdir -p "$OUT"
cp build/hh-pc/embedded.bin build/hh-gen/
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_W=240 OOT_H=240 OOT_ASSETS=build/hh-gen OOT_DATA=/tmp/od OOT_SEED=1234
mkdir -p /tmp/od
run() { # name, from, every, exit, env...
    local name=$1 from=$2 every=$3 exitat=$4; shift 4
    mkdir -p "$OUT/$name"
    env "$@" OOT_EXIT_AT=$exitat OOT_DUMP_FROM=$from OOT_DUMP_EVERY=$every OOT_DUMP="$OUT/$name" \
        timeout -s KILL 400 ./build/hh-pc/oot > "$OUT/$name.log" 2>&1
}
run kokiri  60 30 121 OOT_ENTRANCE=0xEE
run deku    90 30 121 OOT_ENTRANCE=0x209
run field   90 30 121 OOT_ENTRANCE=0x17D
run fieldcd 90 30 121 OOT_ENTRANCE=0xCD
run market  90 30 121 OOT_ENTRANCE=0xB1
run title  200 100 601 OOT_AUTO=1
run pause  160 40 241 OOT_ENTRANCE=0xEE OOT_AUTO=1
ls "$OUT"/*/ | grep -c ppm
