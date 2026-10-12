# Super Mario 64 on the Miyoo Mini

Super Mario 64 for the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a software
renderer, save states and the decompiled game code. **There is no game data in it.** The first time you run it, it builds what it needs from
your own US ROM.

**Status:** built and checked under qemu (2026-10-11); not yet run on a device. The
platform code follows the other Miyoo ports (Mario Kart 64, Diddy Kong Racing, Ocarina of Time,
GoldenEye: their folders in this repository) with the shared kit modules from the shared Miyoo kit.

## How it plays

- **Screen:** the game draws the N64's own 320x240, and each frame is doubled to fill the 640x480 panel. On a bigger panel (752x560 on the
  Mini v4) the picture sits in the middle with black borders. The HUD, text and menus are where the
  N64 puts them.
- **Frame rate:** 30 fps, the game's own rate on the N64. Each frame is shown as soon as it is
  drawn; nothing is blended or interpolated. A frame that takes longer plays a little slower, as on
  the console.
- **Power:** when the OS shuts the game down (the power button, or any
  kill), the game writes the auto save and quits, and the next start asks RESUME GAME or NEW GAME,
  the same as EXIT GAME in the menu.
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, EXIT GAME, in a
  built-in 8x8 font. Volume and brightness are left to the OS. Notices like SAVED SLOT 1 show in the
  top-left corner for two seconds.
- **Camera:** L2 or SELECT is C-up (look around) and R2 is C-down, plain buttons.
- **Two cores:** the Mini has two Cortex-A7s. The game runs on the first. The
  second draws half of every big 3D triangle (its odd rows) while the first draws the other half,
  scans half the screen for the mesh cracks, makes the frame's sound while the first draws, and copies,
  doubles and shows the finished frame and clears the next one's depth while the first runs the next
  frame's game logic. The picture and the sound are the same either way; `threads=1` goes back to one
  core.
- **The CPU clock:** 1500 MHz on the Mini and 1800 MHz on the Mini Plus while the game runs (the
  stock clock is 1200), put back when it quits.

## Install

1. Unzip `SuperMario64-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **Super Mario 64 (USA)**, SHA-1
   `9bef1128717f958171a4afac3ed78ee2bb4e86ce`, 8,388,608 bytes, as `.z64`, `.v64` or `.n64`
   (`.rom`, `.bin`, `.u64`, `.usa` and `.u1` also work), with any file name. The European, Japanese
   and Shindou versions are refused. The game also takes `App/SuperMario64/rom.z64` (looked at
   first), and looks anywhere on the card down to two folders deep.
3. Start **Super Mario 64**. The first start builds the game data from the ROM
   (`App/SuperMario64/data/assets.bin`, about 10 MB). Later starts skip that step, and the ROM isn't
   read again. To redo it, put an empty file named `rebuild` in `App/SuperMario64/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/SuperMario64/`, and every other entry
only starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/SuperMario64/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Super Mario 64.port`, `Roms/PORTS/Imgs/Super Mario 64.png` |
| MinUI, MyMinUI | the home screen | `Roms/Super Mario 64 (SM64)/`, `Emus/miyoomini/SM64.pak/` |
| Allium | Apps | `Apps/Super Mario 64.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in `App/SuperMario64/data/`:
`assets.bin`, `eeprom.bin` (the four Mario files), `state_01.sav` to `state_09.sav` and `state_quick.sav` (the save
states and the auto save), the settings file, `console.log` and `log.txt` from the last session plus the two before
it, and `launch.log` and `games_launch.log`, which record each start.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/SUPERMARIO64/`,
`Roms/SUPERMARIO64/`); this zip no longer has it (the game is in Apps and Ports), so delete those
two folders when you update, or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

**Updating:** unzip the new zip over the old one. Game saves carry over. Save states only load in the
build that made them: an old auto save is removed on start, and old slots are refused. An update may
rebuild the game data from the ROM, so keep the ROM on the card.

## Controls

| Miyoo | N64 | |
|---|---|---|
| D-pad | stick | full speed in 8 directions |
| A / B | A / B | jump / punch, kick, dive |
| X | Z | crouch, ground pound |
| Y | R | camera mode |
| L1 / R1 | C-left / C-right | turn the camera |
| L2 or SELECT | C-up | look around |
| R2 | C-down | camera back |
| START | START | pause |
| MENU | | the game menu (resume, save/load state, exit) |

In the menu: up and down pick an entry, left and right change the slot (LOAD also has the auto save,
one step left of slot 1), A or START selects, and B backs out of a confirmation or closes the menu (so
does MENU).

## Settings

`App/SuperMario64/data/sm64_display.cfg`, one `key=value` per line:

