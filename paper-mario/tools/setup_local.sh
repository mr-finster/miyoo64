#!/bin/sh
# setup_local.sh -- one-time setup on the Mac, the local counterpart of setup_env.sh (which rebuilds a
# cloud container). Run on the Mac from anywhere; idempotent; nothing here touches a ROM.
#   sh tools/setup_local.sh
# Makes, all untracked:
#   pm64hh:build      Docker image (docker/Dockerfile: a thin layer on mario-kart-64's mk64hh:build; ./dev.sh
#                     builds it when missing, ~2 min)
#   refs/papermario   pmret/papermario @ ee32192, blob-less clone (the bench compiles against its include/)
#   refs/hhroot       the qemu-arm sysroot (setup_env.sh hhroot, run in the container)
#   refs/qprof        the Ocarina of Time port's insnprof plugin, built in the container, and a link to Debian's
#                     qemu-arm 7.2, which has TCG plugins enabled (no qemu source build, unlike the cloud)
# Then builds the desktop proxy and the device binary as a check.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p refs/qprof

D=refs/papermario
[ -d "$D/.git" ] || git clone -q --filter=blob:none --no-checkout https://github.com/pmret/papermario "$D"
git -C "$D" checkout -q ee32192

# The plugin sources live in the Ocarina of Time port, next to this folder in the miyoo64 repository.
P=$ROOT/../ocarina-of-time/port/tools/qprof
for f in insnprof.c qemu-plugin.h symprof.py; do
    [ -f "refs/qprof/$f" ] && continue
    cp "$P/$f" "refs/qprof/$f"
done

./dev.sh 'sh tools/setup_env.sh hhroot &&
    gcc -shared -fPIC -O2 -Irefs/qprof refs/qprof/insnprof.c -o refs/qprof/insnprof.so &&
    ln -sfn /usr/bin/qemu-arm refs/qprof/qemu-arm &&
    make -s -C bench && make -s -C bench PLAT=miyoo && make -s -C bench PLAT=miyoo build/miyoo/pm64bench.sym &&
    ls -la bench/build/pc/pm64bench bench/build/miyoo/pm64bench'
echo "ok. The ROM (the user's own, never fetched) goes at the repo root, git-ignored; scenes: refs/cap/m1/*.pms"
