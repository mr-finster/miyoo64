/* hh_game.h -- Banjo-Kazooie: the per-game settings the kit's platform files read, and the helpers game code
 * gets under TARGET_HANDHELD (force-included into every game TU by hh/Makefile.handheld). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "banjo"          /* file names, the state build id, the fallback program name */
#define HH_ENV(name) "BK_" name       /* environment knobs: BK_DATA, BK_RESUME_ANSWER, BK_MENU_TEST */
#define HH_SS_MAGIC  0x424B5353u      /* 'BKSS': save-state header magic */
#define HH_SS_SPARSE 1                /* states skip all-zero 4 KB pages */
#define HH_DEFAULT_3D_PCT 100

#if !defined(__ASSEMBLER__)
#include <stddef.h>
#include "hh_port.h"
#if !defined(HH_PORT_LAYER)
#undef NULL
#define NULL 0 /* the decomp (IDO) uses NULL as a plain 0, also for floats and enums */
#endif
#endif

#endif

/* a 16-bit value as the N64 stores it (frame buffers and textures stay in its byte order) */
#define HH_BE16(x) ((unsigned short)__builtin_bswap16((unsigned short)(x)))

/* a test switch read once (hh_bk.c): game code's debug logs check it in hot paths */
int hh_dbg_env(const char *name);
