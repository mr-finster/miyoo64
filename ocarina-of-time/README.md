# The Legend of Zelda: Ocarina of Time for the Miyoo Mini

A native port of Ocarina of Time, built from the [zeldaret/oot](https://github.com/zeldaret/oot) decompilation by way of [z2442's oot-PSP](https://github.com/z2442/oot-PSP) port, with a software renderer that draws the big 3D triangles on both of the Mini's cores. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `OcarinaOfTime-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **The Legend of Zelda: Ocarina of Time (USA), v1.0**
- SHA-1 `ad69c91157f6705e8ab06c79fe08aad47bb57ba7`
- .z64, .v64 or .n64 (the original 1.0 release, not Rev 1 or Rev 2), renamed to `App/OcarinaOfTime/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start builds the game data from your ROM once (in `App/OcarinaOfTime/data/`). To redo it, put an empty file named `rebuild` in that folder.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Control stick |
| A / B | A / B |
| L1 | Z-target |
| R1 | Shield |
| X / Y | C-left / C-down |
| L2 | C-up |
| R2 | C-right |
| START | Pause |
| START + L1 | N64 L (minimap) |
| MENU | Game menu: save, load, exit |

While you play the ocarina, the d-pad is the four C notes and A is A; B puts it away.

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/OcarinaOfTime/data/debug.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `threads=1` | draw on one core only |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 / 1800; `0` leaves it alone) |
| `rotate=0` | the picture unturned, for an SDL that turns the panel itself |
| `audiofill=0` | the old audio buffering |

Every key is listed in [SETTINGS.md](SETTINGS.md); the `OOT_*` environment variables in `launch.sh` do the same (README_MIYOO.md).

## Building the Miyoo zip

Needs Docker (`./dev.sh` runs in the `oothh:build` image: `docker/Dockerfile.oot`, built on `mk64hh:build` from `../mario-kart-64/docker/Dockerfile`, which has the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`) and your ROM at `port/baseroms/ntsc-1.0/baserom.z64`:

```sh
./dev.sh 'cd port && make setup VERSION=ntsc-1.0'
./dev.sh 'cd port && make -k CC_CHECK=true VERSION=ntsc-1.0 REGION=US'   # the generated headers
./dev.sh 'cd port && make -f Makefile.handheld PLAT=miyoo -j4'
./dev.sh 'tools/build_miyoo.sh'                                          # OcarinaOfTime-miyoo.zip
```

The Miyoo build derives its own `oot_recipes.bin` (how the game data comes from the ROM, as numbers only). `tools/build_miyoo.sh` runs the release gates: the recipes belong to this link, source provenance, the ROM-data region empty in the binary, no library variables in save-state memory, the leak scans, no host paths, only the files `miyoo/game.json` lists.

**A card-ready copy for yourself**, with your ROM inside as `App/OcarinaOfTime/rom.z64`:

```sh
./dev.sh 'PERSONAL=1 tools/build_miyoo.sh'
```

It writes `OcarinaOfTime-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `port/`: the game: the decompilation with the PSP port's changes and this port's renderer and platform code (`port/src/port/hh/`)
- `port/kit/`: the shared Miyoo platform kit (menu, save states, the second core, the CPU clock)
- `tools/build_miyoo.sh`, `miyoo/`: the Miyoo zip
- `README_MIYOO.md`: the Miyoo build in full (every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/OcarinaOfTime/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [zeldaret/oot](https://github.com/zeldaret/oot), the Ocarina of Time decompilation;
- [z2442/oot-PSP](https://github.com/z2442/oot-PSP), the PSP port this one starts from;
- the decomp's bundled tools keep their licences (`port/tools/asm-differ/LICENSE`, `port/tools/fado/LICENSE`, `port/tools/com-plugin/LICENSE`).

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
