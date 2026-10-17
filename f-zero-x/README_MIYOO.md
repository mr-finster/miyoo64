# F-Zero X on the Miyoo Mini

A port to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a software renderer and
save states. **There is no game data in it.** The game reads your own US ROM while it runs, as the N64
reads the cartridge.

**Status:** built and checked under qemu (boot, menus, a Mute City GP race, the menu, save and load
across processes, the power-off save and RESUME GAME, the first-start and no-ROM screens, the second
core frame for frame); not yet run on a Miyoo. The platform code is the
shared Miyoo kit (the shared Miyoo kit: the panel, the menu, the second core and the clock), as
in the Mario Kart 64, Diddy Kong Racing, Ocarina of Time and GoldenEye Miyoo builds.

## How it works on the Mini

- **Screen:** the game draws the N64's own 320x240, doubled onto the 640x480 panel, and fills it: the N64's
  black TV-safe border (which a TV's overscan used to hide) is gone, at the right proportions. A race's 3D is
  drawn out to the screen's edges (the scene's own pixels in the few extra rows at the top and bottom, nothing
  stretched), the race HUD stays pixel-sharp at the N64's size, moved out with the picture, and menu text is
  sharp where it fits. `FZX_FILL=0` in `launch.sh` brings back the bordered N64 picture. On a bigger panel
  (752x560 on the Mini v4) the 640x480 picture sits in the middle with black borders.
- **Speed:** the game runs at its own 60 frames a second: every frame is shown as soon as it is
  drawn. A frame that isn't ready in time is skipped (the game itself never slows down or speeds
  up), with no smoothing, blending or frame pacing tricks.
- **Two cores:** the game runs on the first core, and the second draws half of every 3D frame (the
  odd lines) while the first draws the even ones. The picture is the same pixel for pixel either
  way; `FZX_THREADS=1` in `launch.sh` goes back to one core.
- **CPU clock:** 1500 MHz on the Mini and 1800 MHz on the Mini Plus while the game runs (the stock
  clock is 1200), put back when it quits. `FZX_CPUCLOCK=<MHz>` in `launch.sh` picks another,
  `FZX_CPUCLOCK=0` leaves the clock alone.
- **Power:** when the OS shuts the game down (the power button, or any
  kill), the game saves the auto save and quits, and the next start asks RESUME GAME or NEW GAME,
  the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or 50 s with no button, writes the auto save first, so RESUME
  GAME brings you back even if the Mini then powers itself off while asleep (`FZX_SLEEPSAVE=0` in
  launch.sh turns it off; [Sleep and wake](../docs/SLEEP-AND-WAKE.md)).
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, EXIT GAME, in a
  built-in font. Volume and brightness are left to the OS. Notices like SAVED SLOT 1 show in the
  top-left corner for two seconds.
- **Camera:** SELECT or R2 changes the camera (C-right).

## Install

1. Unzip `FZeroX-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. The ROM: your own **F-Zero X (USA)**, SHA-1 `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`, 16 MB, as
   `.z64`, `.v64` or `.n64` (`.rom`, `.bin` and `.u64` also work), any file name. Put it at
   `App/FZeroX/rom.z64` (looked at first), or in `Roms/N64/`, or anywhere on the card down to two
   folders deep. **Keep it there:** the game reads it while it runs. Japanese and European carts are
   refused, and the screen says which one you have.
3. Start **F-Zero X**. The first start checks the whole ROM (a few seconds, with a progress bar) and
   remembers the result; it checks again only when the file changes. To redo it, put an empty file
   named `rebuild` in `App/FZeroX/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/FZeroX/`, and every other entry only
starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/FZeroX/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/F-Zero X.port`, `Roms/PORTS/Imgs/F-Zero X.png` |
| MinUI, MyMinUI | the home screen | `Roms/F-Zero X (FZX)/`, `Emus/miyoomini/FZX.pak/` |
| Allium | Apps | `Apps/F-Zero X.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in `App/FZeroX/data/`:
`fzerox.sram` (your game saves: records, unlocks, settings), `state_01.sav` to `state_09.sav` and
`state_quick.sav` (the save states and the auto save), `romcheck.txt` / `rompath.txt` (the first
start's result), `console.log` from the last session plus the two before it, and `launch.log` and
`games_launch.log`, which record each start.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/FZEROX/`, `Roms/FZEROX/`); this zip
no longer has it (the game is in Apps and Ports), so delete those two folders when you update, or
the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

