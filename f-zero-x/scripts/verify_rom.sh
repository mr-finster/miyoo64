#!/usr/bin/env bash
# verify_rom.sh - check the player's F-Zero X ROM against the hashes the decomp
# expects and, with --build, prove the decomp rebuilds it byte-for-byte.
#
#   scripts/verify_rom.sh /path/to/rom            # hash check only
#   scripts/verify_rom.sh /path/to/rom --build    # + matching decomp build (Docker)
#
# The ROM never leaves this machine and is never written inside the repo tree:
# the build copies it into refs/fzerox (untracked, and *.z64 is git-ignored there
# and here). Accepts .z64 (big-endian), .v64 (byteswapped) or .n64 (little-endian);
# hashes are always taken on the normalised .z64 byte order.
set -euo pipefail

DECOMP_URL=https://github.com/inspectredc/fzerox
DECOMP_REV=4b00f36ceeabfe657866a7d02f3411545b5f55eb   # pinned 2026-09-17, "Match func_8009DB28 and Credits_IntroDraw (#102)"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROM=${1:?usage: verify_rom.sh <rom> [--build]}
BUILD=${2:-}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
Z64=$TMP/rom.z64

magic=$(head -c4 "$ROM" | od -An -tx1 | tr -d ' \n')
case "$magic" in
  80371240) cp "$ROM" "$Z64" ;;
  37804012) python3 -c 'import sys;d=open(sys.argv[1],"rb").read();b=bytearray(d);b[0::2],b[1::2]=d[1::2],d[0::2];open(sys.argv[2],"wb").write(b)' "$ROM" "$Z64"; echo "note: .v64 (byteswapped) input, normalised to .z64" ;;
  40123780) python3 -c 'import sys,array;a=array.array("I",open(sys.argv[1],"rb").read());a.byteswap();open(sys.argv[2],"wb").write(a.tobytes())' "$ROM" "$Z64"; echo "note: .n64 (little-endian) input, normalised to .z64" ;;
  *) echo "error: not an N64 ROM (magic $magic)"; exit 1 ;;
esac

md5=$(md5sum "$Z64" 2>/dev/null | cut -d' ' -f1 || md5 -q "$Z64")
sha1=$( (sha1sum "$Z64" 2>/dev/null || shasum -a 1 "$Z64") | cut -d' ' -f1)
size=$(wc -c < "$Z64" | tr -d ' ')

# md5 from the decomp's *.md5 files; sha1 as listed by G-Diffuser / gdx-3ds.
ver=
case "$md5:$sha1" in
  753437d0d8ada1d12f3f9cf0f0a5171f:5f658e88ffa9de23cba6986a8fd3d3a90d7b4340) ver=us ;;
  58d200d43620007314304f4e6c9e6528:a418b0151521b76691fa03f8658c8b567c69498b) ver=jp ;;
  ee79a8fe287b5dcaea584439363342fc:af9038bcb543a2b4faaab876dbdab4663859e092) ver=pal ;;
esac
echo "size $size  md5 $md5  sha1 $sha1"
if [ -z "$ver" ]; then
  echo "RESULT: UNKNOWN ROM - not a clean F-Zero X US/JP/PAL rev0 dump (bad dump, hack or rev1?)"
  exit 2
fi
echo "RESULT: F-Zero X $ver rev0 - matches the decomp's expected hash"

[ "$BUILD" = "--build" ] || exit 0

DEC=$ROOT/refs/fzerox
if [ ! -d "$DEC/.git" ]; then
  git clone -q "$DECOMP_URL" "$DEC"
fi
git -C "$DEC" fetch -q origin "$DECOMP_REV" 2>/dev/null || true
git -C "$DEC" checkout -q "$DECOMP_REV"
git -C "$DEC" submodule update -q --init --recursive
cp "$Z64" "$DEC/baserom.$ver.rev0.z64"

# Linux container so the IDO recomp + mips binutils behave the same on macOS hosts.
docker run --rm -v "$DEC":/fzx -w /fzx ubuntu:24.04 bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq make cmake git build-essential binutils-mips-linux-gnu \
      python3 python3-pip python3-venv libpng-dev >/dev/null
  python3 -m venv /venv && /venv/bin/pip install -q -r tools/requirements-python.txt
  export PATH=/venv/bin:\$PATH
  make toolchain
  make VERSION=$ver init 2>&1 | tail -25
" | tee "$ROOT/refs/matching-build-$ver.log"
grep -q "z64.*OK" "$ROOT/refs/matching-build-$ver.log" && echo "MATCHING BUILD: OK ($ver)" || { echo "MATCHING BUILD: FAILED (see refs/matching-build-$ver.log)"; exit 3; }
