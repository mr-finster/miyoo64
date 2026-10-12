/* hh_game.h -- Star Fox 64: the per-game settings handheld-kit's platform files read.
 * Each port keeps its own copy beside its port sources; this one is the kit's
 * reference for that port (and what tests/build.sh compiles against). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "sf64"          /* file names, the state build id */
#define HH_ENV(name) "SF64_" name     /* environment knobs: SF64_DATA, SF64_RESUME_ANSWER, SF64_MENU_TEST */
#define HH_SS_MAGIC  0x53463653u    /* 'SF6S': save-state header magic */
#define HH_GAME_TITLE "STAR FOX 64" /* the Miyoo menu's heading (miyoo_menu.c) */
/* P17's gate on by default (round 12): every-pixel bilinear cost +3.3% and changed nothing visible on a
 * square output here -- the user's rule keeps such a fix off. SF64_MAGBILIN=0 = every pixel bilinear. */
#define HH_MAGBILIN_DEFAULT 1
/* Sparse save states (kit k1.5, from goldeneye-007 e037c3b): a 4 KB page bitmap per range plus only
 * the non-zero pages; a full-size state still loads. */
#define HH_SS_SPARSE 1

#endif