**Updating:** unzip the new zip over the old one. Game saves carry over. Save states only load in the
build that made them: an old auto save is removed on start, and old slots are refused.

**A zip with your ROM inside** (for your own card only): `PERSONAL=1 sh tools/build_miyoo.sh` (see
Building) puts it in as `App/FZeroX/rom.z64`. Never share that zip.

## Controls

| Miyoo | F-Zero X |
|---|---|
| D-pad | Steer; nose up / down in the air; menus |
| A | Accelerate / OK |
| B | Boost (after the first lap) / back |
| X | Air brake (C-down) |
| Y | Look back (C-up) |
| L1 / R1 | Lean left / right. Double tap = side attack; hold one and tap the other = spin attack |
| START | Pause / OK |
| SELECT or R2 | Change camera (C-right) |
| L2 | C-left (machine select's Novice) |
| START + d-pad | The four C buttons (machine-select difficulty) |
| START + L1 | N64 L |
| MENU | The menu: resume, save / load state (slots 1-9, and the auto save), exit |

In the menu: up and down pick an entry, left and right change the slot (LOAD also has the auto save,
one step left of slot 1), A or START selects (twice where it asks ARE YOU SURE?), and B backs out of
a question or closes the menu (so does MENU).

## Switches (environment, set in `App/FZeroX/launch.sh`)

- `FZX_THREADS=1`: one core only (the same frames).
- `FZX_FILL=0`: the N64's picture with its black TV-safe border, as before the fill.
- `FZX_CPUCLOCK=<MHz>` (0 = leave the clock alone).
- `FZX_PACE=cadence`: cadence pacing (it picks an even 60/30 or a 52.5/50/48/45/40 cadence ahead)
  instead of showing each frame as soon as it is drawn.
- `FZX_ROTATE=0`: the picture unturned, for an SDL that turns the panel itself (MinUI's launcher sets
  it). `FZX_MIYOO_MODE=<w>x<h>` forces the panel mode.
- `FZX_PANEL_CHECK=1`: the first 30 presented frames are compared with a scalar reference (the log
  says how many pixels differ).
- `FZX_WALLLOD=1`: the wall sampling made for a 240-column screen (off here, where each N64 pixel is a
  pixel).
- `FZX_SAVEREAPPLY=0`, `FZX_DECLICK=0`, `FZX_LOADSYNC=0`, `FZX_BOOTSYNC=0`, `FZX_LOADPIC=0` (each "as
  before" its round), and these, all on by default:

| Switch | Off gives |
|---|---|
| `FZX_FILL=0` | the N64's black TV-safe borders |
| `FZX_FILL100=0` | with the 3D at 100% (the default), the N64's black TV-safe borders and the menu text squeezed, as before 2026-10-09 (`FZX_FILL=0` turns the fill off at every 3D size); `FZX_FILL100=ab` alternates the two every 5 s |
| `FZX_HUDUNSQ=0` | the race HUD squeezed to 240 wide, as before |
| `FZX_MENU1TO1=0` | menu text stretched with the screen |
| `FZX_LOGOUNSQ=0` | the boot N64 logo squeezed |
| `FZX_FINECAD=0` | only 60, 45, 40 and 30 fps pacing in races |
| `FZX_CLIPGB=0` | the triangle clipping used for speed before 2026-09-30 (1-3 levels of shading off the console's) |
| `FZX_FILLRULE=0` | see-through effects drawn with the old edge rule: a pixel where two of their triangles meet drawn twice (a thin bright line along that edge); on, each such pixel is drawn once, as on the console |
| `FZX_TEXFOG=0` | the city floor under the tracks (Mute City's and every venue floor) drawn dark and unhazed, as before; on, it fades into the course's fog colour with distance, as on the console |
| `FZX_VDSOCLOCK=0` | where the generic timer is not used, the clock read through the kernel, as before; on, through the kernel's vDSO when that is exact and faster (the same game either way) |
| `FZX_FASTROWS=0` | the rows of the main textured triangles set up in the generic loop, as before (the same picture; on, a smaller NEON row loop) |
| `FZX_SPANVEC=0` | the textured rows' per-span values stepped one by one, as before (the same picture; on, as two NEON vectors) |
| `FZX_WALLLOD=0` | the track's walls sampled once a pixel, as before (their stripes break up and crawl more than on the console in the distance); on, sampled where the console's own pixels fall in each pixel, and `FZX_WALLLOD=ab` alternates the two every 5 s |
| `FZX_MIRRORCLAMP=0` | the machines' decals (mirrored and clamped 16-texel tiles) doubled with their mirrored copy: big decal-coloured panels, such as the Blue Falcon's yellow shoulders; on, clamped at the tile's edge as on the console (thin stripes) |
| `FZX_STATICFADE=0` | the static fade (machine settings to the race) as an even fade; on, the picture is eaten away in random specks, as on the console |
| `FZX_CCSHADEA=0` | the debris of an exploding machine all in the machine's colour; on, each shard whitened by its own alpha (pale and white shards), as on the console |
| `FZX_FLATSHADE=0` | triangles drawn with smooth shading off still shaded smoothly (the debris ran into yellow and white gradients); on, one colour a triangle, as on the console |
| `FZX_BLENDCOL=0` | the attack highlight (side or spin attack) and the machine settings screen's outline drawn with the machine's own textures (a thin blue-white edge); on, in the blend colour (a solid red rim, a dark outline), as on the console |

## Performance

Not measured on a Miyoo yet. The estimate, from qemu:
- The picture is 320x240, the clock is 1500 MHz on the Mini and 1800 MHz on
  the Plus, and the second core takes the 3D's critical path to 0.65 of one core's (qemu, the thread
  CPU time of a 20-second race stretch: each core's share, the slower one per batch summed).
- That puts a race frame at about 12 ms (15 in the heaviest stretches) on the Mini and 10 (12.5) on the
  Plus, against the 16.7 ms that 60 fps needs: 60 on the Plus, 60 most of the time on the Mini with an
  occasional skipped frame in the busiest moments. The menus run at 60.

The log (`App/FZeroX/data/console.log`) has a PERF line every 10 seconds (`pics` = frames shown in
that second, `skip` = frames not drawn, `gfx` = drawing time per frame) and an MT line after it
(`3D flushes split` = the 3D shared with the second core, and the clock).

## Building

Needs the union Miyoo Mini toolchain (GCC 8.3, glibc 2.28, the device's SDL 1.2 in its sysroot) at
`/opt/miyoomini-toolchain` (or `MIYOO_TOOLCHAIN`), and the decomp's own N64
build from your ROM first (README.md, "Building"; the port takes its N64 numbers from it, never data):

    make -C game -f Makefile.handheld PLAT=miyoo gen
    make -C game -f Makefile.handheld PLAT=miyoo -j4       # game/build/miyoo/fzerox
    sh tools/build_miyoo.sh                               # FZeroX-miyoo.zip + the release gates
    PERSONAL=1 sh tools/build_miyoo.sh                    # FZeroX-miyoo-PERSONAL.zip, your ROM inside

`tools/build_miyoo.sh` runs the release gates on the files that ship: provenance (every
compiled file tracked in git), the N64 RAM image NOBITS, save states holding game memory only, no
library variable copied into the executable, a leak scan of everything in the zip against your ROM,
no host paths or authoring metadata, and exactly the expected files in the zip. The ROM is read for
the scan, never shipped (except in the PERSONAL zip).

## Where the code is

- `game/src/port/miyoo/`: the Miyoo kit (the shared Miyoo kit, the same files in every port, with
  the console log for their messages): `miyoo_panel` (the panel, paths, SIGTERM, keys), `miyoo_menu`
  (menu, resume prompt, notices), `miyoo_worker` (the second core), `miyoo_clock`; `hh_game.h` names
  the game for them.
- `MIYOO` blocks in the platform files:
  - `gfx_sdl12.c`: 320x240, the 32-bit frame to the panel, MENU (Escape), SIGTERM, no power calls; starts
    the worker and moves the present thread to core 1;
  - `gfx_soft.c`: the second core in `vis_flush` (each 3D batch's odd rows), the wall sampling off;
  - `input.c`: the Mini's keys; `rom_find.c`: the ROM search and its screens in the menu font;
  - `port_main.c`: the data folder, the clock, FAST pacing, the MT line; `audio_sdl.c`: the granted
    sample rate, the video up first; `savestate.c`: the auto save on SIGTERM.
- `HANDHELD` and `HANDHELD_DEVICE` are defined for the Miyoo build too (the shared port code: SDL 1.2,
  NEON, save states).
- `miyoo/game.json`, `miyoo/templates/`, `miyoo/tools/miyoo_pack.py`: the zip (the shared Miyoo kit).
