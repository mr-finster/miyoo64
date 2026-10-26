<h1 align="center">Miyoo64</h1>

<p align="center"><b>Twelve N64 games running natively on the Miyoo Mini family, built from the decompilation projects.</b></p>

<p align="center">
  <a href="https://github.com/mr-finster/miyoo64/releases/latest"><img src="https://img.shields.io/github/v/release/mr-finster/miyoo64" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/licence-MIT-blue" alt="Licence: MIT"></a>
  <img src="https://img.shields.io/badge/device-Miyoo%20Mini%20%7C%20Mini%20Plus%20%7C%20Mini%20Flip-lightgrey" alt="Device: Miyoo Mini, Mini Plus, Mini Flip">
</p>

<p align="center">
  <a href="https://github.com/mr-finster/miyoo64/releases/latest">Download</a> ·
  <a href="#status">Status</a> ·
  <a href="#controls">Controls</a> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="https://github.com/mr-finster/miyoo64/issues">Report a bug</a>
</p>

<p align="center"><img src="https://miyoo-n64.pages.dev/img/montage.jpg" alt="Screenshots of the games running on a Miyoo Mini" width="800"></p>

> [!IMPORTANT]
> No ROMs or game data are included, here or in the downloads. Bring your own ROM: each game reads a copy of a game you own and builds what it needs on the Miyoo itself.

## About

Native ports of twelve N64 games to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip). No emulation: each game's C code, rebuilt by its decompilation community, runs on the Mini's ARM CPU and is drawn by a software renderer that uses both of its cores. Every game has save states (9 slots and an auto save), the same in-game menu (MENU: resume, save, load, exit), and picks up where you left off after a power-off.

