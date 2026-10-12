/* hh_game.h -- Diddy Kong Racing: the per-game settings the port kit's platform files read.
 * Each port keeps its own copy beside its port sources; this one is the kit's
 * reference for that port (and what tests/build.sh compiles against). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "dkr"          /* file names, the state build id, the fallback program name */
#define HH_ENV(name) "DKR_" name     /* environment knobs: DKR_DATA, DKR_RESUME_ANSWER, DKR_MENU_TEST */
#define HH_SS_MAGIC  0x444B5253u    /* 'DKRS': save-state header magic */

/* Save states hold only the .game_data/.game_bss ranges port/gamestate.ld gathers
 * (__gamedata_start/_end, __gamebss_start/_end); SAVESTATE_EXCLUDE is a no-op. */
#define HH_SS_INCLUDE_LIST 1

#endif
