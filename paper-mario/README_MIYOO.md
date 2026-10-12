# Paper Mario on the Miyoo Mini

Paper Mario for the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip): the natively compiled game, a software
renderer and save states. **There is no game data in it.** The first time you run it, it builds what it needs from your own ROM.

**Status:** built and checked under qemu with the US ROM: first start, play, menu, save states,
the power-off save and RESUME, the two cores frame for frame. It hasn't run on a Mini yet.

## How it plays

- **Screen:** the picture fills the 640x480 panel. In the field and in battles the game draws its scene in a
  296x200 box inside a black frame (the N64's overscan margin, which a TV mostly hid); that box is stretched over the
  whole panel. Screens without the frame (the logos, the title,
  the chapter cards) show the whole 320x240 picture doubled. The scaling keeps every pixel sharp: only the one panel
  pixel where two game pixels meet is blended, so text and the HUD stay crisp. `PM64_STRETCH=0` in `debug.cfg` gives
  the bordered picture. On a bigger panel (752x560 on the Mini v4) the 640x480 picture sits in the middle with black
  borders.
- **Speed:** the game runs at its own 30 fps, as on the N64. A frame is shown as soon as it's drawn: no smoothing and no
  in-between frames. The renderer splits each big triangle between the Mini's two cores (same pixels either way), and
  the CPU runs at 1500 MHz on the Mini and 1800 MHz on the Mini Plus while the game is up (the stock clock is 1200).
- **Power:** when the OS shuts the game down (the power button, or any kill), the game writes
  the auto save and quits. The next start asks RESUME GAME or NEW GAME, the same as after EXIT GAME in the menu.
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, EXIT GAME, in a built-in font.
  The OS handles volume and brightness. Notices like SAVED SLOT 1 show in the top-left corner for two seconds.
- **Buttons:** SELECT or R2 is C-up, and L2 is N64 R (the full table is under Controls).

## Install

1. Unzip `PaperMario-miyoo.zip` onto the SD card's root, letting it merge with the folders already there.
2. Put your ROM in `Roms/N64/`, or anywhere on the card up to two folders deep, under any file name. It must be
   **Paper Mario (USA)**, SHA-1 `3837f44cda784b466c9a2d99df70d77c322b97a0`, 40 MB, as `.z64`, `.v64` or `.n64`
   (`.rom`, `.bin`, `.u64` and `.usa` work too). The game checks `App/PaperMario/rom.z64` first. European and Japanese
   carts are refused, and the screen tells you which one you have.
3. Start **Paper Mario**. The first start builds the game data from the ROM
   (`App/PaperMario/data/assets.bin`, about 11 MB). Later starts skip that step, but the game still reads music, maps
   and sprites from the ROM while it runs, so leave the ROM on the card. To redo the first start, put an empty file
   named `rebuild` in `App/PaperMario/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/PaperMario/`. Every other entry just starts that
folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/PaperMario/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Paper Mario.port`, `Roms/PORTS/Imgs/Paper Mario.png` |
| MinUI, MyMinUI | the home screen | `Roms/Paper Mario (PM64)/`, `Emus/miyoomini/PM64.pak/` |
| Allium | Apps | `Apps/Paper Mario.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders meant for other OSes just sit unused. Everything the game writes stays in `App/PaperMario/data/`:

- `assets.bin`
- `pm64.sav` (your in-game saves)
- `state_01.sav` to `state_09.sav` and `state_quick.sav` (the save states and the auto save)
- `debug.cfg` (the settings, if you make one)
- `console.log` from the last session, plus `console.1.log` and `console.2.log` from the two before it
- `launch.log` and `games_launch.log`, which record each start

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/PAPERMARIO/`, `Roms/PAPERMARIO/`);
this zip no longer has it (the game is in Apps and Ports), so delete those two folders when you
update, or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

**Updating:** unzip the new zip over the old one. In-game saves carry over. Save states only load in the build that
made them, so an old auto save is removed when the game starts and old slots are refused.

`tools/port/miyoo.sh` builds the zip, using the packer shared by all the ports (`miyoo/tools/miyoo_pack.py`, from
the shared Miyoo kit) and this port's `miyoo/game.json`. With `PERSONAL=1` it puts your ROM in as
`App/PaperMario/rom.z64`. That zip is for your own card only: never share it.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | walk and run (the stick), menus |
| A | jump, talk, confirm |
| B | hammer, back |
| X | the partner's ability (C-down) |
| Y | spin dash (Z) |
| L1 | items (C-left) |
| R1 | partners (C-right) |
| SELECT or R2 | show or hide the status bar (C-up) |
| L2 | N64 R: pages lists down, moves to the pause menu's next tab |
| START | pause |
| MENU | the game menu (resume, save or load a state, exit) |

In the menu, up and down pick an entry, and left and right change the slot. LOAD also offers the auto save, one step
left of slot 1. A or START selects. B backs out of a confirmation or closes the menu, and so does MENU.

## Settings

The settings file is `App/PaperMario/data/debug.cfg`, with one `PM64_NAME=value` per line. These are only on
the Mini:

| Setting | Default | What |
|---|---|---|
| `PM64_THREADS` | 2 | `1` draws on one core only, as before (for an A/B on the device) |
| `PM64_CPUCLOCK` | (the model's) | the CPU clock in MHz: 1500 on the Mini and 1800 on the Mini Plus. `0` leaves it as the OS set it. The old clock comes back when the game quits |
| `PM64_ROTATE` | 1 | `0` draws the picture unturned, for an SDL that turns the panel itself (MinUI's launcher sets this) |
| `PM64_STRETCH` | 1 | `0` keeps the N64's black frame around the field and battle scenes (the bordered picture of the first Miyoo build) |
| `PM64_LOADPIC` | 1 | `0` shows the game's boot frames before a resumed auto save loads, as before |
| `PM64_SPLIT` | 64 | the smallest triangle (in pixels) that both cores share |

Environment variables for testing (set them in `launch.sh`):
- `PM64_MIYOO_MODE=640x480` forces the panel mode.
- `PM64_LOG=0` sends the game's messages to the terminal instead of `console.log`.
- `PM64_PANEL_DUMP=<dir>` (with `PM64_PANEL_DUMP_EVERY=<n>`) writes the panel as you see it, every nth frame, as PPM.
- `PM64_PANEL_CHECK=1` compares the first 30 presented frames with a plain reference, and logs the result.

## Speed

On the Mini, each `PERF` line in `console.log` (one every 300 frames) ends with
`mt on split <split>/<triangles> tris <n>% px, jobs <n>, sleeps <n>`, so you can see how much of the 3D the second core
shared. A frame's work over 33.3 ms counts as `late`.
