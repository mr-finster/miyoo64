# MK64 → Miyoo Mini build environment
# x86_64 Debian (runs under Colima vz + Rosetta on Apple Silicon).
# Bookworm = current Debian stable (bullseye's security pocket 404s now that it is
# oldstable). Newer glibc runs the toolchain's x86_64 binaries fine.
#
# Provides these build capabilities:
#   M1  N64 ROM build .......... mips-linux-gnu binutils + decomp tool deps
#   M2  desktop software render  native x86_64 gcc + SDL2 dev
#   Miyoo Mini cross build ..... /opt/miyoomini-toolchain (union toolchain: arm-linux-gnueabihf-gcc
#                                8.3, glibc 2.28, the device's SDL 1.2 in the sysroot)
FROM --platform=linux/amd64 debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# i386 multiarch: the mk64 decomp is 32-bit code (N64/PSP/Miyoo are all 32-bit).
# The desktop debug build is -m32 so it is a faithful proxy for the 32-bit device
# (pointer/size_t/struct widths match — the u32-vs-uintptr_t decl conflicts and the
# 64-bit-uncleanness only appear in a 64-bit build).
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc g++ make cmake pkg-config \
      gcc-multilib g++-multilib \
      git wget curl ca-certificates xxd file bsdmainutils less nano gdb valgrind \
      python3 python3-pip python3-venv python3-dev \
      squashfs-tools qemu-user \
      binutils-mips-linux-gnu \
      libaudiofile-dev libcapstone-dev zlib1g-dev libpng-dev \
      libsdl2-dev libsdl2-dev:i386 \
      fonts-open-sans xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Miyoo Mini: shauninman's union toolchain (the one Onion and MiniUI build with), x86_64 host.
RUN wget -q -O /tmp/mm.tar.xz https://github.com/shauninman/miyoomini-toolchain-buildroot/releases/download/v0.0.3/miyoomini-toolchain.tar.xz \
    && tar xJf /tmp/mm.tar.xz -C /opt && rm /tmp/mm.tar.xz \
    && test -x /opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc

WORKDIR /work
CMD ["/bin/bash"]
