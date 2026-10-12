#!/usr/bin/env bash
# M1: build GoldenEye 007 (NTSC-U) byte-for-byte from the n64decomp/007 code checkout
# (refs/007, tools/fetch_decomp.sh) plus the player's own ROM -- without any of the
# ROM-derived asset data the decomp repo commits. Everything game-derived comes from the
# ROM given here:
#   1. the decomp's extractor, run on EVERY row of scripts/filelist.u.csv (the repo marks
#      203 rows "don't extract" because it commits C versions of them);
#   2. text/brief files get the .bin names the Makefile expects; 5 names in the Makefile's
#      lists that the retail ROM never links get empty stand-ins;
#   3. the decomp's own scripts convert prop/chr/gun models to C;
#   4. tools/m1_segments.py writes the 9 data segments the repo builds from committed C as
#      .incbin of the ROM's own ranges, with symbol offsets from headers, names and
#      tools/m1_symbols.json (recovered by tools/m1_recover_syms.py);
#   5. make, then the sha1 must equal the retail ROM's.
# Work tree: m1/ge007 (git-free copy of refs/007; untracked). Takes ~10 min on 6 cores.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROM="${1:?usage: tools/m1_build.sh <path to your GoldenEye 007 (USA) dump>}"
W="$ROOT/m1/ge007"
rm -rf "$W"; mkdir -p "$ROOT/m1"
rsync -a --exclude .git "$ROOT/refs/007/" "$W/"
python3 "$ROOT/tools/verify_rom.py" "$ROM" --write-z64 "$W/baserom.u.z64"

"$ROOT/dev.sh" 'cd /work/m1/ge007 && make -s -C tools >/work/m1/tools.log 2>&1 \
  && ./scripts/extract_baserom.u.sh >/work/m1/extract.log 2>&1 \
  && awk -F, "BEGIN{OFS=\",\"} {\$5=1; print}" scripts/filelist.u.csv >/work/m1/filelist.all.u.csv \
  && tools/extractor/extractor baserom.u.z64 /work/m1/filelist.all.u.csv >/work/m1/extract_all.log 2>&1'

cd "$W"
for d in text brief; do
  for f in assets/obseg/$d/*; do
    case "$f" in *.bin|*.ld|*.md) ;; *) [ -f "$f" ] && ln -f "$f" "$f.bin" ;; esac
  done
done
for f in setup/Ump_setupdamZ setup/Ump_setupdepotZ setup/Ump_setupdestZ setup/Ump_setuprunZ stan/Tbg_sho_all_p_stanZ; do
  : > "assets/obseg/$f.bin"
done
python3 "$ROOT/tools/m1_segments.py" "$W"

"$ROOT/dev.sh" 'cd /work/m1/ge007 && make convert_props convert_chrs convert_guns >/work/m1/convert.log 2>&1 \
  && make create_directories >/work/m1/make.log 2>&1 \
  && make -j6 build/u/assets/font/fontBankGothic.o build/u/assets/font/fontZurichBold.o >>/work/m1/make.log 2>&1 \
  && make -j6 >>/work/m1/make.log 2>&1; sha1sum build/u/ge007.u.z64 baserom.u.z64 || true'
[ "$(shasum "$W/build/u/ge007.u.z64" | cut -c1-40)" = abe01e4aeb033b6c0836819f549c791b26cfde83 ] \
  && echo "M1 OK: build/u/ge007.u.z64 matches the retail NTSC-U ROM byte for byte" \
  || { echo "M1 FAILED: see m1/make.log"; exit 1; }
