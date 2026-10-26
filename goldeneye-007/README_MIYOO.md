# GoldenEye 007 on the Miyoo Mini

A port of GoldenEye 007 to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a
software renderer, save states and an in-game menu. **There is no game data in it.** It builds what
it needs from your own US ROM every time it starts.

**Status:** plays well on a Mini Plus with Onion, started from Apps (2026-10-02). The one-zip release
layout below (2026-10-09) still needs its first device run on Onion's Ports list, MinUI, Allium and
spruce.

## How it works on the Mini

- **Screen:** the game draws the N64's own 320x240 picture, and each frame is doubled to fill the
  640x480 panel (the picture is 4:3, as on the N64). Gameplay fills the whole screen: the N64 draws its view between black bands at
  the top and bottom, and this port shows more of the level there instead (`screen=n64` below brings
  the bands back).
- **Power:** when the OS shuts the game down (the power button, or any
  kill), the game saves the auto save and quits, and the next start asks RESUME GAME or NEW GAME,
  the same as EXIT GAME in the menu (also with the menu open, since 1.1.2).
- **Sleep:** a POWER press to sleep, or 50 s with no button, writes the auto save first, so RESUME
  GAME brings you back even if the Mini then powers itself off while asleep (`GE_SLEEPSAVE=0` in
  launch.sh turns it off; [Sleep and wake](../docs/SLEEP-AND-WAKE.md)).
- **Speed:** the Mini has two processor cores, and the second one shares the 3D drawing (the
  picture is the same pixel for pixel either way). The game raises the CPU clock while it runs (1500 MHz on the Mini,
  1800 MHz on the Mini Plus, from the OS's 1200; below what Onion itself asks for its heaviest games) and puts it
  back when it quits.
  Frames are not held to the screen's refresh: a frame starts as soon as the last one is done, while the game's
  own clock still follows real time. In the missions a frame may come at any screen refresh (up to 60 fps), and the
  motion steps by the real time between frames so it stays even (`minstep=2` holds 30 a second, `fracstep=0` steps
  whole refreshes).
- **Menu:** RESUME, SAVE STATE, LOAD STATE (slots 1-9 and the auto save), CONTROLS (the button remap,
  kept in `data/controls.cfg`), EXIT GAME, in a built-in font. Volume and brightness are left to the OS. Notices like SAVED SLOT 1 show in the top-left
  corner for two seconds.

## Install

1. Unzip `GoldenEye-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **GoldenEye 007 (USA)**, SHA-1
   `abe01e4aeb033b6c0836819f549c791b26cfde83`, as `.z64`, `.v64` or `.n64`, with any file name.
   The game also takes `App/GoldenEye/rom.z64`, and looks anywhere on the card down to two
   folders deep.
3. Start **GoldenEye 007**. The first start shows a progress screen while it checks the dump and keeps
   a copy as `App/GoldenEye/data/goldeneye.z64` (after that the original dump can go; keep the copy).
   A problem (no ROM found, the wrong dump, a full card) stays on the screen until you press a
   button. It fills the screen on a Mini v4 or Mini Flip too. Later starts go straight to the game. To redo the first start, put an empty file named
   `rebuild` in `App/GoldenEye/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/GoldenEye/`, and every other entry
only starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/GoldenEye/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/GoldenEye 007.port`, `Roms/PORTS/Imgs/GoldenEye 007.png` |
| MinUI, MyMinUI | the home screen | `Roms/GoldenEye 007 (GE007)/`, `Emus/miyoomini/GE007.pak/` |
| Allium | Apps | `Apps/GoldenEye 007.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Folders for other OSes just sit unused. Everything the game writes stays in `App/GoldenEye/data/`:
the game's save `eeprom.bin`, the save states `state_NN.sav` and the auto save, `console.log` from the
last session plus the two before it, and `launch.log` and `games_launch.log`, which record each start.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/GOLDENEYE/`, `Roms/GOLDENEYE/`); this
zip no longer has it (the game is in Apps and Ports), so delete those two folders and
`Roms/GoldenEye (GE007)/` when you update, or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

The zip comes from `miyoo/tools/miyoo_pack.py` (shared by all the ports, from the shared Miyoo kit)
with this port's `miyoo/game.json`. Given `--rom`, it checks the dump and puts it in as
`App/GoldenEye/rom.z64`.

## Controls

| Miyoo | Game |
|---|---|
| D-pad | walk forward and back, turn (a tap turns a little, holding ramps up) |
| L1 / R1 | strafe left / right; while aiming, lean |
| B | fire |
| Y (hold) | aim: the d-pad moves the crosshair; past its edge the view turns or looks up and down |
| A | action, doors, reload |
| X | next weapon; X+B previous weapon; X+A detonate remote mines |
| L2 | look down (crouch while aiming) |
| R2 | look up |
| START | pause (the watch) |
| MENU | the game menu (resume, save / load state, exit game) |
| SELECT | not used |

In the game's menus, A selects and B goes back. On the watch the d-pad is the N64's d-pad: a press
moves one weapon or gadget in the list, and holding it steps on slowly, as on the N64. In the MENU overlay: up and down pick an entry, left
and right change the slot, A or START selects (twice where it asks ARE YOU SURE?), and B backs out
of a question or closes the menu (so does MENU).

## Settings

An optional settings file, `App/GoldenEye/data/debug.cfg`, takes one `name=value` per line (`#`
starts a comment). The name is the setting without its `GE_`, lowercase (`hud1to1=0`) or in full
(`GE_HUD1TO1=0`). These matter on the Mini:

| Key | Default | What |
|---|---|---|
| `rotate` | 1 | 0 draws the picture unturned, for an SDL that turns the panel itself (the MinUI launcher sets 0) |
| `screen` | | `n64`: gameplay in the N64's view between black bands, as on the console (a little faster) |
| `cpuclock` | 1500 / 1800 | the CPU clock in MHz while the game runs (Mini / Mini Plus; the OS's is 1200, Onion's heaviest cores ask up to 1600 / 1900); `0` leaves it alone |
| `uncap` | 1 | `0`: frames start at screen refreshes as on the N64 (slower: a frame that takes 2.2 refreshes waits for the third) |
| `minstep` | 1 | in the missions a frame may follow the last one at the next screen refresh (up to 60 fps; with `fracstep=1` the motion stays even). `2`: frames held to 30 a second, as before 1014b |
| `fracstep` | 1 | the motion (walking, turning, the camera, guns) steps by the real time since the last frame instead of whole screen refreshes, so frames that come 1.5 or 2.4 refreshes apart move evenly; the game's speed and its frame counters are the same. `0`: whole refreshes, as before 1014b |
| `threads` | 2 | `1`: draw on one core only (the second core shares the 3D drawing; for comparing speed) |
| `intres` | 100 | the 3D's size in percent under the full-size HUD (85 or lower: faster, softer) |

The renderer's own keys:

| Key | Default | What |
|---|---|---|
| `hud1to1` | 1 | HUD text and counters at 1:1 (0: squeezed, the N64 layout) |
| `hudcorner` | 1 | HUD in the corners (0: the N64's margins) |
| `hudmsg1to1` | 1 | the top message box at 1:1, long lines wrapped (0: squeezed) |
| `introtext` | 1 | mission intro captions stay put when the intro is skipped (0: they drop 8 rows) |
| `halfsky` | 0 | 1: half-resolution sky (faster, softer; cracks the Dam's cliffs); 2: all but the Runway |
| `detailgrain` | 0 | 1: the ground detail texture where a level has one (~3 ms a frame) |
| `pshade` | 1 | shading fixed to surfaces (0: the N64's, where baked shadows slide on big floors) |
| `miplod` | 2 | wall and floor texture sharpness. 2: the N64's rule, half a level sharper; 1: the N64's; 3: sharpest (some shimmer); 0: the old pick (stripes crawl; a little faster) |
| `mipfar` | 0 | experimental: 2 or 3 leaves the far mip levels (that one on) unfiltered (faster, rougher far walls) |
| `trilin` | 1 | with `miplod=0`: floors blended across mip levels (0: ~1% cheaper) |
| `mipbias` | 0 | 1: blurrier distant textures (~0.9 ms faster on the Dam); -0.5: sharper |
| `ownfast` | 0 | 1: a faster-on-paper edge test for see-through effects (slower on the device) |
| `2dcomb` | 1 | the gun-barrel swirl behind SELECT FILE (0: a plain gradient) |
| `filledge` | 1 | stretch over the thin black strip at the menus' right edge |
| `2dbilin` | 1 | smooth enlarged 2D images (0: sharp) |
| `fogpx` | 0 | 0: per-vertex fog, as on the N64; 2: steady fog on the Surface snowfields; 1: every level |
| `fogpxshade` | 1 | with `fogpx` on: the snow keeps its shading where the fog is cut (0: it pulses while walking) |
| `dither` | 1 | dither the screen against sky banding (0: off) |
| `fillrule` | 1 | smoke and flames without a seam across the screen (0: the old rule) |
| `nearw` | 1 | walls right in front of the camera stay solid (0: the old near clip) |
| `tjfill` | 1 | close hairline cracks where level triangles don't quite meet (0: off) |

## Building

You need your own ROM and the Miyoo Mini toolchain (miyoomini-toolchain v0.0.3: GCC 8.3, glibc 2.28,
the device's SDL 1.2 in its sysroot) at `/opt/miyoomini-toolchain`, or `docker/Dockerfile.miyoo`:

1. `tools/fetch_decomp.sh` and `tools/port_assets.sh`.
2. `make -C port -f Makefile.handheld PLAT=miyoo -j6`
3. `tools/m1_build.sh "<your dump>"` once (the map the table scrub needs).
4. `tools/build_miyoo.sh "<your dump>"` writes `GoldenEye-miyoo.zip`.

`build_miyoo.sh` runs the release gates: testing unlocks off, provenance (only
tracked source), the ROM tables scrubbed and the leak scan against your ROM, no host paths or
metadata, and only the files `miyoo/game.json` lists.

**A card-ready copy for yourself** (your ROM goes inside, so there's nothing else to copy):

    PERSONAL=1 tools/build_miyoo.sh "<your dump>"

Unzip `GoldenEye-miyoo-PERSONAL.zip` onto the card's root. It contains your ROM (as
`App/GoldenEye/rom.z64`), so keep it to yourself (git ignores it).


## Things to check on a new OS

- **The picture is upside down:** put `rotate=0` in `App/GoldenEye/data/debug.cfg`.
- **It doesn't start:** look in `App/GoldenEye/data/`. No `games_launch.log` means the OS never ran
  the launcher (Apps entries don't write one); a `launch.log` with no fresh `console.log` means the
  game didn't start.
- **No sound:** `launch.log` shows whether `libpadsp.so` was preloaded.