| Key | Default | Meaning |
|---|---|---|
| `threads=` | `2` | `1` draws on one core only (for an A/B on the device) |
| `cpuclock=` | (the model's) | the CPU clock in MHz: 1500 on the Mini and 1800 on the Mini Plus by default; `0` leaves it as the OS set it |

Environment variables for testing (set them in `launch.sh`): `SM64_THREADS=1|2` and
`SM64_CPUCLOCK=<MHz>` override the file; `SM64_ROTATE=0` draws the picture unturned (the MinUI launcher
sets it: MinUI's SDL turns the panel itself); `SM64_MIYOO_MODE=640x480` forces the panel mode;
`SM64_PANEL_CHECK=1` checks the first 30 presented frames against a scalar reference (the log says
how many pixels differ); `SM64_SPLIT=<px>` is the smallest triangle (in pixels of area) shared with the
second core (default 64, with at least 4 rows); `SM64_LOG=0` keeps the output on the terminal.

The PERF lines in `console.log` (every 25 s) end with the second core's counts, e.g. `| mt on split
1200/5000 tris 90% px, jobs 1500, sleeps 30, refs 0/0`: split triangles out of all, the share of the 3D
pixels drawn by both cores, the worker's jobs, how often the game thread had to wait for it, and the
rows a share handed back to the game thread (and shares that stopped early). The line starts with
`work X ms avg (iter Y ...) rendered N/M`: work is the time a frame takes without the pacing sleep, so
under 33.3 ms means 30 fps with room, over it means the game plays a little slower.

Each part of the second core's work has its own switch (`=0`: as before, for an A/B):
`SM64_PRESJOB` (the copy, doubling and depth clear on the second core), `SM64_AUDIOJOB` (the frame's
sound there), `SM64_CRACKSPLIT` (the crack scan on both cores), `SM64_SPLITREF` (a share that hands a
row back goes on with its other rows), `SM64_SPLITEDGE` (keeps two cores from ever writing the same
pixel: leave it on).

**A/B on the device without a computer:** put `bench=1` in `App/SuperMario64/data/sm64_bench.cfg` and
play normally (a heavy level such as Jolly Roger Bay or Bob-omb Battlefield is best) for a few minutes.
The PERF lines then come every 5 s and alternate `bench all-on` with one switch off: `mt-off` (one
core, as `threads=1`), `presjob-off`, `audiojob-off`, `cracksplit-off`, `splitref-off`. Compare each
OFF window's `work` with the all-on windows next to it. Delete the file to go back to normal.

## Building

Needs the union Miyoo Mini toolchain (GCC 8.3, glibc 2.28, the device's SDL 1.2 in the sysroot) at
`/opt/miyoomini-toolchain` (or `MIYOO_TOOLCHAIN`), the decomp's data (`sh tools/decomp_data.sh`) and
your ROM as `port/baserom.us.z64` (the decomp's build extracts textures and sounds from it; none of
that ships):

    cd port && make PLAT=miyoo -j4                  # build/us_miyoo/sm64, assets.bin, recipes.bin
    make PLAT=miyoo assets-recipes                  # only when the asset layout changes (slow)
    cd .. && sh tools/build_miyoo.sh                # SuperMario64-miyoo.zip + the release gates
    PERSONAL=1 sh tools/build_miyoo.sh              # SuperMario64-miyoo-PERSONAL.zip: your ROM inside

`tools/build_miyoo.sh` runs the release gates on the files that ship: every compiled
source is tracked in git, the asset region carries no bytes, every code pointer in the game data lands
on a function, no library variable is copied into the save-state region (glibc's stdio aside), nothing
matches the ROM (the leak scan), no host paths or authoring metadata, only the listed files; then the
zip's own listing. The PERSONAL zip holds your ROM as `App/SuperMario64/rom.z64`: keep it to yourself.

## Where the code is

- `port/kit/platform/miyoo_panel.c/.h`, `miyoo_menu.c`, `miyoo_font.h`, `miyoo_worker.c/.h`,
  `miyoo_clock.c/.h`: the shared kit modules (the shared Miyoo kit, as they are): the panel and
  paths, the menu, the second core, the CPU clock.
- `MIYOO` blocks in the platform files:
  - `src/hh/gfx_soft.c`: the second core's row split (`raster_tri`'s NEON rows, `tri_rows`) and the
    PERF line's `mt` counts
  - `src/hh/gfx_sdl12.c`: 320x240, the present (the 32-bit frame straight to the panel), MENU,
    SIGTERM, the worker, `sm64_display.cfg`
  - `src/hh/hh_main.c`: the data folder and log, the clock
  - `src/hh/assets_handheld.c`: the ROM search, and the first-start screen in the menu's font
  - `src/hh/audio_sdl.c`: video before audio, the sample rate SDL grants
  - `src/hh/controller_sdl.c`: buttons
  - `kit/platform/savestate.c`: the power-off is SIGTERM's auto save
- `HANDHELD` and `MIYOO` are both defined for the Miyoo build; everything outside the `MIYOO` blocks is
  the common handheld code.
- `miyoo/`: the packer and its templates (the shared Miyoo kit) and this port's `game.json`.
