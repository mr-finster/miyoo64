/*
 * Save-state include-list (see port/sdl/savestate.c).
 *
 * The Makefile renames the .data/.bss of every game object (decomp/, the
 * generated trig tables, and the port files that hold game state) to
 * .game_data/.game_bss. They are collected here, between fixed symbols, and a
 * save state is exactly these two ranges. Everything else -- the port layer,
 * SDL, libc, the renderer's caches, the audio ring -- is outside them by
 * construction, so a load can never overwrite a live host pointer.
 */
SECTIONS
{
  .game_data : ALIGN(64) {
    __gamedata_start = .;
    *(.game_data)
    . = ALIGN(64);
    __gamedata_end = .;
  }
}
INSERT AFTER .data;

SECTIONS
{
  .game_bss (NOLOAD) : ALIGN(64) {
    __gamebss_start = .;
    *(.game_bss)
    . = ALIGN(64);
    __gamebss_end = .;
  }
}
INSERT AFTER .bss;
