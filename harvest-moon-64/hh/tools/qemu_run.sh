#!/bin/sh
# hh/tools/qemu_run.sh <outdir> <retraces> [dump_every] [keys]: build, pack, run under qemu-arm (one run at a time).
# From the decomp root. keys: HM64_KEYS, "<retrace>:<key>+|-,..." (a b z start l r cu cd cl cr du dd dl dr up down left right).
# Then: python3 hh/tools/sheet.py <outdir> <outdir>/sheet.png [cols]
set -e
make -f hh/Makefile.handheld -j4 >/dev/null
make -f hh/Makefile.handheld pack >/dev/null
OUT=$1; rm -rf "$OUT"; mkdir -p "$OUT/data"
S=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
SDL_VIDEODRIVER=dummy HM64_DATA=$OUT/data HM64_PACK=build/hh/hm64-pack.bin HM64_NOPACE=1 HM64_EXIT_AT=$2 \
  HM64_DUMP=$OUT HM64_DUMP_EVERY=${3:-30} HM64_KEYS="$4" timeout ${TMO:-1800} nice -n 19 qemu-arm -L $S build/hh/hm64 \
  > "$OUT/log.txt" 2>&1 || true
tail -3 "$OUT/log.txt"
