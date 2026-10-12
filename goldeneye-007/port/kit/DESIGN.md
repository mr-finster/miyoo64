# The port kit: design

## Why

Nine ports carry copies of MK64's platform files, and each copy gets fixed on its own: the
same bug is found and mended several times, and a fix made in one port rarely reaches the
others. The kit is the one copy. Decided by the user, 2026-09-24 (option C): the platform
layer and the rasterizer move into the kit; `gfx_pc.c` stays in each port.

## How the copies differ today (measured 2026-09-24 against the MK64 port bbe66b9)

Changed lines against MK64's file (`+added/-removed`):

| File | MK64 lines | DKR | Star Fox | SM64 | What differs |
|---|---|---|---|---|---|
| `handheld_menu.c` | 571 | +3/-3 | +3/-3 | +3/-3 | Only the game's name: the environment prefix and the settings file name. |
| `handheld_menu.h` | 31 | 0 | 0 | 0 | Nothing. |
| `savestate.c` | 438 | +61/-98 | +16/-6 | +19/-15 | DKR: an include list instead of the hole, and a backstop that comes back as it was. Star Fox: stdin/stdout/stderr kept across a desktop load. SM64: the asset region's game-state head is saved. All: the name. |
| `savestate.h` | 52 | +3/-1 | 0 | 0 | DKR: `SAVESTATE_EXCLUDE` is a no-op. |
| `controller_sdl.c` | 122 | +136/-48 | +86/-66 | +90/-62 | Button maps (per game) and the hotkey handling. |
| `gfx_sdl12.c` | 624 | +25/-19 | +72/-25 | +13/-13 | Present thread, power-off signal, 3D RES, settings, test hooks. |
| `assets_handheld.c` | 788 | - | - | +109/-66 | ROM finder, cache, error screens. |

F-Zero X, OoT, Smash, Paper Mario and GoldenEye have no menu or save states yet (stubs).

## K1: menu and save states (tag k1)

- **Settings, not edits.** The kit's files read `hh_game.h` from the port: `HH_GAME_ID`
  (file names, the state's build id, the fallback program name), `HH_ENV(name)` (the
  environment prefix: `<GAME>_DATA`, `<GAME>_RESUME_ANSWER`, `<GAME>_MENU_TEST`),
  `HH_SS_MAGIC`, and optionally `HH_SS_INCLUDE_LIST` and `HH_BOOT_LOAD_FRAME`. What each
  of DKR, Star Fox and SM64 changed by hand becomes a setting or a default.
- **Both save-state layouts.** A state is a list of memory ranges: the hole layout saves
  `[__data_start, end)` as two ranges around the `dontsave` section (or one range, with the
  section kept live across the load); the include list saves `.game_data` and `.game_bss`.
  The header records every range, so a state from another layout or build is rejected
  before any memory is touched. The end of the hole layout is `__assets_state_end` when a
  port defines it (SM64), else `__assets_start` when an asset region follows `.bss`, else
  `_end`. The asset symbols are weak, so a port without an asset region still links.
- **Folded-in fixes.** Star Fox's stdio guard (glibc desktop builds copy the stdio handles
  into `.bss`); DKR's backstop swap (`port_audio_backstop_swap`, weak: without it the
  backstop comes back on, as on MK64); a load can no longer fail on `malloc(0)` when the
  `dontsave` section is empty (MK64 treated a NULL from it as out of memory).
- **B6, key-ups in the menu.** Both menu loops pass `SDL_KEYUP` on to the port's
  `controller_handheld_key` (weak), so a button let go inside the menu no longer stays held
  after RESUME. Opening the menu counts as a chord, so a hotkey held into it doesn't fire its
  tap when released there.
- **B7, what the player is told.** SAVE and LOAD results are shown after they happen:
  `SAVED SLOT n`, `LOADED SLOT n`, `LOADED AUTO SAVE`, or `SAVE FAILED` / `LOAD FAILED`
  (MK64 showed `SAVED SLOT n` before the save ran, and even when it failed). A failed EXIT
  GAME save is shown before the quit.
