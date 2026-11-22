> Historical M0.7a test notes. For the hardware-tested PAL alpha and its exact install path, see [README_PAL_V80.md](README_PAL_V80.md).

# PAL M0.7a isolated display test

This is an experimental Miyoo-only build, not a release. `PAL264=1` uses a
320x264 render target and maps its rows directly to the panel; it leaves the
PAL minimap coordinates untouched. The PAL display aspect is treated as 4:3,
so the taller pixel grid does not select the narrow-screen HUD adjustments.
On a 640x480 panel every source row is represented, with nearest-neighbour
rows repeated once or twice. Lines can have unequal apparent thickness. The
renderer has 10% more pixels than at 320x240, and the non-integral panel
scaling may also cost time; measure performance on the device.

The separate package uses `App/DiddyKongRacingPALM07a` and
`data/pal-v80-m07a` under that app. It does not share the existing PAL app's
settings, asset cache or saves. A matching Europe Rev 1 ROM is required; no ROM
is packaged. Save states are build-specific and must not be copied between
builds. Compare the same race and overworld view with matching settings and
independent in-game progress, and record picture, frame rate and console log.
Do not install until approved.

Build in the toolchain container:

    make PLAT=miyoo VERSION=pal_v80 PAL264=1 -j3
    mkdir -p build/m07a-package
    cp build/miyoo-pal-v80-m07a/dkr build/m07a-package/dkr
    /opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-strip --remove-section=.comment build/m07a-package/dkr
    python3 miyoo/tools/miyoo_pack.py miyoo/game-pal-v80-m07a.json --build build/m07a-package -o build/DiddyKongRacingPALM07a-UNSCANNED.zip

This direct test package does not run the release script's provenance, save
region, ROM leak and metadata gates; do not distribute it as a release.
The preserved reference is `build/m07a-reference/dkr-m06`, copied from the
existing package staging directory, with its SHA-256 in the adjacent file. Its
identity as the binary actually installed for the earlier hardware test needs
confirmation against the device.

## Package audit

The original UNSCANNED archive has SHA-256
`3e87a93a57c85d89c7405dee1670157efdf01d6fe6c1d7fe0ec3416cec82aa00`.
Its contained executable has SHA-256
`4a232905272da9b2404d373ec47616ca0a0c9b9a5d62e0c49d795332352877a7`.
The reference binary has SHA-256
`d44e131ac635624c2737b3261fec38202eb249c59a56f04304a39397b285f680`,
matching the supplied hardware reference. The archive has 13 entries: the
separate app's executable, launcher, config, icon and marker; its Ports
shortcut and icon; and separate MinUI and Allium launchers. There are no file
paths in common with the existing M0.6 package.

`tools/check_m07a_package.sh` checks this existing archive, runs the M0.6
tracked-input, ELF/save-region, package metadata and ROM leak gates, and only
copies it to the HARDWARE-TEST name after all pass. The test package manifest
must first be tracked by Git. A matching canonical Europe Rev 1 ROM (SHA-1
`b7f628073237b3d211d40406aa0884ff8fdd70d5`) must be supplied; it is read only.
The tracking check is the original M0.6 provenance gate, not proof that a dirty
working tree produced the binary. Keep the build and source snapshot together.
