/* hh_swap.h: load-time byte-order conversions of ROM data (hh_swap.c), called from game
 * code under #ifdef PORT. */
#ifndef HH_SWAP_H
#define HH_SWAP_H

#include <ultra64.h>

void hh_swap_animation_data(void *blob, u32 size, const s32 *table1, const s32 *table2);

void hh_swap_audio_bank(void *ctl, u32 size);
void hh_swap_music_table(void *blob, u32 size);
void hh_swap_cseq_header(void *seq);


/* Display lists + vertices in a blob loaded from the ROM, whose segment addresses use
 * segment 'seg' (-1: none) with the blob starting at segment offset 'segoff'. */
typedef struct {
    u8 *base;
    u32 size;
    s32 seg;
    u32 segoff;
    u8 *seen;
} HhGfxBlob;
void hh_gfx_blob_init(HhGfxBlob *b, void *base, u32 size, s32 seg, u32 segoff);
void hh_gfx_blob_dl(HhGfxBlob *b, u32 off);
void hh_gfx_blob_vtx(HhGfxBlob *b, u32 off, u32 n);
void hh_gfx_blob_free(HhGfxBlob *b);

void hh_swap_global_images(void *seg, u32 size, const u32 *dls, s32 ndls, const u32 *tables, s32 ntables);
void hh_tex_level_to_be(u8 *p, s32 width, s32 height, s32 format);

void hh_swap_model(void *file, u32 size, s32 numswitches, s32 numtextures, const char *name);

void hh_swap_font(void *font, u32 size);

void hh_swap_text_bank(void *bank, u32 size);
void hh_swap_briefing(void *file, u32 size);

void hh_swap_dl_blob(void *base, u32 size, s32 seg, const u32 *starts, s32 n);

/* Level geometry (bg.c) and collision tiles (stan). */
void hh_swap_u32_array(void *p, u32 bytes);
void hh_swap_vtx_array(void *p, u32 bytes);
void hh_swap_bg_header(void *data, u32 size);
void hh_swap_stan(void *file, u32 size);

/* Stage setup files (Usetup*Z). */
void hh_swap_setup(void *file, u32 size);

#endif
