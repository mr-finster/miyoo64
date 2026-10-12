#!/bin/bash
# The Miyoo build's second core, checked frame for frame under qemu-arm (which runs the worker thread for real):
# every scene below drawn three ways -- one core (OOT_THREADS=1), the default split (OOT_SPLITPX, 800), and every
# triangle of two rows or more split (OOT_SPLITPX=1 OOT_SPLITROWS=2) -- and every dumped frame compared byte for byte.
# Any difference is a bug.
#   port/tools/handheld/miyoo_wkcheck.sh <your ROM> [out dir] [every] [exit]     (after make -f Makefile.handheld PLAT=miyoo)
# Frames stay under the out dir (default /tmp/miyoo_wkcheck): they are ROM-derived, never commit them.
# QJOBS=<n> runs n at a time (default 2). Under qemu the timing says nothing about the device (the handoff alone costs
# tens of microseconds there); the device A/B is debug.cfg threads=1.
set -u
ROM=${1:?usage: miyoo_wkcheck.sh <ROM> [out dir] [every] [exit]}
OUT=${2:-/tmp/miyoo_wkcheck}; EVERY=${3:-5}; EXIT=${4:-301}; J=${QJOBS:-2}
P=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$P/build/hh-miyoo/oot
SR=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/arm-linux-gnueabihf/sysroot
test -x "$BIN" || { echo "no $BIN: build PLAT=miyoo first"; exit 1; }
command -v qemu-arm >/dev/null || { echo "qemu-arm missing (apt install qemu-user)"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/tmpl"
# the game data once (the first start, from the ROM), shared read-only by every run
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_DATA="$OUT/tmpl" OOT_ROM="$ROM" OOT_THREADS=1 OOT_EXIT_AT=1 \
    timeout -s KILL 600 qemu-arm -L "$SR" "$BIN" > "$OUT/tmpl.log" 2>&1
test -f "$OUT/tmpl/data/segments/oot_psp_assets.bin" || { echo "the first start failed: $OUT/tmpl.log"; exit 1; }
run() { # mode scene envs...
    local mode=$1 name=$2; shift 2
    local d="$OUT/$mode/$name" w="$OUT/work_${mode}_$name"
    mkdir -p "$d" "$w"; ln -s "$OUT/tmpl/data" "$w/data"; cp "$OUT/tmpl/embedded.bin" "$w/"
    case $mode in
        one) set -- OOT_THREADS=1 "$@" ;;
        all) set -- OOT_SPLITPX=1 OOT_SPLITROWS=2 "$@" ;;
    esac
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_DATA="$w" OOT_ROM="$ROM" OOT_SEED=1234 "$@" \
        OOT_EXIT_AT=$EXIT OOT_DUMP_EVERY=$EVERY OOT_DUMP="$d" timeout -s KILL 3000 qemu-arm -L "$SR" "$BIN" > "$d.log" 2>&1
    cp "$w/console.log" "$d.console.log" 2>/dev/null; rm -rf "$w"
}
SCENES='kokiri|OOT_ENTRANCE=0xEE OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
deku|OOT_ENTRANCE=0x209 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
dekuin|OOT_ENTRANCE=0x0 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
field|OOT_ENTRANCE=0xCD OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
fieldn|OOT_ENTRANCE=0xCD OOT_DAYTIME=0x0000 OOT_TIMELOCK=0x0000
market|OOT_ENTRANCE=0xB1 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
kakariko|OOT_ENTRANCE=0xDB OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
zora|OOT_ENTRANCE=0x108 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
dodongo|OOT_ENTRANCE=0x4 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
gohma|OOT_ENTRANCE=0x40F OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
tot|OOT_ENTRANCE=0x53 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
lostwoods|OOT_ENTRANCE=0x11E OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
goron|OOT_ENTRANCE=0x14D OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
jabu|OOT_ENTRANCE=0x28 OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
dmtrail|OOT_ENTRANCE=0x13D OOT_DAYTIME=0x8000 OOT_TIMELOCK=0x8000
title|OOT_AUTO=1'
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
bad = 0
for mode in ('split', 'all'):
    tot = diff = 0
    for scene in sorted(os.listdir(os.path.join(out, 'one'))):
        a = os.path.join(out, 'one', scene)
        if not os.path.isdir(a):
            continue
        b = os.path.join(out, mode, scene)
        fa = sorted(f for f in os.listdir(a) if f.endswith('.ppm'))
        fb = sorted(f for f in os.listdir(b) if f.endswith('.ppm')) if os.path.isdir(b) else []
        if fa != fb:
            print('  %s %s: frame lists differ (%d vs %d)' % (mode, scene, len(fa), len(fb))); bad += 1
        d = [f for f in set(fa) & set(fb) if open(os.path.join(a, f), 'rb').read() != open(os.path.join(b, f), 'rb').read()]
        tot += len(set(fa) & set(fb)); diff += len(d)
        if d:
            print('  %s %s: %d frames differ, e.g. %s' % (mode, scene, len(d), sorted(d)[:3]))
        log = os.path.join(out, mode, scene + '.console.log')
        wk = re.findall(r'\| wk [^\n]*', open(log).read()) if os.path.exists(log) else []
        if mode == 'split' and wk:
            print('  %-10s %s' % (scene, wk[-1]))
    print('%s vs one core: %d frames compared, %d differ' % (mode, tot, diff))
    bad += diff
sys.exit(1 if bad else 0)
PY
