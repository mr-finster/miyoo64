#!/bin/bash
# The Miyoo build's second core, checked frame for frame under qemu-arm (which runs the worker thread for real; OoT's
# port/tools/miyoo_wkcheck.sh for Paper Mario): every scene below drawn three ways -- one core (PM64_THREADS=1),
# the default split (PM64_SPLIT 64 px, 4 rows), and nearly every triangle split (PM64_SPLIT=1 PM64_SPLITROWS=2) -- and
# every dumped frame (the renderer's own 320x240, PM64_SHOTS_RAW=1) compared byte for byte. Any difference is a bug.
#   tools/port/miyoo_wkcheck.sh <your ROM> [out dir] [frames] [every]      (after tools/port/miyoo.sh built build/game/miyoo)
# Frames stay under the out dir (default refs/miyoo_wkcheck): they are ROM-derived, never commit them. QJOBS=<n> runs n
# at a time (default 2), at low priority. Sound off (SDL_AUDIODRIVER=none): the game then makes its samples by the
# retrace count, so every run steps the same. Under qemu the timing says nothing about the device.
set -u
ROM=${1:?usage: miyoo_wkcheck.sh <ROM> [out dir] [frames] [every]}
P=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${2:-$P/refs/miyoo_wkcheck}; FRAMES=${3:-330}; EVERY=${4:-5}; J=${QJOBS:-2}
BIN=$P/build/game/miyoo/pm64
SR=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/arm-linux-gnueabihf/sysroot
test -x "$BIN" || { echo "no $BIN: run tools/port/miyoo.sh first"; exit 1; }
command -v qemu-arm >/dev/null || { echo "qemu-arm missing (apt install qemu-user)"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/tmpl"
# the game data once (the first start: assets.bin from the ROM with recipes.bin), shared by every run
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=none PM64_DATA="$OUT/tmpl" PM64_ROM="$ROM" PM64_THREADS=1 PM64_FRAMES=1 \
    PM64_LOG=0 PM64_PRIO=0 timeout -s KILL 1200 nice -n 15 qemu-arm -L "$SR" "$BIN" > "$OUT/tmpl.log" 2>&1
test -f "$OUT/tmpl/assets.bin" || { echo "the first start failed: $OUT/tmpl.log"; exit 1; }
run() { # mode scene envs...
    local mode=$1 name=$2; shift 2
    local d="$OUT/$mode/$name" w="$OUT/work_${mode}_$name"
    mkdir -p "$d" "$w"; ln -s "$OUT/tmpl/assets.bin" "$w/assets.bin"
    case $mode in
        one) set -- PM64_THREADS=1 "$@" ;;
        all) set -- PM64_SPLIT=1 PM64_SPLITROWS=2 "$@" ;;
    esac
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=none PM64_DATA="$w" PM64_ROM="$ROM" PM64_SEED=1234 PM64_PRIO=0 "$@" \
        PM64_FRAMES=$FRAMES PM64_SHOTS_EVERY=$EVERY PM64_SHOTS_RAW=1 PM64_SHOTS="$d" \
        timeout -s KILL 3600 nice -n 15 qemu-arm -L "$SR" "$BIN" > "$d.log" 2>&1
    cp "$w/console.log" "$d.console.log" 2>/dev/null; rm -rf "$w"
}
SCENES=${SCENES:-'attract|PM64_X=0
goomba|PM64_MAP=kmr_02
toadtown|PM64_MAP=mac_00
castle|PM64_MAP=kkj_00
koopa|PM64_MAP=nok_01
desert|PM64_MAP=sbk_02
battle1|PM64_BATTLE=0x0100
battle2|PM64_BATTLE=0x0400
battle3|PM64_BATTLE=0x0B00'}
for mode in one split all; do
    n=0
    while IFS='|' read -r name envs; do
        run $mode "$name" $envs &
        n=$((n + 1)); [ $((n % J)) -eq 0 ] && wait
    done <<< "$SCENES"
    wait
done
python3 - "$OUT" <<'PY'
import os, re, sys
out = sys.argv[1]
scenes = sorted(os.listdir(os.path.join(out, 'one')))
scenes = [s for s in scenes if os.path.isdir(os.path.join(out, 'one', s))]
total = {m: [0, 0] for m in ('split', 'all')}
for s in scenes:
    one = sorted(f for f in os.listdir(os.path.join(out, 'one', s)) if f.endswith('.ppm'))
    row = [s, '%d frames' % len(one)]
    for m in ('split', 'all'):
        d = os.path.join(out, m, s)
        other = sorted(f for f in os.listdir(d) if f.endswith('.ppm')) if os.path.isdir(d) else []
        diff = sum(1 for f in one if f not in other or open(os.path.join(out, 'one', s, f), 'rb').read() != open(os.path.join(d, f), 'rb').read())
        diff += sum(1 for f in other if f not in one)
        total[m][0] += len(one); total[m][1] += diff
        log = open(os.path.join(out, m, s + '.console.log'), errors='replace').read() if os.path.exists(os.path.join(out, m, s + '.console.log')) else ''
        sp = re.findall(r'mt on split (\d+)/(\d+) tris (\d+)% px', log)
        row.append('%s %d differ%s' % (m, diff, (' (split %s%% px)' % '/'.join(x[2] for x in sp[-3:])) if sp else ''))
    print('  '.join(row))
for m in ('split', 'all'):
    print('one core vs %s: %d frames, %d differ' % (m, total[m][0], total[m][1]))
ok = all(t[0] > 0 and t[1] == 0 for t in total.values())
sys.exit(0 if ok else 1)
PY
