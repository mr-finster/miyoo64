#!/bin/sh
# Identify a player-supplied Super Smash Bros. ROM and check it against the
# hashes ssb-decomp-re expects. Reads the file only; never copies or uploads it.
# Accepts .z64 (big-endian), .v64 (byte-swapped) or .n64 (little-endian) and
# reports the hash of the normalised .z64 form, which is what the decomp needs.
# Usage: tools/verify_rom.sh /path/to/rom [--link-into refs/ssb-decomp-re]
set -eu
ROM=${1:?usage: verify_rom.sh ROM [--link-into DECOMP_DIR]}
US_SHA1=e2929e10fccc0aa84e5776227e798abc07cedabf
JP_SHA1=4b71f0e01878696733eefa9c80d11c147ecb4984

magic=$(head -c 4 "$ROM" | od -An -tx1 | tr -d ' \n')
case "$magic" in
	80371240) fmt=z64; conv=cat ;;
	37804012) fmt=v64; conv="dd conv=swab status=none" ;;
	40123780) fmt=n64; conv="python3 -c import\ sys,array;a=array.array('I',sys.stdin.buffer.read());a.byteswap();sys.stdout.buffer.write(a.tobytes())" ;;
	*) echo "not an N64 ROM (magic $magic)" >&2; exit 2 ;;
esac
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
if [ "$fmt" = n64 ]; then
	python3 -c "import sys,array;a=array.array('I',open(sys.argv[1],'rb').read());a.byteswap();sys.stdout.buffer.write(a.tobytes())" "$ROM" > "$tmp"
else
	sh -c "$conv" < "$ROM" > "$tmp"
fi
sha1=$(sha1sum "$tmp" | cut -d' ' -f1)
size=$(wc -c < "$tmp")
title=$(dd if="$tmp" bs=1 skip=32 count=20 status=none | tr -d '\0')
code=$(dd if="$tmp" bs=1 skip=59 count=4 status=none)
case "$sha1" in
	$US_SHA1) ver=us ;;
	$JP_SHA1) ver=jp ;;
	*) ver=unknown ;;
esac
echo "format=$fmt size=$size title='$title' code=$code sha1=$sha1 version=$ver"
[ "$ver" = unknown ] && { echo "sha1 matches neither US nor JP; the decomp will not build from it" >&2; exit 1; }
if [ "${2:-}" = --link-into ]; then
	dest="${3:?decomp dir}/baserom.$ver.z64"
	if [ "$fmt" = z64 ]; then ln -sf "$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")" "$dest"
	else cp "$tmp" "$dest"; fi
	echo "baserom placed at $dest (untracked, inside refs/)"
fi
