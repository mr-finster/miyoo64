# Settings file

- Optional. Put it at `App/MajorasMask/data/debug.cfg`, one `key=value` per line.
- A missing key keeps its default.
- The in-game menu's OPTIONS page changes these keys and writes them to this file. Your other lines stay.
  Adding a key to the page: [docs/OPTIONS.md](../docs/OPTIONS.md).
- The game reads only the keys below from this file. The other switches are variables in `launch.sh`
  ([README_MIYOO.md](README_MIYOO.md#settings-in-launchsh)). A line here wins over the same variable in `launch.sh`.

| Key | Default | Applies | What it does |
|---|---|---|---|
| `controls` | `0` | at once | Controls: `0` = ORIGINAL, the N64's; `1` = MODERN, for Link and the Fierce Deity (Deku, Goron and Zora Link keep the N64's): X jumps and C-left moves to SELECT (and stays on START + left) with the automatic jump and ledge grab at edges off, A held after a roll runs faster and the roll or a landing goes straight into the run, rolls chained and steered, a little steering in the air. Each part also has its own key below, which wins over this one (`MM_CONTROLS`) |
| `jumpbutton` | as `controls` | restart | `1` = X jumps and SELECT gives C-left (on the ground: the game's own jump off an edge, long when running) (`MM_JUMPBUTTON`) |
| `autojump` | as `controls` | restart | `0` = running off an edge doesn't jump by itself (only with `jumpbutton=1`) (`MM_AUTOJUMP`) |
| `ledgegrab` | as `controls` | restart | `0` = walking off an edge, Link falls instead of hanging on to it (ladders, vines and grabbing a ledge in mid-air stay) (`MM_LEDGEGRAB`) |
| `sprint` | `100`, `150` with MODERN | restart | The run's speed in % after a roll with A still held (100 to 200; `100` = off); with it, a roll or a landing with A held goes straight into the run (`MM_SPRINT`) |
| `betterroll` | as `controls` | restart | `1` = A again near a roll's end rolls again, and the d-pad steers a roll (`MM_BETTERROLL`) |
| `airsteer` | as `controls` | restart | `1` = the d-pad steers a little in the air (at most 45 degrees either side) (`MM_AIRSTEER`) |
| `jumpstrength` | `70` | restart | With jumpbutton: the jump's least speed x10 (standing, the game's edge jump barely leaves the ground) (`MM_JUMPSTRENGTH`) |
| `fineaim` | `1` | at once | Fine aim: in first person and aiming, the d-pad starts at a small push of the stick and eases up to full tilt; `0` = full tilt at once (`MM_FINEAIM`) |
| `fineaimpct` | `20` | at once | With fineaim: where the d-pad starts, in percent of full tilt (5 to 100), for the first 250 ms (`MM_FINEAIMPCT`) |
| `fineaimms` | `1000` | at once | With fineaim: when the d-pad reaches full tilt, in ms (`MM_FINEAIMMS`) |
| `msgbox1to1` | `1` | restart | Dialog text at the N64's size, the box widened to fill the screen; `0` = the old layout (box and text squeezed to 0.92) |
| `hudfilter` | `1` | at once | Smoothed HUD, sprites and text at 240; `0` = plain scaling |
| `declick` | `1` | restart | The sound fades out and back in around the menu and a save-state load instead of stopping dead; `0` = the old hard cuts |
| `fps30` | `0` | at once | 30 frames a second: the in-between frames drawn with everything that moves blended between the game's 20 updates a second (actors, the camera, effects, Epona, scrolling textures, the dialog box, the light halos), the game's speed unchanged; where the pictures don't fit it steps down (26.7, 25, 24, 23.3 fps) and then to 20. Each picture is held by the panel's vsyncs. `0` = 20 fps, as the N64 (`OOT_FPS30`) |
| `entrance` | none | restart | For testing: start straight in that place with the debug save, skipping the title and the file select (scene << 9 \| spawn << 4: South Clock Town `0xD800`, North `0xD600`, East `0xD200`, West `0xD400`, Laundry Pool `0xDA00`, Termina Field `0x5400`, Southern Swamp `0x8400`, Woodfall `0x8600`, Deku Palace `0x5000`, Mountain Village (winter) `0x9A00`, Great Bay Coast `0x6800`, Romani Ranch `0x6400`, Ikana Canyon `0x2000`, Stock Pot Inn `0xBC00`). Its saves go to `mm-flash-debug.bin`, apart from the real ones (`MM_ENTRANCE`) |
| `time` | the debug save's | restart | With entrance: the clock, `0x8000` noon, `0x4000` 06:00, `0xC000` 18:00 (`MM_TIME`) |
| `day` | the debug save's | restart | With entrance: the day, `1` to `3` (`MM_DAY`) |
| `prof` | off | restart | `1`: the sampling profiler, `data/prof.txt` every 30 s (`OOT_PROF`; report: `ocarina-of-time/port/tools/handheld/prof_report.py`) |
