# Star Fox 64 on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a software renderer and save
states. **There is no game data in it.** The first time it runs, it builds what it needs from your own Star Fox 64 ROM (US,
version 1.1).

**Status:** built and checked under qemu (2026-10-11): first start, menu, save/load, SIGTERM and resume, one core = two cores frame for frame; not yet run on a Mini. The platform code is
the shared Miyoo kit (the shared Miyoo kit, the same files as the Mario Kart 64, Diddy Kong Racing,
Ocarina of Time and GoldenEye Miyoo ports).

## How it works on the Mini

- **Screen:** the game draws the N64's own 320x240 picture (the HUD, the radio and the 3D exactly as the N64
  lays them out), and each frame is doubled to fill the 640x480
  panel. The N64 kept an 8-pixel border black (a TV's overscan hid it); here the scene is drawn into it too, so
  the picture fills the whole panel at the right 4:3 shape (nothing is stretched: the game's view already covers
  the full frame). `fill=0` in the settings file (or `SF64_FILL=0`) brings the border back. On a bigger panel
  (752x560 on the Mini v4) the 640x480 picture sits in the middle.
- **Frame rate:** the game's own, as on the N64: about 30 fps in play, lower in heavy moments if the Mini
  can't keep up. Each frame is shown as soon as it is ready; nothing is blended or smoothed.
- **Power:** when the OS shuts the game down (the power button, or any kill), the
  game saves the auto save and quits, and the next start asks RESUME GAME or NEW GAME, the same as EXIT GAME
  in the menu.
- **Menu:** the menu every Miyoo port shares: RESUME, SAVE STATE, LOAD STATE, EXIT GAME, in a built-in font.
  Volume and brightness are the OS's. Notices like SAVED SLOT 1 show in the top-left corner for two seconds.
- **Buttons:** C-right (SELECT), C-up (L2) and the menu-only L (R2) are plain buttons.
- **Two cores:** the Mini has two Cortex-A7s. The game runs on the first, and the second
  draws every other row of each big 3D triangle while the first draws the rest. The picture is the same pixel
  for pixel either way; `threads=1` in the settings file goes back to one core.
- **CPU clock:** 1500 MHz on the Mini and 1800 MHz on the Mini Plus while the game runs (the stock clock is
  1200), put back when it quits. `cpuclock=` in the settings file changes it.

## Install

1. Unzip `StarFox64-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **Star Fox 64 (USA) (Rev 1)**, SHA-1
   `09f0d105f476b00efa5303a3ebc42e60a7753b7a`, 12,582,912 bytes, as `.z64`, `.v64` or `.n64` (`.rom`, `.bin`,
   `.u64` and `.usa` also work), with any file name. Other versions and regions are refused, and the screen
   says which one you have. The game also takes `App/StarFox64/rom.z64` (looked at first), and looks anywhere
   on the card down to two folders deep.
3. Start **Star Fox 64**. The first start builds the game data from the ROM (`App/StarFox64/data/sf64.rom`,
   about 12 MB) in a few seconds. Later starts skip that step. To redo it, put an empty file named `rebuild`
   in `App/StarFox64/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/StarFox64/`, and every other entry only
starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/StarFox64/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Star Fox 64.port`, `Roms/PORTS/Imgs/Star Fox 64.png` |
| MinUI, MyMinUI | the home screen | `Roms/Star Fox 64 (SF64)/`, `Emus/miyoomini/SF64.pak/` |
| Allium | Apps | `Apps/Star Fox 64.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in `App/StarFox64/data/`:
`sf64.rom` (the game data built from your ROM), `eeprom.bin` (your progress), `state_01.sav` to
`state_09.sav` and `state_quick.sav` (the save states and the auto save), `console.log` from the last
session plus the two before it, and `launch.log` and `games_launch.log`, which record each start. The
settings file is `App/StarFox64/sf64_display.cfg`.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/STARFOX64/`, `Roms/STARFOX64/`); this
zip no longer has it (the game is in Apps and Ports), so delete those two folders when you update,
or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

**Updating:** unzip the new zip over the old one. Your progress carries over. Save states only load in the
build that made them: an old auto save is removed on start, and old slots are refused.

The zip comes from `miyoo/tools/miyoo_pack.py` (shared by all the ports, from the shared Miyoo kit) with this
port's `miyoo/game.json`. Given `--rom`, it checks the dump and puts it in as `App/StarFox64/rom.z64`.

## Controls

| Miyoo | N64 | In the game |
|---|---|---|
| D-pad | Stick | Steer (ramps up while held, so a tap makes a small correction) |
| A | A | Fire (hold to charge a lock-on) |
| B | B | Smart bomb |
| L1 / R1 | Z / R | Bank; double-tap to barrel roll |
| X | C-left | Boost |
| Y | C-down | Brake |
| SELECT | C-right | Answer ROB / radio prompts |
| L2 | C-up | Change the camera |
| R2 | L | Menu-only L |
| START | Start | Pause |
| MENU | | The game menu: resume, save state, load state, exit |

In the menu: up and down pick an entry, left and right change the slot (LOAD also has the auto save, one
step left of slot 1), A or START selects, and B backs out of a confirmation or closes the menu (so does MENU).

