/* hh_game.h -- Star Fox 64: the per-game settings the kit's platform files read.
 * Each port keeps its own copy beside its port sources; this one is the kit's
 * reference for that port (and what tests/build.sh compiles against). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "sf64"          /* file names, the state build id, the relaunch's fallback name */
#define HH_ENV(name) "SF64_" name     /* environment knobs: SF64_DATA, SF64_RESUME_ANSWER, SF64_MENU_TEST */
#define HH_SS_MAGIC  0x53463653u    /* 'SF6S': save-state header magic */

#endif
