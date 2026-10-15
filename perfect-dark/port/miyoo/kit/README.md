# The port kit

The shared platform layer for the N64 ports (MK64, OoT, DKR, F-Zero X, Star Fox 64,
Smash, Paper Mario, GoldenEye, SM64): one copy of the code every port otherwise copies from
MK64 and then fixes on its own. The platform layer came first, then the rasterizer (`gfx_soft.c`); the display-list front end
(`gfx_pc.c`) stays in each port.

Changes to the kit are made once and picked up by every port. Ports
vendor a tagged copy (`git subtree`), so each repo still builds on its own, and a port moves
to a new tag on purpose, after its own checks.

## What is in it

| Path | What |
|---|---|
| `platform/handheld_menu.c/.h` | the handheld OS in-game menu (RESUME, SAVE, LOAD, 3D FILTER, 3D RES, VOLUME, BRIGHTNESS, EXIT GAME) and the RESUME GAME / NEW GAME launch prompt. |
| `platform/savestate.c/.h` | Save states, the power-off save and resume, EXIT GAME's auto save. Two memory layouts: the MK64 "hole" and DKR's include list. |
| `ports/<game>/hh_game.h` | The per-game settings each port keeps beside its sources: name, environment prefix, save-state magic and layout. |
| `tests/run.sh` | Save-state tests (both layouts) and a device + desktop compile of every file against every port's settings, in a port's build image. |

`DESIGN.md` has how the ports differ today, what the kit does about each difference, the
order the rest arrives in, and the open questions. `CHANGELOG.md` lists the tags.

## Adopting it (K1: menu and save states)

1. Vendor the kit: `git subtree add --prefix=port/kit <port kit checkout> k1 --squash`.
2. Build `port/kit/platform/*.c` in place of the port's own `handheld_menu.c` and `savestate.c`,
   and delete those four files.
3. Copy `ports/<game>/hh_game.h` into a directory on the port's include path (next to its
   other port headers).
4. Check: the menu dump (`<GAME>_MENU_TEST=<dir>`), a save in one process and a load in a
   fresh one (MK64's and SM64's `qstate`), then the device. Record the kit tag in the port's notes.

## Testing the kit

    tests/run.sh                 # in sm64hh:build; IMAGE=<port>hh:build to pick another

## Rules

Allowlist `.gitignore`; no ROM or ROM-derived data, ever.

## Credits

The menu follows the handheld OS convention of the official emulators (the copied `menu.c`,
DrUm78's `gfx_sdl_menu.c` from [DrUm78/sm64-port](https://github.com/DrUm78/sm64-port), gnuboy's launch prompt). The save states and the platform
code around them come from the MK64 port, which is built on beckerd's PSP port of the
Mario Kart 64 decompilation; the renderer lineage is sm64-port's.
