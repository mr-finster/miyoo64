/* hh_game.h -- GoldenEye 007: the per-game settings the port kit's platform files read
 * (port/kit, a subtree of the shared kit). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "goldeneye"     /* file names, the state build id, the fallback program name */
#define HH_ENV(name) "GE_" name       /* environment knobs: GE_DATA, GE_RESUME_ANSWER, GE_MENU_TEST */
#define HH_SS_MAGIC  0x47453753u    /* 'GE7S': save-state header magic */
#define HH_SS_SPARSE 1
#define HH_DEFAULT_3D_PCT 100         /* the 3D's size, % of the screen (GE_INTRES in debug.cfg overrides: 90% ran slower, round 26) */               /* states skip all-zero 4 KB pages: 8.6 MB -> ~1-3 MB (savestate.c) */

#endif
