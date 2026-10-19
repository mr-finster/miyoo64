# Harvest Moon 64 for the Miyoo Mini

A native port of Harvest Moon 64, built from the [harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp)
decompilation, with the handheld layer of this repository's Paper Mario port (the other nusys game: its nusys and
libultra layer, renderer and Miyoo kit). It runs on the Miyoo Mini family (Mini, Mini Plus, Mini v4, Mini Flip).

**Version 1.0.0.** The whole game plays; it was compared scene by scene against two N64 renderers (angrylion and
rice) and played through four in-game days under test, with the diary saves, save states and the auto save.

**No game data is included**, here or in the zip. You need your own ROM.

Download `HarvestMoon64-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the
[top-level README](../README.md#install) says.

## Your ROM

- **Harvest Moon 64 (USA)**
- SHA-1 `90631460f1876a14849df0541d534012b410a34c`
- .z64, .v64 or .n64, renamed to `App/HarvestMoon64/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the
  card down to two folders deep, under any file name)
- The first start builds the game data from your ROM once, into `App/HarvestMoon64/data/` (16 MB). Keep the ROM on
  the card: an update can need to build it again.

## Controls

| Miyoo | N64 | In the game |
|---|---|---|
| D-pad | Stick | walk (run is the stick's full tilt) |
| A | A | talk, pick up, throw, check, confirm |
| B | B | use the tool, cancel |
| X | Z | show the held item's name |
| Y | C-down | eat the held item |
| L1 | C-left | whistle for the horse |
| R1 | C-right | whistle for the dog |
| L2 | C-up | put the held item in the rucksack |
| R2 / SELECT | R / L | turn the camera on the farm |
| START | START | the pause screen |
| MENU | | Game menu: resume, save state, load state, exit |

## Known issues

- Textures are drawn sharp: the N64 smooths them (bilinear filtering), so the picture is crisper than on a TV.
- Diary saves from N64 emulators don't load (the port keeps the battery save in the Mini's byte order).
- Only played on the original Mini so far. The Mini Plus, Mini v4 and Mini Flip, and Onion's Ports list, MinUI,
  Allium and spruce still need a first run.

## More

[README_MIYOO.md](README_MIYOO.md) has the details: the screen, the speed, power and sleep, saves, and the settings.

## Building the Miyoo zip

On the Miyoo Mini toolchain (`/opt/miyoomini-toolchain`, as in the other games) with Python 3, `binutils-mips-linux-gnu`
and your ROM:

    tools/setup.sh <dir> <your Harvest Moon 64 (USA) .z64>    # hm64-decomp at the pinned commit + decomp.patch + hh/
    make -C <dir> -f hh/Makefile.handheld -j4 recipe          # build/hh/hm64 and its first-start recipe
    tools/package_public.sh <dir> HarvestMoon64-miyoo.zip     # the executable and the recipe: no ROM data

`tools/setup.sh` clones hm64-decomp at the commit this port is based on (`9824b88`), applies `decomp.patch` (every
change behind `TARGET_HANDHELD`, the N64 build unchanged) and copies `hh/`, then runs the decomp's own setup and
matching N64 build, the Miyoo build (`hh/Makefile.handheld`) and the data pack (`hh/tools/hm64_pack.py`). Everything
read from the ROM stays in `<dir>`.

- **The N64 RAM at its own address.** The game keeps its sprite, map, font, text and bytecode buffers at fixed N64
  addresses, so the game maps 8 MB at 0x80000000 and the executable is linked at 0x40000000; `hh/tools/hm64_syms.py`
  names the buffers and every ROM segment at their N64 addresses, from the matching N64 ELF.
- **The data pack:** the ROM byte for byte, except the cutscene and dialogue bytecode and the text banks, which the
  decomp builds from assembly: each is assembled again for ARM and linked against the native executable, so the
  bytecode's addresses of game variables are the native ones. The pack belongs to one link: build it again after
  every link. The public zip holds the recipe instead (`hh/tools/hm64_recipe.py`: op numbers, lengths and the build's
  own addresses), and the Miyoo builds the pack from the player's ROM at the first start.
- **Big-endian data** (sprites, maps, fonts) is read through `BE16`/`BE32` (identity on the N64), and the renderer
  takes textures and palettes in the N64's byte order.
- **One thread** for the game: the main loop's waits for the retrace pace the game, poll the pad and run the retrace
  callback; a graphics task is drawn at once.

`decomp.patch` also fixes undefined behaviour the N64 build got away with (an array used past its end, two local
buffers copied past their end, a title-screen function falling off its end), keeps libultra's `sinf`/`cosf`
bit-identical to the N64's (`hh/game/hh_trig.c`), and times the pad's auto-repeat in game steps as on the N64.

`tools/package_personal.sh <dir> <out.zip>` makes a card-ready zip WITH the data pack built from your ROM. Keep it
to yourself.

## Credits

- [harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp) (CC0): the decompilation. The game
  code is theirs.
- The handheld layer comes from this repository's Paper Mario port (its renderer after beckerd's Mario Kart 64 PSP
  renderer), with the Perfect Dark port's audio mixer and the Ocarina of Time and Majora's Mask ports' first-start
  design.

Harvest Moon 64 belongs to its rights holders (Victor Interactive Software, now Marvelous; published by Natsume). This
is an unofficial port, not connected to them, Nintendo or Miyoo.
