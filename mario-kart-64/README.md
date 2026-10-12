# Mario Kart 64 for the Miyoo Mini

A native port of Mario Kart 64, built from the [n64decomp/mk64](https://github.com/n64decomp/mk64) decompilation by way of beckerd's portable PSP tree. The game's C code runs on the Mini's ARM CPU, drawn by a software renderer that splits the 3D between both cores, at the N64's 30 fps. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `MarioKart64-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **Mario Kart 64 (USA)**
- SHA-1 `579c48e211ae952530ffc8738709f078d5dd215e`
- .z64, .v64 or .n64, renamed to `App/MarioKart64/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start builds the game data from your ROM (`App/MarioKart64/assets.bin`, about 13 MB); later starts skip that step.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Steer |
| A | Accelerate |
| B | Brake, reverse |
| R1 or X | Hop, power slide |
| L1 or Y | Use item |
| L2 | N64 L |
| R2 or SELECT | Change the HUD |
| START | Pause |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/MarioKart64/mk64_display.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `threads=1` | draw on one core only (default `2`) |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 on the Mini, 1800 on the Mini Plus; `0` leaves it alone) |
| `rotate180=0` | the picture unturned, for an SDL that turns the panel itself |

`MK64_*` environment variables in `launch.sh` override these; README_MIYOO.md lists them all, with the renderer keys (`unsquish3d=`, `hud=`, `neon=` ...).

## Building the Miyoo zip

Everything builds in the Docker image from `docker/Dockerfile` (the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`); `./dev.sh` runs a command in it with this folder at `/work`. The build needs your ROM as `base-psp/baserom.us.z64` (read, never shipped).

```sh
git submodule update --init mario-kart-64/base-psp/tools/torch     # from the repository root
docker build --platform linux/amd64 -t mk64hh:build docker/        # once
./dev.sh 'cd /work/base-psp && make -C tools && python3 extract_assets.py us && make assets'   # once
./dev.sh 'cd /work/base-psp && make -f Makefile.handheld PLAT=miyoo -j8'
./dev.sh 'sh /work/build_miyoo.sh'                                  # MarioKart64-miyoo.zip
```

`build_miyoo.sh` runs the release gates (provenance, no game data in the executable, the leak scan against your ROM, no host paths, only the listed files) and stops if one fails. If the asset layout changed, the build asks for `make -f Makefile.handheld PLAT=miyoo assets-recipes`, which writes `base-psp/tools/handheld/recipes_miyoo.json` (numbers only).

**A card-ready copy for yourself**, with your ROM inside as `App/MarioKart64/rom.z64`:

```sh
./dev.sh 'cd /work/base-psp && make -f Makefile.handheld PLAT=miyoo -j8 && PERSONAL=1 sh /work/build_miyoo.sh'
```

It writes `MarioKart64-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `base-psp/`: the game: the decomp's code with beckerd's portable layer; the platform code is in `base-psp/src/port/sdl/` (`miyoo_*.c` for the Mini)
- `build_miyoo.sh`, `miyoo/`: the Miyoo zip: the packer, its launch templates, the icon and `game.json`
- `tools/`: the release gates' leak scan and provenance check
- `README_MIYOO.md`: the Miyoo build in full (how it works on the Mini, every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/MarioKart64/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [n64decomp/mk64](https://github.com/n64decomp/mk64), the Mario Kart 64 decompilation;
- [beckerd/psp_mk64_portable](https://github.com/beckerd/psp_mk64_portable), the portable tree and the on-device asset scheme (its terms: `base-psp/LICENSE`);
- the handheld platform layer of [DrUm78/sm64-port](https://github.com/DrUm78/sm64-port) and the [sm64-port](https://github.com/sm64-port/sm64-port) renderer lineage.

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
