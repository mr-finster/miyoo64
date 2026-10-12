/* hh_game.h -- Mario Kart 64: the per-game settings the kit's platform files read.
 * Each port keeps its own copy beside its port sources; this one is the kit's
 * reference for that port (and what tests/build.sh compiles against). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "mk64"          /* file names, the state build id, the relaunch's fallback name */
#define HH_ENV(name) "MK64_" name     /* environment knobs: MK64_DATA, MK64_RESUME_ANSWER, MK64_MENU_TEST */
#define HH_SS_MAGIC  0x4D4B3653u    /* 'MK6S': save-state header magic */

#endif
