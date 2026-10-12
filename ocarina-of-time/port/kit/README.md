# handheld-kit

The shared platform layer for the N64 ports in this repository (Mario Kart 64, Ocarina of Time, Diddy Kong
Racing, F-Zero X, Star Fox 64, Smash, Paper Mario, GoldenEye, Super Mario 64): one copy of the platform code
instead of one per port. Each port keeps a copy of it beside its sources, so each folder builds on its own.

## What is in it

| Path | What |
|---|---|
| `platform/savestate.c/.h` | Save states and the auto save (EXIT GAME, and the OS shutting the game down). Two memory layouts: the MK64 "hole" and DKR's include list. |
| `platform/handheld_menu.h` | The menu API the game calls (notices, the resume prompt); the Miyoo build implements it in `miyoo_menu.c`. |
| `platform/miyoo_menu.c`, `miyoo_font.h` | The Miyoo menu (RESUME, SAVE STATE, LOAD STATE, EXIT GAME), the resume prompt and the notices, in a built-in 8x8 font. |
| `platform/miyoo_worker.c/.h` | The second core's worker thread. |
| `platform/miyoo_clock.c/.h` | The CPU clock: raised while the game runs, put back at exit. |
| `platform/hh_priority.c`, `hh_declick.h` | The game's process priority; the short fades around the menu and loads. |
| `ports/<game>/hh_game.h` | The per-game settings each port keeps beside its sources: name, environment prefix, save-state magic and layout. |
| `tests/run.sh` | Save-state tests (both layouts) and an ARM + desktop compile of every file against every port's settings, in a port's build image. |

## Testing the kit

    tests/run.sh                 # in sm64hh:build; IMAGE=<port>hh:build to pick another

## Rules

Allowlist `.gitignore`; no ROM or ROM-derived data, ever.

## Credits

The save states and the platform code around them come from the Mario Kart 64 port, which is built on
beckerd's PSP port of the Mario Kart 64 decompilation; the renderer lineage is sm64-port's.
