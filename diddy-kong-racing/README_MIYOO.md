# Diddy Kong Racing on the Miyoo Mini

A port of Diddy Kong Racing to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with
a software renderer, save states and an in-game menu. **There is no game data in it.** It copies
what it needs from your own US 1.1 ROM the first time you run it.

**Status:** plays on a Mini Plus with Onion (2026-10-09: races hold 30 fps with 8-11 ms of every
frame to spare, on one core). Since then the renderer also uses the Mini's second core (see "The
second core" below): built and checked frame for frame under qemu, not yet on the device. The
platform code is MK64's Miyoo port (`../mario-kart-64`), carried over to DKR's copy of
the same files.

## How it works on the Mini

- **Screen:** the game draws at 320x240, the N64's own width, and each frame is doubled to fill the 640x480 panel. On a bigger panel
  (752x560 on the Mini v4) the 640x480 picture sits in the middle with black borders.
- **Power:** when the OS shuts the game down (the power button, or any
  kill), the game saves the auto save and quits, and the next start asks RESUME GAME or NEW GAME,
  the same as EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or 50 s with no button, writes the auto save first, so RESUME
  GAME brings you back even if the Mini then powers itself off while asleep (`DKR_SLEEPSAVE=0` in
  launch.sh turns it off; [Sleep and wake](../docs/SLEEP-AND-WAKE.md)).
- **Menu:** the same menu as the other Miyoo ports (GoldenEye's `miyoo_menu.c`): RESUME, SAVE
  STATE, LOAD STATE, EXIT GAME, in a built-in 8x8 font at 640x480. Volume and brightness are
  left to the OS. Notices like SAVED SLOT 1 show in the top-left corner for two seconds.
- **Camera:** SELECT or R2 is the camera button (C-Up).
- **Two cores:** the Mini has two Cortex-A7s. The game runs on the first,
  and the second draws half of every big 3D triangle (its odd rows) and of the sky while the first
  draws the other half, and shows the frames. The picture is the same pixel for pixel either way;
  `threads=1` in the settings file goes back to one core.

## Install

1. Unzip `DiddyKongRacing-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **Diddy Kong Racing (USA) (Rev 1)**, SHA-1
   `6d96743d46f8c0cd0edb0ec5600b003c89b93755`, 12,582,912 bytes, as `.z64`, `.v64` or `.n64`
   (`.rom`, `.bin`, `.u64`, `.usa` and `.u1` also work), with any file name. US 1.0, PAL and
   Japanese carts are refused, and the screen says which one you have. The game also takes
   `App/DiddyKongRacing/rom.z64` (looked at first), and looks anywhere on the card down to two
   folders deep.
3. Start **Diddy Kong Racing**. The first start copies the game data out of the ROM
   (`App/DiddyKongRacing/data/dkr_assets.bin`, about 11 MB). Later starts skip that step, and the
   ROM isn't read again. To redo it, put an empty file named `rebuild` in
   `App/DiddyKongRacing/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/DiddyKongRacing/`, and every
other entry only starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/DiddyKongRacing/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Diddy Kong Racing.port`, `Roms/PORTS/Imgs/Diddy Kong Racing.png` |
| MinUI, MyMinUI | the home screen | `Roms/Diddy Kong Racing (DKR)/`, `Emus/miyoomini/DKR.pak/` |
| Allium | Apps | `Apps/Diddy Kong Racing.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in
`App/DiddyKongRacing/data/`: `dkr_assets.bin`, `eeprom.bin` (your game saves), `state_01.sav` to
`state_09.sav` and `state_quick.sav` (the save states and the auto save), the settings file,
`console.log` from the last session plus the two before it, and `launch.log` and
`games_launch.log`, which record each start.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/DIDDYKONG/`, `Roms/DIDDYKONG/`); this
zip no longer has it (the game is in Apps and Ports), so delete those two folders when you update,
or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

**Updating:** unzip the new zip over the old one. Game saves carry over. Save states only load in
the build that made them: an old auto save is removed on start, and old slots are refused.

The zip comes from `miyoo/tools/miyoo_pack.py` (shared by all the ports, from the shared Miyoo kit) with this port's `miyoo/game.json`. Given `--rom`, it checks the dump and puts it in as
`App/DiddyKongRacing/rom.z64`, the zip a ROM-builder page hands back.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | steer (plane: pitch), menus |
| A | accelerate, confirm |
| B | brake, back (B + Down reverses) |
| R1 or X | hop and powerslide; plane tricks |
| L1 or Y | fire item |
| L2 | N64 L |
| R2 or SELECT | camera zoom (C-Up) |
| START | pause |
| MENU | the game menu (resume, save/load state, exit) |

