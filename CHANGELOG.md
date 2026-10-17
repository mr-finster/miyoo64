# Changelog

Newest first. Downloads: the [releases](https://github.com/mr-finster/miyoo64/releases). Each game's own
fixes are also on https://miyoo-n64.pages.dev (Changelog under each game).

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
