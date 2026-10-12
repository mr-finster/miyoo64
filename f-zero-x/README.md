# F-Zero X for the Miyoo Mini

A native port of F-Zero X, built from the [inspectredc/fzerox](https://github.com/inspectredc/fzerox) decompilation. It runs at the game's own 60 frames a second, fills the screen without the N64's black border, and draws the 3D on both of the Mini's cores. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `FZeroX-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **F-Zero X (USA)**
- SHA-1 `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340`
- .z64, .v64 or .n64, renamed to `App/FZeroX/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The game reads your ROM while it runs, as the N64 reads the cartridge, so keep it on the card.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Steer |
| A | Accelerate |
| B | Boost, back |
| X | Air brake |
| Y | Look back |
| L1 / R1 | Lean (double tap: side attack) |
| L2 | C-left |
| R2 or SELECT | Change camera |
| START | Pause |
| START + D-pad | C buttons |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

F-Zero X's switches are environment variables: set them in `App/FZeroX/launch.sh`. The ones a player might use:

| Setting | Effect |
|---|---|
| `FZX_FILL=0` | the N64's picture with its black border |
| `FZX_THREADS=1` | one core only (the same frames) |
| `FZX_CPUCLOCK=<MHz>` | the CPU clock while the game runs (`0` leaves it alone) |
| `FZX_PACE=cadence` | cadence frame pacing (an even 60/30 or a 52.5/50/48/45/40 cadence) instead of showing each frame as soon as it is drawn |
| `FZX_ROTATE=0` | the picture unturned, for an SDL that turns the panel itself |

README_MIYOO.md ("Switches") lists them all.

## Building the Miyoo zip

Needs Docker: `./dev.sh` runs in the `fzxhh:build` image (`docker/Dockerfile`, built on `mk64hh:build` from `../mario-kart-64/docker/Dockerfile`, which has the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`). The port takes its N64 numbers (addresses, sizes) from the decomp's own N64 build of your ROM, never data; the ROM goes at `game/baserom.us.rev0.z64`.

```sh
git submodule update --init --recursive f-zero-x/game/tools     # from the repository root
./dev.sh 'cd game && /usr/bin/python3 -m venv .venv && .venv/bin/pip install -r tools/requirements-python.txt pillow && make toolchain'   # once
./dev.sh 'cd game && make VERSION=us init -j6'                   # the N64 build; must end "fzerox.us.rev0.z64: OK"
./dev.sh 'make -C game -f Makefile.handheld PLAT=miyoo gen'        # once per fresh build folder, without -j
./dev.sh 'make -C game -f Makefile.handheld PLAT=miyoo -j4'        # game/build/miyoo/fzerox
./dev.sh 'sh tools/build_miyoo.sh'                               # FZeroX-miyoo.zip
```

`tools/build_miyoo.sh` runs the release gates: provenance, the N64 RAM image NOBITS, save states holding game memory only, no library variable copied into the executable, a leak scan of the zip against your ROM, no host paths, exactly the expected files.

**A card-ready copy for yourself**, with your ROM inside as `App/FZeroX/rom.z64`:

```sh
./dev.sh 'PERSONAL=1 sh tools/build_miyoo.sh'
```

It writes `FZeroX-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `game/`: the decompilation with the port: `game/src/port/` (renderer, SDL platform, `miyoo/` kit), `game/Makefile.handheld`
- `tools/`: the release gates, the generated-table builders, sweeps and profilers
- `tools/build_miyoo.sh`, `miyoo/`: the Miyoo zip
- `README_MIYOO.md`: the Miyoo build in full (every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/FZeroX/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [inspectredc/fzerox](https://github.com/inspectredc/fzerox), the F-Zero X decompilation (CC0, `game/LICENSE`); its tools are git submodules at the commits it pins;
- the platform layer builds on `../mario-kart-64`, which builds on the [sm64-port](https://github.com/sm64-port/sm64-port) renderer.

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
