# Star Fox 64 for the Miyoo Mini

A native port of Star Fox 64, built from the [sonicdcer/sf64](https://github.com/sonicdcer/sf64) decompilation, with a software renderer that draws the 3D on both of the Mini's cores. It fills the screen without the N64's black border. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**No game data is included**, here or in the zip. You need your own ROM.

Download `StarFox64-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **Star Fox 64 (USA), version 1.1**
- SHA-1 `09f0d105f476b00efa5303a3ebc42e60a7753b7a`
- .z64, .v64 or .n64, renamed to `App/StarFox64/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the card down to two folders deep)
- The first start builds the game data from your ROM (`App/StarFox64/data/sf64.rom`).

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Steer |
| A | Fire (hold: lock-on) |
| B | Smart bomb |
| L1 / R1 | Bank (double tap: roll) |
| X | Boost |
| Y | Brake |
| L2 | Camera |
| R2 | N64 L (menus) |
| SELECT | Answer ROB |
| START | Pause |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out; the next start offers RESUME GAME or NEW GAME.

## Settings

Optional: a text file `App/StarFox64/sf64_display.cfg`, one `key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `fill=0` | keep the N64's 8-pixel black border |
| `threads=1` | draw on one core only (default `2`) |
| `cpuclock=<MHz>` | the CPU clock while the game runs (default 1500 / 1800; `0` leaves it alone) |
| `bomb=0` | the N64's full smart-bomb blast (slower) |
| `reticle=0` | smoothed crosshairs, as on the N64 |

An empty file named `bench` in `App/StarFox64/` runs the replay benchmark instead of the game. The `SF64_*` environment variables are in README_MIYOO.md.

## Building the Miyoo zip

Needs Docker: `./dev.sh` runs in the `sf64hh:build` image (`docker/Dockerfile.sf64`, built on `mk64hh:build` from `../mario-kart-64/docker/Dockerfile`, which has the Miyoo Mini toolchain at `/opt/miyoomini-toolchain`), and your ROM at `port/baserom.us.rev1.z64`:

```sh
tools/fetch_tools.sh                                            # the decomp's host tools, at the pinned commits
./dev.sh 'make -C port toolchain && make -C port init'          # the decomp's own N64 build from your ROM
./dev.sh 'make -C port -f Makefile.handheld PLAT=miyoo all -j4'   # port/build/miyoo: sf64, recipes.bin, embedded.bin
./dev.sh 'tools/build_miyoo.sh'                                 # StarFox64-miyoo.zip
```

`tools/build_miyoo.sh` runs the release gates: every compiled source tracked in git, no bytes in the game-data regions, no library variable in the save-state region, no window of any shipped file matching the ROM, no host paths or authoring metadata, no co-author or generated-with lines in the commits, and exactly the listed files.

**A card-ready copy for yourself**, with your ROM inside as `App/StarFox64/rom.z64`:

```sh
./dev.sh 'tools/build_miyoo.sh <your ROM>'
```

It writes `StarFox64-miyoo-PERSONAL.zip`. It contains your ROM, so keep it to yourself.

## Layout

- `port/`: the decompilation with the port: `port/src/port/` (renderer, SDL platform, the Miyoo kit), `port/Makefile.handheld`
- `tools/`: the release gates, the leak scan, the tool fetch
- `tools/build_miyoo.sh`, `miyoo/`: the Miyoo zip
- `README_MIYOO.md`: the Miyoo build in full (every switch, how the second core works, what to check on a new OS)

## Reporting a bug

Open an issue in this repository and attach `App/StarFox64/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [sonicdcer/sf64](https://github.com/sonicdcer/sf64), the Star Fox 64 decompilation, and its contributors (CC0, `port/LICENSE.md`);
- the software renderer and platform code of `../mario-kart-64`.

Not affiliated with or endorsed by Nintendo or any other rights holder. You need your own copy of the game.
