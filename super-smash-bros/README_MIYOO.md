# Super Smash Bros. on the Miyoo Mini

Super Smash Bros. for the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), with a software
renderer, save states and the decompiled game code. **There is no game data in it.** It copies what it
needs from your own US ROM the first time you run it.

No multiplayer: VS Mode is one player against CPUs.

**Status (2026-10-11):** built and checked under qemu with a real ROM (first start, frames identical on one and two
cores, save states, the OS's power-off, the menu); not yet run on a device.

## How it plays

- **Screen:** the game draws the N64's own 320x240 picture, and each frame is doubled to fill the
  640x480 panel (on the 752x560 Mini v4 panel, the 640x480 picture sits in the middle). The picture is
  4:3, as on the N64. The N64 draws its picture inside a thin black border (a TV's edge hid it); that
  frame is stretched over the whole screen as it goes out (sharp: one column in 15 and one row in 11 shown twice). `SSB64_FILLBORDER=0`
  below brings the border back.
- **Speed:** the game runs at its own 60 frames a second wherever the Mini can draw that fast. A picture
  is shown as soon as it is ready; when one comes late, that one picture is skipped and the game keeps
  its speed. There is no frame blending or interpolation. The Mini has two cores: the second draws half of every big 3D triangle (its odd rows) while the first draws
  the other half, and the picture is the same pixel for pixel either way. The game raises the CPU clock
  while it runs (1500 MHz on the Mini, 1800 MHz on the Mini Plus, from the OS's 1200; below what Onion
  itself asks for its heaviest games) and puts it back when it quits.
- **Power:** when the OS shuts the game down (the power button, or any kill),
  the game saves the auto save and quits, and the next start asks RESUME GAME or NEW GAME, the same as
  EXIT GAME in the menu.
- **Sleep:** a POWER press to sleep, or 50 s with no button, writes the auto save first, so RESUME
  GAME brings you back even if the Mini then powers itself off while asleep (`SSB64_SLEEPSAVE=0` in
  launch.sh turns it off; [Sleep and wake](../docs/SLEEP-AND-WAKE.md)).
- **Menu:** the same menu as the other Miyoo ports: RESUME, SAVE STATE, LOAD STATE, EXIT GAME, in a
  built-in font. Volume and brightness are left to the OS. Notices like SAVED SLOT 1 show in the
  top-left corner for two seconds.

## Install

1. Unzip `SuperSmashBros-miyoo.zip` onto the SD card's root (let it merge with the folders there).
2. Put your ROM in `Roms/N64/`. It must be **Super Smash Bros. (USA)**, SHA-1
   `e2929e10fccc0aa84e5776227e798abc07cedabf`, 16 MB, as `.z64`, `.v64` or `.n64`, with any file
   name. The game also takes `App/SuperSmashBros/rom.z64` (looked at first), and looks anywhere on the
   card down to two folders deep.
3. Start **Super Smash Bros**. The first start copies the game data out of the ROM
   (`App/SuperSmashBros/data/romdata.bin`, about 14 MB). Later starts skip that step, and the ROM isn't
   read again. To redo it, put an empty file named `rebuild` in `App/SuperSmashBros/data/`.

The same zip works on every Miyoo OS. The game itself is in `App/SuperSmashBros/`, and every other
entry only starts that folder, so they all share one copy and one save folder:

| OS | Where it shows up | From the zip |
|---|---|---|
| Onion, the stock OS, spruce | Apps | `App/SuperSmashBros/` |
| Onion, with its Ports Collection | Ports | `Roms/PORTS/Shortcuts/Super Smash Bros.port`, `Roms/PORTS/Imgs/Super Smash Bros.png` |
| MinUI, MyMinUI | the home screen | `Roms/Super Smash Bros (SSB64)/`, `Emus/miyoomini/SSB64.pak/` |
| Allium | Apps | `Apps/Super Smash Bros.pak/` |

The Ports entry needs Onion's Ports Collection (Apps > Package Manager > Verified). If it doesn't
show after the update, run `~Import ports` at the bottom of the Ports list (it keeps this entry) or
delete `Roms/PORTS/Shortcuts/Shortcuts_cache6.db`. The zip also makes an empty `Roms/PORTS/Games/`,
which that import needs; nothing of the game is copied there.
Allium also lists Onion's `Roms/PORTS` (as Ports Collection), where this entry starts nothing: on
Allium, start the game from Apps.

Builds before 2026-10-12 also put a Games entry on Onion (`Emu/SMASHBROS/`, `Roms/SMASHBROS/`); this
zip no longer has it (the game is in Apps and Ports), so delete those two folders when you update,
or the game is listed twice.
The first Ports entry (also 2026-10-12) sat in `Roms/PORTS/Shortcuts/_N64/`: delete that folder too.

Folders for other OSes just sit unused. Everything the game writes stays in
`App/SuperSmashBros/data/`: `romdata.bin`, `sram.bin` (unlocks, records, options), `state_01.sav` to
`state_09.sav` and `state_quick.sav` (the save states and the auto save), `bench.cfg` (settings, if
you make one), `console.log` from the last session plus the one before it, and `launch.log` and
`games_launch.log`, which record each start.

### Bringing a save from an emulator or a flash cart

Drop your `.sra`, `.srm` or `.sav` into `App/SuperSmashBros/data/` and start the game. It finds the
file, works out its byte order, converts it and installs it as `sram.bin`; the old `sram.bin` is kept
as `sram.bin.bak-<date>`, and your file is renamed `<name>.imported` so it is not applied twice.
`console.log` has one line saying what it found (`save import: ...: OK, ...` or why not). If a save
state or the auto save is older than the import, loading it keeps the imported unlocks and records.

`sram.bin` is not a cartridge image (it holds the game's save in the handheld's byte order), so do
not rename a `.sra` to `sram.bin` yourself; if you already did, the next start converts that too.

Checked: Project64 and mupen64plus `.sra`, RetroArch Mupen64Plus-Next / ParaLLEl `.srm` (the whole
combined file), EverDrive / flash-cart `.sav` (the cartridge's own byte order), and 16-bit swapped
files. The save must be from the USA version: a PAL or Japanese save, a blank one or another game's
is refused and nothing is changed. The save states (`state_*.sav`) are never touched by this.

The zip comes from `miyoo/tools/miyoo_pack.py` (shared by all the ports, from the shared Miyoo kit)
with this port's `miyoo/game.json`. Given `--rom`, it checks the dump and puts it in as
`App/SuperSmashBros/rom.z64`. (The name has no full stop on the Mini: the OSes' entries add their own
extension after it.)

## Controls

Control scheme 1, plus the Mini's extra shoulder buttons:

| Miyoo | N64 |
|---|---|
| D-pad | Stick (full tilt) |
| R1 (hold) | the d-pad becomes a half stick: walk, tilts |
| A / B | A / B |
| X | C (jump; tap = short hop) |
| Y or L2 | Z (shield; with A = grab) |
| L1 | R (A + Z: shield-grab) |
| SELECT or R2 | L (taunt) |
| START | START |
| MENU | the game menu (resume, save/load state, exit) |

In the menu: up and down pick an entry, left and right change the slot, A or START selects, and B backs
out of a confirmation or closes the menu (so does MENU).

## Settings

The settings file is `App/SuperSmashBros/data/bench.cfg`, one `key=value` per
line (every switch is in [SETTINGS.md](SETTINGS.md)). The same as an environment variable in `launch.sh`:
`SSB64_<KEY>`. Miyoo only:

| Key / variable | Default | Meaning |
|---|---|---|
| `threads=1` / `SSB64_THREADS=1` | off | draw on one core only, for an A/B (the picture is the same either way) |
| `SSB64_SPLIT=<px>` | `64` | the smallest triangle (pixels of area) both cores draw |
| `cpuclock=` / `SSB64_CPUCLOCK` | the model's | the CPU clock in MHz while the game runs: 1500 on the Mini, 1800 on the Mini Plus; `0` leaves it as the OS set it. It goes back when the game quits. |
| `SSB64_FILLBORDER=0` | `4` | the N64's black border around the picture (`1`: the stretch smoothed) |
| `SSB64_ROTATE=0` | turned | draw the picture unturned, for an SDL that turns the panel itself (the MinUI launcher sets it) |
| `SSB64_LOADPIC=0` | on | show the boot's first pictures before RESUME GAME's load |

`console.log` shows `worker: on, 2 cores online (...)` once the first big triangle is drawn, and each
PERF line ends in `mt on split <split>/<3D triangles> tris <share of the NEON rows' pixels drawn by both>%
px, jobs N, sleeps N`. `launch.sh` turns the second core on if the OS left it off, and `data/launch.log`
lists the cores online. `clock: 1200 -> 1500 MHz (model 283; ...)` near the top says what the clock did.

## Building

Needs the union Miyoo Mini toolchain (the one Onion and MinUI build with: GCC 8.3, glibc 2.28, and the
device's SDL 1.2 in the sysroot) at `/opt/miyoomini-toolchain` (or `MIYOO_TOOLCHAIN`):

    make -C port PLAT=miyoo -j4
    sh tools/build_miyoo.sh <your ROM>    # SuperSmashBros-miyoo.zip + the release gates

`tools/build_miyoo.sh` runs the release gates: source provenance, no library
variables in save-state memory, the leak scan against the ROM and its decompressed reloc files (the
executable's decomp tables under the 144 KB cap), no host paths or authoring metadata, and only the
files `miyoo/game.json` lists. The ROM is read for the scan, never shipped.

**A card-ready copy for yourself** (your ROM goes inside, so there's nothing else to copy):

    PERSONAL=1 sh tools/build_miyoo.sh <your ROM>

Unzip `SuperSmashBros-miyoo-PERSONAL.zip` onto the card's root. It contains your ROM (as
`App/SuperSmashBros/rom.z64`), so keep it to yourself (git ignores zips).

## Where the code is

- `port/kit/platform/miyoo_panel.c/.h`, `miyoo_menu.c`, `miyoo_font.h`, `miyoo_worker.c/.h`,
  `miyoo_clock.c/.h`: the shared Miyoo kit (the panel and paths, the menu every Miyoo port shares,
  the second core's worker, the clock), copied as they are.
- `MIYOO` blocks in the existing files handle the rest:
  - `gfx_soft.c`: the big triangles' rows split between the two cores (`tri_rows_mt`)
  - `gfx_sdl12.c`: the panel (the 320x240 frame copied out, doubled and turned on the present thread),
    MENU (Escape), SIGTERM, LOADPIC, the 320-wide base
  - `gfx_soft.c`: the stretch of the N64 frame (fill mode 4) allowed at 4:3
  - `input.c`: buttons
  - `romdata.c`: the first-start screens in the menu's font, the ROM search (`rom.z64` next to the
    executable, the data folder, `Roms/N64`, the card)
  - `hh_port.c`: the data folder; `main.c`: the clock, the PERF line's `mt` part; `audio_sdl.c`: video
    before audio
  - `savestate.c`: the kit's Miyoo lines (no OS power-off countdown; SIGTERM
    at the resume prompt quits with the auto save kept)
- `HANDHELD` and `MIYOO` are both defined for the Miyoo build; everything outside the `MIYOO` blocks is
  the common handheld code.

## Things to check on a new OS

- **The picture is upside down:** set `SSB64_ROTATE=0` in that OS's launcher.
- **It doesn't start:** look in `App/SuperSmashBros/data/`. No `games_launch.log` means the OS never ran
  the launcher; a `launch.log` with no fresh `console.log` means the game didn't start.
- **No sound:** `launch.log` shows whether `libpadsp.so` was preloaded (it is whenever the OS's
  audioserver runs).
