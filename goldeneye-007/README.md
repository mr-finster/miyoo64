# GoldenEye 007 for the Miyoo Mini

A native port of GoldenEye 007, built from the [n64decomp/007](https://github.com/n64decomp/007) decompilation, with a software renderer that shares the 3D between the Mini's two cores. Gameplay fills the screen: where the N64 draws black bands, this port shows more of the level. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `GoldenEye-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **GoldenEye 007 (USA)**
- SHA-1 `abe01e4aeb033b6c0836819f549c791b26cfde83`
- .z64, .v64 or .n64, renamed to `App/GoldenEye/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start builds what the game needs from your ROM, in `App/GoldenEye/data/`.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Walk and turn |
| A | Action, doors, reload |
| B | Fire |
| X | Next weapon |
| Y (hold) | Aim |
| L1 / R1 | Strafe (lean while aiming) |
| L2 / R2 | Look down / up |
| START | Pause |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/GoldenEye/data/debug.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `screen=n64` | gameplay in the N64's view between black bands (a little faster) |
| `minstep=1` | in the missions, up to 60 fps instead of 30 (the motion is less even) |
| `intres=<percent>` | the 3D's size under the full-size HUD (default 100; 85 or lower is faster and softer) |
| `threads=1` | draw on one core only (default `2`) |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 / 1800; `0` leaves it alone) |
| `rotate=0` | the picture unturned, for an SDL that turns the panel itself |

README_MIYOO.md lists the renderer's other keys; `#` starts a comment.

## Building the Miyoo zip

You need your own ROM and the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`; `docker/Dockerfile.miyoo` makes an image with it (`docker build --platform linux/amd64 -t gemm:build -f docker/Dockerfile.miyoo docker`, then `GE_IMAGE=gemm:build ./dev.sh ...`). The decompilation is not in this repository: `tools/fetch_decomp.sh` makes a code-only, sparse checkout of n64decomp/007 at a pinned commit (none of its ROM-derived data).

```sh
tools/fetch_decomp.sh                                         # refs/007 at the pinned commit
tools/port_assets.sh                                          # the decomp headers the code includes
GE_IMAGE=gemm:build ./dev.sh make -C port -f Makefile.handheld PLAT=miyoo -j6
GE_IMAGE=gemm:build ./dev.sh tools/m1_build.sh "<your ROM>"   # once: the map the table scrub needs
GE_IMAGE=gemm:build ./dev.sh tools/build_miyoo.sh "<your ROM>"  # GoldenEye-miyoo.zip
```

`build_miyoo.sh` runs the release gates: testing unlocks off, provenance (only tracked source), the ROM tables scrubbed and the leak scan against your ROM, no host paths or metadata, only the files `miyoo/game.json` lists.

**A card-ready copy for yourself**, with your ROM inside as `App/GoldenEye/rom.z64`:

```sh
GE_IMAGE=gemm:build ./dev.sh 'PERSONAL=1 tools/build_miyoo.sh "<your ROM>"'
```

It writes `GoldenEye-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `port/`: the game code (from the decompilation), the renderer and platform layer (`port/hh/`) and the shared kit (`port/kit/`)
- `tools/`: the decomp fetch, the build helpers, the release gates and the leak scan
- `tools/build_miyoo.sh`, `miyoo/`: the Miyoo zip
- `README_MIYOO.md`: the Miyoo build in full (how it works on the Mini, every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/GoldenEye/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [n64decomp/007](https://github.com/n64decomp/007), the GoldenEye 007 decompilation (fetched at build time at a pinned commit);
- the renderer and platform layer descend from [sm64-port](https://github.com/sm64-port/sm64-port) and [beckerd's PSP port](https://github.com/beckerd/psp_mk64_portable) of the Mario Kart 64 decompilation, by way of `../mario-kart-64`.

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
