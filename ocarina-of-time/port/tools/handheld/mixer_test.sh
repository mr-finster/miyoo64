#!/bin/sh
# Build tools/handheld/mixer_test.c for the handheld and run it under qemu-arm (in the container).
set -e
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc -O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 \
    -std=gnu11 -fwrapv -o /tmp/mixer_test /work/port/tools/handheld/mixer_test.c
qemu-arm -L $SR /tmp/mixer_test
