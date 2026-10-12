#!/bin/sh
# check_rom.sh <rom> -- identify a Paper Mario ROM against the pmret decomp's checksums.
# Converts .v64 (byteswapped) / .n64 (little-endian) dumps to .z64 order in a temp file first.
# Reads the ROM locally only; prints the version and nothing else about its contents.
set -eu
rom=${1:?usage: check_rom.sh <rom>}
[ -f "$rom" ] || { echo "skip: $rom is not a file"; exit 1; }
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
magic=$(od -An -tx1 -N4 "$rom" | tr -d ' \n')
case $magic in
  80371240) cp "$rom" "$tmp" ;;                                   # z64 (big-endian)
  37804012) python3 -c 'import sys;d=bytearray(open(sys.argv[1],"rb").read());d[0::2],d[1::2]=d[1::2],d[0::2];open(sys.argv[2],"wb").write(d)' "$rom" "$tmp" ;;  # v64
  40123780) python3 -c 'import sys,array;a=array.array("I",open(sys.argv[1],"rb").read());a.byteswap();open(sys.argv[2],"wb").write(a.tobytes())' "$rom" "$tmp" ;;  # n64
  *) echo "not an N64 ROM (magic $magic)"; exit 1 ;;
esac
sum=$(sha1sum "$tmp" | cut -d' ' -f1)
case $sum in
  3837f44cda784b466c9a2d99df70d77c322b97a0) v=us ;;
  b9cca3ff260b9ff427d981626b82f96de73586d3) v=jp ;;
  2111d39265a317414d359e35a7d971c4dfa5f9e1) v=pal ;;
  5c724685085eba796537573dd6f84aaddedc8582) v=ique ;;
  *) echo "sha1 $sum: NOT a known Paper Mario dump (bad dump or hack)"; exit 2 ;;
esac
echo "sha1 $sum: Paper Mario $v ($magic order) OK"
