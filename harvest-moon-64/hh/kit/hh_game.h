/* hh_game.h -- Harvest Moon 64: the per-game settings the kit's platform files read. */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "hm64"
#define HH_ENV(name) "HM64_" name
#define HH_GAME_TITLE "HARVEST MOON 64"
#define HH_SS_MAGIC  0x484D3653u    /* 'HM6S' */

/* The state: the game's .data and .bss (hh/gamestate.ld) plus the N64's RAM at 0x80000000 (hh_game_extra_range,
 * hh_host.c), as sparse pages (the RAM's free heap is mostly zero). */
#define HH_SS_INCLUDE_LIST 1
#define HH_SS_SPARSE 1

/* a resume loads at mainLoop's third wait (hh_host.c: states are taken and restored only there) */
#define HH_BOOT_LOAD_FRAME 3

#endif
