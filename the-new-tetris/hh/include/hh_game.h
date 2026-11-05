/* hh_game.h -- The New Tetris: the per-game settings the kit's platform files read, and the helpers game code
 * gets under TARGET_HANDHELD (force-included into every game TU by hh/Makefile.handheld). */
#ifndef HH_GAME_H
#define HH_GAME_H

#define HH_GAME_ID   "tnt"          /* file names, the state build id, the fallback program name */
#define HH_ENV(name) "TNT_" name       /* environment knobs: BK_DATA, BK_RESUME_ANSWER, BK_MENU_TEST */
#define HH_SS_MAGIC  0x50575353u      /* 'BKSS': save-state header magic */
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
void hh_idle_forever(void);

/* halfword i of big-endian ROM data (image headers) */
#define HH_BEU16(p, i) ((unsigned short)__builtin_bswap16(((unsigned short *)(p))[i]))
/* a big-endian word read from the ROM into a Dram_u (03A750.c's tables), to host order */
#define HH_FIXW(v) ((v).data = __builtin_bswap32((v).data))
/* the fx asset table (06FFD0.c): its header and entries read from the ROM, to host order; the assets themselves are
 * swapped by hh_fx_swap (hh_fx.c) */
#define HH_FX42(v) ((v).num_assets = (s32)__builtin_bswap32((v).num_assets), (v).end = (s32)__builtin_bswap32((v).end))
#define HH_FX41(v) ((v).type = (s16)__builtin_bswap16((v).type), (v).is_compressed = (s16)__builtin_bswap16((v).is_compressed), \
    (v).compressed_len = (s32)__builtin_bswap32((v).compressed_len), (v).decompressed_len = (s32)__builtin_bswap32((v).decompressed_len), \
    (v).offset = (s32)__builtin_bswap32((v).offset))
void hh_fx_swap(int type, void *data, int n);
