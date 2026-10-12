# kit

The shared platform layer of the N64 ports in this repository (MK64, OoT, DKR, F-Zero X, Star Fox 64,
Smash, Paper Mario, GoldenEye, SM64): one copy of the code every port otherwise copies from
MK64 and then fixes on its own. The display-list front end (`gfx_pc.c`) and the rasterizer stay in
each port.

Changes to the kit are made once and picked up by every port. Each port vendors a tagged copy, so
each folder still builds on its own, and a port moves to a new tag on purpose, after its own checks.

## What is in it

| Path | What |
|---|---|
| `platform/miyoo_panel.c/.h`, `miyoo_menu.c`, `miyoo_font.h` | The Miyoo Mini's panel, signals and paths; the in-game menu (RESUME, SAVE STATE, LOAD STATE, EXIT GAME) and the RESUME GAME / NEW GAME launch prompt, in a built-in 8x8 font. |
| `platform/miyoo_worker.c/.h`, `miyoo_clock.c/.h` | The Mini's second core (half of every big triangle) and its CPU clock. |
| `platform/savestate.c/.h` | Save states and EXIT GAME's auto save. Two memory layouts: the MK64 "hole" and DKR's include list. |
| `platform/handheld_menu.h` | The menu and power hooks the kit's code calls (`miyoo_menu.c` and the port's platform file provide them). |
| `platform/hh_priority.c` | The game's scheduling priority. |
| `ports/<game>/hh_game.h` | The per-game settings each port keeps beside its sources: name, environment prefix, save-state magic and layout. |

## Rules

Allowlist `.gitignore`; no ROM or ROM-derived data, ever.

## Credits

The save states and the platform code around them come from the MK64 port, which is built on
beckerd's PSP port of the Mario Kart 64 decompilation; the renderer lineage is sm64-port's.
