# Diddy Kong Racing for the Miyoo Mini

A native port of Diddy Kong Racing, built from the Diddy Kong Racing decompilation (DavidSM64 and contributors) with a software renderer that shares the 3D and the sky between the Mini's two cores. Races hold the game's own 30 fps. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `DiddyKongRacing-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **Diddy Kong Racing (USA), Rev 1 (version 1.1)**
- SHA-1 `6d96743d46f8c0cd0edb0ec5600b003c89b93755`
- .z64, .v64 or .n64, renamed to `App/DiddyKongRacing/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start copies the game data out of your ROM (`App/DiddyKongRacing/data/dkr_assets.bin`, about 11 MB); after that the ROM isn't read again.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Steer (plane: pitch) |
| A | Accelerate |
| B | Brake (B + Down: reverse) |
| R1 or X | Hop, power slide |
| L1 or Y | Fire item |
| L2 | N64 L |
| R2 or SELECT | Camera zoom |
| START | Pause |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/DiddyKongRacing/data/dkr_display.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `fps=60` | show every frame as soon as it is drawn (default `30`, the game's own hold) |
| `threads=1` | draw on one core only (default `2`) |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 / 1800; `0` leaves it alone) |
| `rotate180=0` | the picture unturned, for an SDL that turns the panel itself |

An empty file named `benchmark` in `App/DiddyKongRacing/data/` runs the built-in benchmark once at the next start. The `DKR_*` environment variables (README_MIYOO.md) override the file.

## Building the Miyoo zip

Needs the Miyoo Mini toolchain (GCC 8.3, glibc 2.28, the device's SDL 1.2) at `/opt/miyoomini-toolchain` or `$MIYOO_TOOLCHAIN`: the `mk64hh:build` image from `../mario-kart-64/docker/Dockerfile` has it, and `./dev.sh` runs in the `oothh:build` image built on top of it (`../ocarina-of-time/docker/Dockerfile.oot`). The build itself needs no ROM; the release gates read it for the leak scan.

```sh
./dev.sh 'make PLAT=miyoo -j6'                        # build/miyoo/dkr
./dev.sh 'sh tools/build_miyoo.sh <your ROM>'         # DiddyKongRacing-miyoo.zip
```

Without a ROM, `NO_LEAK_SCAN=1 sh tools/build_miyoo.sh` runs every other gate and writes `DiddyKongRacing-miyoo-UNSCANNED.zip`, which is not a release.

**A card-ready copy for yourself**, with your ROM inside as `App/DiddyKongRacing/rom.z64`:

```sh
./dev.sh 'PERSONAL=1 sh tools/build_miyoo.sh <your ROM>'
```

It writes `DiddyKongRacing-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `decomp/`: the decompilation's code (CC0, `decomp/LICENSE.md`)
- `port/`: the platform layer, renderer and audio; `port/sdl/miyoo_*.c` for the Mini
- `Makefile`, `tools/build_miyoo.sh`, `miyoo/`: the build and the Miyoo zip
- `tools/`: release gates, regression sweeps and small tests
- `README_MIYOO.md`: the Miyoo build in full (how it works on the Mini, every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/DiddyKongRacing/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- the [Diddy Kong Racing decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing) by DavidSM64 and contributors (CC0, `decomp/LICENSE.md`);
- Golden Balloon's ([akratch/goldenballoon](https://github.com/akratch/goldenballoon)) asset byte-order layer, adapted in `port/asset_swap.c/.h` (MIT; the notice is at the top of both files);
- DavidSM64's F3DDKR microcode disassembly, the reference for the renderer;
- the Fast3D renderer of the Super Mario 64 PC port lineage, reworked for software rendering, and conventions from DrUm78's Super Mario 64 port ([DrUm78/sm64-port](https://github.com/DrUm78/sm64-port)), which the platform layer is based in part on.

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
