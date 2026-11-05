# The New Tetris for the Miyoo Mini

A native port of The New Tetris, built from the [tnt-splat decompilation](https://github.com/chris-gilmore/tnt-splat)
(chris-gilmore/tnt-splat) with libultra's headers and C from [decompals/ultralib](https://github.com/decompals/ultralib),
on top of this repository's Banjo-Kazooie port (its libultra on coroutine threads with the N64's priorities, the
software renderer, the Miyoo kit). It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**Version 1.0.0, a beta:** Marathon, Sprint, Ultra, the Wonders and their 3D scenes, and the original music all run.
It has had little play on real devices yet. Please report what you find.

**No game data is included**, here or in the zip. You need your own ROM.

Download `NewTetris-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the
[top-level README](../README.md#install) says.

## Your ROM

- **The New Tetris (USA)**
- SHA-1 `83fff25e82181a6993f28c91b9eeb8430396838b` (12 MB, the .z64 byte order)
- .z64, .v64 or .n64, under any name: in `App/NewTetris/` (as `rom.z64` it is used where it is), in `Roms/N64/`, or
  anywhere on the card down to two folders deep
- The European and Japanese versions are refused; the screen says which one it found.
- The first start checks the ROM and keeps a copy in the game's own byte order, `App/NewTetris/data/tnt.z64`
  (12 MB). The game reads its pictures, scenes and music from that copy while it runs.

## Controls

| Miyoo | N64 | Does |
|---|---|---|
| D-pad | D-pad | Move; up and down drop |
| A / X | A | Rotate, OK |
| B / Y | B | Rotate back, back |
| L1 / R1 / L2 / R2 | L / R / Z | Hold piece |
| SELECT | C-down | C-down |
| START | START | Pause |
| MENU | | Game menu: resume, save state, load state, controls, exit |

MENU > CONTROLS moves any of these to another button ([how](../README.md#controls)).

## Known issues

- Text is a little sharper than on the N64 (no N64 video filter).
- On the original Mini, Marathon may draw 30 frames a second (a test build did; the game itself keeps its speed).
  Menus and vs CPU run at 60.

## More

[README_MIYOO.md](README_MIYOO.md) has the details: the screen, power and sleep, saves, and the files on the card.

## Building the Miyoo zip

On the Miyoo Mini toolchain (`/opt/miyoomini-toolchain`, as in the other games) with Python 3 (numpy) and your ROM:

    tools/setup.sh <dir>                     # the decomp and ultralib at the pinned commits + decomp.patch + hh/
    tools/build_miyoo.sh <dir> <your ROM>    # NewTetris-miyoo.zip + the release gates

`tools/setup.sh` clones the decompilation at the commit this port is based on (`bbbded9f`) and ultralib (`e24c8367`),
applies `decomp.patch` (every change behind `TARGET_HANDHELD`) and copies `hh/` (the Miyoo build:
`hh/Makefile.handheld`). The ROM is used by the leak scan only.

The game takes no game data into its executable: pictures, scenes, text and music are read from the player's ROM at
run time, so there is no data pack to build. `tools/build_miyoo.sh` builds with this repository's commit as the version
(the first line `console.log` writes, `SESSION tnt The New Tetris <version> version <commit>`) and runs the release
gates: the version line is in the executable; a leak scan of every shipped file against the ROM and every asset in it
inflated (`tools/tnt_inflate.py`, to a scratch file that is deleted), in the N64's byte order and in the Mini's (none at
all in the launchers and icon; at most 2048 8-byte windows in the executable, all of them tables the decompilation's
own C sources compile to); no host paths or authoring metadata; and exactly the files `miyoo/game.json` lists (no ROM).

`make -f hh/Makefile.handheld PLAT=pc` builds a headless 32-bit x86 copy for tests.

## Credits

- [chris-gilmore/tnt-splat](https://github.com/chris-gilmore/tnt-splat): the decompilation. The game code is theirs,
  under their terms; it has no licence file of its own, so this folder carries only `decomp.patch`, and
  `tools/setup.sh` fetches the decompilation from its own repository.
- [decompals/ultralib](https://github.com/decompals/ultralib): libultra's headers and C, under its own terms.
- The handheld layer (libultra's OS on coroutine threads, the renderer, the Miyoo kit) comes from this repository's
  Banjo-Kazooie and GoldenEye 007 ports, and the renderer from the Super Mario 64 PC port's Fast3D.

The New Tetris is Nintendo's and H2O Entertainment's; Tetris is The Tetris Company's. This is an unofficial port, not
connected to them or to Miyoo.
