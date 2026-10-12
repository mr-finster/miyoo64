/* hh_game.h -- Super Mario 64: the per-game settings handheld-kit's platform files read.
 * Each port keeps its own copy beside its port sources; this one is the kit's
 * reference for that port (and what tests/build.sh compiles against). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "sm64"          /* file names, the state build id */
#define HH_ENV(name) "SM64_" name     /* environment knobs: SM64_DATA, SM64_RESUME_ANSWER, SM64_MENU_TEST */
#define HH_SS_MAGIC  0x534D3653u    /* 'SM6S': save-state header magic */

/* port/Makefile links the asset region's game-state head first and marks its end
 * with __assets_state_end; savestate.c picks the symbol up by itself. */

#endif
