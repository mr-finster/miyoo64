#!/bin/sh
# check_rom.sh <rom> [--install]
# Identifies a player-supplied Diddy Kong Racing ROM by SHA1 against the
# decomp's ver/verification table. Accepts .z64/.v64/.n64 byte orders; hashes
# the normalised big-endian (.z64) image. With --install, writes the .z64 copy
# into refs/Diddy-Kong-Racing/baseroms/ (untracked) for the matching build.
# Never commit, upload or print ROM bytes.
set -eu
rom=${1:?usage: check_rom.sh <rom> [--install]}
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

magic=$(od -An -tx1 -N4 "$rom" | tr -d ' \n')
case $magic in
  80371240) cp "$rom" "$tmp"; order=z64 ;;
  37804012) dd if="$rom" of="$tmp" conv=swab status=none; order=v64 ;;
  40123780) python3 - "$rom" "$tmp" <<'E'
import sys, array
a = array.array('I', open(sys.argv[1], 'rb').read()); a.byteswap()
open(sys.argv[2], 'wb').write(a.tobytes())
E
            order=n64 ;;
  *) echo "not an N64 ROM (magic $magic)"; exit 1 ;;
esac

sum=$(sha1sum "$tmp" | cut -d' ' -f1)
case $sum in
  0cb115d8716dbbc2922fda38e533b9fe63bb9670) ver=us.v77  ; name="US 1.0" ;;
  6d96743d46f8c0cd0edb0ec5600b003c89b93755) ver=us.v80  ; name="US 1.1" ;;
  dd5d64dd140cb7aa28404fa35abdcaba33c29260) ver=pal.v77 ; name="PAL 1.0" ;;
  b7f628073237b3d211d40406aa0884ff8fdd70d5) ver=pal.v80 ; name="PAL 1.1" ;;
  23ba3d302025153d111416e751027cef11213a19) ver=jpn.v79 ; name="JPN 1.0" ;;
  *) echo "unknown DKR ROM: sha1 $sum (byte order $order)"; exit 2 ;;
esac
echo "OK: $name ($ver), byte order $order, sha1 $sum, $(wc -c < "$tmp") bytes"

if [ "${2:-}" = "--install" ]; then
  dst="$here/refs/Diddy-Kong-Racing/baseroms/baserom.$ver.z64"
  cp "$tmp" "$dst"
  echo "installed (untracked): $dst"
fi
