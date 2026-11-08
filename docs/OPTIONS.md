# MENU > OPTIONS

The in-game menu's OPTIONS page lists a game's settings. Each one is a `key=value` line of the game's
`data/debug.cfg`, the file the game already reads at start, so the page and the file always agree. Ocarina of
Time and Majora's Mask have it.

- **The page:** `kit-options/miyoo_options.c` (the canonical copy; `kit-options/sync.sh` copies it next to each
  game's `miyoo_menu.c`, and `--check` finds copies that differ).
- **The rows:** one table per game: `ocarina-of-time/port/src/port/hh/hh_options.c`,
  `majoras-mask/hh/src/hh_options.c`.
- **On the device:** up/down picks a row, left/right (or A) changes its value, B or MENU goes back. On the way out the
  changed keys are written to `debug.cfg`: only those lines are rewritten (a missing key is added at the end),
  every other line and comment stays, and the file is replaced in one step (`debug.cfg.new`, then renamed).
  Saves and save states are never touched.

## Adding a setting

Make the setting work from `debug.cfg` first, then add one row to the game's table:

```c
{ .label = "FINE AIM", .key = "fineaim", MIYOO_OPT_ONOFF, .def = "1",
  .when = MIYOO_OPT_NOW, .apply = FineAimApply,
  .help = "AIMING AND FIRST PERSON: THE D-PAD STARTS GENTLY, THEN SPEEDS UP" },
{ .label = "PRERENDERED ROOMS", .key = "crop3d",
  MIYOO_OPT_VALUES({ "2", "FOLLOW LINK" }, { "1", "CENTRED" }, { "0", "SQUEEZED" }), .def = "2",
  .help = "TRUE SHAPE, THE VIEW FOLLOWING LINK OR CENTRED; OR SQUEEZED TO FIT" },
{ .label = "PICTURE" },   /* a section header: a label and no key */
```

- `label`: what the page shows, capitals, up to 19 characters.
- `key`: the `debug.cfg` key.
- `MIYOO_OPT_VALUES({ value, name }, ...)`: the choices, each the value written to the file and the name shown
  (up to 11 characters). `MIYOO_OPT_ONOFF` is `1` ON / `0` OFF.
- `def`: the value the game uses when the file has no line. It must match the code's default.
- `help`: optional, one line in capitals, up to about 70 characters. It shows at the bottom for the selected row.
- `env`: optional. It names the variable the code reads with `getenv()`. Without it, the name is the game's prefix
  plus the key in capitals: `fineaim` becomes `OOT_FINEAIM` in Ocarina of Time and `MM_FINEAIM` in Majora's Mask.
  Majora's Mask's renderer and sound read `OOT_` names, so its rows for them set `env`.
- A value in the file that is not one of the choices (`fineaimpct=33`) shows as it is. It is kept until the player
  changes that row.

Then add the key to the game's SETTINGS.md.

## Applies now or after a restart

- **Restart (`MIYOO_OPT_RESTART`, the default):** the game reads the setting once, at start. The page marks a
  changed row with `*` and shows RESTART TO APPLY. The running game is not changed.
- **Now (`MIYOO_OPT_NOW`):** each change sets the row's variable (`setenv`) and calls `apply(value)` right away.
  `apply` must put the new value into whatever the code already read. Fine aim forgets its values
  (`HhInput_FineAimReload`) and reads them again. The HUD filter sets `gfx_soft_hud_box`. If the code reads the
  variable each time it is used, `apply` can be NULL.

`apply` runs on the game thread while the menu is open, between frames. It must not touch game state that save
states hold. Anything it changes stays outside save states, so loading a state never undoes a setting.

## Naming keys

- Lower case letters, digits and `_`, shorter than 24 characters (Ocarina of Time's loader ignores longer keys).
- One word for the feature, as the code's variable names it: `fineaim`, `crop3d`, `hudfilter`. Related keys share
  the first word: `fineaim`, `fineaimpct`, `fineaimms`.
- On/off settings are `1` and `0`, with the old behaviour on one of them. Numbers are plain numbers with no units.
- Keep a key once it ships. If you rename one, still read the old name (`aimramp` for `fineaim`).
- Leave out switches that are only for speed and draw the same picture. They are for testing and belong in
  SETTINGS.md only.

## How each game reads the file

- **Ocarina of Time:** `hh_main.c` `HhLoadDebugCfg` turns every `key=value` line into `OOT_<KEY>`. That covers the
  table's keys and every other key in SETTINGS.md.
- **Majora's Mask:** `main()` calls `miyoo_options_load()`, which reads only the keys in the table into their
  variables. Its other switches stay in `launch.sh`.
- **Another game:** vendor `miyoo_options.c/.h` (add its kit folder to `kit-options/sync.sh`) and add it to the
  build. Then add an OPTIONS item to its `miyoo_menu.c` (see Ocarina of Time's), write a table, and call
  `miyoo_options_load()` at start if nothing reads `debug.cfg` yet. If `debug.cfg` is not in `port_save_dir()`,
  define `miyoo_options_dir()`.

## Tests

- `port/kit/tests/run.sh` (Ocarina of Time's kit) runs `tests/options_test.c`. It checks the rewrite (comments,
  CR LF lines, a last line with no newline, look-alike keys), the atomic write and the loader.
- On a build, `<GAME>_MENU_TEST=<dir>` dumps the page (`options_*.ppm`) with the menu's other screens.
- `<GAME>_MENU_KEYS="150:menu down down down down a right b menu"` presses those buttons at that presented frame.
  `<GAME>_MENU_SHOT=<dir>` saves every picture the menu shows. Together they walk the page in a headless run.