## Settings

The settings file is `App/StarFox64/sf64_display.cfg`, one `key=value` per line:

| Key | Default | Meaning |
|---|---|---|
| `fill=` | `1` | `0` keeps the N64's 8-pixel black border |
| `threads=` | `2` | `1` draws on one core only (for an A/B on the device) |
| `cpuclock=` | (the model's) | the CPU clock in MHz: 1500 on the Mini and 1800 on the Mini Plus by default (the stock clock is 1200), `0` leaves it as the OS set it |
| `reticle=` | `1` | `1`: crisp crosshairs; `0`: smoothed, as on the N64 |
| `bomb=` | `1` | `1`: a lighter smart bomb (a smaller blue flash, one glow ring, half the splash sprays); `0`: the N64's full blast (slow) |
| `fillrule=` | `1` | `1`: see-through effects draw each pixel once where two of their triangles meet, as on the N64; `0`: the old rule (a thin bright line along that edge) |
| `vdsoclock=` | `1` | `1`: the clock read in user space (the kernel's vDSO) when it is exact and faster; `0`: through the kernel (the same game either way) |

**The Bench:** an empty file named `bench` in `App/StarFox64/` runs the replay Bench instead of the
game (one stretch of Corneria replayed from a save state, 41 windows, then it quits by itself), with the
second core on and off in turn, in a folder of its own (`App/StarFox64/benchrun/`, which builds its own copy
of the game data on its first run). Delete `bench` to play again, and `benchrun/` when done.
`python3 port/tools/handheld/benchread.py benchrun/data/console.log` tabulates it ("two-cores" against
"one-core").

Environment variables for testing (set them in `launch.sh`): `SF64_MIYOO_MODE=<w>x<h>` forces the panel mode;
`SF64_ROTATE=0` draws the picture unturned (the MinUI launcher sets it: MinUI's SDL turns the panel itself);
`SF64_THREADS=1`; `SF64_CPUCLOCK=<MHz>`; `SF64_SPLITPX=<px>` / `SF64_SPLITROWS=<n>`, the smallest triangle
shared with the second core (1600 px of bounding box, 8 rows); `SF64_LOADPIC=0` shows the game's boot frames
before RESUME GAME's load; `SF64_PANEL_CHECK=1` compares the first 30 presented frames with a scalar
reference (the log says how many pixels differ); `SF64_LOG=-` keeps the output on the terminal.

## Building

Needs the union Miyoo Mini toolchain (GCC 8.3, glibc 2.28, the device's SDL 1.2 in the sysroot) at
`/opt/miyoomini-toolchain` (or `MIYOO_TOOLCHAIN`), your ROM at `port/baserom.us.rev1.z64`, and the decomp's
own N64 build of it (`tools/fetch_tools.sh`, then `make -C port toolchain && make -C port init`; README.md):

    make -C port -f Makefile.handheld PLAT=miyoo all -j4    # port/build/miyoo: sf64, recipes.bin, embedded.bin
    tools/build_miyoo.sh                                  # StarFox64-miyoo.zip, after the release gates
    tools/build_miyoo.sh <your ROM>                       # StarFox64-miyoo-PERSONAL.zip: your ROM inside

`tools/build_miyoo.sh` runs the release gates on the files that ship: every compiled source is
tracked in git, the game-data regions hold no bytes, no library variable is copied into the save-state region
(glibc's stdio handles excepted: the save states keep the live ones), no window of any shipped file matches the
ROM (zero tolerance), no host paths or authoring metadata, no co-author or generated-with lines in the commits, and the game's
folder holds exactly the executable, `recipes.bin`, `embedded.bin` and the packer's launcher, config and icon.
A PERSONAL zip contains your ROM (as `App/StarFox64/rom.z64`): keep it to yourself (git ignores it).

## Where the code is

- `port/src/port/sdl/miyoo_panel.c/.h`, `miyoo_menu.c`, `miyoo_font.h`, `miyoo_worker.c/.h`,
  `miyoo_clock.c/.h`: the shared kit (the shared Miyoo kit, cd7e109): the panel (2x, turned 180 degrees,
  centred), paths and SIGTERM; the menu; the second core; the CPU clock. The menu takes SF64's bilinear gate
  default from `hh_game.h`.
- `MIYOO` blocks in the existing files: `gfx_sdl12.c` (the 320x240 frame, gamma'd in 32 bits, to the panel;
  MENU; SIGTERM; the worker; LOADPIC), `gfx_soft.c` (the row split), `main.c` (the data folder and log, video
  before audio, the clock, the Bench), `controller_sdl.c` (buttons), `audio_sdl.c` (the granted rate),
  `savestate.c` (the auto save on SIGTERM), `port_romimage.c` (the ROM search and the first-start screen).
- `HANDHELD` is defined for the Miyoo build too (the shared port code: soft renderer, SDL 1.2, save states).

## Legal

No ROM, and no game data taken from one, is included in this repository or in the zip. You need your own copy
of the game. This project is not affiliated with or endorsed by Nintendo. Star Fox is a trademark of Nintendo.
