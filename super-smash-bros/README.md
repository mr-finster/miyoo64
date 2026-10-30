# Super Smash Bros. for the Miyoo Mini

A native port of Super Smash Bros., built from the Smash 64 decompilation ([VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re), by way of its port fork [JRickey/ssb-decomp-re](https://github.com/JRickey/ssb-decomp-re)), with a software renderer that shares the 3D between the Mini's two cores. Single player only: VS Mode is one player against CPUs. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `SuperSmashBros-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **Super Smash Bros. (USA)**
- SHA-1 `e2929e10fccc0aa84e5776227e798abc07cedabf`
- .z64, .v64 or .n64, renamed to `App/SuperSmashBros/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start extracts the game data from your ROM once (`App/SuperSmashBros/data/romdata.bin`, about 14 MB); later starts don't read the ROM.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Move |
| R1 (hold) | Walk, tilts |
| A / B | A / B |
| X | Jump |
| Y | Shield (with A: grab) |
| L1 | N64 R |
| SELECT | Taunt |
| L2 / R2 | N64 C-left / C-right: the fighter's colour on the character select (jump in a fight) |
| START | Start |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/SuperSmashBros/data/bench.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `threads=1` | draw on one core only |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 / 1800; `0` leaves it alone) |
| `SSB64_FILLBORDER=0` | (in `launch.sh`) the N64's black border around the picture |
| `SSB64_ROTATE=0` | (in `launch.sh`) the picture unturned, for an SDL that turns the panel itself |

Every switch is listed in [SETTINGS.md](SETTINGS.md); each also works as an `SSB64_<NAME>` environment variable in `launch.sh`.

## Building the Miyoo zip

Needs Docker: `./dev.sh` runs in the `ssb64hh:build` image (`docker/Dockerfile`, built on `mk64hh:build` from `../mario-kart-64/docker/Dockerfile`, which has the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`). The build needs no ROM; the release gates read it for the leak scan (and decompress its reloc files with `tools/reloc_dump.c`).

```sh
./dev.sh 'make -C port PLAT=miyoo -j4'
./dev.sh 'sh tools/build_miyoo.sh <your ROM>'    # SuperSmashBros-miyoo.zip
```

`tools/build_miyoo.sh` runs the release gates: source provenance, no library variables in save-state memory, the leak scan against the ROM and its decompressed reloc files, no host paths or authoring metadata, only the files `miyoo/game.json` lists.

**A card-ready copy for yourself**, with your ROM inside as `App/SuperSmashBros/rom.z64`:

```sh
./dev.sh 'PERSONAL=1 sh tools/build_miyoo.sh <your ROM>'
```

It writes `SuperSmashBros-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `port/decomp/`: the decompilation's code and headers (its game data is not imported)
- `port/hh/`: the port: renderer, platform, ROM data, BattleShip-derived code
- `port/kit/`: the shared platform kit
- `tools/build_miyoo.sh`, `miyoo/`: the Miyoo zip
- `README_MIYOO.md`: the Miyoo build in full (every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/SuperSmashBros/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- the Smash 64 decompilation, [VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re), and its port fork [JRickey/ssb-decomp-re](https://github.com/JRickey/ssb-decomp-re);
- code from [JRickey/BattleShip](https://github.com/JRickey/BattleShip) (MIT) and the Starship audio mixer ([HarbourMasters/Starship](https://github.com/HarbourMasters/Starship), MIT): the notices are in [port/NOTICE.md](port/NOTICE.md).

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
