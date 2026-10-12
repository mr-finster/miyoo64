# Ocarina of Time on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a software renderer,
save states and a first-start data builder. **There is no game data in it.** It builds everything
from your own USA 1.0 ROM the first time you run it.

**Status:** plays on a Mini Plus with Onion from Apps at the game's own 20 fps, with both cores drawing
(2026-10-10). This build adds the CPU clock (1800 MHz on the Mini Plus, `cpuclock=` below), not yet
run on a device. Onion's Ports list, MinUI, Allium and spruce still need a first run.

## How it works on the Mini

- **Screen:** the game draws at 320x240, the N64's own size, and each frame is doubled to fill
  the 640x480 panel, turned 180 degrees as the panel is mounted.
- **Power:** when the OS shuts the game down (the power button, or any
  SIGTERM), the game saves the auto save and quits, and the next start asks RESUME GAME or NEW
  GAME, the same as EXIT GAME in the menu. (At that question itself, a shutdown just quits and
  keeps the auto save.)
- **Menu:** the same menu as the other Miyoo ports (GoldenEye's `miyoo_menu.c`): RESUME, SAVE
  STATE, LOAD STATE, EXIT GAME, in a built-in 8x8 font. Volume and brightness are left to the OS.
  Notices like SAVED SLOT 1 show in the top-left corner for two seconds.
- **First start:** screens while it looks for the ROM and builds the data, in the menu's font.
- **Both cores:** the Mini has two CPU cores. The big 3D triangles are drawn
  half by each core, with the picture the same pixel for pixel. `threads=1` in `debug.cfg` (below)
  draws on one core, for comparing.

## Install

1. Unzip `OcarinaOfTime-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **The Legend of Zelda: Ocarina of Time (USA 1.0)**,
   SHA-1 `ad69c91157f6705e8ab06c79fe08aad47bb57ba7` (the .z64 byte order), as `.z64`, `.v64`,
   `.n64`, `.u64`, `.rom` or `.bin`, with any file name. The game also takes
   `App/OcarinaOfTime/rom.z64`, and looks anywhere on the card down to two folders deep. Other
   versions (USA 1.1 / 1.2, European, Japanese) are refused with a message saying which one it is.
3. Start **Ocarina of Time**. The first start checks the ROM and builds the game data into
   `App/OcarinaOfTime/data/` (about 52 MB), once. After that the ROM is no longer needed (keep it
   on the card anyway: an update may need to rebuild).

The same zip works on every Miyoo OS. The game itself is in `App/OcarinaOfTime/`, and every other
entry only starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/OcarinaOfTime/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Ocarina of Time.port`, `Roms/PORTS/Imgs/Ocarina of Time.png` |
| MinUI, MyMinUI | the home screen | `Roms/Ocarina of Time (OOT)/`, `Emus/miyoomini/OOT.pak/` |
| Allium | Apps | `Apps/Ocarina of Time.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in
`App/OcarinaOfTime/data/`: the game data it built, the in-game save (`oot-psp-save.bin`), the save
states and the auto save, `console.log` from the last session, and `launch.log` and
`games_launch.log`, which record each start. To rebuild the game data, put an empty file named
`rebuild` in that folder; saves are not touched.

**Updating:** unzip the new zip over the old one. The game rebuilds what it needs by itself. The
in-game save carries over; save states don't, so save in-game before you update.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/OCARINA/`, `Roms/OCARINA/`); this zip
no longer has it (the game is in Apps and Ports), so delete those two folders when you update, or
the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

The zip comes from `miyoo/tools/miyoo_pack.py` (shared by all the ports, from the shared Miyoo kit) with this port's `miyoo/game.json`. Given `--rom`, it checks the dump and puts it in as
`App/OcarinaOfTime/rom.z64`, the zip a ROM-builder page hands back.

## Controls

| Miyoo | N64 |
|---|---|
| D-pad | Stick (in first person and aiming it eases in for finer aim) |
| A / B | A / B |
| L1 | Z (target) |
| R1 | R (shield) |
| X / Y | C-left / C-down |
| R2 | C-right |
| L2 | C-up (first person, Navi) |
| START (tap) | START (pause) |
| START + R1, or START + up | C-up |
| START + left / down / right | C-left / C-down / C-right |
| START + L1 | L (minimap on/off) |
| MENU | the game menu (resume, save/load state, exit) |

While you play the ocarina, the d-pad is the four C notes and A is A; B puts the ocarina away.

In the menu: up and down pick an entry, left and right change the slot, A or START selects, and
B backs out of a confirmation or closes the menu (so does MENU).

## Settings

The settings file is `App/OcarinaOfTime/data/debug.cfg`, one
`key=value` per line (the keys are in [SETTINGS.md](SETTINGS.md)). Miyoo only:
`rotate=0` draws the picture unturned, for an SDL that turns the panel itself. The same as an
environment variable in `launch.sh`: `OOT_ROTATE=0` (the MinUI launcher sets it: MinUI's SDL turns
the panel itself).

