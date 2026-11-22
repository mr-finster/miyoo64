# Perfect Dark PAL for Miyoo

This alpha adds European `pal-final` support to Mr-Finster's native Miyoo
Perfect Dark port. The European ROM contains German and other European languages;
individual language settings have not all been tested. Other Perfect Dark ROM
versions are not automatically compatible with the PAL build.

Supply your own big-endian PAL final ROM at `Roms/N64/PerfectDarkPAL.z64`
on the SD card and extract the release ZIP at the card root. The app is installed
separately in `App/PerfectDarkPAL/`. The required ROM hashes are:

- MD5: `d9b5cd305d228424891ce38e71bc9213`
- SHA-1: `a663d3f4eee0b198471132db92e9639a9edd1985`

The launcher verifies the SHA-1; its fallback scanner can copy a verified ROM
into the app data directory. Do not distribute ROMs or extracted assets.

## Build and package

From `perfect-dark/`, use the toolchain described in README.md. The fetch is
pinned to PC-port commit `32a1cb9f268dd3ac73016801025c6bbbfa20130f`.
It supplies PAL asset metadata for locally generated headers; no ROM is needed
to build. PAL and NTSC objects and headers use separate directories.

```sh
ROMID=pal-final tools/fetch_assets.sh
ROMID=pal-final tools/gen_headers.sh
make -j4 ROMID=pal-final
mkdir -p build/zip-pal
/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-strip --strip-all --remove-section=.comment -o build/zip-pal/pd build/miyoo/pal-final/pd.arm
python3 miyoo/tools/miyoo_pack.py miyoo/game-pal.json --build build/zip-pal --layouts '' -o PerfectDarkPAL.zip --check
```

For USA, follow README.md and use the original `miyoo/game.json` and its own
`build/zip` staging directory. The PAL build uses the existing Miyoo renderer,
controller integration and PC-port PAL segment, file, language, font, audio and
video code.

## Validation and limitations

The M0.1 release was tested on a Miyoo Mini Plus with Onion OS: the app started,
gameplay was smooth and sound worked. A complete playthrough, long-term stability,
ordinary saves, save/load states, all language settings, menus and scenes have not
been fully tested. Earlier QEMU testing verified the PAL ROM and initialized PAL
segments and rendered the Nintendo intro; QEMU ran without audio.

The release ZIP is the hardware-tested M0.1 artifact. The source branch separates
PAL and USA packaging; a build from that branch has not been retested on hardware.