In the menu: up and down pick an entry, left and right change the slot (LOAD also has the auto
save, one step left of slot 1), A or START selects, and B backs out of a confirmation or closes the
menu (so does MENU).

## Settings

An optional settings file, `App/DiddyKongRacing/data/dkr_display.cfg`, takes one `key=value` per
line:

| Key | Default | Meaning |
|---|---|---|
| `rotate180=` | `1` | `0` draws the picture unturned, for an SDL that turns the panel itself |
| `threads=` | `2` | `1` draws on one core only, as before (for an A/B on the device) |
| `cpuclock=` | (the model's) | the CPU clock in MHz: 1500 on the Mini and 1800 on the Mini Plus by default (the stock clock is 1200), `0` leaves it as the OS set it; put back when the game quits |
| `fps=` | `30` | a firm 30 fps hold (since 1014b also in light scenes, which used to jump to 60 and back; `DKR_HOLD30=0`: the game's own rule again); `60` shows every frame as soon as it is drawn (see "Speed" below) |
| `fillrule=` | `1` | `0` draws see-through effects with the old edge rule (a thin bright line where two triangles meet) |
| `vdsoclock=` | `1` | `0` reads the clock through the kernel instead (the game is the same either way) |

An empty file named `benchmark` in `App/DiddyKongRacing/data/` runs the built-in benchmark once
at the next start (results in `bench.log` there).

Environment variables for testing (set them in `launch.sh`):
- `DKR_MIYOO_MODE=640x480` forces the panel mode.
- `DKR_ROTATE=0` or `1` overrides `rotate180=` (the MinUI launcher sets 0: MinUI's SDL turns the
  panel itself).
- `DKR_LOG=0` keeps the output on the terminal.
- `DKR_THREADS=1` or `2` overrides `threads=`.
- `DKR_CPUCLOCK=<MHz>` overrides `cpuclock=`; `DKR_FPS=30` or `60` overrides `fps=`. `DKR_HOLD30=0`: with fps=30, let light scenes run at 60 as the game's own rule does (before 1014b). `DKR_RATELOG=1`: every 300 frames, the fields per frame and game time against the clock in `console.log`.
- `DKR_WATCHDOG=<s>`: the stall report after that many seconds without a picture (default 3; 0 turns
  it off). See "If it freezes" below.
- `DKR_FIXEDSTACK=0`: the game loop on the process's own stack, as before (see "If it freezes").
- From the speed audit, each with its switch (`=0`: as before): `DKR_SAVEREAPPLY`
  (a state load keeps the card's newer save), `DKR_DECLICK` (no click after the menu or a load),
  `DKR_LOADSYNC` / `DKR_BOOTSYNC` (the renderer's flags match its cache after a load and at boot),
  `DKR_SNDPQUEUE=150..600` (the sound queue, 600), `DKR_SNDPGUARD`, `DKR_FOGTEST`, `DKR_PSROWS`.
- `DKR_LOADPIC=0`: show the game's boot frames before RESUME GAME's load, as before.
- `DKR_FASTROWS=0`: off; on the Mini it stays on, because the second core's share of a
  triangle is drawn in those rows (off: one core for the 3D).
- `DKR_SPLIT=<px>` is the smallest triangle (in pixels of area) shared with the second core
  (default 64; with at least 4 rows).
- `DKR_D32_CHECK=1` checks the first 30 presented frames against a scalar reference (the log says
  how many pixels differ); `DKR_PANEL_DUMP=<dir>` writes every 150th panel frame as a PPM.

## Building

Needs the union Miyoo Mini toolchain (the one Onion and MinUI build with: GCC 8.3, glibc 2.28,
and the device's SDL 1.2 in the sysroot) at `/opt/miyoomini-toolchain` (or `MIYOO_TOOLCHAIN`;
the MK64 port's `docker/Dockerfile` installs it). The build itself needs no ROM:

    make PLAT=miyoo -j6                        # build/miyoo/dkr
    sh tools/build_miyoo.sh <your ROM>         # DiddyKongRacing-miyoo.zip + the release gates

`tools/build_miyoo.sh` runs the release gates on the files that really
ship (it packs to a temporary zip, unpacks it and checks that): every compiled source is tracked
in git (and the packer, its templates and `miyoo/game.json`), the save-state region is sound (no
library variable copied into it), the game's folder holds only the executable and the packer's
launcher, config and icon, nothing in the zip matches the ROM (the leak scan), and no host paths
or authoring metadata get in. The packer refuses authoring metadata or host paths in anything it
zips too.

**The leak scan needs your ROM** (it is read, never shipped). Without one, the script stops and
asks for it; `NO_LEAK_SCAN=1 sh tools/build_miyoo.sh` runs every other gate and writes
`DiddyKongRacing-miyoo-UNSCANNED.zip`, which is not a release.

**A card-ready copy for yourself** (your ROM goes inside, so there's nothing else to copy):

    PERSONAL=1 sh tools/build_miyoo.sh <your ROM>

Unzip `DiddyKongRacing-miyoo-PERSONAL.zip` onto the card's root. It contains your ROM (as
`App/DiddyKongRacing/rom.z64`), so keep it to yourself (git ignores it).

## Where the code is

- `port/sdl/miyoo_video.c/.h`: the 320x240 surface, and the 2x scale to the 32 bpp panel, turned
  180 degrees (NEON for the game's frames, alpha forced opaque).
- `port/sdl/miyoo_menu.c`, `miyoo_font.h`: the menu, the resume prompt and the notices, ported
  from GoldenEye's (by way of MK64's) so every Miyoo port has the same menu. `handheld_menu.c` is
  not used by the Miyoo build.
- `port/sdl/miyoo_worker.c/.h`: the second core (the shared Miyoo kit, the same file in every
  port): one worker thread on core 1, the game thread on core 0, a handoff that spins briefly
  before sleeping.
- `MIYOO` blocks in the existing platform files handle the rest:
  - `gfx_soft.c`: the row split (`raster_tri`'s NEON rows, `tri_rows`, and the held sky's flush),
    and the PERF line's `mt` counts
  - `controller_sdl.c`: buttons
  - `gfx_sdl12.c`: the present (the 32-bit frame straight to the panel), MENU and SIGTERM; starts
    the worker and moves the present thread to core 1
  - `assets_dkr.c`: ROM search, and the first-start screen in the menu's font
  - `savestate.c`: keeps glibc's stdio across a load
  - `audio_sdl.c`: uses the sample rate SDL grants
  - `handheld_screen.c`: 320x240 by default, `rotate180=` and `DKR_ROTATE`
  - `main.c`: data folder and log
- `HANDHELD` is defined for the Miyoo build too: everything else is the shared platform layer.

## The second core

`gfx_soft.c` shares each 3D triangle that goes to the NEON rows (`tri_rows`, nearly every 3D
pixel) and has at least 4 rows and 64 pixels of area between both cores: the game thread (core
0) draws its even rows, the worker (core 1) its odd rows, and the triangle is finished before the
next one starts. The held sky (drawn in one go when something first needs what is under it) is
shared the same way, by screen line. Exact, frame for frame: `tri_rows` computes each row from the
triangle's constants and its own row number alone (the edge and interpolant bases are recomputed
per row; nothing steps from one row to the next), so a row is drawn the same by either core, and
the halves write different rows of the colour and depth buffers. All of the triangle's setup is
done before the handoff and the worker only reads it; what the rows write besides pixels is kept
per core: the worker has its own copy of the triangle's row state (DKR_ZSKIP's run of misses,
which only decides which spans get tested), its own span skip counts and pixel count. The present
thread moves to core 1 too, so core 0 is the game's alone.

The PERF lines in `console.log` end with the counts, e.g. `| mt on split 2627/18096 tris 98% px,
jobs 2627, sleeps 73`: split triangles out of all, the share of the pixels drawn that both cores
drew, the worker's jobs (triangles and sky flushes) and how often the game thread had to sleep
waiting for it. `threads=1` (or `DKR_THREADS=1`) prints `mt off` and draws exactly the same frames.

## Speed

- **The CPU clock.** The game runs the CPU at 1500 MHz on the Mini and 1800 MHz on the Mini Plus
  (the stock clock is 1200 MHz; Onion's own ceilings for its heaviest cores are 1600 and 1900), by
  the same PLL sequence as Onion's `cpuclock`, and puts the clock and governor back when it quits
  (also after a crash). `console.log` says `clock: 1200 -> 1500 MHz (model 283; ...)`. `cpuclock=`
  in the settings file picks another clock, `cpuclock=0` leaves it alone. A game killed outright
  (not quit) leaves the clock up until the OS sets it again.
- **Frame rate.** The game paces itself by the 60 Hz fields that pass, as on the console: when its
  frames take under 16.7 ms for 20 frames in a row it runs at 60 fps by itself, and a slower frame
  is held to 30 fps. On the Mini the default (since 1014b) holds every frame to 30 fps, light scenes
  too, so the rate no longer jumps to 60 and back (`DKR_HOLD30=0`: the game's own rule). `fps=60` (or
  `DKR_FPS=60`) drops the hold: every frame shows as soon as it is drawn and the game advances by the
  time that passed (a 20 ms frame alternates one and two fields, ~50 fps); nothing is blended.
  `fps:` in `console.log` says which.
- **Both cores** draw the 3D (see "The second core" below).

## If it freezes

A watchdog thread (`port/sdl/miyoo_watchdog.c`) watches the pictures. Once the first one is up, if
none follows for 3 seconds outside the menu, it writes a block of `WATCHDOG` lines to `console.log`
and syncs it to the card, so a freeze leaves evidence even when the game has to be killed:

- `WATCHDOG game:` what the game thread is waiting on (running its own code, the worker's rows of
  a triangle or lines of the sky, the present thread) and the loop's stage and iteration;
- `WATCHDOG worker:` the second core's handoff (jobs handed and done, which side is asleep, the job
  it is running), `WATCHDOG present:` the present thread (waiting for a picture, drawing, flipping
  the panel), `WATCHDOG audio:` the audio ring and the load mixer;
- `WATCHDOG pc <thread>:` each thread's program counter and return address (`addr2line -f -e dkr
  <pc> <lr>` with the build's binary names the functions);
- `WATCHDOG task` lines for every thread of the game from `/proc` (run state, CPU time, the cores
  it may use, the kernel function it sleeps in), and `WATCHDOG proc` lines for every process on the
  device that is running or real-time, with the load average.

It writes again every 10 seconds while the stall lasts (three blocks at most). At 6 seconds it
tries one recovery step, logged as `WATCHDOG recovery`: it wakes the second core's handoff and lets
every thread of the game run on both cores. `WATCHDOG over: pictures again after N ms` says when
pictures came back, and how long after the recovery step (right after it: the step is what freed
it). Please send `console.log` from any freeze.

The game loop also runs on a stack of its own at a fixed address (`stack: fixed at 0x50000000` in
the log). A save state holds a few addresses on the game thread's stack (an object spawned from an
entry built on the stack keeps it, and buffers keep the odd stale one). The binary and the game's
memory pool sit at fixed addresses, so every other pointer in a state means the same in any
process, but the device's kernel puts the process stack somewhere else at every start. Those few
addresses then pointed at random after RESUME GAME in a new process, while a load within one
session, or any start under qemu (whose stack is always at the same place), never had that.
With the fixed stack they mean the same in every process. `DKR_FIXEDSTACK=0` turns it off.

The freeze the watchdog then caught (2026-10-10, 55 fps) was the sound player: its handler empties
the game's sound event queue once per audio frame (1/30 s), and every game frame posts to it (each
kart's engine pitch, volume, pan). At 60 fps two frames' posts wait for one audio frame, and in a
busy race the 150-event queue overflowed; when the dropped post was the handler's own repeating
event, the queue ran empty and the handler spun for ever. The queue now holds 600 events (the most
a race reached at 60 fps under qemu was 190), and if a player's repeating event is ever lost anyway
it is posted again (`audio: the sound player's API event was lost` in the log) instead of spinning.
`console.log` ends with `audio: event queues: N posts dropped, M lost API events` (both 0 is right).

## Things to check on a new OS

- **The picture is upside down:** set `rotate180=0` (or `DKR_ROTATE=0` in that OS's launcher).
- **It doesn't start:** look in `App/DiddyKongRacing/data/`. No `games_launch.log` means the OS
  never ran the launcher; a `launch.log` with no fresh `console.log` means the game didn't start.
- **No sound:** `console.log` shows the rate SDL granted (`audio: SDL out ... Hz`), and
  `launch.log` shows whether `libpadsp.so` was preloaded.
