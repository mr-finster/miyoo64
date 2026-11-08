# Settings file

- Optional. Put it at `App/MajorasMask/data/debug.cfg`, one `key=value` per line.
- A missing key keeps its default.
- The in-game menu's OPTIONS page changes these keys and writes them to this file. Your other lines stay.
  Adding a key to the page: [docs/OPTIONS.md](../docs/OPTIONS.md).
- The game reads only the keys below from this file. The other switches are variables in `launch.sh`
  ([README_MIYOO.md](README_MIYOO.md#settings-in-launchsh)). A line here wins over the same variable in `launch.sh`.

| Key | Default | Applies | What it does |
|---|---|---|---|
| `fineaim` | `1` | at once | Fine aim: in first person and aiming, the d-pad starts at a small push of the stick and eases up to full tilt; `0` = full tilt at once (`MM_FINEAIM`) |
| `fineaimpct` | `20` | at once | With fineaim: where the d-pad starts, in percent of full tilt (5 to 100), for the first 250 ms (`MM_FINEAIMPCT`) |
| `fineaimms` | `1000` | at once | With fineaim: when the d-pad reaches full tilt, in ms (`MM_FINEAIMMS`) |
| `msgbox1to1` | `1` | restart | Dialog text at the N64's size, the box widened to fill the screen; `0` = the old layout (box and text squeezed to 0.92) |
| `hudfilter` | `1` | at once | Smoothed HUD, sprites and text at 240; `0` = plain scaling |
| `declick` | `1` | restart | The sound fades out and back in around the menu and a save-state load instead of stopping dead; `0` = the old hard cuts |
