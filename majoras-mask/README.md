# The Legend of Zelda: Majora's Mask for the Miyoo Mini

A native port of Majora's Mask, built from the [zeldaret/mm](https://github.com/zeldaret/mm) decompilation on top
of this repository's Ocarina of Time port (its software renderer, audio, the Miyoo kit). It runs on the Miyoo Mini
family (Mini, Mini Plus, Mini v4, Mini Flip).

**Version 1.0.4, a beta:** the whole game plays, and it was compared scene by scene against an N64 renderer
(angrylion), but it has had less play on real devices than the other games. Please report what you find.

**No game data is included**, here or in the zip. You need your own ROM.

Download `MajorasMask-miyoo.zip` from https://miyoo-n64.pages.dev, and install it as the
[top-level README](../README.md#install) says.

## Your ROM

- **The Legend of Zelda: Majora's Mask (USA)**
- SHA-1 `d6133ace5afaa0882cf214cf88daba39e266c078`
- .z64, .v64 or .n64, renamed to `App/MajorasMask/rom.z64` (the game also looks in `Roms/N64/` and anywhere on the
  card down to two folders deep, under any file name)
- The first start builds the game data from your ROM once, into `App/MajorasMask/data/` (about 49 MB). After that
  the ROM is not read again; keep it on the card anyway, as an update may need to rebuild the data.

## Controls

| Miyoo | Does |
|---|---|
| D-pad | Control stick |
| A / B | A / B |
| L1 | Z-target |
| R1 | Shield |
| X / Y | C-left / C-down |
| L2 | C-up (first person, Tatl) |
| R2 | C-right |
| START | Pause |
| START + R1, or START + up | C-up |
| START + left / down / right | C-left / C-down / C-right |
| START + L1 | N64 L (the minimap on/off) |
| MENU | Game menu: resume, save state, load state, exit |

While you play an instrument, the d-pad is the four C notes and A is A; B puts it away.

## Known issues

- The Astral Observatory's tunnel (under East Clock Town) is drawn about twice as bright as on the N64.
- The picture is a little sharper than the N64's: the N64's video filter softens every frame, and this port, like
  the others here, doesn't copy it. Snow and ice show it most.
- In the Giants' Chamber cutscenes, a faint second image of Link can show in close-ups.

## More

[README_MIYOO.md](README_MIYOO.md) has the details: the screen, power and sleep, saves, the settings in
`launch.sh`, and the build.

## Building the Miyoo zip

On the Miyoo Mini toolchain (`/opt/miyoomini-toolchain`, as in the other games) with Python 3 and your ROM:

    tools/setup.sh <dir> <your Majora's Mask (USA) .z64>   # zeldaret/mm at the pinned commit + decomp.patch + hh/
    tools/build_miyoo.sh <dir>                             # MajorasMask-miyoo.zip + the release gates

`tools/setup.sh` clones zeldaret/mm at the commit this port is based on, applies `decomp.patch` (every change behind
`TARGET_HANDHELD`) and copies `hh/` (the Miyoo build: `hh/Makefile.handheld`), then runs the decomp's own setup and
matching N64 build (its generated files and the symbols of every asset), builds the data pack (`hh/tools/mm_pack.py`:
the game's assets compiled for ARM), the game, and the first-start recipe (`hh/tools/mm_recipes.py`: how the Miyoo
rebuilds the pack from the ROM, as op numbers only). Everything read from the ROM stays in `<dir>`.

The decompilation also compiles some ROM data into the game's code (overlay textures, the debug font: about 86 KB in
120 arrays). The executable that ships has those arrays zeroed, with a table of where each one's bytes are in the data
pack (`hh/tools/mm_embed.py`); the game copies them back at boot and checks each one's CRC. The arrays stay where they
are, so save states are not affected.

`tools/build_miyoo.sh` runs the release gates: the recipe is derived again from the pack and must match byte for
byte; no ROM data left in any texture, palette or font of the executable; a leak scan of every shipped file against the ROM and the decompressed ROM (none at all in the recipe, at most
8 KB of decomp-built tables in the executable); no host paths or authoring metadata; and exactly the files
`miyoo/game.json` lists (no ROM, no pack).

The release zips are linked with `PIN_BASE=<the first release's link map and objects>` (`hh/Makefile.handheld`), so
every game address stays where the first release had it and save states carry over from one release to the next.
A build without it is the same code with another layout: its save states and the release zip's don't mix.

`tools/pack_miyoo_personal.sh <dir> <out.zip>` makes a card-ready zip WITH the data pack built from your ROM. Keep it
to yourself.

## Credits

- [zeldaret/mm](https://github.com/zeldaret/mm): the decompilation. The game code is theirs.
- The handheld layer (renderer, audio mixer, libultra on pthreads, input, the Miyoo kit) comes from this repository's
  Ocarina of Time port, itself built with [z2442/oot-PSP](https://github.com/z2442/oot-PSP), and the renderer from
  the Super Mario 64 PC port's Fast3D.

Majora's Mask is Nintendo's. This is an unofficial port, not connected to Nintendo or Miyoo.
