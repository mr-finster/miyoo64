# The New Tetris on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), built on the
[chris-gilmore/tnt-splat](https://github.com/chris-gilmore/tnt-splat) decompilation. It uses a software renderer and has save states.
**There is no game data in it.** It reads everything from your own USA ROM.

**Status:** version 1.0.1, a beta. Known issues: [README.md](README.md#known-issues).

## How it plays

- **Screen:** the game draws its N64 picture and the Mini scales it to the panel. On a Mini v4 or Mini Flip it fills
  the panel's full height. It is turned 180 degrees, as the panel is mounted.
- **Speed:** the game runs at the N64's pace (its frames come from the same 60 Hz retrace count).
- **Power:** when the OS shuts the game down (the power button, or any SIGTERM), the game saves the auto save and
  quits. The next start asks RESUME GAME or NEW GAME, the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or a while with no button, writes the auto save first.
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, CONTROLS and EXIT GAME.
- **Saves:** the game's own saves (the N64 cartridge's SRAM) go to `data/sram.bin`. Save states are separate and
  only load in the build that made them.

## Install

1. Unzip `NewTetris-miyoo.zip` onto the SD card's root, letting it merge with the folders there.
2. Put your ROM in `App/NewTetris/` as `rom.z64`, or in `Roms/N64/` under any name. It must be **The New Tetris
   (USA)**, SHA-1 `83fff25e82181a6993f28c91b9eeb8430396838b` (the .z64 byte order), 12 MB. Any byte order works.
3. Start **The New Tetris**. The first start finds and checks the ROM and keeps a copy in
   `App/NewTetris/data/tnt.z64`, once, with a bar on the screen.

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/NewTetris/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/The New Tetris.port`, `Roms/PORTS/Imgs/The New Tetris.png` |
| MinUI, MyMinUI | the home screen | `Roms/The New Tetris (TNT)/`, `Emus/miyoomini/TNT.pak/` |
| Allium | Apps | `Apps/The New Tetris.pak/` |

Everything the game writes stays in `App/NewTetris/data/`: the ROM copy, the game's saves (`sram.bin`), the save
states and the auto save, the button layout from MENU > CONTROLS (`controls.cfg`), `console.log` from the last
session (its first line names the build, `SESSION tnt The New Tetris 1.0.1 version <commit> ...`) and `launch.log`.

**Updating:** unzip the new zip over the old one. The game's saves carry over. Save states and the auto save belong to
the build that made them.

## Controls

[README.md](README.md#controls). In the game menu, up and down pick an entry, and left and right change the slot. A or
START selects; B or MENU backs out.

## Settings

In `App/NewTetris/data/debug.cfg`, one `TNT_NAME=value` per line (or in `launch.sh`):

| Setting | Default | What |
|---|---|---|
| `TNT_OUTFREQ` | 48000 | the sound's output rate in Hz: the OS mixer's own, so the game's 36 kHz is resampled once, in the game; `32000`: as before 1.0.1 |

## Credits and licence

The game code is the [chris-gilmore/tnt-splat](https://github.com/chris-gilmore/tnt-splat) decompilation, under its
authors' terms (it has no licence file; `decomp.patch` is a change to it and the decompilation itself is fetched from
its own repository). The renderer, the Miyoo kit and the save states come from this project's GoldenEye 007 and
Banjo-Kazooie ports and are shared with them. This port's own code is MIT, as the rest of the project
([LICENSE](../LICENSE)). The New Tetris belongs to Nintendo and H2O Entertainment; this project ships none of its data
and is not connected to them, The Tetris Company or Miyoo.
