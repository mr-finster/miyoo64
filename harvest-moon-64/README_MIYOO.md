# Harvest Moon 64 on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), built on the
[harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp) decompilation. It uses a software
renderer and has save states and a first-start data builder. **There is no game data in it.** It builds everything
from your own USA ROM the first time you run it.

**Status:** version 1.0.0. It plays on the Mini (OG, 640x480) with Onion from Apps; test builds have been played on
that device. The Mini Plus, Mini v4 and Flip, and Onion's Ports list, MinUI, Allium and spruce still need a first run.
Known issues: [README.md](README.md#known-issues).

## How it plays

- **Screen:** the game draws at 320x240, the N64's own size. On a Mini each frame is doubled to fill the 640x480
  panel. On a Mini v4 or Mini Flip it is drawn at the panel's full height. Either way it is turned 180 degrees, as the
  panel is mounted.
- **Speed:** the game runs 20 steps a second everywhere. That is the N64 game's speed on the farm, measured against an
  N64 emulator (the in-game clock moves 10 game seconds a step, so an hour takes 18 seconds). In the emulator some
  light scenes (the inside of the house, the title) ran faster; that has not been checked on a real N64.
- **Power:** when the OS shuts the game down (the power button, or any SIGTERM), the game saves the auto save and
  quits. The next start asks RESUME GAME or NEW GAME, the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or 50 s with no button, writes the auto save first
  (`HM64_SLEEPSAVE=0` turns it off).
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE and EXIT GAME, in a built-in 8x8
  font. Volume and brightness are left to the OS.
- **Saves:** the diary (the game's own save) goes to `data/hm64.sram`, the battery save of the cartridge. Save states
  are separate and only load in the build that made them.
- **First start:** screens while it looks for the ROM and builds the data, in the menu's font. They are full screen on
  a Mini v4 or Mini Flip too.
- **Both cores:** big triangles are drawn half by each core, with the same picture pixel for pixel.

## Install

1. Unzip `HarvestMoon64-miyoo.zip` onto the SD card's root, letting it merge with the folders there.
2. Put your ROM in `App/HarvestMoon64/` as `rom.z64`, or in `Roms/N64/` under any name. It must be **Harvest Moon 64
   (USA)**, SHA-1 `90631460f1876a14849df0541d534012b410a34c` (the .z64 byte order), 16 MB. Any byte order works
   (`.z64`, `.v64`, `.n64`; `.rom`, `.bin`, `.u64` and `.usa` too). The game also looks anywhere on the card down to
   two folders deep. Other versions are refused with a message that says which one you have.
3. Start **Harvest Moon 64**. The first start checks the ROM and builds the game data into
   `App/HarvestMoon64/data/hm64-pack.bin` (16 MB), once. Keep the ROM on the card: an update can need it again.

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/HarvestMoon64/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Harvest Moon 64.port`, `Roms/PORTS/Imgs/Harvest Moon 64.png` |
| MinUI, MyMinUI | the home screen | `Roms/Harvest Moon 64 (HM64)/`, `Emus/miyoomini/HM64.pak/` |
| Allium | Apps | `Apps/Harvest Moon 64.pak/` |

Everything the game writes stays in `App/HarvestMoon64/data/`:
- the game data it built (`hm64-pack.bin`, `hm64-pack.id`);
- the diary saves (`hm64.sram`);
- the save states (`state_01.sav` ...) and the auto save (`state_quick.sav`);
- `console.log` from the last session, `console.1.log` and `console.2.log` from the two before;
- `launch.log` and `games_launch.log`, which record each start.

**Updating:** unzip the new zip over the old one. The diary saves carry over. Save states and the auto save belong to
the build that made them: an auto save from an older build is kept aside as `state_quick.sav.rejected`, not loaded.
Write in the diary before you update.

Diary saves from an N64 emulator do not load here (the port keeps the save in the Mini's byte order).

## Controls

| Miyoo | N64 | In the game |
|---|---|---|
| D-pad | Stick | walk (run is the stick's full tilt) |
| A | A | talk, pick up, throw, check, confirm |
| B | B | use the tool, cancel |
| X | Z | show the held item's name |
| Y | C-down | eat the held item |
| L1 | C-left | whistle for the horse |
| R1 | C-right | whistle for the dog |
| L2 | C-up | put the held item in the rucksack |
| R2 / SELECT | R / L | turn the camera on the farm |
| START | START | the pause screen |
| MENU | | the game menu (resume, save/load state, exit) |

In the menu, up and down pick an entry, and left and right change the slot. A or START selects; B or MENU backs out.

## Settings

In `App/HarvestMoon64/data/debug.cfg`, one `HM64_NAME=value` per line (or in `launch.sh`):

| Setting | Default | What |
|---|---|---|
| `HM64_ROTATE` | 1 | `0` draws the picture unturned, for an SDL that turns the panel itself (the MinUI launcher sets it) |
| `HM64_CPUCLOCK` | the model's | the CPU clock in MHz while the game runs: 1500 on the Mini, 1800 on the Mini Plus; `0` leaves it as the OS set it. The old clock comes back when the game quits |
| `HM64_THREADS` | 2 | `1` draws on one core only |
| `HM64_FLIPFILL` | 1 | `0` keeps the 2x picture with a border on a Mini v4 / Flip |
| `HM64_SLEEPSAVE` | 1 | `0`: no auto save at a POWER press or when idle; `HM64_SLEEPSAVE_IDLE=<s>` sets the idle time (`0` no idle save) |
| `HM64_HZ` | 20 | the game's steps a second |
| `HM64_WATCHDOG` | 5 | seconds without a frame before the stall report in `console.log`; `0` off. MENU then quits a stuck game |

To build the game data again (a new ROM, or a damaged card), delete `data/hm64-pack.id`; the next start rebuilds it.

## Credits and licence

The game code is the [harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp) decompilation
(CC0). The renderer, the Miyoo kit, the save states and the first-start builder come from this project's other ports
(Paper Mario's renderer and kit, after beckerd's Mario Kart 64 PSP renderer; the Perfect Dark port's audio mixer;
Ocarina of Time's and Majora's Mask's first-start design) and are shared with them. This port's own code is MIT, as
the rest of the project ([LICENSE](../LICENSE)). Harvest Moon 64 belongs to its rights holders (Victor Interactive Software, now
Marvelous; published by Natsume); this project ships none of its data and is not connected to them, Nintendo or Miyoo.
