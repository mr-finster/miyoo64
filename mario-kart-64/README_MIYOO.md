# Mario Kart 64 on the Miyoo Mini

A port of Mario Kart 64 to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a
software renderer, save states and an in-game menu. **There is no game data in it.** It builds
everything from your own US ROM the first time you run it.

**Status:** plays well on a Mini Plus with Onion, started from Apps (2026-10-02; 30 fps with 8-11 ms
of every frame to spare, 2026-10-09). The one-zip release layout below (2026-10-09) still needs its
first device run on Onion's Ports list, MinUI, Allium and spruce. Since 2026-10-09 the renderer
also uses the Mini's second core (see "The second core"): checked frame for frame under qemu, not
yet on the device.

## How it works on the Mini

- **Screen:** the game draws at 320x240, the N64's own width, and each frame is doubled to fill the 640x480 panel. On a bigger panel
  (752x560 on the Mini v4) the 640x480 picture sits in the middle with black borders.
- **Power:** when the OS shuts the game down (the power button, or any
  kill), the game saves the auto save and quits, and the next start asks RESUME GAME or NEW GAME,
  the same as EXIT GAME in the menu.
- **Menu:** the same menu as the other Miyoo ports (GoldenEye's `miyoo_menu.c`): RESUME, SAVE
  STATE, LOAD STATE, EXIT GAME, in a built-in 8x8 font at 640x480. Volume and brightness are
  left to the OS. Notices like SAVED SLOT 1 show in the top-left corner for two seconds.
- **Two cores:** the Mini has two Cortex-A7s. The game runs on the first,
  and the second draws half of every big 3D triangle (its odd rows) while the first draws the
  other half, and shows the frames. The picture is the same pixel for pixel either way (see
  "The second core" below); `threads=1` in the settings file goes back to one core.

## Install

1. Unzip `MarioKart64-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **Mario Kart 64 (USA)**, SHA-1
   `579c48e211ae952530ffc8738709f078d5dd215e`, as `.z64`, `.v64` or `.n64`, with any file name.
   The game also takes `App/MarioKart64/rom.z64`, and looks anywhere on the card down to two
   folders deep.
3. Start **Mario Kart 64**. The first start builds the game data
   (`App/MarioKart64/assets.bin`, about 13 MB). Later starts skip that step.

The same zip works on every Miyoo OS. The game itself is in `App/MarioKart64/`, and every other
entry only starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/MarioKart64/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Mario Kart 64.port`, `Roms/PORTS/Imgs/Mario Kart 64.png` |
| MinUI, MyMinUI | the home screen | `Roms/Mario Kart 64 (MK64)/`, `Emus/miyoomini/MK64.pak/` |
| Allium | Apps | `Apps/Mario Kart 64.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in
`App/MarioKart64/`: `assets.bin`, the settings, `console.log` from the last session plus the two
before it, and in `data/` the save states, the auto save, and `launch.log` and
`games_launch.log`, which record each start.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/MARIOKART64/`, `Roms/MARIOKART64/`);
this zip no longer has it (the game is in Apps and Ports), so delete those two folders when you
update, or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

The zip comes from `miyoo/tools/miyoo_pack.py` (shared by all the ports, from the shared Miyoo kit) with this port's `miyoo/game.json`. Given `--rom`, it checks the dump and puts it in as
`App/MarioKart64/rom.z64`, the zip a ROM-builder page hands back.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | steer |
| A | accelerate |
| B | brake / reverse |
| R1 or X | hop / power slide |
| L1 or Y | use item |
| L2 | N64 L |
| R2 or SELECT | switch the race HUD (C-right) |
| START | pause |
| MENU | the game menu (resume, save/load state, exit) |

In the menu: up and down pick an entry, left and right change the slot, A or START selects, and
B backs out of a confirmation or closes the menu (so does MENU).

## Settings

An optional settings file, `App/MarioKart64/mk64_display.cfg`, takes one `key=value` per line:

