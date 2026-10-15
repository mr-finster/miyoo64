/* hh_game.h -- Perfect Dark: the per-game settings the port kit's platform files read. */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "perfectdark"   /* file names, the state build id, the fallback program name */
#define HH_ENV(name) "PD_" name       /* environment knobs: PD_DATA, PD_RESUME_ANSWER, PD_MENU_TEST */
#define HH_SS_MAGIC  0x50443753u    /* 'PD7S': save-state header magic */
#define HH_SS_SPARSE 1
#define HH_DEFAULT_3D_PCT 100

#endif
