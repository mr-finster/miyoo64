#!/bin/sh
# setup_env.sh -- rebuild the working environment in a fresh (cloud) container. Idempotent: each
# step is skipped when its result is already there. Nothing here touches a ROM.
#   sh tools/setup_env.sh [all|base|sdk|hhroot|qprof|decomp]     (default: all but decomp)
# Installs / creates:
#   base    apt: gcc-multilib (the -m32 desktop proxy), qemu-user, squashfs-tools, mupen64plus
#           + dev headers (scene capture, tools/cap), glib/ninja (qemu build), pillow
#   sdk     checks the Miyoo Mini toolchain at MIYOO_TOOLCHAIN (default /opt/miyoomini-toolchain:
#           GCC 8.3, glibc 2.28, the device's SDL 1.2)
#   hhroot  refs/hhroot: a qemu-arm sysroot from the toolchain's. Under qemu, keep no mnt/ in it
#           (the bench searches /mnt for scenes).
#   qprof   refs/qprof: qemu-arm v8.2.2 built with --enable-plugins (stock Ubuntu qemu-user has
#           plugins compiled out: "unknown option 'plugin'") + the OoT port's insnprof.c plugin and
#           symprof.py, for tools/qprof.sh.
#   decomp  refs/papermario (pmret/papermario @ ee32192) with its venv, pigment64 0.4.2 and
#           crunch64-cli -- only for the matching build (M0) and Yay0/leak-scan work. Needs the
#           player's ROM copied to refs/papermario/ver/us/baserom.z64 by hand before ./configure us.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WHAT=${1:-default}
want() { [ "$WHAT" = all ] || [ "$WHAT" = "$1" ] || { [ "$WHAT" = default ] && [ "$1" != decomp ]; }; }
mkdir -p "$ROOT/refs"

if want base; then
    need=""
    for p in gcc-multilib qemu-user squashfs-tools mupen64plus-ui-console mupen64plus-rsp-hle \
             mupen64plus-video-rice libmupen64plus-dev libglib2.0-dev ninja-build python3-venv; do
        dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"
    done
    if [ -n "$need" ]; then
        echo "apt:$need"
        apt-get update -q >/dev/null 2>&1 || true
        DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends $need >/dev/null
    fi
    python3 -c "import PIL" 2>/dev/null || pip install -q pillow 2>/dev/null || pip install -q --break-system-packages pillow
    echo "base: ok"
fi

if want sdk; then
    MTC=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
    [ -x "$MTC/usr/bin/arm-linux-gnueabihf-gcc" ] || { echo "sdk: no Miyoo toolchain at $MTC (set MIYOO_TOOLCHAIN)"; exit 1; }
    "$MTC/usr/bin/arm-linux-gnueabihf-gcc" --version | head -1
fi

if want hhroot; then
    F=$ROOT/refs/hhroot S=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}/usr/arm-linux-gnueabihf/sysroot
    if [ ! -e "$F/lib/libc.so.6" ]; then
        mkdir -p "$F/lib" "$F/usr"
        cp -a "$S/lib/." "$F/lib/"
        ln -sfn "$S/usr/lib" "$F/usr/lib"
    fi
    rmdir "$F/mnt" 2>/dev/null || true
    echo "hhroot: $F"
fi

if want qprof; then
    Q=$ROOT/refs/qprof
    mkdir -p "$Q"
    if [ ! -x "$Q/qemu-arm" ]; then
        [ -d "$Q/qemu-src" ] || git clone -q --depth 1 --branch v8.2.2 https://github.com/qemu/qemu.git "$Q/qemu-src"
        mkdir -p "$Q/qemu-src/b"
        (cd "$Q/qemu-src/b" && ../configure --target-list=arm-linux-user --enable-plugins --disable-docs \
            --disable-werror --disable-tools >/dev/null && ninja -j"$(nproc)" qemu-arm >/dev/null)
        cp "$Q/qemu-src/b/qemu-arm" "$Q/qemu-arm"
    fi
    if [ ! -f "$Q/insnprof.so" ]; then
        # the plugin lives in the Ocarina of Time port, next to this folder in the miyoo64 repository
        P=$ROOT/../ocarina-of-time/port/tools/qprof
        [ -f "$P/insnprof.c" ] || { echo "qprof: no $P/insnprof.c"; exit 1; }
        for f in insnprof.c qemu-plugin.h symprof.py; do cp "$P/$f" "$Q/$f"; done
        gcc -shared -fPIC -O2 -I"$Q" "$Q/insnprof.c" -o "$Q/insnprof.so"
    fi
    echo "qprof: QEMU=$Q/qemu-arm PLUGIN=$Q/insnprof.so"
fi

if want decomp; then
    D=$ROOT/refs/papermario
    [ -d "$D/.git" ] || git clone -q https://github.com/pmret/papermario "$D"
    git -C "$D" checkout -q ee32192
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q binutils-mips-linux-gnu cpp-mips-linux-gnu \
        gcc-mips-linux-gnu zlib1g-dev libyaml-dev >/dev/null
    [ -d "$D/.venv" ] || (cd "$D" && python3 -m venv .venv && . .venv/bin/activate && \
        pip install -q -U pip setuptools wheel && pip install -q -r tools/configure/requirements.txt)
    command -v cargo >/dev/null || . "$HOME/.cargo/env"
    command -v pigment64 >/dev/null || cargo install -q pigment64 --version 0.4.2
    command -v crunch64 >/dev/null || cargo install -q crunch64-cli
    echo "decomp: $D (put the player's ROM at ver/us/baserom.z64, then: . .venv/bin/activate; ./configure us; ninja)"
fi
