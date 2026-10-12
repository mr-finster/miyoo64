#!/usr/bin/env bash
# Derive oot_recipes.bin for one link (Makefile.handheld, after embedded.bin): the
# region's symbols, its base, the executable's address range (pointer literals must
# fall inside it) and the link's stamp, then tools/handheld/hh_recipes.c.
#   recipes.sh <nm> <readelf> <oot_embedded.elf> <build_stamp.c> <asset_segments.c> \
#              <baserom dir> <oot_psp_assets.bin> <embedded.bin> <tool> <out>
set -euo pipefail
NM=$1 READELF=$2 ELF=$3 STAMP_C=$4 SEGS=$5 BASEROM=$6 PACK=$7 EMB=$8 TOOL=$9 OUT=${10}
SYMS=$OUT.syms
$NM -S "$ELF" > "$SYMS"
BASE=0x$($NM "$ELF" | awk '$3 == "__hhassets_start" { print $1 }')
LO=; HI=0
while read -r va msz; do
    [ -n "$LO" ] || LO=$va
    e=$(( va + msz )); [ $e -gt $HI ] && HI=$e
done < <($READELF -lW "$ELF" | awk '$1 == "LOAD" { print $3, $6 }')
STAMP=0x$(sed -n 's/.*hh_build_stamp = 0x\([0-9a-fA-F]*\);.*/\1/p' "$STAMP_C")
"$TOOL" "$SEGS" "$BASEROM" "$PACK" "$EMB" "$SYMS" "$BASE" "$LO" "$(printf '0x%x' $HI)" "$STAMP" "$OUT"
