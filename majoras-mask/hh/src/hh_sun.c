/* hh_sun.c -- the sun drawn whole (issue #37).
 *
 * The game draws the sun with gameplay_keep's gSunDL: three strips of one 64x64 I4 picture (rows 0-30, 31-46,
 * 47-63, at 0x79B10, 0x79EF0 and 0x7A0F0), each loaded with a 16-bit LoadBlock into a render tile of format I,
 * size 8-bit, 64 texels a row. Read that way the 4-bit picture shows two half-width suns side by side, and the
 * three strips stack into a grid of small suns: what the port drew, and what the issue's screenshot shows. The
 * decompilation lists the three textures as I4 parts of one 64x64 picture, and 2 Ship 2 Harkinian (the PC port)
 * draws them as that one picture. This list does the same: the day and the evening picture (gSunEvening1Tex at
 * 0x7A310, blended by the game's combiner as before) loaded whole as 64x64 I4 at TMEM 0 and 0x100, one quad over
 * the strips' corners (x -31..32, y -31..32), texel for texel where the strips meant to be. The matrix, the
 * colours and the combiner the game sets around gSunDL are unchanged.
 *
 * gfx_pc.c's G_DL runs this list in place of gSunDL; MM_SUNWHOLE=0 in launch.sh draws gSunDL as before.
 */
#include "ultra64.h"
#include <stdlib.h>

#define HH_SUN_DAY     ((void*)0x04079B10) /* gSun1Tex: the whole 64x64 I4 picture starts with the first strip */
#define HH_SUN_EVENING ((void*)0x0407A310) /* gSunEvening1Tex */
#define HH_SUN_DL      0x0407AB70u         /* gSunDL */

static Vtx sSunVtx[4] = {
    { { { -31, -31, 0 }, 0, { 0 << 5, 0 << 5 }, { 255, 255, 255, 255 } } },
    { { { 32, -31, 0 }, 0, { 63 << 5, 0 << 5 }, { 255, 255, 255, 255 } } },
    { { { -31, 32, 0 }, 0, { 0 << 5, 63 << 5 }, { 255, 255, 255, 255 } } },
    { { { 32, 32, 0 }, 0, { 63 << 5, 63 << 5 }, { 255, 255, 255, 255 } } },
};

static Gfx sSunWholeDL[] = {
    gsSPMatrix(0x01000000, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW),
    gsDPPipeSync(),
    gsDPLoadTextureBlock_4b(HH_SUN_DAY, G_IM_FMT_I, 64, 64, 0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
                            6, 6, G_TX_NOLOD, G_TX_NOLOD),
    gsDPLoadMultiBlock_4b(HH_SUN_EVENING, 0x0100, 1, G_IM_FMT_I, 64, 64, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                          G_TX_NOMIRROR | G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD),
    gsSPVertex(sSunVtx, 4, 0),
    gsSP2Triangles(0, 1, 2, 0, 2, 1, 3, 0),
    gsSPEndDisplayList(),
};

/* The list to run for a G_DL to w1: the whole sun for gSunDL (unless MM_SUNWHOLE=0), else NULL. */
Gfx* hh_sun_dl_for(uintptr_t w1) {
    static int sOn = -1;
    if (w1 != HH_SUN_DL) {
        return NULL;
    }
    if (sOn < 0) {
        const char* e = getenv("MM_SUNWHOLE");
        sOn = !(e != NULL && e[0] == '0');
    }
    return sOn ? sSunWholeDL : NULL;
}
