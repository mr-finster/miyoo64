# Majora's Mask on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), built on the
[zeldaret/mm](https://github.com/zeldaret/mm) decompilation. It uses a software renderer and has save
states and a first-start data builder. **There is no game data in it.** It builds everything from your
own USA ROM the first time you run it.

**Status:** version 1.0.0, a beta. It plays on the Mini (OG, 640x480) with Onion from Apps at the game's own
20 fps; test builds have been played on that device. The Mini Plus, Mini v4 and Flip, and Onion's Ports list,
MinUI, Allium and spruce still need a first run. Known issues: [README.md](README.md#known-issues).

## How it works on the Mini

- **Screen:** the game draws at 320x240, the N64's own size. On a Mini each frame is doubled to fill the
  640x480 panel. On a Mini v4 or Mini Flip it is drawn at the panel's full height. Either way it is
  turned 180 degrees, as the panel is mounted.
- **Frame rate:** gameplay runs at 20 fps and the pause menu at 30, as on the N64. Frames go to the
  panel on its refresh, so they come out evenly.
- **Power:** when the OS shuts the game down (the power button, or any SIGTERM), the game saves the
  auto save and quits. The next start asks RESUME GAME or NEW GAME, the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or 50 s with no button, writes the auto save first
  (`MM_SLEEPSAVE=0` in launch.sh turns it off; [Sleep and wake](../docs/SLEEP-AND-WAKE.md)).
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE and EXIT GAME, in a
  built-in 8x8 font. Volume and brightness are left to the OS.
- **Saves:** the in-game saves (owl statues, the Song of Time) go to `data/mm-flash.bin`, written off
  the frame. A copy that fails the game's own checksum is kept aside as `mm-flash.bin.bad-<time>`.
- **First start:** screens while it looks for the ROM and builds the data, in the menu's font. They
  are full screen on a Mini v4 or Mini Flip too.
- **Both cores:** the big 3D triangles are drawn half by each core, with the same picture pixel for
  pixel.

## Install

1. Unzip `MajorasMask-miyoo.zip` onto the SD card's root, letting it merge with the folders there.
2. Put your ROM in the game's folder as `App/MajorasMask/rom.z64`. It must be **The Legend of Zelda:
   Majora's Mask (USA)**, SHA-1 `d6133ace5afaa0882cf214cf88daba39e266c078` (the .z64 byte order); .v64 and
   .n64 dumps work too. The game also looks in `Roms/N64/` and anywhere on the card down to two folders
   deep, under any file name. Other versions are refused with a message.
3. Start **Majora's Mask**. The first start checks the ROM and builds the game data into
   `App/MajorasMask/data/` (about 49 MB), once.

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/MajorasMask/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Majora's Mask.port`, `Roms/PORTS/Imgs/Majora's Mask.png` |
| MinUI, MyMinUI | the home screen | `Roms/Majora's Mask (MM)/`, `Emus/miyoomini/MM.pak/` |
| Allium | Apps | `Apps/Majora's Mask.pak/` |

Everything the game writes stays in `App/MajorasMask/data/`:
- the game data it built;
- the in-game save (`mm-flash.bin`);
- the save states and the auto save;
- `console.log` from the last session;
- `launch.log` and `games_launch.log`, which record each start.

**Updating:** unzip the new zip over the old one. The in-game save carries over, and so do save states. If
the new version needs different game data, the next start rebuilds it from your ROM (`data/mm_pack.id`
records which data it is), so keep the ROM on the card.

## Controls

| Miyoo | N64 |
|---|---|
| D-pad | Stick |
| A / B | A / B |
| L1 | Z (target) |
| R1 | R (shield) |
| X / Y | C-left / C-down |
| R2 | C-right |
| L2 | C-up (first person, Tatl) |
| START (tap) | START (pause) |
| START + R1, or START + up | C-up |
| START + left / down / right | C-left / C-down / C-right |
| START + L1 | L (the minimap on/off) |
| MENU | the game menu (resume, save/load state, exit) |

While you play an instrument, the d-pad is the four C notes and A is A. B puts it away.

## Settings (in `launch.sh`)

- `MM_ROTATE=0`: draw the picture unturned, for an SDL that turns the panel itself (the MinUI launcher
  sets it).
- `MM_CPUCLOCK=<MHz>`: the CPU clock while the game runs. `0` leaves it as the OS set it.
- `MM_THREADS=1`: draw on one core.
- `MM_SLEEPSAVE=0`: no auto save before sleep.
- `MM_PACE=0`: frames go up when they are drawn rather than on the panel's refresh.

## Building

The steps and the release gates: [README.md](README.md#building-the-miyoo-zip).

## Credits

The game code is the [zeldaret/mm](https://github.com/zeldaret/mm) decompilation. The renderer, the
first-start builder and the Miyoo kit come from this project's Ocarina of Time port (itself built with
[z2442/oot-PSP](https://github.com/z2442/oot-PSP)) and are shared with the other ports. Majora's Mask
is Nintendo's; this project ships none of its data.
