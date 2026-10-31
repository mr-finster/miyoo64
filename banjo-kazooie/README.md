# Banjo-Kazooie for the Miyoo Mini

A native port of Banjo-Kazooie, built from the [banjo-kazooie decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie)
(100% decompiled, CC0) on top of this repository's GoldenEye 007 port (Rare's engine too: its libultra on coroutine
threads with the N64's priorities, the software renderer, the Miyoo kit) and Harvest Moon 64's audio mixer. It runs on
the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**Version 1.0.0, a beta:** the whole game runs. The intro and the title were compared against an N64 renderer
(angrylion) scene by scene, and every world was entered and checked, but it has had little play on real devices yet.
Please report what you find.

**No game data is included**, here or in the zip. You need your own ROM.

Download `BanjoKazooie-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the
[top-level README](../README.md#install) says.

## Your ROM

- **Banjo-Kazooie (USA)**, the first release (version 1.0)
- SHA-1 `1fe1632098865f639e22c11b9a81ee8f29c75d7a` (16 MB, the .z64 byte order)
- .z64, .v64 or .n64, under any name: in `App/BanjoKazooie/` (as `rom.z64` it is used where it is), in `Roms/N64/`,
  or anywhere on the card down to two folders deep
- The USA version 1.1 (Rev A), the European and the Japanese versions are refused; the screen says which one it found.
- The first start checks the ROM and keeps a copy in the game's own byte order, `App/BanjoKazooie/data/banjo.z64`
  (16 MB). The game reads its levels, models and sounds from that copy while it runs. After that the ROM you put on the
  card is not read again.

## Controls

| Miyoo | N64 | Does |
|---|---|---|
| D-pad | Stick | Move (full tilt: Banjo runs) |
| SELECT (hold) | | The stick at half tilt: walk, tiptoe |
| A | A | Jump |
| B | B | Attack |
| L1 | Z | Crouch; with A or B the Z moves (Beak Barge, Beak Buster, Flap Flip ...) |
| R1 | R | Camera behind Banjo |
| X | C-up | First person (with Z: eggs forward) |
| Y | C-down | Eggs backward, camera zoom |
| L2 / R2 | C-left / C-right | Turn the camera (with Z: Talon Trot, Wonderwing) |
| START | START | Pause |
| MENU | | Game menu: resume, save state, load state, controls, exit |

MENU > CONTROLS moves any of these to another button ([how](../README.md#controls)).

## Known issues

- Click Clock Wood in winter is too slow on the original Mini: about 37 ms a frame (an estimate from the emulator,
  not measured on a device), so it plays below the N64's speed there. The other seasons and worlds are lighter.
- The molehills' shadow (Bottles' hills in Spiral Mountain and the worlds) is drawn darker than on the N64.
- Over the intro, the port's frame pacing drifts from the N64's by about 100 frames; scenes and music still match.
- Not yet played on a real Mini with this build's renderer: speed numbers are from the emulator.

## More

[README_MIYOO.md](README_MIYOO.md) has the details: the screen, power and sleep, saves, the settings in
`launch.sh`, and the build.

## Building the Miyoo zip

On the Miyoo Mini toolchain (`/opt/miyoomini-toolchain`, as in the other games) with Python 3 (numpy), Rust's
`cargo` and your ROM:

    tools/setup.sh <dir> <your Banjo-Kazooie (USA 1.0) .z64>   # the decomp at the pinned commit + decomp.patch + hh/
    tools/build_miyoo.sh <dir>                                  # BanjoKazooie-miyoo.zip + the release gates

`tools/setup.sh` clones the decompilation at the commit this port is based on (`e8d31fa5`), applies `decomp.patch`
(every change behind `TARGET_HANDHELD`) and copies `hh/` (the Miyoo build: `hh/Makefile.handheld`). The ROM is copied
into `<dir>` for the leak scan only, with the decompressed ROM the decomp's own tool makes from it.

The game takes no game data into its executable: models, textures, levels, text and music are read from the player's
ROM at run time and turned to the Mini's byte order as they load (`hh/hh_assets.c`), so there is no data pack to
build. `tools/build_miyoo.sh` builds with this repository's commit as the version (the first line `console.log`
writes, `SESSION banjo Banjo-Kazooie <version> version <commit>`, rewritten by every build) and runs the release
gates: the version line is in the executable; a leak scan of every shipped file against the ROM, the decompressed ROM
and every packed asset inflated, in the N64's byte order and in the Mini's (none at all in the launchers and icon; at
most 2048 8-byte windows in the executable, all of them tables the decompilation's own C sources compile to: id lists,
render modes, sound and collision tables, no art); no host paths or authoring metadata; and exactly the files
`miyoo/game.json` lists (no ROM).

`make -f hh/Makefile.handheld PLAT=pc` builds a headless 32-bit x86 copy for tests (frame dumps, `BK_PADSCRIPT`,
`BK_WARP=<map>:<exit>`), and `SAN=address,undefined` its sanitizer build.

## Credits

- [banjo.decomp/banjo-kazooie](https://gitlab.com/banjo.decomp/banjo-kazooie) (CC0): the decompilation. The game code
  is theirs.
- The handheld layer (libultra's OS on coroutine threads, the renderer, the Miyoo kit) comes from this repository's
  GoldenEye 007 port, the n_audio mixer from its Harvest Moon 64 port, and the renderer from the Super Mario 64 PC
  port's Fast3D.

Banjo-Kazooie is Rare's and Nintendo's. This is an unofficial port, not connected to Rare, Nintendo, Microsoft or Miyoo.