The second core and the clock, also Miyoo only (the same as `OOT_THREADS` / `OOT_SPLITPX` /
`OOT_CPUCLOCK` in `launch.sh`):

| Key | Default | Meaning |
|---|---|---|
| `threads=1` | off | draw on one core only, for an A/B (the picture is the same either way) |
| `splitpx=` | `800` | the pixels a triangle needs before both cores draw it (larger: fewer, bigger splits) |
| `cpuclock=` | the model's | the CPU clock in MHz while the game runs: 1800 on the Mini Plus and 1500 on the Mini by default (with margin, below Onion's ceilings of 1900 / 1600); `0` leaves it as the OS set it. It goes back when the game quits. |

Sound, Miyoo only: `audiofill=0` (or `OOT_AUDIOFILL=0`) gives the old audio buffering. The default keeps one
more SDL callback's worth queued (+16 ms), so a late audio update no longer leaves a gap (the `underruns +N` on
the AUDIO lines in `console.log`).

The v1.0.2 switches (SETTINGS.md) are all on by default: `modifyvtx=0` (the model seams
as before), `declick=0` (no fades around the menu and loads), `savereapply=0`, `loadsync=0`, `bootsync=0`, `loadpic=0`,
`fastrows=0`, `zskip=0`. An `fps=` line from an older test build is ignored.

`console.log` shows `worker: on, 2 cores online` at the start, and each PERF line ends in
`wk <jobs a frame> sl <waits that slept a frame> split <share of the 3D pixels drawn by both>`.
`launch.sh` turns the second core on if the OS left it off, and `data/launch.log` lists the
cores online.

## Building

Needs the union Miyoo Mini toolchain (the one Onion and MinUI build with: GCC 8.3, glibc 2.28,
and the device's SDL 1.2 in the sysroot) at `/opt/miyoomini-toolchain` (or `MIYOO_TOOLCHAIN`).
With your ROM at `port/baseroms/ntsc-1.0/baserom.z64`:

    cd port
    make setup VERSION=ntsc-1.0
    make -k CC_CHECK=true VERSION=ntsc-1.0 REGION=US    # the generated headers
    make -f Makefile.handheld PLAT=miyoo -j4
    cd .. && tools/build_miyoo.sh                        # OcarinaOfTime-miyoo.zip + the release gates

The Miyoo build derives its own `oot_recipes.bin` (how the game data comes from the ROM, as
numbers only), within a fixed budget of literal words.

`tools/build_miyoo.sh` runs the release gates: the recipes belong to this
link and derive again byte for byte within the literal budget, source provenance, the ROM-data
region empty in the binary, no library variables in save-state memory, the embedded-data and
whole-ROM leak scans (including the recipes), no host paths or authoring metadata, and only the
files `miyoo/game.json` lists. The packer refuses authoring metadata or host paths in anything it
zips.

**A card-ready copy for yourself** (your ROM goes inside, so there's nothing else to copy):

    tools/build_miyoo.sh <your ROM>       # or PERSONAL=1 tools/build_miyoo.sh

Unzip `OcarinaOfTime-miyoo-PERSONAL.zip` onto the card's root. It contains your ROM (as
`App/OcarinaOfTime/rom.z64`), so keep it to yourself (git ignores it).

## Where the code is

- `port/kit/platform/miyoo_menu.c`, `miyoo_font.h`: the menu, the resume prompt and the notices,
  GoldenEye's, so every Miyoo port has the same menu (`handheld_menu.h`: the menu API the game calls).
- `port/kit/platform/miyoo_worker.c/.h` (the shared Miyoo kit, 5eab41f): the second core's worker thread.
- `port/kit/platform/miyoo_clock.c/.h` (the shared Miyoo kit, 30024bc): the CPU clock (set at start from hh_convert.c, put back at exit).
- `MIYOO` blocks in the existing files handle the rest:
  - `gfx_soft.c`: the big triangles' rows split between the two cores (`raster_rows`, the row loop
    in `gfx_soft_rows.inc.c`)
  - `gfx_sdl12.c`: the 640x480 panel (the 2x NEON present on its own thread, turned 180
    degrees), MENU (Escape) and SIGTERM
  - `hh_input.c`: buttons
  - `hh_screen.c`: 320x240
  - `hh_romscreen.c`: the first-start screens in the menu's font
  - `hh_convert.c`: the ROM search (`rom.z64` next to the executable, the data folder,
    `Roms/N64`, the card)
  - `hh_port.c`: the data folder
  - `savestate.c`: SIGTERM saves the auto save
- `HANDHELD` is defined for the Miyoo build too (the shared port code: soft renderer, SDL 1.2, the kit).

## Things to check on a new OS

- **The picture is upside down:** set `rotate=0` in `data/debug.cfg` (or `OOT_ROTATE=0` in that
  OS's launcher).
- **It doesn't start:** look in `App/OcarinaOfTime/data/`. No `games_launch.log` means the OS
  never ran the launcher; a `launch.log` with no fresh `console.log` means the game didn't start.
- **No sound:** `launch.log` shows whether `libpadsp.so` was preloaded (it is whenever the OS's
  audioserver runs).
