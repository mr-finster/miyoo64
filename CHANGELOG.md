# Changelog

Newest first. Downloads: the [releases](https://github.com/mr-finster/miyoo64/releases). Each game's own
fixes are also on https://miyoo-n64.pages.dev (Changelog under each game).

## 1.3.1 (Majora's Mask 1.0.4 beta)

- Majora's Mask: voices are back (Link's and Deku Link's shouts, the townspeople, the Gorman brothers' laugh, Anju)
  and so are drums (the Bremen Mask march, drum parts in the music). The sound code refused every drum and every
  sound effect of the instrument banks as "not loaded yet" because of how memory is laid out on the handheld.
- Majora's Mask: no more extra eyes on the Keaton Mask and Deku Link; mirrored textures clamped at their edge drew a
  mirrored copy past it.

## 1.3.0 (all twelve games rebuilt)

Every game is rebuilt: Majora's Mask 1.0.3, Harvest Moon 64 1.0.1, Perfect Dark 1.0.5, Ocarina of Time 1.1.5, and
1.1.4 for the other eight.

- All twelve games: your ROM can go anywhere on the SD card, under any name. Each game looks in its own folder
  (`App/<Game>/`) first, then in `Roms/` and its folders, then anywhere on the card up to two folders deep. Any .z64,
  .v64 or .n64 dump in any byte order (and .bin/.rom where a game took them before). Each candidate's header and
  SHA-1 are checked; a wrong version, another game's ROM or a damaged dump is passed over and the search goes on, and
  a game's own data files are never taken for a ROM. The path that worked is remembered (`data/rom_path.txt`). With
  no ROM on the card, the screen says so and where to put it. The same search code in every game
  (`miyoo_romfind.h`).
- Harvest Moon 64: a data file left by an older test build (`data/hm64-pack.bin`) could be taken for the ROM and
  stop the first start; the game's own files are now never candidates.
- The eleven games other than Majora's Mask: the renderer handles the RSP's edge cases as the RSP does (Majora's Mask
  had these in 1.0.1 and 1.0.2): a matrix pop never removes the last matrix; display-list calls nest at most 10 deep
  (F3D/F3DEX) or 18 (F3DEX2); the light count, triangle indices, vertex loads and texture loads stay within their
  buffers and TMEM. The picture is unchanged (the same frames as 1.2.2 under test).
- The ten games other than Majora's Mask and Harvest Moon 64: on a Mini v4 / Flip (752x560) the in-game MENU fills
  the screen (the last game frame, dimmed, behind the menu box) instead of a 640x480 box on black. 640x480 unchanged.
- Each game honours its own `<GAME>_ROTATE=0` (MinUI), checked in all twelve executables.
- Majora's Mask: on the Mini Plus the data pack is now read ahead at every start (1.0.1 and 1.0.2 asked the kernel
  with a call the Plus's kernel doesn't have, so nothing was read ahead there); scenes and sounds load without
  waiting on the SD card. The opening and Clock Town were checked against an N64 recording again:
  no sound effect missing.
- Save states (RESUME GAME) from 1.2.x do not load in the rebuilt games: they are kept as `.rejected`. In-game saves
  carry over.

## 1.2.2 (Majora's Mask 1.0.2 beta)

A Majora's Mask fix; the other eleven games are exactly the 1.2.1 zips.

- Majora's Mask: sound effects that were missing or barely audible now play: the fairies' whoosh at the start,
  Skull Kid's noises and others. Their volume envelopes sit in the sound-effect sequence's own data, which the
  game reads in the N64's byte order; the port read them in the Mini's, so the sound rose far too slowly.
- Majora's Mask: the renderer now copes with what the N64's graphics chip shrugs off instead of crashing: vertex,
  display list, matrix and texture addresses outside the game's memory, vertex indices past the buffer,
  display lists nested deeper than the N64 allows, and a data read with a broken size.
- Save states and the auto save from 1.0.1, 1.0.0 and the test builds load; the game data is not built again.

## 1.2.1 (Majora's Mask 1.0.1 beta)

A Majora's Mask hotfix; the other eleven games are exactly the 1.2.0 zips.

- Majora's Mask: no more crash in the Astral Observatory. The room with the telescope ended one matrix pop below the
  bottom of the renderer's matrix stack; the next matrix was then written over the renderer's own state, and the
  next textured triangle crashed. The RSP never pops its last matrix, and now neither does the port.
