# Changelog

## k1.7 proposal, cross-port consistency pass (2026-10-01, branch sweep-2026-10-01)

The lead's consistency list after the sweep: kit fixes other ports' sweeps made, taken into this copy with
the same text as the copy that made them.

- savestate.c save_finish fsyncs the state file once more after the rename, still open, before the folder's
  fsync, and closes it after (the Paper Mario port 93bf6f9, after the SSB64 port cd5128c; the same text as PM64's
  copy). On the card's vfat (Linux 4.14 vfat_rename) a rename over an existing state moves the new file into
  the old file's directory slot and only marks its inode dirty, so the slot's new start cluster and size
  reach the card at that inode's writeback (~30 s) or its own fsync; the folder fsync writes the removed .tmp
  entry and the FAT with the old state's clusters freed. A hard power cut in that window (after a menu SAVE
  or the EXIT GAME save) left the state pointing at freed clusters. A
  close failure no longer fails a save. In the save's log line "close" is now the rename, this fsync and the
  close, and "rename" the folder's fsync. GoldenEye's eeprom.bin and first-start goldeneye.z64 got the same
  (port/hh/hh_hw.c, hh_card.c). Checked: the five save-state builds pass (plain gcc); strace of a save shows
  fsync, rename, fsync(file), close, fsync(folder).
- savestate.c, the sparse loader: a page the state holds as zero is cleared only if memory there isn't zero
  already (the DKR port 2da5231, the same text adapted to this copy's ss_page_len). The memset of every zero
  run made each page this process never touched (the N64 DRAM's free heap, unused thread stacks: 70-89% of
  GoldenEye's 8.6 MB state is zero pages) 4 KB of real process memory on a resume at boot, which the 64 MB
  device can only free by swapping it out to the card, and the next save's page scan reads it back. The
  same bytes either way. Checked (test harness, the real savestate.c, 8 MB of zero .bss with a page in
  64 set, saved, then loaded into a fresh process): +8,516 kB resident before, +576 kB now, content equal;
  with zero-in-the-state pages dirtied before the load, content still equal. The five save-state builds pass.

## k1.7 proposal, bug sweep (2026-10-01, branch sweep-2026-10-01)

GoldenEye's copy, brought up to the canonical kit where it lagged, plus one kit change shared with
OoT's sweep proposal. What this copy already had: k1.5's sparse states, third range, per-stage save
timing, failure reasons and the folder fsync after the rename (GoldenEye e037c3b / e0a9c32 are where
the kit took them from), k1.6's hh_priority.c (called at the top of main, hh_boot.c), and the X50
captions below. Not taken: OoT's K8 320x240 menu.

- savestate.c, k1.5's failed-save log (DKR 15ead91), which this copy lacked: a save that failed before
  its fwrites logged `fsync -826443 ... rename 826443` (t_sync/t_close started at 0, not at t_write);
  now 0 ms. The open failure reads "open: <strerror>" as in the kit: GoldenEye's own "open <path>: ..."
  (e0a9c32) put a 520-byte path into the 96-byte reason and failed tests/run.sh on -Wformat-truncation
  (the slot is already in the line).
- tests/run.sh runs k1.5's sparse tests (ss_hole_sparse, ss_hole_sparse_swap): GoldenEye ships
  HH_SS_SPARSE, and its copy of run.sh never built it.

## k1.7 proposal, exit-save captions only (2026-10-01): EXIT GAME froze on the device

From the OoT port (branch sweep-2026-10-01; the canonical kit is the lead's). This copy took
only these two kit changes from OoT's k1.7 (its k1.5/k1.6 content: see the section above):

- savestate.c: the exit save shows SAVING while it writes. The screen otherwise held the last frame with the
  music off for up to a few seconds on a slow card, which reads as a freeze.
- handheld_menu_notify_wait(msg): a blocking caption for "SAVE FAILED" right before the quit. The detached notify
  thread (k1.4) could lose the race with exit() before it forked, so the banner never showed.
- tests/ss_test.c: the good exit save now expects the SAVING caption.
- For every port (not in the kit): shut SDL down on every normal exit (gfx_wm_shutdown), and log the exit
  line to console.log. GoldenEye: port_request_exit (hh_gfx.c).

## k1.4 (2026-09-25): OS captions off the game thread

- handheld_menu_notify ran system("notif set ...") on the game thread: a fork plus a shell script while
  the game waited. On F-Zero X's device bench every caption cost one 53-99 ms late frame and 2-5 audio
  underruns, while the banner itself (drawn by the kernel's fbtft) cost nothing measurable. The command
  now runs on a detached thread (falling back to the old blocking call if no thread can start).

## k1.3 (2026-09-25): no music after EXIT GAME

- The menu unmuted the mixer as it closed, so the exit save (up to ~1 s on the SD card) ran with
  the game's audio live and the music played on before the game quit (DynaMight, MK64 v1.0 and
  v35; MK64's diagnosis). Closing for EXIT GAME (or SDL_QUIT) now leaves the mixer muted; the amp
  and keymap are restored as on any other close.

## k1.2 (2026-09-25): every textured pixel bilinear

- The magnified-only bilinear gate (magbilin, MK64 round 13) decided bilinear or nearest per
  triangle, so a floor switched between smooth and sharp along triangle edges as the camera moved.
  The user saw it on DKR's device ("floor triangles seem to move around ... texture flash a bit").
  DKR round 16: the real game filters everything; on Ancient Lake ~5.8k of 57.6k px a frame
  were drawn sharp; frame-to-frame flicker on a 60-frame drive fell 31%; cost +3.7% instructions.
- settings_load no longer reads the display cfg: the magbilin=1 in older cfgs was written by older
  menus, not chosen. <GAME>_MAGBILIN=1 (HH_ENV) turns the gate back on for a developer A/B.
- Ports that vendor k1.2 should also default gfx_soft.c's gfx_soft_magbilin to 0.

## k1.1 (2026-09-24): no 3D options

- The user's call: 3D FILTER and 3D RES leave the menu. Every game draws its 3D at 100% with
  SMOOTH (bilinear) filtering, whatever an older cfg on the card says; the cfg is still read for
  the hidden magbilin A/B switch. The menu is RESUME, SAVE, LOAD, VOLUME, BRIGHTNESS, EXIT GAME.

## k1 (2026-09-24): menu and save states

From the MK64 port bbe66b9 (`base-psp/src/port/sdl`).

- The game's name, environment prefix and save-state magic come from the port's `hh_game.h`.
- Save states are a list of ranges: MK64's hole layout and DKR's include list
  (`HH_SS_INCLUDE_LIST`), with SM64's `__assets_state_end` bound, Star Fox's stdio guard and
  DKR's backstop swap folded in; a load can no longer fail on `malloc(0)` when the
  `dontsave` section is empty.
- B6: key-ups that arrive while the menu or the launch prompt is up release the game's
  buttons; opening the menu counts as a chord.
- B7: SAVE and LOAD results are shown once they have happened (`SAVE FAILED`, `LOAD FAILED`
  when they didn't); a failed EXIT GAME save is shown before the quit.
- The state header changed (a range list), so states written before k1 are rejected, as
  after any rebuild.
