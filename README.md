# Miyoo64

Native ports of nine N64 games to the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip),
built from the N64 decompilation projects made by their communities. No emulation: each game's
C code runs on the Mini's ARM CPU, drawn by a software renderer that uses both of its cores.
You need your own ROM. Nothing from the games is in this repository or in the downloads.

Downloads: https://miyoo-n64.pages.dev (this repository holds the source only).

## Games

| Game | Folder | Built from |
|---|---|---|
| Mario Kart 64 | [mario-kart-64](mario-kart-64/) | [n64decomp/mk64](https://github.com/n64decomp/mk64) |
| Diddy Kong Racing | [diddy-kong-racing](diddy-kong-racing/) | [DavidSM64/Diddy-Kong-Racing](https://github.com/DavidSM64/Diddy-Kong-Racing) |
| The Legend of Zelda: Ocarina of Time | [ocarina-of-time](ocarina-of-time/) | [zeldaret/oot](https://github.com/zeldaret/oot) |
| GoldenEye 007 | [goldeneye-007](goldeneye-007/) | [n64decomp/007](https://github.com/n64decomp/007) |
| F-Zero X | [f-zero-x](f-zero-x/) | [inspectredc/fzerox](https://github.com/inspectredc/fzerox) |
| Star Fox 64 | [star-fox-64](star-fox-64/) | [sonicdcer/sf64](https://github.com/sonicdcer/sf64) |
| Paper Mario | [paper-mario](paper-mario/) | [pmret/papermario](https://github.com/pmret/papermario) |
| Super Smash Bros. | [super-smash-bros](super-smash-bros/) | [VetriTheRetri/ssb-decomp-re](https://github.com/VetriTheRetri/ssb-decomp-re) |
| Super Mario 64 | [super-mario-64](super-mario-64/) | [n64decomp/sm64](https://github.com/n64decomp/sm64) |

Each folder is one game and builds on its own. Every game has save states (9 slots and an auto
save), the same in-game menu (MENU: resume, save, load, exit), and picks up where you left off
after a power-off.

## Install

1. Download the game's zip from https://miyoo-n64.pages.dev (the zips are only there, not on GitHub).
2. Put your Miyoo's SD card in your computer.
3. Unzip the download and copy its folders (App, Roms and the rest) to the root of the card,
   merging them with the folders already there. On Windows, drag them onto the card and choose to
   replace the files.
   On a Mac, don't drag the folders themselves: Finder replaces a folder of the same name, which
   would remove your other apps. Open each folder and drag what's inside into the matching folder
   on the card.
4. Put your ROM in the game's folder as `App/<Game>/rom.z64`. Any .z64, .v64 or .n64 dump works,
   renamed to rom.z64. The table below has the folder and the version each game needs.
5. Eject the card, put it back in the Miyoo and start the game from Apps, or from Ports on Onion.
   The first start takes a minute.
   Don't see Ports on Onion? Turn on the Ports Collection in Apps > Package Manager.

## You need your own ROM

Each game checks the dump and refuses any other version (the screen says which one it found).

| Game | ROM folder | Version | SHA-1 |
|---|---|---|---|
| Mario Kart 64 | `App/MarioKart64/` | Mario Kart 64 (USA) | `579c48e211ae952530ffc8738709f078d5dd215e` |
| Diddy Kong Racing | `App/DiddyKongRacing/` | Diddy Kong Racing (USA), Rev 1 (version 1.1) | `6d96743d46f8c0cd0edb0ec5600b003c89b93755` |
| Ocarina of Time | `App/OcarinaOfTime/` | The Legend of Zelda: Ocarina of Time (USA), v1.0 | `ad69c91157f6705e8ab06c79fe08aad47bb57ba7` |
| GoldenEye 007 | `App/GoldenEye/` | GoldenEye 007 (USA) | `abe01e4aeb033b6c0836819f549c791b26cfde83` |
| F-Zero X | `App/FZeroX/` | F-Zero X (USA) | `5f658e88ffa9de23cba6986a8fd3d3a90d7b4340` |
| Star Fox 64 | `App/StarFox64/` | Star Fox 64 (USA), version 1.1 | `09f0d105f476b00efa5303a3ebc42e60a7753b7a` |
| Paper Mario | `App/PaperMario/` | Paper Mario (USA) | `3837f44cda784b466c9a2d99df70d77c322b97a0` |
| Super Smash Bros. | `App/SuperSmashBros/` | Super Smash Bros. (USA) | `e2929e10fccc0aa84e5776227e798abc07cedabf` |
| Super Mario 64 | `App/SuperMario64/` | Super Mario 64 (USA) | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` |

The SHA-1 is of the big-endian `.z64` byte order; `.v64` and `.n64` dumps of the same ROM are
converted. Besides `App/<Game>/rom.z64`, each game also looks in `Roms/N64/` and anywhere on the
card down to two folders deep, under any file name. F-Zero X and Paper Mario read the ROM while
they run, so keep it on the card for those two.

## Controls

The controls table of each game is in its folder's README:
[Mario Kart 64](mario-kart-64/README.md#controls),
[Diddy Kong Racing](diddy-kong-racing/README.md#controls),
[Ocarina of Time](ocarina-of-time/README.md#controls),
[GoldenEye 007](goldeneye-007/README.md#controls),
[F-Zero X](f-zero-x/README.md#controls),
[Star Fox 64](star-fox-64/README.md#controls),
[Paper Mario](paper-mario/README.md#controls),
[Super Smash Bros.](super-smash-bros/README.md#controls),
[Super Mario 64](super-mario-64/README.md#controls).

MENU opens the game menu in every game: resume, save state, load state (slots 1-9 and the auto
save), exit. Volume and brightness are the OS's.

## Supported OSes

One zip works on every Miyoo OS. The game lives in `App/<Game>/`; the other entries only start
that folder, so they share one copy and one save folder.

| OS | Where the game shows up |
|---|---|
| Onion | Apps, and Ports (with the Ports Collection) |
| Stock OS | Apps |
| MinUI, MyMinUI | the home screen |
| spruce | Apps |
| Allium | Apps |

## Building from source

Every game builds in Docker, on the Miyoo Mini toolchain Onion and MinUI use (GCC 8.3, glibc 2.28
and the device's SDL 1.2 in the sysroot, at `/opt/miyoomini-toolchain`). Most games' build images
start from the Mario Kart 64 image, so build that one first:

```sh
docker build --platform linux/amd64 -t mk64fk:build mario-kart-64/docker
```

Each game's `dev.sh` then runs a command in that game's image. GoldenEye 007 also has its own
Miyoo image (`goldeneye-007/docker/Dockerfile.miyoo`).

Mario Kart 64 and F-Zero X use the decompilations' tools as git submodules:

```sh
git submodule update --init --recursive
```

Most builds read your ROM: to extract what the decomp's build needs, to derive the recipes the
game uses to rebuild its data on the Miyoo (numbers only), and for the release gates' leak scan,
which refuses to package a zip if any file in it matches the ROM. Nothing read from the ROM is
written into this repository (each folder's `.gitignore` is an allowlist) or the public zip.
GoldenEye 007, Paper Mario and Super Mario 64 fetch their decompilation's code or data at build time,
at a pinned commit.

The steps for each game are in its README, under "Building the Miyoo zip":
[Mario Kart 64](mario-kart-64/README.md#building-the-miyoo-zip),
[Diddy Kong Racing](diddy-kong-racing/README.md#building-the-miyoo-zip),
[Ocarina of Time](ocarina-of-time/README.md#building-the-miyoo-zip),
[GoldenEye 007](goldeneye-007/README.md#building-the-miyoo-zip),
[F-Zero X](f-zero-x/README.md#building-the-miyoo-zip),
[Star Fox 64](star-fox-64/README.md#building-the-miyoo-zip),
[Paper Mario](paper-mario/README.md#building-the-miyoo-zip),
[Super Smash Bros.](super-smash-bros/README.md#building-the-miyoo-zip),
[Super Mario 64](super-mario-64/README.md#building-the-miyoo-zip).

## Reporting bugs

Open an issue: https://github.com/mr-finster/miyoo64/issues. Say which game, which OS and
version, and which Miyoo (Mini, Mini Plus ...), and attach `console.log` from `App/<Game>/data/`
(Mario Kart 64: `App/MarioKart64/console.log`). It records the last run; the logs of earlier runs
are kept next to it.

## Layout

- `<game>/`: one game each (see its README)
- `<game>/miyoo/`: the Miyoo packer (`miyoo/tools/miyoo_pack.py`, the same in every game), its
  launch templates and the game's `game.json`
- `.github/ISSUE_TEMPLATE/`: the bug report form

## Credits

Built on the work of the decompilation communities:

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

The renderers descend from the Super Mario 64 PC port's Fast3D, reworked for software rendering.
Each folder keeps its upstream licence and notice files (`LICENSE`, `LICENSE.md`, `NOTICE.md`);
its README's Credits section has the details.

## Disclaimer

Unofficial ports, rebuilt from the N64 decompilation projects made by their communities. Written
with help from AI coding tools and tested on a Miyoo Mini. Nothing from the original games is
hosted here: no ROMs, no game data. Each port reads a copy of a game you own and builds what it
needs on the Miyoo itself. Not connected to Nintendo, Rare, Miyoo or any other rights holder.

## Licence

MIT for this project's own code: see LICENSE. Each game folder's upstream decompilation and third-party
code keeps its own licence: CC0 for the Diddy Kong Racing, F-Zero X and Star Fox 64 decompilations
(`diddy-kong-racing/decomp/LICENSE.md`, `f-zero-x/game/LICENSE`, `star-fox-64/port/LICENSE.md`), beckerd's
terms in `mario-kart-64/base-psp/LICENSE`, the MIT notices for BattleShip and Starship in
`super-smash-bros/port/NOTICE.md` and for Golden Balloon in `diddy-kong-racing/port/asset_swap.c`, and the
upstream projects' own terms for the rest.

## Support

If you enjoy it, you can buy me a coffee: https://buymeacoffee.com/mrfinster
