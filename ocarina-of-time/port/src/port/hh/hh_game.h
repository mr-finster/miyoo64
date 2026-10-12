/* hh_game.h -- Ocarina of Time: the per-game settings handheld-kit's platform files read
 * (port/kit, k1.5). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "oot"          /* file names, the state build id */
#define HH_GAME_TITLE "OCARINA OF TIME" /* the Miyoo Mini menu's heading (kit miyoo_menu.c) */
#define HH_ENV(name) "OOT_" name     /* environment knobs: OOT_DATA, OOT_RESUME_ANSWER, OOT_MENU_TEST */
#define HH_SS_MAGIC  0x4F4F5453u    /* 'OOTS': save-state header magic */

/* Save states hold only the .game_data/.game_bss ranges port/gamestate.ld gathers
 * (__gamedata_start/_end, __gamebss_start/_end): every decomp object's .data and .bss, the
 * generated object/scene/audio tables, and the port variables tagged HH_GS_BSS / HH_GS_DATA
 * below. SAVESTATE_EXCLUDE is a no-op: the rest of the port layer, SDL, libc, the renderer's
 * caches and the audio ring lie outside both ranges by construction. */
#define HH_SS_INCLUDE_LIST 1

/* Sparse states (kit k1.5): each range goes to the file as a 4 KB page bitmap plus only the
 * pages that aren't all zero (OoT: the zeroed heaps and buffers); a load zero-fills the rest.
 * Full-size states (the header without the sparse flag) still load. */
#define HH_SS_SPARSE 1

/* A port-side variable that is game state: a load must restore it with the game's memory
 * (the system heap, the segment table, the scheduler, the asset loader's record of what is
 * in RAM, the main loop's graphics context...). HH_GS_BSS for zero-initialised variables
 * (GCC rejects a non-zero initialiser in a .bss section), HH_GS_DATA for the rest. A host
 * handle, a malloc'd pointer or a wall-clock time must never carry either: a load from an
 * earlier boot would hand it back dead (MK64 v26, environ). */
#define HH_GS_BSS  __attribute__((section(".bss.hhgs")))
#define HH_GS_DATA __attribute__((section(".data.hhgs")))

#endif