| Key | Default | Meaning |
|---|---|---|
| `rotate180=` | `1` | `0` draws the picture unturned, for an SDL that turns the panel itself |
| `threads=` | `2` | `1` draws on one core only, as before (for an A/B on the device) |
| `cpuclock=` | (the model's) | the CPU clock in MHz: 1500 on the Mini and 1800 on the Mini Plus by default (the stock clock is 1200), `0` leaves it as the OS set it; put back when the game quits |
| `unsquish3d=` | `1` | `1` draws the 3D at its true shape; `0` is the older squeezed view |
| `hud=` | `1` | `1` draws the race HUD at its true shape; `0` is the older squeezed HUD |
| `neon=` | on | `0` turns off the ARM NEON speed-ups (for troubleshooting only) |

Environment variables for testing (set them in `launch.sh`):
- `MK64_MIYOO_MODE=640x480` forces the panel mode.
- `MK64_ROTATE=0` or `1` overrides `rotate180=` (the MinUI launcher sets 0: MinUI's SDL turns the
  panel itself).
- `MK64_LOG=0` keeps the output on the terminal.
- `MK64_THREADS=1` or `2` overrides `threads=`.
- `MK64_CPUCLOCK=<MHz>` overrides `cpuclock=`.
- `MK64_WATCHDOG=<s>`: the stall report after that many seconds without a picture (default 3; 0
  turns it off). See "If it freezes" below.
- `MK64_SPLIT=<px>` is the smallest triangle (in pixels of area) shared with the second core
  (default 64; with at least 4 rows).

## Building

`docker/Dockerfile` installs the union Miyoo Mini toolchain (the one Onion and MiniUI build with:
GCC 8.3, glibc 2.28, and the device's SDL 1.2 in the sysroot) at `/opt/miyoomini-toolchain`.
Build the image once, then:

    ./dev.sh 'cd /work/base-psp && make -f Makefile.handheld PLAT=miyoo -j8'
    ./dev.sh 'sh /work/build_miyoo.sh'          # MarioKart64-miyoo.zip + the release gates

`recipes.bin` needs recipes for this build's asset layout. With no
`tools/handheld/recipes_miyoo.json`, the build uses `tools/handheld/recipes_handheld.json`, and `emit_recipes.py`
compares the layout id. If the layout differs, the build stops and asks for:

    ./dev.sh 'cd /work/base-psp && make -f Makefile.handheld PLAT=miyoo assets-recipes'

which writes `tools/handheld/recipes_miyoo.json` (numbers only, safe to commit).

**A card-ready copy for yourself** (your ROM goes inside, so there's nothing else to copy):

    docker build -t mk64hh:build docker/    # once
    ./dev.sh 'cd /work/base-psp && make -f Makefile.handheld PLAT=miyoo -j8 && PERSONAL=1 sh /work/build_miyoo.sh'

Unzip `MarioKart64-miyoo-PERSONAL.zip` onto the card's root. It contains your ROM (as
`App/MarioKart64/rom.z64`), so keep it to yourself (git ignores it).

`build_miyoo.sh` runs the release gates: provenance, NOBITS asset region,
no libc variables in the save-state region, the ROM leak scan, no host paths, and only the files
`miyoo/game.json` lists. The packer refuses authoring metadata or host paths in anything it zips.

## Where the code is

- `base-psp/src/port/sdl/miyoo_video.c/.h`: the 320x240 surface, and the 2x scale to the
  32 bpp panel, turned 180 degrees.
- `base-psp/src/port/sdl/miyoo_menu.c`, `miyoo_font.h`: the menu, the resume prompt and the
  notices, ported from GoldenEye's so every Miyoo port has the same menu. `handheld_menu.c` is
  not used by the Miyoo build.
- `base-psp/src/port/sdl/miyoo_worker.c/.h`: the second core (the shared Miyoo kit, the same
  file in every port): one worker thread on core 1, the game thread on core 0, a handoff that
  spins briefly before sleeping.
- `MIYOO` blocks in the existing platform files handle the rest:
  - `gfx_soft.c`: the row split (`raster_tri`), and the PERF line's `mt` counts
  - `controller_sdl.c`: buttons
  - `gfx_sdl12.c`: MENU and SIGTERM; starts the worker and moves the present thread to core 1
  - `assets_handheld.c`: ROM search, and the first-start screen in the menu's font
  - `savestate.c`: no OS resume; keeps glibc's stdio across a load
  - `audio_sdl.c`: uses the sample rate SDL grants
  - `main.c`: data folder and log
- `HANDHELD` is defined for the Miyoo build too: everything else is the shared platform layer.

## The second core

`gfx_soft.c` hands each 3D triangle with at least 4 rows and 64 pixels of area to both cores: the
game thread (core 0) draws its even rows, the worker (core 1) its odd rows, and the triangle is
finished before the next one starts. Each row is computed from the triangle's constants and its
own row number alone (the renderer recomputes the row's edge and interpolant bases per row;
nothing steps from one row to the next), so a row is drawn the same by either core, and the two
halves write different rows of the colour and depth buffers. The worker redoes the triangle's
setup itself; everything that setup reads is fixed before the handoff, and what the rows write
besides pixels (the pixel count, the span skip's counts) is kept per core and added up after.
The present thread moves to core 1 too, so core 0 is the game's alone.

The PERF lines in `console.log` end with the counts, e.g. `| mt on split 412/1130 tris 97% px,
jobs 412, sleeps 3`: split triangles out of all, their share of the pixels drawn, and how often
the game thread had to sleep waiting for the worker. `threads=1` (or `MK64_THREADS=1`) prints
`mt off` and draws exactly what the shared drawing draws.

## Speed

- **The CPU clock.** The game runs the CPU at 1500 MHz on the Mini and 1800 MHz on the Mini Plus
  (the stock clock is 1200 MHz; Onion's own ceilings for its heaviest cores are 1600 and 1900), by
  the same PLL sequence as Onion's `cpuclock`, and puts the clock and governor back when it quits
  (also after a crash). `console.log` says `clock: 1200 -> 1500 MHz (model 283; ...)`. `cpuclock=`
  in the settings file picks another clock, `cpuclock=0` leaves it alone. A game killed outright
  (not quit) leaves the clock up until the OS sets it again.
- **Frame rate: 30 fps, as on the console.** MK64 draws a frame for every two 60 Hz logic ticks
  (`gTickSpeed = 2` in one-player races, 2 to 4 in split screen), and much of what moves between
  frames (animations, the camera, the HUD, effects) steps once per drawn frame, not per tick. Drawing
  at 60 would need either half-size ticks everywhere (those per-frame steps would run twice as fast)
  or interpolated frames (as the PC port SpaghettiKart does). Interpolation was tried (2026-10-10) and
  taken out again: the blended pictures ghost the image. An `fps=` line left in the settings file is
  ignored (one log line says so).
- **Both cores** draw the 3D (see "The second core" below).
- **The speed round** (`fastrows=`, `spanvec=`, `shadepx=`, `zskip=`, `wraprr=`, the
  same pixels either way, all on) runs on both cores' rows. On the Mini it is unmeasured. `speedbench=1` in the settings file cycles them per PERF
  window (AB lines in console.log) for a device A/B; a race drifts, so read many windows.

## Save states and sound

- **A state keeps the game's save:** after a load the card's `eeprom.bin` goes back over the state's
  copy when it is newer, so loading an older state never rolls back records or cups, nor writes old
  progress over newer (`MK64_SAVEREAPPLY=0`: as before). console.log says `eeprom: ...` after each load.
- **No click:** the sound fades out (3 ms) and back in (10 ms) around the menu, a load and an empty
  buffer, at whatever rate the Mini's SDL grants (`MK64_DECLICK=0`: as before).
- **After a load the renderer starts clean** (`MK64_LOADSYNC=0`: as before), and at boot its depth flags
  match its cache (`MK64_BOOTSYNC=0`). **RESUME GAME shows no boot frames** before the state loads
  (`MK64_LOADPIC=0`: as before).

## If it freezes

A watchdog thread (`src/port/sdl/miyoo_watchdog.c`, the same as Diddy Kong Racing's) watches the
pictures. Once the first one is up, if none follows for 3 seconds outside the menu, it writes a
block of `WATCHDOG` lines to `console.log` and syncs it to the card: what the game thread is
waiting on (its own code, the worker's rows of a triangle, the present thread) with the loop's
stage, iteration and game state; the second core's handoff (jobs handed and done, which side is
asleep, the job it runs); the present thread (waiting, drawing, flipping); the audio ring and the
backstop mixer; each thread's program counter (`addr2line -f -e mk64 <pc> <lr>`); every thread of
the game from `/proc` (state, CPU time, cores, the kernel function it sleeps in); and every running
or real-time process on the device. Again every 10 seconds while it lasts (three blocks at most);
at 6 seconds one recovery step (`WATCHDOG recovery`: the handoff woken, every thread allowed on
both cores), and `WATCHDOG over` says when pictures came back. Please send `console.log` from any
freeze. (MK64's own `STALL <n> ms:` lines are slow frames, not freezes.)

## Things to check on a new OS

- **The picture is upside down:** set `rotate180=0` (or `MK64_ROTATE=0` in that OS's launcher).
- **It doesn't start:** look in `App/MarioKart64/data/`. No `games_launch.log` means the OS never
  ran the launcher; a `launch.log` with no fresh `App/MarioKart64/console.log` means the game
  didn't start.
- **No sound:** `console.log` shows the rate SDL granted (`audio: SDL out ... Hz`), and
  `launch.log` shows whether `libpadsp.so` was preloaded.