- **Checks** (`tests/run.sh`): the save-state tests pass in both layouts, with and without
  a backstop swap (save, load, host-side state kept live, a truncated state rejected before
  memory is touched, a failed save keeps the old state, EXIT's failed save is shown). Every
  file compiles with `-Wall -Wextra` and no warnings for the device (the toolchain's gcc
  10.2, musl) and the desktop, once each against the MK64, DKR, Star Fox and SM64 settings.
  B6 is checked by the pilot port (no SDL 1.2 desktop build in the images).

### Decided by the user (k1.1)

No 3D options (2026-09-24): SMOOTH is better than SHARP in every way, and every game is to run
its 3D at 100%, so the menu drops 3D FILTER and 3D RES and the platform forces both. Nearest
filtering and lower 3D rungs stay only as developer switches for A/B work, never in a player's
menu or hotkeys.

### Decided by the user (K1)

A failed auto save (power-off or EXIT GAME, e.g. a full card) leaves the previous auto save
on the card, and the next launch offers RESUME GAME for that older session. Asked on
2026-09-24 whether to keep it, remove it or mark it ("RESUME OLDER SAVE"): **keep as is.**
The only change is that a failed EXIT GAME save is now shown (B7).

## Later tags

- **K2, input.** The handheld key layer: key state, a hotkey fires on release only when no other
  button went down (MK64 8a44bee), the menu chord, and a per-game button map table. Fixes
  the hotkey-while-held bugs in DKR, F-Zero X, Star Fox and OoT.
- **K3, power and present.** From `gfx_sdl12.c`: the power-off signal, the present thread, 3D RES and its settings, test hooks.
- **K4, ROM and saves.** The ROM finder (Star Fox 4e9483c's search, `.u1`, B9's wording, a
  rejected cache dropped), the error screens, and MK64 65d7488's atomic EEPROM writer.
- **K5, tools and gates.** Provenance, NOBITS, no COPY relocations, the leak scan, gates 2b
  and 4, and the test tools (qsweep, qprof, qstate, hudab, trans_sweep).
- **K6, the rasterizer.** `gfx_soft.c` converged from MK64 round 28 and OoT v13-v16 (plus
  DKR's row-range composite and F-Zero X's z-first hot_span) behind per-game flags, with a
  bit-exact sweep in every port that takes it, x86 and ARM. With 3D fixed at 100% the unified
  path (sIntW == sNatW: no seed, no composite) is the normal case in every port but Star Fox,
  whose native frame is 320x240; its row-range composite (sf64 f7561b8) adds a start_frame
  reset of the seed and touch trackers that DKR's origin (a0ee8b5) lacks. Candidate for it: MK64's
  see-through texel skip (mk64 2be0689): a precomputed bit per 8888 texel, "its
  bilinear 2x2 neighbourhood is all alpha 0", lets a pixel group skip the fetch when alpha
  comes from the texel (exact: those pixels fail the alpha test anyway). At DK 100% ~26k such
  texels a frame are fetched at full price; measure the share first. The raster is not
  memory-bound (MK64 round 29: ~10-15% of the time on a model of the A7's caches), so the
  wins are in pixel work, not prefetching.

- **K7, audio.** `mixer.c` with Star Fox's bit-exact NEON kernels (the Star Fox 64 port 323f5a6: aMix,
  aResample and ADPCM match their C loops; the old sm64-port kernels didn't) and its
  `mixer_test` (200k random trials per kernel, NEON against a renamed scalar build), the
  aHiLoGain byte-count fix, and `audio_sdl.c` (osAiGetLength as the queue minus a cushion; the
  backstop mixes only while the game thread renders or paces, never mid-logic or muted; a
  virtual AI clock for deterministic audio dumps).

- **K8, screen size.** An opt-in 320x240 output from the settings file (the user's call, 2026-09-24:
  MK64 first as a test for a player with a 320x240 handheld device, then every port as it is released).
  At 320x240 the 3D renders at the N64's own size and is
  presented 1:1 (no squeeze, no HUD area filter), with the menu and first-start screens centred.
  Lifted from MK64's version (f760b73 + 46e125f); nobody here owns such a device, so it ships as
  experimental, checked on the desktop proxy and under qemu. Keep the menu's widened backdrop in the
  art's own pixel format: MK64's first version converted zone_bg.png to ARGB, SDL's blit took another
  path, and the dimmed backdrop came out lighter (39,58,97 against 240's 25,37,62).

## Order and rules

- Pilot SM64 (a near-fresh MK64 copy), then Star Fox and DKR; MK64 moves last, behind its
  own device test (it is live). F-Zero X, OoT, Smash, Paper Mario and GoldenEye take the
  menu and save states when they get to them.
- Ports pin a tag and move on purpose; nothing reaches MK64 players without an MK64 device
  test.
- A kit change carries its own checks (`tests/run.sh`, and a sweep for anything that draws).
- Until a port adopts the kit, every platform or rasterizer change is logged with its
  commit, so the kit can pick it up.
- Allowlist `.gitignore`, no ROM data.
