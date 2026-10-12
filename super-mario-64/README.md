# Super Mario 64 for the Miyoo Mini

A native port of Super Mario 64, built from the [n64decomp/sm64](https://github.com/n64decomp/sm64) decompilation by way of [sm64-port](https://github.com/sm64-port/sm64-port), with a software renderer that shares the 3D between the Mini's two cores. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `SuperMario64-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **Super Mario 64 (USA)**
- SHA-1 `9bef1128717f958171a4afac3ed78ee2bb4e86ce`
- .z64, .v64 or .n64, renamed to `App/SuperMario64/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start builds the game data from your ROM (`App/SuperMario64/data/assets.bin`); after that the ROM is only needed again for an update.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Move |
| A | Jump |
| B | Punch, kick, dive |
| X | Crouch, ground pound |
| Y | Camera mode |
| L1 / R1 | Turn the camera |
| L2 or SELECT | Look around |
| R2 | Camera back |
| START | Pause |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/SuperMario64/data/sm64_display.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `threads=1` | draw on one core only (default `2`) |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 / 1800; `0` leaves it alone) |

The `SM64_*` environment variables in `launch.sh` override these (`SM64_ROTATE=0` draws the picture unturned); README_MIYOO.md lists them.

## Building the Miyoo zip

Needs Docker: `./dev.sh` runs in the `sm64hh:build` image (`docker/Dockerfile.sm64`, built on `mk64hh:build` from `../mario-kart-64/docker/Dockerfile`, which has the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`). The decomp's data directories are not in this repository: `tools/decomp_data.sh` restores them from sm64-port at a pinned commit (they compile only into the asset region that never ships). Your ROM goes at `port/baserom.us.z64` (the decomp's build extracts textures and sounds from it; none of that ships).

```sh
sh tools/decomp_data.sh                               # the decomp's data, from sm64-port at the pinned commit
./dev.sh 'cd /work/port && make PLAT=miyoo -j4'       # build/us_miyoo/sm64, assets.bin, recipes.bin
./dev.sh 'sh /work/tools/build_miyoo.sh'              # SuperMario64-miyoo.zip
```

Only when the asset layout changes: `make PLAT=miyoo assets-recipes` (slow; numbers only). `tools/build_miyoo.sh` runs the release gates: every compiled source tracked, no bytes in the asset region, every code pointer in the game data on a function, no library variable in the save-state region, the leak scan, no host paths, only the listed files.

**A card-ready copy for yourself**, with your ROM inside as `App/SuperMario64/rom.z64`:

```sh
./dev.sh 'PERSONAL=1 sh /work/tools/build_miyoo.sh'
```

It writes `SuperMario64-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `port/`: sm64-port's tree with the port: `port/src/hh/` (renderer, SDL platform, the Miyoo kit), `port/kit/` (the shared kit)
- `tools/assets/`: the asset recipes (numbers only), provenance and leak scan
- `tools/build_miyoo.sh`, `miyoo/`: the Miyoo zip
- `README_MIYOO.md`: the Miyoo build in full (every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/SuperMario64/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [n64decomp/sm64](https://github.com/n64decomp/sm64), the Super Mario 64 decompilation;
- [sm64-port/sm64-port](https://github.com/sm64-port/sm64-port), the PC port this one is based on (its data is fetched at build time at a pinned commit);
- [fgsfdsfgs/sm64-port](https://github.com/fgsfdsfgs/sm64-port) (`dos` branch), the software renderer this one started from;
- [DrUm78/sm64-port](https://github.com/DrUm78/sm64-port), an earlier handheld port, which the port layer is based in part on (its paths and key handling);
- the port layer of `../mario-kart-64` and [beckerd/psp_mk64_portable](https://github.com/beckerd/psp_mk64_portable)'s asset recipe scheme.

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
