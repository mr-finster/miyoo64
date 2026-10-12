# F-Zero X build environment: the MK64 image (../mario-kart-64/docker/Dockerfile,
# tag mk64hh:build: the Miyoo Mini toolchain, i386 multiarch + SDL2 for the -m32 desktop proxy,
# binutils-mips for the decomp) plus qemu for the ARM tools (tools/sweep.sh save-arm,
# tools/qprof.sh). Built by ./dev.sh when missing:
#   docker build --platform linux/amd64 -t fzxhh:build docker/
FROM --platform=linux/amd64 mk64hh:build
RUN apt-get update && apt-get install -y --no-install-recommends qemu-user \
    && rm -rf /var/lib/apt/lists/*
# qemu-user 7.2's plugin API header (QEMU_PLUGIN_VERSION 1, GPL-2.0-or-later), for
# tools/qprof_insn.c. The same file as ocarina-of-time's tools/qprof/qemu-plugin.h.
COPY qemu-plugin.h /opt/qemu-plugin/include/qemu/qemu-plugin.h
# qemu-arm -L root: the toolchain's sysroot, with no /mnt of its own (qemu would redirect the
# game's /mnt lookups into it).
RUN rmdir /opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot/mnt 2>/dev/null || true
ENV QEMU=qemu-arm \
    QEMU_SRC=/opt/qemu-plugin \
    ARMROOT=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot \
    PYTHON=/usr/bin/python3
