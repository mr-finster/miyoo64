# Changelog

Newest first. Downloads: the [releases](https://github.com/mr-finster/miyoo64/releases). Each game's own
fixes are also on https://miyoo-n64.pages.dev (Changelog under each game).

## 1.2.0 (Majora's Mask 1.0.0 beta)

A new game: The Legend of Zelda: Majora's Mask (USA ROM), version 1.0.0, a beta. The other ten games are exactly the
1.1.4 zips (nothing changed, RESUME GAME carries over). Perfect Dark stays 1.0.4.

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
