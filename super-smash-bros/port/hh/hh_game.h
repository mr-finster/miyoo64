/* hh_game.h -- Super Smash Bros.: the per-game settings the kit's platform files read
 * (port/kit, k1.7 as vendored from ocarina-of-time). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "ssb64"          /* file names, the state build id, the relaunch's fallback name */
#define HH_GAME_TITLE "SUPER SMASH BROS." /* the Miyoo menu's heading and resume prompt (kit miyoo_menu.c) */
#define HH_ENV(name) "SSB64_" name     /* environment knobs: SSB64_DATA, SSB64_RESUME_ANSWER, SSB64_MENU_TEST */
#define HH_SS_MAGIC  0x53534253u      /* 'SSBS': save-state header magic */

/* Save states hold only the .game_data/.game_bss ranges port/gamestate.ld gathers
 * (__gamedata_start/_end, __gamebss_start/_end): every decomp object's .data and .bss (the game,
 * its libultra audio and gu), minus BattleShip's netplay objects; the coroutines (the game's
 * threads: their stacks and contexts); the BattleShip bridge (hh/bs: the reloc tables, the audio
 * bridge and mixer, the fighter registry, the N64 OS stubs); the SRAM mirror; and the port
 * variables tagged HH_GS_BSS / HH_GS_DATA. SAVESTATE_EXCLUDE is a no-op: the rest of the port
 * layer, SDL, libc, the renderer's caches and the audio ring lie outside both ranges. */
#define HH_SS_INCLUDE_LIST 1

/* Sparse states (kit k1.5): each range goes to the file as a 4 KB page bitmap plus only the
 * pages that aren't all zero (Smash: the scene arena's free tail, the shallow coroutine stacks). */
#define HH_SS_SPARSE 1

/* A port-side variable that is game state. HH_GS_BSS for zero-initialised variables, HH_GS_DATA
 * for the rest. A host handle, a malloc'd pointer or a wall-clock time must never carry either. */
#define HH_GS_BSS  __attribute__((section(".bss.hhgs")))
#define HH_GS_DATA __attribute__((section(".data.hhgs")))

#endif
