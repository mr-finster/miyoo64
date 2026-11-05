# Pilotwings 64 on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), built on the
[n64decomp/pw64](https://github.com/n64decomp/pw64) decompilation. It uses a software renderer and has save states.
**There is no game data in it.** It reads everything from your own USA ROM.

**Status:** version 1.0.0, a beta. Known issues: [README.md](README.md#known-issues).

## How it plays

- **Screen:** the game draws its N64 picture and the Mini scales it to the panel. On a Mini v4 or Mini Flip it fills
  the panel's full height. It is turned 180 degrees, as the panel is mounted.
- **Speed:** the game runs at the N64's pace (its frames come from the same 60 Hz retrace count).
- **Power:** when the OS shuts the game down (the power button, or any SIGTERM), the game saves the auto save and
  quits. The next start asks RESUME GAME or NEW GAME, the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or a while with no button, writes the auto save first.
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, CONTROLS and EXIT GAME.
- **Saves:** the game's own saves (the N64 cartridge's EEPROM) go to `data/eeprom.bin`. Save states are separate and
  only load in the build that made them.

## Install

1. Unzip `Pilotwings64-miyoo.zip` onto the SD card's root, letting it merge with the folders there.
2. Put your ROM in `App/Pilotwings64/` as `rom.z64`, or in `Roms/N64/` under any name. It must be **Pilotwings 64
   (USA)**, SHA-1 `ec771aedf54ee1b214c25404fb4ec51cfd43191a` (the .z64 byte order), 8 MB. Any byte order works.
3. Start **Pilotwings 64**. The first start finds and checks the ROM and keeps a copy in
   `App/Pilotwings64/data/pw64.z64`, once, with a bar on the screen.

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/Pilotwings64/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Pilotwings 64.port`, `Roms/PORTS/Imgs/Pilotwings 64.png` |
| MinUI, MyMinUI | the home screen | `Roms/Pilotwings 64 (PW64)/`, `Emus/miyoomini/PW64.pak/` |
| Allium | Apps | `Apps/Pilotwings 64.pak/` |

Everything the game writes stays in `App/Pilotwings64/data/`: the ROM copy, the game's saves (`eeprom.bin`), the save
states and the auto save, the button layout from MENU > CONTROLS (`controls.cfg`), `console.log` from the last
session (its first line names the build, `SESSION pw64 Pilotwings 64 1.0.0 version <commit> ...`) and `launch.log`.

**Updating:** unzip the new zip over the old one. The game's saves carry over. Save states and the auto save belong to
the build that made them.

## Controls

[README.md](README.md#controls). In the game menu, up and down pick an entry, and left and right change the slot. A or
START selects; B or MENU backs out.

## Credits and licence

The game code is the [n64decomp/pw64](https://github.com/n64decomp/pw64) decompilation (MIT); `decomp.patch` is a
change to it under the same licence. The renderer, the Miyoo kit and the save states come from this project's
GoldenEye 007 and Banjo-Kazooie ports and are shared with them. This port's own code is MIT, as the rest of the
project ([LICENSE](../LICENSE)). Pilotwings 64 belongs to Nintendo; this project ships none of its data and is not
connected to Nintendo, Paradigm or Miyoo.
