# Diddy Kong Racing PAL 1.1 alpha

The PAL alpha supports **Diddy Kong Racing (Europe) (En,Fr,De) (Rev 1)**,
commonly called European PAL 1.1, with English, French and German. It requires
your own canonical big-endian `.z64` ROM (12,582,912 bytes), SHA-1
`b7f628073237b3d211d40406aa0884ff8fdd70d5`. No ROM or game data is included.

## Install the tested alpha package

1. Download the M0.7b hardware-test ZIP from the
   [v0.1.0-alpha prerelease](https://github.com/SunnyBoon/miyoo64/releases/tag/dkr-pal-v0.1.0-alpha).
   Its M0.7b name identifies the tested build; this is the alpha release asset.
2. Extract it at the root of the SD card. Place your exact ROM at
   `App/DiddyKongRacingPALM07b/rom.z64` and launch **Diddy Kong Racing PAL
   M07b** from Apps or Onion Ports.
3. Its settings, asset cache and saves are under
   `App/DiddyKongRacingPALM07b/data/pal-v80-m07b`. Keep these separate from
   US, M0.6 and M0.7a data; save states are specific to the binary that made
   them. The PAL loader validates the normalized ROM and cache before use.

The alpha was tested on a Miyoo Mini Plus: startup, title, options, racing,
HUD, minimap, right-edge correction and smooth performance were confirmed.
This is not a complete playthrough; split-screen and special views have not
been comprehensively validated. The viewport correction may also shift HUD
triangles or special views that share its exact viewport.

## PAL implementation and build

The PAL version uses the 50 Hz game path, PAL-specific ROM and asset-region
validation, and a 320x264 render target. Its output retains 4:3 display
proportions. The experimental full-screen PAL viewport adjustment moves the
matching 3D viewport four source pixels right without changing its width.

The published asset is the hardware-tested M0.7b binary from local source
commit `94736a0e07a30364763767d230dc3ea028f19b2a`; its embedded revision
string is `unknown+` because its build container did not see Git metadata.
The release asset bytes are unchanged. The current PR applies the PAL changes
to a newer upstream base that also contains subsequent DKR audio, input and
sleep/save changes. That integrated combination has not been tested on
hardware and is not claimed to be the released binary.

With the toolchain described in [README.md](README.md#building-the-miyoo-zip),
build the PAL M0.7b configuration with:

```sh
./dev.sh 'make PLAT=miyoo VERSION=pal_v80 PAL264=1 M07B=1 -j3'
```

The unstripped executable is `build/miyoo-pal-v80-m07b/dkr`.
[README_PAL_M07B.md](README_PAL_M07B.md) describes the packaging and checks;
its check script expects the locally preserved reference packages. The
standard PAL `VERSION=pal_v80` configuration remains distinct and does not
select this 264-line viewport experiment by itself.

The DKR decompilation is CC0; see `decomp/LICENSE.md`. The existing port's
other credits and license notices are listed in [README.md](README.md#credits).
