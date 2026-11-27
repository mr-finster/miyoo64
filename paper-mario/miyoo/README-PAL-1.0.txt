Paper Mario PAL 1.0.0
=====================
Unofficial native port for Miyoo Mini Plus with Onion OS.
The European PAL game supports English, German, French and Spanish.

ROM
---
Supply your own Paper Mario (Europe) PAL ROM in big-endian .z64 format:
  /mnt/SDCARD/Roms/N64/PaperMarioPAL.z64
Size: 67108864 bytes
SHA-1: 2111d39265a317414d359e35a7d971c4dfa5f9e1
The ROM is not included. The game checks its identity on first discovery.
Other supported ROM search paths continue to work.

INSTALL AND START
-----------------
Back up your saves first. Extract the ZIP to the root of the Onion SD card,
then place your own ROM at the path above. Start Paper Mario PAL in Apps or
Ports. Both launch the same App/PaperMarioPAL installation and save data.
The first launch builds an asset cache from the ROM and can take longer.

UPDATING AND SAVES
------------------
The app folder is App/PaperMarioPAL/ and its data folder is
App/PaperMarioPAL/data/. These names are intended to stay the same for future
releases. The package contains no saves, and installation does not automatically
import, delete, or replace existing data.

To bring a regular in-game save from an older alpha or test app, first keep a
backup, then manually copy its pm64.sav to App/PaperMarioPAL/data/ before
playing. The published 0.9-alpha app used App/PaperMarioPALv09Alpha/data/;
other test apps use their own folders. If the destination already has a save,
do not overwrite it: preserve both copies and decide which one to use. Save
states are tied to their build stamp; compatibility between different builds
is not promised. Regular in-game save migration still needs hardware verification.

NOTES AND CREDITS
-----------------
The PAL work includes improvements to star pieces, texture sampling and
caching, foliage edges, depth comparison, wall-fire combining and other game
scenes. An earlier ARM32 build passed release gates including binary/recipes
pairing, build stamp and ROM/asset leak checks. Earlier Miyoo Mini Plus testing
covered Apps and Ports launch, languages, menus, regular save/load,
savestates and many renderer and gameplay scenes. Texture-edge/coverage artefacts
and some visual differences in Dry Dry Ruins remain. The overlay runtime host
test remains blocked by eight unlinked asset symbols; test coverage is not complete.
The later review fixes and replacement artwork need a new release build and testing.
Based on the miyoo64 project and Paper Mario decompilation contributors.
Source: https://github.com/SunnyBoon/miyoo64
Unofficial community software; Nintendo and Paper Mario belong to their owners.

Paper Mario PAL 1.0 does not include the Universal Guide.