- Majora's Mask: the first session after the game data is built runs like any later one. The game now starts again
  in a fresh process once the build is done (the ROM's cached pages dropped), and every session asks the kernel to
  read the whole data pack ahead, instead of loading it from the SD card one small page at a time as each scene,
  object and sound bank is first used. Before, the first minutes (the opening cutscenes) could run slowly, with the
  sound out of step or missing, on a Mini Plus.
- Save states and the auto save from 1.0.0 and the test builds load in 1.0.1; the game data is not rebuilt.

## 1.2.0 (Majora's Mask 1.0.0 beta, Harvest Moon 64 1.0.0)

Two new games: The Legend of Zelda: Majora's Mask (USA ROM), version 1.0.0, a beta, and Harvest Moon 64 (USA ROM),
version 1.0.0. The other ten games are exactly the 1.1.4 zips (nothing changed, RESUME GAME carries over). Perfect
Dark stays 1.0.4.

- Majora's Mask: the whole game, built from the [zeldaret/mm](https://github.com/zeldaret/mm) decompilation on the
  Ocarina of Time port's renderer and audio. Save states, the game menu, the auto save on power-off and before
  sleep, and the Mini v4 / Flip full screen, as in the other games. 20 fps, the N64's own rate.
- Compared scene by scene against an N64 renderer (angrylion): Clock Town by day, by night, on the second day and
  in the final hours, the four regions and their temples, the bosses' rooms, the pause screens, the Song of Time,
  the transformations.
- The first start builds the game data (about 49 MB) from your ROM, once; put it in `App/MajorasMask/` as
  `rom.z64`, or anywhere on the card ([README](majoras-mask/README.md#your-rom)).
- Known issues: the Astral Observatory's tunnel is drawn about twice as bright as on the N64; the picture is a
  little sharper than the N64's (no copy of the N64's video filter, as in the other games; snow and ice show it
  most); a faint second image of Link in close-ups of the Giants' Chamber cutscenes.
- Harvest Moon 64: the whole game, built from the
  [harvestwhisperer/hm64-decomp](https://github.com/harvestwhisperer/hm64-decomp) decompilation on the Paper Mario
  port's renderer and Miyoo kit. Save states, the game menu, the auto save on power-off and before sleep, and the
  Mini v4 / Flip full screen, as in the other games. The diary saves go to `App/HarvestMoon64/data/hm64.sram`.
- Compared scene by scene against two N64 renderers (angrylion and rice): the farm through the seasons, the village,
  the shops and houses, the mountain and the beach, festivals and the menus; four in-game days played through with
  diary saves, save states and the auto save.
- The first start builds the game data (16 MB) from your ROM, once; put it in `App/HarvestMoon64/` as `rom.z64`, or
  anywhere on the card ([README](harvest-moon-64/README.md#your-rom)).
- Known issues: textures are drawn sharp (the N64 smooths them); diary saves from N64 emulators don't load.

## 1.1.4 (Perfect Dark 1.0.4)

A hotfix for Ocarina of Time; the other nine games are exactly the 1.1.3 zips.

- Ocarina of Time: the diving game Zora (Silver Scale) now comes back every time you visit Zora's Domain. Before,
  he was only there on your first visit after starting the game, so if you left without the Silver Scale you
  could not win it until you quit and restarted.
- Ocarina of Time: when the Ganon's Tower escape timer runs out, you are sent back to the start of the escape
  every time, as on the N64 (before: only the first time after starting the game).
- Ocarina of Time: more of the same kind. On the N64, each character's or object's code and data are loaded
  fresh when it appears; the port keeps them all in memory, so some of them remembered things from an earlier
  visit (counters, "already done" flags). They now start fresh where the N64's would, which also fixes the
  Arwing (a cheat-only character) crashing the game when shot down. `OOT_OVLRESET=0` in launch.sh turns it off.
- Ocarina of Time is a new build, so a RESUME GAME auto save from 1.1.3 is not offered after the update (it is
  kept as `state_quick.sav.rejected`). Your in-game saves are kept.

## 1.1.3 (Perfect Dark 1.0.4)

- All ten games, Mini v4 and Mini Flip on Onion: a game started from the GameSwitcher (the recent games list)
  now fills the 752x560 screen, as it does from Ports or Apps. Before, it ran in a small 640x480 picture. Onion
  sets the screen to 752x560 only for a game that says it supports it, and puts it back to 640x480 after every
  game; the zips now carry Onion's two markers (`App/<Game>/full_resolution`, and `FullResolution=1` in the
  Ports shortcut). Nothing changes on a Mini or Mini Plus (640x480 screen).
- All ten games: on a 752x560 Mini, `data/launch.log` notes the screen mode the game found at start.
- The game builds are unchanged from 1.1.2 (the same binaries, so RESUME GAME carries over from 1.1.2); only the
  packaging changed (the markers above, the Ports shortcut and `launch.sh`).

## 1.1.2 (Perfect Dark 1.0.3)

- All ten games: the auto save is written when the Mini goes to sleep (a POWER press, or 50 s with no
  button, before Onion's idle sleep). If the Mini then powers itself off while asleep, RESUME GAME at the next
  start puts you back where you were, even when Onion's power-off is too quick for the exit save. How it works:
  [Sleep and wake](docs/SLEEP-AND-WAKE.md#the-sleep-save-112). `<GAME>_SLEEPSAVE=0` in launch.sh turns it off.
- GoldenEye 007 and Perfect Dark: a power-off with the MENU open (or at the RESUME GAME prompt) now saves the
  auto save, as in the other games.
- Perfect Dark: the game save (`eeprom.bin`) is written safely (a new file, synced, then renamed), so a
  power-off during a save can no longer empty it or roll it back. RESUME GAME is offered every time: about half
  the starts used to throw the auto save away as "from another build".
- All ten games: an auto save that doesn't match the game is no longer deleted. It is kept as
  `state_quick.sav.rejected`, and `console.log` says why.
- Super Smash Bros.: a save from an emulator or a flash cart (`.sra`, `.srm`, `.sav`, any byte order) dropped
  into `App/SuperSmashBros/data` is imported at the next start; the old save is kept as a backup. Before, the
  game always started fresh. How: [Bringing a save](super-smash-bros/README_MIYOO.md#bringing-a-save-from-an-emulator-or-a-flash-cart).
- Ocarina of Time and GoldenEye 007: the first-start screen fills the Mini Flip / Mini v4 screen instead of
  showing squashed.
- Mario Kart 64: 1.1.2 on the first line of `console.log`.

## 1.1.1 (Perfect Dark 1.0.2)

- All ten games: no more slowdown and choppy sound after the Mini sleeps and wakes. Onion's sleep left the CPU
  at 400 MHz on wake, so the game ran at a third of its speed until it was restarted. Each game now puts its
  clock back within a second of waking and logs one `wake:` line. How it works:
  [Sleep and wake](docs/SLEEP-AND-WAKE.md).
- Mario Kart 64 and Diddy Kong Racing: the stall watchdog no longer takes a sleep for a stall (it reported
  one on every wake, and could unpin the game's threads after a long sleep).
- Mario Kart 64: the version is on the first line of `console.log`.

## 1.1.0

- Perfect Dark is out: the full solo campaign, save states and resume, GoldenEye-style controls
  (Perfect Dark (USA) Rev 1). Later in the same release, Perfect Dark 1.0.1 takes the ROM in any byte order
  and finds it anywhere on the card, checked by SHA-1, with errors shown on screen.
- Every game fills the whole screen on the Mini v4 and Mini Flip (752x560), and the black border some
  Mini v4 players saw on and off is gone. A regular Mini or Mini Plus is unchanged. `<GAME>_FLIPFILL=0`
  in the game's launch.sh brings back the 640x480 picture.
- Fixes checked against a real N64:
  - Paper Mario: the glows (Star Rod, Kammy's orb, lightning), a white glow cloud for the Star Spirits,
    no white box on Bowser in the intro.
  - F-Zero X: machine decals, the attack rim, the static fade into a race and explosion debris.
  - Star Fox 64: space and fades in the N64's dark grey instead of pure black.
  - GoldenEye 007: soft shadow ovals on the Runway snow.

## 1.0.2

- GoldenEye 007: smoother and faster, a frame whenever one is ready, with motion following the real frame
  time (Dam: from 30 to about 36-50 fps).
- Diddy Kong Racing: a steady 30 fps instead of jumps to 60 in light scenes.

## 1.0.1

- All games: frames on time. A short sleep on the Mini lasts a whole 10 ms tick, so the last 12 ms of each
  frame are now waited out on the clock.
- GoldenEye 007: an optional real-time motion step (`fracstep=1`).

## 1.0

- First release: Mario Kart 64, Diddy Kong Racing, Ocarina of Time, GoldenEye 007, F-Zero X, Star Fox 64,
  Paper Mario, Super Smash Bros. and Super Mario 64.
