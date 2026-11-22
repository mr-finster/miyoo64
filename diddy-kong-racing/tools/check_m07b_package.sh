#!/bin/bash
# Check the existing M0.7b archive. The ROM is read only and never packaged.
set -eu -o pipefail
cd "$(dirname "$0")/.."
archive=build/DiddyKongRacingPALM07b-UNSCANNED.zip
binary=build/miyoo-pal-v80-m07b/dkr
out=build/DiddyKongRacingPALM07b-HARDWARE-TEST.zip
rom=${1:-}
tc=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/bin/arm-linux-gnueabihf
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0
printf '%s\n' '--- archive integrity and paths ---'
python3 - "$archive" "$work/unpack" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    if z.testzip(): sys.exit('archive CRC failure')
    names=z.namelist()
    if len(names)!=len(set(names)): sys.exit('duplicate paths')
    for i in z.infolist():
        n=os.path.normpath(i.filename)
        if n.startswith('..') or os.path.isabs(n) or i.filename.startswith('App/DiddyKongRacingPAL/'):
            sys.exit('unsafe or old installation path: '+i.filename)
    for previous in ('DiddyKongRacingPAL-miyoo.zip', 'build/DiddyKongRacingPALM07a-HARDWARE-TEST.zip'):
        with zipfile.ZipFile(previous) as old:
            overlap={n for n in names if not n.endswith('/')} & set(old.namelist())
            if overlap: sys.exit('existing package file paths overlap with '+previous+': '+str(sorted(overlap)))
    z.extractall(sys.argv[2])
PY
printf '%s\n' '--- source provenance (M0.6 tracked-input gate) ---'
make -s PLAT=miyoo VERSION=pal_v80 PAL264=1 M07B=1 print-sources | tr ' ' '\n' | sed '/^$/d' > "$work/sources"
[ "$(wc -l < "$work/sources")" -ge 100 ] || { echo 'FAIL: source list incomplete'; exit 1; }
while IFS= read -r f; do
    case "$f" in build/miyoo-pal-v80-m07b/gen/trig_tables.c) continue ;; esac
    if ! git ls-files --error-unmatch "$f" >/dev/null 2>&1; then echo "UNTRACKED: $f"; failed=1; fi
done < "$work/sources"
for f in miyoo/game-pal-v80-m07b.json miyoo/tools/miyoo_pack.py miyoo/templates/* miyoo/icon.png; do
    if ! git ls-files --error-unmatch "$f" >/dev/null 2>&1; then echo "UNTRACKED: $f"; failed=1; fi
done
printf '%s\n' '--- ELF, exact stripped binary, and save-state regions ---'
"$tc-readelf" -h "$work/unpack/App/DiddyKongRacingPALM07b/dkr" > "$work/elf"
grep -E 'Class:|Machine:|Flags:' "$work/elf"
grep -Eq 'Class: +ELF32' "$work/elf"
grep -Eq 'Machine: +ARM' "$work/elf"
grep -Eq 'Flags:.*hard-float ABI' "$work/elf"
"$tc-readelf" -SW "$binary" | grep -E '\.game_bss +NOBITS' >/dev/null
cp "$binary" "$work/dkr"
"$tc-strip" --remove-section=.comment "$work/dkr"
cmp "$work/dkr" "$work/unpack/App/DiddyKongRacingPALM07b/dkr"
sym() { "$tc-nm" "$binary" | awk -v s="$1" '$3 == s && !found { print $1; found=1 }'; }
ds=$(sym __gamedata_start); de=$(sym __gamedata_end); bs=$(sym __gamebss_start); be=$(sym __gamebss_end)
[ -n "$ds" ] && [ -n "$de" ] && [ -n "$bs" ] && [ -n "$be" ]
"$tc-readelf" -rW "$binary" | awk -v ds="0x$ds" -v de="0x$de" -v bs="0x$bs" -v be="0x$be" '$3 == "R_ARM_COPY" {print $1, $5}' > "$work/copies"
while read -r addr name; do
    v=$((16#$addr))
    if (( (v >= 16#$ds && v < 16#$de) || (v >= 16#$bs && v < 16#$be) )); then echo "R_ARM_COPY inside save region: $name"; failed=1; fi
done < "$work/copies"
echo "game_data: $ds-$de; game_bss: $bs-$be"
printf '%s\n' '--- package contents and metadata ---'
[ "$(find "$work/unpack/App/DiddyKongRacingPALM07b" -maxdepth 1 -type f -printf '%f\n' | sort | tr '\n' ' ')" = 'config.json dkr full_resolution icon.png launch.sh ' ] || { echo 'unexpected app contents'; failed=1; }
if find "$work/unpack" -type f -print0 | xargs -0 grep -l -a -E 'xmpmeta|Photoshop|/Users/|/home/|/root/'; then echo 'metadata or host path found'; failed=1; fi
printf '%s\n' '--- PAL ROM and leak scan ---'
if [ -z "$rom" ] || [ ! -f "$rom" ]; then
    echo 'FAIL: canonical PAL ROM not supplied'; failed=1
elif [ "$(sha1sum "$rom" | cut -d' ' -f1)" != b7f628073237b3d211d40406aa0884ff8fdd70d5 ]; then
    echo 'FAIL: ROM SHA-1 mismatch'; failed=1
elif ! find "$work/unpack" -type f -print0 | xargs -0 python3 tools/leak_scan.py --version pal_v80 --rom "$rom"; then
    echo 'FAIL: leak scan'; failed=1
fi
printf '%s\n' '--- M0.6 hardware reference ---'
printf '%s  %s\n' d44e131ac635624c2737b3261fec38202eb249c59a56f04304a39397b285f680 build/m07a-reference/dkr-m06 | sha256sum -c - || failed=1
if [ "$failed" -ne 0 ]; then echo 'FAILED: no checked package created'; exit 1; fi
[ ! -e "$out" ] || { echo "Already exists: $out"; exit 1; }
cp "$archive" "$out"
sha256sum "$out"
python3 -m zipfile -l "$out"
echo 'PASS: checked hardware-test package created'
