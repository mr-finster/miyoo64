# Pilotwings 64 for the Miyoo Mini

A native port of Pilotwings 64, built from the [Pilotwings 64 decompilation](https://github.com/n64decomp/pw64)
(n64decomp/pw64, MIT) on top of this repository's Banjo-Kazooie port (its libultra on coroutine threads with the N64's
priorities, the software renderer, the Miyoo kit), with the classic libultra audio mixer written for this port. It
runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**Version 1.0.0, a beta:** the hang glider, rocket belt, gyrocopter and the bonus games all run, and the sound was
checked against a real N64's output. It has had little play on real devices yet. Please report what you find.

**No game data is included**, here or in the zip. You need your own ROM.

Download `Pilotwings64-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the
[top-level README](../README.md#install) says.

## Your ROM

- **Pilotwings 64 (USA)**
- SHA-1 `ec771aedf54ee1b214c25404fb4ec51cfd43191a` (8 MB, the .z64 byte order)
- .z64, .v64 or .n64, under any name: in `App/Pilotwings64/` (as `rom.z64` it is used where it is), in `Roms/N64/`,
  or anywhere on the card down to two folders deep
- The European and Japanese versions are refused; the screen says which one it found.
- The first start checks the ROM and keeps a copy in the game's own byte order, `App/Pilotwings64/data/pw64.z64`
  (8 MB). The game reads its levels, models and sounds from that copy while it runs.

## Controls

| Miyoo | N64 | Does |
|---|---|---|
| D-pad | Stick | Fly |
| SELECT (hold) | | The stick at half tilt, for gentle moves |
| A | A | Thrust, OK |
| B | B | Brake, back |
| L1 | Z | Photo, fire |
| R1 | R | R button |
| X / Y | C-up / C-down | Camera view |
| L2 / R2 | C-left / C-right | Camera left and right |
| START | START | Pause |
| MENU | | Game menu: resume, save state, load state, controls, exit |

MENU > CONTROLS moves any of these to another button ([how](../README.md#controls)).

## Known issues

- Not yet played much on a real Mini with this build: speed numbers are from the emulator.

## More

[README_MIYOO.md](README_MIYOO.md) has the details: the screen, power and sleep, saves, and the files on the card.

## Building the Miyoo zip

On the Miyoo Mini toolchain (`/opt/miyoomini-toolchain`, as in the other games) with Python 3 (numpy) and your ROM:

    tools/setup.sh <dir>                     # the decomp at the pinned commit + decomp.patch + hh/
    tools/build_miyoo.sh <dir> <your ROM>    # Pilotwings64-miyoo.zip + the release gates

`tools/setup.sh` clones the decompilation at the commit this port is based on (`11a76263`), applies `decomp.patch`
(every change behind `TARGET_HANDHELD`) and copies `hh/` (the Miyoo build: `hh/Makefile.handheld`). The ROM is used by
the leak scan only.

The game takes no game data into its executable: models, textures, levels, text and sound are read from the player's
ROM at run time and turned to the Mini's byte order as they load, so there is no data pack to build.
`tools/build_miyoo.sh` builds with this repository's commit as the version (the first line `console.log` writes,
`SESSION pw64 Pilotwings 64 <version> version <commit>`) and runs the release gates: the version line is in the
executable; a leak scan of every shipped file against the ROM and every MIO0 chunk in it inflated, in the N64's byte
order and in the Mini's (none at all in the launchers and icon; at most 2048 8-byte windows in the executable, all of
them tables the decompilation's own C sources compile to); no host paths or authoring metadata; and exactly the files
`miyoo/game.json` lists (no ROM).

`make -f hh/Makefile.handheld PLAT=pc` builds a headless 32-bit x86 copy for tests.

## Credits

- [n64decomp/pw64](https://github.com/n64decomp/pw64) (MIT): the decompilation. The game code is theirs, and
  `decomp.patch` keeps its MIT licence.
- The handheld layer (libultra's OS on coroutine threads, the renderer, the Miyoo kit) comes from this repository's
  Banjo-Kazooie and GoldenEye 007 ports, and the renderer from the Super Mario 64 PC port's Fast3D.

Pilotwings 64 is Nintendo's. This is an unofficial port, not connected to Nintendo, Paradigm or Miyoo.
