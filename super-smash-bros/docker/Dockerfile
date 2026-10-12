# Super Smash Bros. build environment: the MK64 image (mario-kart-64
# docker/Dockerfile, tag mk64hh:build: Debian bookworm x86_64, gcc-multilib and
# SDL2 i386 for the -m32 desktop proxy, the Miyoo Mini toolchain at
# /opt/miyoomini-toolchain) plus what the ARM tests need. Build it once: dev.sh
# does it when it is missing.
#   docker build --platform linux/amd64 -t ssb64hh:build docker
FROM --platform=linux/amd64 mk64hh:build
RUN apt-get update && apt-get install -y --no-install-recommends \
      qemu-user python3-pil \
    && rm -rf /var/lib/apt/lists/*
# qemu-arm -L $ARMROOT runs the Miyoo binaries (tools/sweep.sh arm): the
# toolchain's sysroot has the device's glibc and SDL 1.2.
ENV ARMROOT=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
