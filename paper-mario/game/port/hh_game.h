/* hh_game.h -- Paper Mario: the per-game settings the kit's platform files read (kit/, k1.6). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "pm64"          /* file names, the state build id, the relaunch's fallback name */
#define HH_ENV(name) "PM64_" name     /* environment knobs: PM64_DATA, PM64_RESUME_ANSWER, PM64_MENU_TEST */
#define HH_GAME_TITLE "PAPER MARIO" /* the Miyoo menu's heading and resume prompt (kit miyoo_menu.c), as the other ports */
#define HH_SS_MAGIC  0x504D3653u    /* 'PM6S': save-state header magic */

/* The hole layout: .data and .bss from __data_start to the asset region, minus the "dontsave" section. The port's
 * host side (the screen, the sound ring, the renderer, the save file, the asset loader, the kit's own files) is put
 * in that section whole by build/game/dontsave.ld (tools/port/gen_build.py), so a load never overwrites a live host
 * pointer. The asset region (10.9 MB of the decomp's data, almost all of it writable) is not in the range: the game
 * changes a few pages of it, and port_state.c carries those pages inside the state. */

/* Sparse states (kit k1.5): pages that are all zero (the heaps, port_state.c's unused staging pages) are left out. */
#define HH_SS_SPARSE 1

#endif