Downloads: the [latest release](https://github.com/mr-finster/miyoo64/releases/latest), or https://miyoo-n64.pages.dev. What changed in each release: [Changelog](CHANGELOG.md).

## Status

| Game | State | Highlights |
|---|---|---|
| Perfect Dark | ✅ Playable | The full solo campaign, checked mission by mission against the N64; fills the Mini v4 and Flip screens |
| Mario Kart 64 | ✅ Playable | Course fog, mirror mode and chrome reflections drawn as on the N64 |
| Diddy Kong Racing | ✅ Playable | Steady 30 fps; ground lighting, fog bands and HUD icons fixed |
| Ocarina of Time | ✅ Playable | Shop backgrounds, torch flames and Lake Hylia's water fixed (USA 1.0 ROM) |
| GoldenEye 007 | ✅ Playable | Fills the screen, smooth real-time motion, 36–50 fps on Dam; the busiest views (Jungle) are heavier |
| F-Zero X | ✅ Playable | Fills the screen; full speed with fog, sparks and the energy aura |
| Star Fox 64 | ✅ Playable | Fills the screen; motion-blur warps, fog and explosions as on the N64 |
| Paper Mario | ✅ Playable | Fills the screen; battle swirls, title cards and Tidal Wave drawn |
| Super Smash Bros. | ✅ Playable | Hit flashes, shields and costume colours as on the N64 |
| Super Mario 64 | ✅ Playable | Metal Mario, coloured fog and the castle window fixed, both cores drawing |
| Majora's Mask | 🧪 Beta (1.0.6) | New: the whole game, compared scene by scene against the N64; known issues in its [README](majoras-mask/README.md#known-issues) |
| Harvest Moon 64 | ✅ Playable (1.0.2) | New: the whole game, the diary saves and the farm's days as on the N64; known issues in its [README](harvest-moon-64/README.md#known-issues) |

Every game has 9 save-state slots plus an auto save, and picks up where you left off after a power-off, also when the Mini sleeps and then powers itself off ([Sleep and wake](docs/SLEEP-AND-WAKE.md)). Super Smash Bros. can take a save from an emulator or a flash cart ([how](super-smash-bros/README_MIYOO.md#bringing-a-save-from-an-emulator-or-a-flash-cart)).

On the Mini v4 and Mini Flip (752x560 screens) every game fills the whole screen (`<GAME>_FLIPFILL=0` in the game's launch.sh brings back the 640x480 picture with a border). Not tested on a real Flip yet.

| Game | Folder | Built from |
|---|---|---|
| Perfect Dark | [perfect-dark](perfect-dark/) | [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark), from [n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark) |
| Mario Kart 64 | [mario-kart-64](mario-kart-64/) | [n64decomp/mk64](https://github.com/n64decomp/mk64) |
| Diddy Kong Racing | [diddy-kong-racing](diddy-kong-racing/) | [DavidSM64/Diddy-Kong-Racing](https://github.com/DavidSM64/Diddy-Kong-Racing) |
| The Legend of Zelda: Ocarina of Time | [ocarina-of-time](ocarina-of-time/) | [zeldaret/oot](https://github.com/zeldaret/oot) |
| GoldenEye 007 | [goldeneye-007](goldeneye-007/) | [n64decomp/007](https://github.com/n64decomp/007) |
| F-Zero X | [f-zero-x](f-zero-x/) | [inspectredc/fzerox](https://github.com/inspectredc/fzerox) |
| Star Fox 64 | [star-fox-64](star-fox-64/) | [sonicdcer/sf64](https://github.com/sonicdcer/sf64) |
| Paper Mario | [paper-mario](paper-mario/) | [pmret/papermario](https://github.com/pmret/papermario) |
| Super Smash Bros. | [super-smash-bros](super-smash-bros/) | [VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re) |
| Super Mario 64 | [super-mario-64](super-mario-64/) | [n64decomp/sm64](https://github.com/n64decomp/sm64) |
| The Legend of Zelda: Majora's Mask | [majoras-mask](majoras-mask/) | [zeldaret/mm](https://github.com/zeldaret/mm) |
| Harvest Moon 64 | [harvest-moon-64](harvest-moon-64/) | [harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp) |

Each folder is one game and builds on its own.

## Install

1. Download the game's zip from the [latest release](https://github.com/mr-finster/miyoo64/releases/latest) or https://miyoo-n64.pages.dev.
2. Put your Miyoo's SD card in your computer.
3. Unzip the download and copy the folders your OS needs to the root of the card, merging them with the folders already there (Onion: `App` and `Roms`; MinUI: `App`, `Emus` and `Roms`; Allium: `App` and `Apps`; stock and spruce: `App`). On Windows, drag them onto the card and choose to replace the files. On a Mac, don't drag folders in Finder (Replace deletes the whole folder on the card); unzip with Terminal instead: `unzip -o <zip> -d /Volumes/<card>`.
4. Put your ROM in the game's folder as `App/<Game>/rom.z64`. Any .z64, .v64 or .n64 dump works, renamed to rom.z64. The table below has the version each game needs.
5. Eject the card, put it back in the Miyoo and start the game. The first start takes a minute.

| OS | Where the game shows up |
|---|---|
| Onion | Apps, and Ports (with the Ports Collection) |
| Stock OS | Apps |
| MinUI, MyMinUI | the home screen |
| spruce | Apps |
| Allium | Apps |

### Which ROM

Each game checks the dump and refuses any other version (the screen says which one it found).

| Game | ROM folder | Version | SHA-1 |
|---|---|---|---|
| Perfect Dark | `App/PerfectDark/` | Perfect Dark (USA), Rev 1 (version 1.1) | `af8788ac4d1a57260eae9c53ffe851fcf2a3319b` |
| Mario Kart 64 | `App/MarioKart64/` | Mario Kart 64 (USA) | `579c48e211ae952530ffc8738709f078d5dd215e` |
| Diddy Kong Racing | `App/DiddyKongRacing/` | Diddy Kong Racing (USA), Rev 1 (version 1.1) | `6d96743d46f8c0cd0edb0ec5600b003c89b93755` |
| Ocarina of Time | `App/OcarinaOfTime/` | The Legend of Zelda: Ocarina of Time (USA), v1.0 | `ad69c91157f6705e8ab06c79fe08aad47bb57ba7` |
| GoldenEye 007 | `App/GoldenEye/` | GoldenEye 007 (USA) | `abe01e4aeb033b6c0836819f549c791b26cfde83` |
| F-Zero X | `App/FZeroX/` | F-Zero X (USA) | `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340` |
| Star Fox 64 | `App/StarFox64/` | Star Fox 64 (USA), version 1.1 | `09f0d105f476b00efa5303a3ebc42e60a7753b7a` |
| Paper Mario | `App/PaperMario/` | Paper Mario (USA) | `3837f44cda784b466c9a2d99df70d77c322b97a0` |
| Super Smash Bros. | `App/SuperSmashBros/` | Super Smash Bros. (USA) | `e2929e10fccc0aa84e5776227e798abc07cedabf` |
| Super Mario 64 | `App/SuperMario64/` | Super Mario 64 (USA) | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` |
| Majora's Mask | `App/MajorasMask/` | The Legend of Zelda: Majora's Mask (USA) | `d6133ace5afaa0882cf214cf88daba39e266c078` |
| Harvest Moon 64 | `App/HarvestMoon64/` | Harvest Moon 64 (USA) | `90631460f1876a14849df0541d534012b410a34c` |

The SHA-1 is of the big-endian `.z64` byte order; `.v64` and `.n64` dumps of the same ROM are
converted. Besides `App/<Game>/rom.z64`, each game also looks in `Roms/N64/` and anywhere on the
card down to two folders deep, under any file name. F-Zero X, Paper Mario and Perfect Dark read
the ROM while they run, so keep it on the card for those three.

## Controls

| Button | Every game |
|---|---|
| <kbd>MENU</kbd> | Game menu: resume, save state, load state, controls, exit |
| <kbd>Up</kbd> / <kbd>Down</kbd> | Pick a menu entry |
| <kbd>Left</kbd> / <kbd>Right</kbd> | Change the save slot |
| <kbd>A</kbd> / <kbd>START</kbd> | Select |
| <kbd>B</kbd> / <kbd>MENU</kbd> | Close the menu |
| <kbd>Power</kbd> | Saves the auto save on the way out, and before the Mini sleeps |

Each game's own button layout is in its folder's README:
[Perfect Dark](perfect-dark/README.md#controls),
[Mario Kart 64](mario-kart-64/README.md#controls),
[Diddy Kong Racing](diddy-kong-racing/README.md#controls),
[Ocarina of Time](ocarina-of-time/README.md#controls),
[GoldenEye 007](goldeneye-007/README.md#controls),
[F-Zero X](f-zero-x/README.md#controls),
[Star Fox 64](star-fox-64/README.md#controls),
[Paper Mario](paper-mario/README.md#controls),
[Super Smash Bros.](super-smash-bros/README.md#controls),
[Super Mario 64](super-mario-64/README.md#controls),
[Majora's Mask](majoras-mask/README.md#controls),
[Harvest Moon 64](harvest-moon-64/README.md#controls).

**MENU > CONTROLS** shows that layout: each N64 button with the Miyoo button that gives it. Pick a row with A,
then press the Miyoo button you want for it; if that button already does something else, the screen offers to
swap the two. The change is saved for that game alone (`App/<Game>/data/controls.cfg`), and RESET TO DEFAULT puts
the layout above back. The game's own modes (aiming, chords such as START + L1, the menus' layout) move with the
buttons. MENU and the d-pad cannot be changed, so the menu is always reachable.

## How it's built

Each game is its decompilation project's C code compiled for the Mini's ARM CPU, with a port layer per game: a software renderer descended from the Super Mario 64 PC port's Fast3D (reworked to split each frame's drawing across both cores), audio through the Mini's SDL 1.2, and a shared kit for the panel, the game menu, save states and the CPU clock (which it puts back after the Mini sleeps: [Sleep and wake](docs/SLEEP-AND-WAKE.md)). On first start each game rebuilds its data from your ROM using recipes (numbers only, no game data) shipped in the zip.

## Build it yourself

Every game builds in Docker, on the Miyoo Mini toolchain Onion and MinUI use (GCC 8.3, glibc 2.28
and the device's SDL 1.2 in the sysroot, at `/opt/miyoomini-toolchain`). Most games' build images
start from the Mario Kart 64 image, so build that one first:

```sh
docker build --platform linux/amd64 -t mk64hh:build mario-kart-64/docker
```

Each game's `dev.sh` then runs a command in that game's image. GoldenEye 007 also has its own
Miyoo image (`goldeneye-007/docker/Dockerfile.miyoo`). Perfect Dark has no `dev.sh`: its `make`
runs the toolchain directly, in the Mario Kart 64 image or wherever `/opt/miyoomini-toolchain` is.

Mario Kart 64 and F-Zero X use the decompilations' tools as git submodules:

```sh
git submodule update --init --recursive
```

Most builds read your ROM: to extract what the decomp's build needs, to derive the recipes the
game uses to rebuild its data on the Miyoo (numbers only), and for the release gates' leak scan,
which refuses to package a zip if any file in it matches the ROM. Nothing read from the ROM is
written into this repository (each folder's `.gitignore` is an allowlist) or the public zip.
GoldenEye 007, Paper Mario, Super Mario 64, Majora's Mask and Harvest Moon 64 fetch their decompilation's code or data at build time,
at a pinned commit; Perfect Dark fetches the PC port's asset metadata (names and IDs) the same way,
and builds without the ROM.

The steps for each game are in its README, under "Building the Miyoo zip":
[Perfect Dark](perfect-dark/README.md#building-the-miyoo-zip),
[Mario Kart 64](mario-kart-64/README.md#building-the-miyoo-zip),
[Diddy Kong Racing](diddy-kong-racing/README.md#building-the-miyoo-zip),
[Ocarina of Time](ocarina-of-time/README.md#building-the-miyoo-zip),
[GoldenEye 007](goldeneye-007/README.md#building-the-miyoo-zip),
[F-Zero X](f-zero-x/README.md#building-the-miyoo-zip),
[Star Fox 64](star-fox-64/README.md#building-the-miyoo-zip),
[Paper Mario](paper-mario/README.md#building-the-miyoo-zip),
[Super Smash Bros.](super-smash-bros/README.md#building-the-miyoo-zip),
[Super Mario 64](super-mario-64/README.md#building-the-miyoo-zip),
[Majora's Mask](majoras-mask/README.md#building-the-miyoo-zip),
[Harvest Moon 64](harvest-moon-64/README.md#building-the-miyoo-zip).

## How it's made

Made with the help of AI coding tools. Every change is checked before release: each game's frames are compared against the previous build in an emulator (qemu) on deterministic runs, release gates refuse a zip that contains anything matching the ROM or a host path, and builds are played on a real Miyoo Mini Plus before they go up. More on the approach and the tooling: [How it was made](docs/HOW-IT-WAS-MADE.md).

## Reporting bugs

Open an issue: https://github.com/mr-finster/miyoo64/issues. Say which game, which OS and
version, and which Miyoo (Mini, Mini Plus ...), and attach `console.log` from `App/<Game>/data/`
(Mario Kart 64: `App/MarioKart64/console.log`). It records the last run; the logs of earlier runs
are kept next to it.

## Credits

Built on the work of the decompilation communities:

- Perfect Dark: [n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark) (MIT) and
  [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark), its PC port
- Mario Kart 64: [n64decomp/mk64](https://github.com/n64decomp/mk64), with
  [beckerd/psp_mk64_portable](https://github.com/beckerd/psp_mk64_portable)'s portable tree
- Diddy Kong Racing: [DavidSM64/Diddy-Kong-Racing](https://github.com/DavidSM64/Diddy-Kong-Racing)
  (CC0), with Golden Balloon's ([akratch/goldenballoon](https://github.com/akratch/goldenballoon))
  asset byte-order layer (MIT)
- Ocarina of Time: [zeldaret/oot](https://github.com/zeldaret/oot), with
  [z2442/oot-PSP](https://github.com/z2442/oot-PSP)
- GoldenEye 007: [n64decomp/007](https://github.com/n64decomp/007)
- F-Zero X: [inspectredc/fzerox](https://github.com/inspectredc/fzerox) (CC0)
- Star Fox 64: [sonicdcer/sf64](https://github.com/sonicdcer/sf64) (CC0)
- Paper Mario: [pmret/papermario](https://github.com/pmret/papermario)
- Super Smash Bros.: [VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re)
  and [JRickey/ssb-decomp-re](https://github.com/JRickey/ssb-decomp-re), with code from
  [JRickey/BattleShip](https://github.com/JRickey/BattleShip) (MIT) and the
  [HarbourMasters/Starship](https://github.com/HarbourMasters/Starship) audio mixer (MIT)
- Super Mario 64: [n64decomp/sm64](https://github.com/n64decomp/sm64),
  [sm64-port/sm64-port](https://github.com/sm64-port/sm64-port),
  [fgsfdsfgs/sm64-port](https://github.com/fgsfdsfgs/sm64-port) and
  [DrUm78/sm64-port](https://github.com/DrUm78/sm64-port)
- Majora's Mask: [zeldaret/mm](https://github.com/zeldaret/mm), on the Ocarina of Time port's handheld layer
- Harvest Moon 64: [harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp) (CC0), on the
  Paper Mario port's handheld layer

The renderers descend from the Super Mario 64 PC port's Fast3D, reworked for software rendering.
Each folder keeps its upstream licence and notice files (`LICENSE`, `LICENSE.md`, `NOTICE.md`);
its README's Credits section has the details.

## Licence

MIT for this project's own code: see LICENSE. Each game folder's upstream decompilation and third-party
code keeps its own licence: CC0 for the Diddy Kong Racing, F-Zero X and Star Fox 64 decompilations
(`diddy-kong-racing/decomp/LICENSE.md`, `f-zero-x/game/LICENSE`, `star-fox-64/port/LICENSE.md`), MIT for the
Perfect Dark decompilation and its PC port (`perfect-dark/LICENSE`), beckerd's
terms in `mario-kart-64/base-psp/LICENSE`, the MIT notices for BattleShip and Starship in
`super-smash-bros/port/NOTICE.md` and for Golden Balloon in `diddy-kong-racing/port/asset_swap.c`, and the
upstream projects' own terms for the rest.

Unofficial ports, not connected to Nintendo, Rare, Miyoo or any other rights holder.

## Support

If you enjoy it, you can buy me a coffee: https://buymeacoffee.com/mrfinster
