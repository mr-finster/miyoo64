# Perfect Dark for the Miyoo Mini

A native port of Perfect Dark, built from [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark), the PC port of the [n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark) decompilation, with the GoldenEye 007 port's software renderer in place of the PC port's OpenGL one, sharing the 3D between the Mini's two cores. The full solo campaign, checked mission by mission against a real N64 picture. It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip), full screen on the Mini v4 and Flip.

**No game data is included**, here or in the zip. You need your own ROM.

Download `PerfectDark-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the [top-level README](../README.md#install) says.

## Your ROM

- **Perfect Dark (USA), Rev 1 (version 1.1)**
- SHA-1 `af8788ac4d1a57260eae9c53ffe851fcf2a3319b`
- Any byte order (.z64, .v64 or .n64), under any file name, anywhere on the card down to two folders deep (the root, `Roms/`, `Roms/N64/`, ...), or as `App/PerfectDark/rom.z64`.
- On the first start the game finds it, checks its SHA-1 and saves a .z64 copy as `App/PerfectDark/data/perfectdark.z64` (a `rom.z64` in the game's folder is used where it is, checked once). The screen shows the progress, and says why a dump is refused: the PAL or Japanese version, the USA 1.0 (Rev 0), damaged or modified, or no ROM found.
- The game reads the ROM while it runs, so keep that copy (or the `rom.z64`) on the card. To look for your ROM again, put a file named `rebuild` in `App/PerfectDark/data/`.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Walk and turn |
| A | Action, reload. Hold: switch fire mode (or A + B) |
| B | Fire |
| X | Next weapon (hold: wheel) |
| Y (hold) | Aim: the D-pad moves the crosshair |
| L1 / R1 | Strafe |
| L2 / R2 | Look down / up (aiming: crouch / stand) |
| SELECT + D-pad | N64 D-pad |
| START | Pause |
| MENU | Game menu: save, load, exit |

In the menu, up and down pick an entry, left and right change the save slot, A or START selects, and B or MENU closes it. The power button saves the auto save on the way out (also with the menu open); the next start offers RESUME GAME or NEW GAME. A POWER press to sleep, or 50 s with no button, writes the auto save first, so RESUME GAME brings you back even if the Mini then powers itself off while asleep (`PD_SLEEPSAVE=0` in launch.sh turns it off; [Sleep and wake](../docs/SLEEP-AND-WAKE.md)). Your game save (`eeprom.bin`) is written safely: a power-off during a save can't empty it.

## Settings

`App/PerfectDark/data/pd.ini`, written on the first start, one `Key=value` per line. The ones a player might use:

| Setting | Effect |
|---|---|
| `Game.UnlockAll=1` | every mission open (default `0`: missions unlock as you play, as on the N64) |
| `Audio.Gain=<percent>` | the output level (default 80; 100 is the game's full mix) |
| `Audio.BassCut=<Hz>` | cuts the low bass the Mini's speaker buzzes on (default 100; `0` turns it off) |

An existing `pd.ini` keeps the values it already has.

Switches for `App/PerfectDark/launch.sh`, as `export <switch>` lines before the last line:

| Switch | Effect |
|---|---|
| `PD_FLIPFILL=0` | Mini v4 / Mini Flip: the picture at 640x480 with a border, not full screen |
| `PD_ROTATE=0` | the picture unturned, for an SDL that turns the panel itself |

## Known issues

- The busiest rooms and the menus can run slower.

## Building the Miyoo zip

You need the Miyoo Mini toolchain at `/opt/miyoomini-toolchain` (the Mario Kart 64 image, `mk64hh:build`, has it), git and Python 3. The game code is in this folder; the per-ROM asset metadata its ID headers are made from (asset names and IDs, the PC port's `src/assets/<romid>/*.json`) is not: `tools/fetch_assets.sh` makes a sparse checkout of fgsfdsfgs/perfect_dark at a pinned commit and copies only those files into `src/assets/ntsc-final/` (gitignored). No ROM is needed to build.

```sh
tools/fetch_assets.sh                    # src/assets/ntsc-final from the pinned commit
tools/gen_headers.sh                     # src/generated/ntsc-final: the ID headers the code includes
make -j4                                 # build/miyoo/pd.arm
mkdir -p build/zip && /opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-strip -o build/zip/pd build/miyoo/pd.arm
python3 miyoo/tools/miyoo_pack.py miyoo/game.json --build build/zip -o PerfectDark-miyoo.zip --check
```

In Docker: `docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work mk64hh:build make -j4`.

**A card-ready copy for yourself**, with your ROM inside as `App/PerfectDark/rom.z64`: add `--rom "<your ROM>"` to the last line. That zip contains your ROM, so keep it to yourself.

`tools/qemu_run.sh "<your ROM>" <run dir>` runs the build under qemu-arm (no display, no sound) and dumps frames, for checks.

## Layout

- `src/`, `include/`: the game code, from the decompilation by way of the PC port, with the Miyoo changes
- `port/src/`, `port/include/`: the PC port's platform layer (ROM loading, config, file system)
- `port/miyoo/`: the Miyoo layer: video, input and sound for the Mini's SDL 1.2, the software renderer (`gfx/`) and the shared kit (`kit/`: game menu, save states, second core, CPU clock)
- `miyoo/`: the zip's `game.json`, icon, launch templates and packer
- `tools/`: the asset fetch and header generator, the decompilation's own tools

## Reporting a bug

Open an issue in this repository and attach `App/PerfectDark/data/console.log` from the run that went wrong (the logs of earlier runs are kept next to it).

## Credits

Built on:

- [n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark), the Perfect Dark decompilation (MIT, `LICENSE`), and Ryan Dwyer for it and `pd-extract`;
- [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark), the PC port this one is built from (its platform layer, 60 fps and bug fixes), and the people its own credits name: doomhack's [PD port](https://github.com/doomhack/perfect_dark), the [sm64-port](https://github.com/sm64-port/sm64-port) authors (the audio mixer), the [libultraship](https://github.com/Kenix3/libultraship) Fast3D authors (Ship of Harkinian team, Emill, MaikelChan), the Mouse Injector and 1964GEPD authors, Raf for the 64-bit port, and everyone who contributed to it;
- lieff's [minimp3](https://github.com/lieff/minimp3) (CC0, `port/include/external/minimp3.h`);
- the renderer and platform kit come from `../goldeneye-007`, and descend from [sm64-port](https://github.com/sm64-port/sm64-port) and [beckerd's PSP port](https://github.com/beckerd/psp_mk64_portable) of the Mario Kart 64 decompilation, by way of `../mario-kart-64`.

Not affiliated with or endorsed by Nintendo, Rare or any other rights holder. You need your own copy of the game.
