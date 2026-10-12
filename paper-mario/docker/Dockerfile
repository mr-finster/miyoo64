# paper-mario build environment on the Mac: a thin layer on mario-kart-64's image (mk64hh:build =
# Debian bookworm x86_64, gcc-multilib for the -m32 desktop proxy, the Miyoo Mini toolchain at /opt/miyoomini-toolchain,
# squashfs-tools). Adds what this repo's tools need on top:
#   qemu-user            ARM runs of the device build (Debian's qemu-arm has TCG plugins enabled,
#                        so tools/qprof.sh uses it as is; no source build as in the cloud container)
#   mupen64plus + dev    scene capture from the player's ROM (tools/cap)
#   python3-pil          contact sheets (tools/cap/sheet.py)
#   zip, unzip           the release zips
#   clang, ninja-build   tools/port/miyoo.sh (the decomp's generated code, the game build)
# Build: docker build --platform linux/amd64 -t pm64hh:build docker   (dev.sh does it when missing)
FROM mk64hh:build
RUN apt-get update && apt-get install -y --no-install-recommends \
      qemu-user mupen64plus-ui-console mupen64plus-rsp-hle libmupen64plus-dev \
      python3-pil zip unzip clang ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
CMD ["/bin/bash"]
