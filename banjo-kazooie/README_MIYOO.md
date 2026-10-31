# Banjo-Kazooie on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), built on the
[banjo.decomp/banjo-kazooie](https://gitlab.com/banjo.decomp/banjo-kazooie) decompilation. It uses a software
renderer and has save states. **There is no game data in it.** It reads everything from your own USA ROM.

**Status:** version 1.0.0, a beta. It runs the whole game in the emulator (qemu) and its test builds started on a
Mini Plus with Onion; this build's speed on the Mini, Mini v4 and Flip, and the other OSes, still need a first run.
Known issues: [README.md](README.md#known-issues).

## How it plays

- **Screen:** the game draws its N64 picture and the Mini doubles it to the 640x480 panel. On a Mini v4 or Mini Flip
  (752x560) it fills the panel's full height. Either way it is turned 180 degrees, as the panel is mounted.
- **Speed:** the game runs at the N64's pace (its frames come from the same 60 Hz retrace count). The heaviest scene
  is Click Clock Wood in winter ([Known issues](README.md#known-issues)).
- **Both cores:** big triangles are drawn half by each core, with the same picture pixel for pixel.
- **Power:** when the OS shuts the game down (the power button, or any SIGTERM), the game saves the auto save and
  quits. The next start asks RESUME GAME or NEW GAME, the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or a while with no button, writes the auto save first (`BK_SLEEPSAVE=0` turns it
  off). The CPU clock the game set is put back after the Mini wakes.
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, CONTROLS and EXIT GAME.
- **Saves:** the game's own saves (the three files on the N64 cartridge's EEPROM) go to `data/eeprom.bin`; the game
  writes them without stopping the picture. Save states are separate and only load in the build that made them.

## Install

1. Unzip `BanjoKazooie-miyoo.zip` onto the SD card's root, letting it merge with the folders there.
2. Put your ROM in `App/BanjoKazooie/` as `rom.z64`, or in `Roms/N64/` under any name. It must be **Banjo-Kazooie
   (USA)** version 1.0, SHA-1 `1fe1632098865f639e22c11b9a81ee8f29c75d7a` (the .z64 byte order), 16 MB. Any byte order
   works (`.z64`, `.v64`, `.n64`). The game also looks anywhere on the card down to two folders deep.
3. Start **Banjo-Kazooie**. The first start finds and checks the ROM and keeps a copy in
   `App/BanjoKazooie/data/banjo.z64` (16 MB), once, with a bar on the screen.

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/BanjoKazooie/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Banjo-Kazooie.port`, `Roms/PORTS/Imgs/Banjo-Kazooie.png` |
| MinUI, MyMinUI | the home screen | `Roms/Banjo-Kazooie (BK)/`, `Emus/miyoomini/BK.pak/` |
| Allium | Apps | `Apps/Banjo-Kazooie.pak/` |

Everything the game writes stays in `App/BanjoKazooie/data/`:
- the ROM copy (`banjo.z64`) and where the ROM was found (`rom_path.txt`);
- the game's saves (`eeprom.bin`);
- the save states (`state_01.sav` ...) and the auto save (`state_quick.sav`);
- the button layout from MENU > CONTROLS (`controls.cfg`);
- `console.log` from the last session, `console.1.log` and `console.2.log` from the two before; its first lines name
  the build (`SESSION banjo Banjo-Kazooie 1.0.0 version <commit> ...`);
- `launch.log`, which records each start.

To do the first start's job again (a new ROM, or a damaged copy), put a file named `rebuild` in `data/`; the next
start finds, checks and copies the ROM anew and removes it.

**Updating:** unzip the new zip over the old one. The game's saves carry over. Save states and the auto save belong to
the build that made them.

## Controls

[README.md](README.md#controls). In the game menu, up and down pick an entry, and left and right change the slot. A or
START selects; B or MENU backs out.

## Settings

In `App/BanjoKazooie/data/debug.cfg`, one `BK_NAME=value` per line (or in `launch.sh`):

| Setting | Default | What |
|---|---|---|
| `BK_ROTATE` | 1 | `0` draws the picture unturned, for an SDL that turns the panel itself (the MinUI launcher sets it) |
| `MIYOO_CPUCLOCK` | the model's | the CPU clock in MHz while the game runs; `0` leaves it as the OS set it. The old clock comes back when the game quits |
| `MIYOO_THREADS` | 2 | `1` draws on one core only |
| `BK_FLIPFILL` | 1 | `0` keeps the 2x picture with a border on a Mini v4 / Flip |
| `BK_SLEEPSAVE` | 1 | `0`: no auto save at a POWER press or when idle; `BK_SLEEPSAVE_IDLE=<s>` sets the idle time (`0` no idle save) |
| `BK_DITHER` | 1 | `0`: the picture's colours cut to the panel's without dithering |

## Credits and licence

The game code is the [banjo.decomp/banjo-kazooie](https://gitlab.com/banjo.decomp/banjo-kazooie) decompilation (CC0).
The renderer, the Miyoo kit and the save states come from this project's GoldenEye 007 port, the audio mixer from its
Harvest Moon 64 port, and are shared with them. This port's own code is MIT, as the rest of the project
([LICENSE](../LICENSE)). Banjo-Kazooie belongs to Rare and Nintendo; this project ships none of its data and is not
connected to them, Microsoft or Miyoo.
