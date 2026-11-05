/* hh_fx.c: The New Tetris's "fx" assets (06FFD0.c: the 3D backgrounds, their textures, cameras, lights, scenes and
 * animations) in the handheld's byte order.
 *
 * The game reads an asset straight from the inflated ROM data as C structs, then turns its offsets into pointers
 * (06FFD0.c's per-type functions). On the N64 that data is big-endian; here it is swapped to host order once, as the
 * asset is loaded (func_800AA14C, decomp.patch), before those functions run. Nearly every field is 32 bits (counts,
 * floats, offsets, display-list words, matrices), so each asset is swapped by 32-bit words, except the places known
 * to hold 16-bit fields (vertices, viewports, a few s16 pairs) or bytes (textures, vertex colours), which a per-type
 * walk marks first. Texture bytes stay in the N64's order: the renderer reads them so.
 *
 * Types (UnkStruct_* in include/newtetris/06FFD0.h; the ROM part starts at the struct's first non-header field):
 *   1 texture (UnkStruct_71): s16 x3, u8 x2, display list, image bytes
 *   2 model (UnkStruct_72): display lists, Vtx[], material list
 *   3 camera (UnkStruct_73): floats, Vp
 *   4 light (UnkStruct_74): floats
 *   5 scene (UnkStruct_75): node list (UnkStruct_49: u16, u8, u8, words), id lists, UnkStruct_68 (f32, s16, s16),
 *     an s16 list
 *   6 animation (UnkStruct_50/51/52/53): words, with s16 pairs in 52 and 53 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { U_W = 0, U_H, U_B }; /* per 2-byte unit: part of a 32-bit word, a halfword, two bytes */

static u8 *sD;
static s32 sN;
static u8 *sU; /* sN / 2 units */
static int sErr;

static u32 rd32(s32 o) { return (o >= 0 && o + 4 <= sN) ? ((u32)sD[o] << 24 | (u32)sD[o + 1] << 16 | (u32)sD[o + 2] << 8 | sD[o + 3]) : 0; }
static u32 rd16(s32 o) { return (o >= 0 && o + 2 <= sN) ? ((u32)sD[o] << 8 | sD[o + 1]) : 0; }

static void mark(s32 o, s32 n, int kind)
{
    s32 i;
    if (o < 0 || n < 0 || o + n > sN || (o & 1)) {
        if (sErr++ < 20)
            fprintf(stderr, "[fx] mark %d at 0x%x+0x%x outside 0x%x\n", kind, (unsigned)o, (unsigned)n, (unsigned)sN);
        return;
    }
    for (i = 0; i < n; i += 2)
        sU[(o + i) >> 1] = (u8)kind;
}

static void mark_vtx(s32 o, s32 n)
{
    s32 i;
    for (i = 0; i < n; i++) {
        mark(o + 16 * i, 12, U_H); /* ob[3], flag, tc[2] */
        mark(o + 16 * i + 12, 4, U_B); /* cn[4] */
    }
}

static void apply(void)
{
    s32 o;
    for (o = 0; o + 4 <= sN; o += 4) {
        u8 a = sU[o >> 1], b = sU[(o >> 1) + 1], t;
        u8 *p = sD + o;
        if (a == U_W && b == U_W) {
            t = p[0]; p[0] = p[3]; p[3] = t;
            t = p[1]; p[1] = p[2]; p[2] = t;
            continue;
        }
        if (a != U_B) { t = p[0]; p[0] = p[1]; p[1] = t; }
        if (b != U_B) { t = p[2]; p[2] = p[3]; p[3] = t; }
    }
    if (o + 2 <= sN && sU[o >> 1] != U_B) { /* a trailing halfword */
        u8 t = sD[o]; sD[o] = sD[o + 1]; sD[o + 1] = t;
    }
}

static void fx_texture(void)
{
    s32 img = (s32)rd32(12);
    mark(0, 6, U_H);
    mark(6, 2, U_B);
    if (img > 0 && img < sN)
        mark(img, (sN - img) & ~1, U_B);
}

static void fx_model(void)
{
    s32 vtx = (s32)rd32(0x10), nv = (s32)rd32(0x14);
    if (vtx > 0 && nv > 0)
        mark_vtx(vtx, nv);
    if (rd32(0x18) != 0 && sErr++ < 20)
        fprintf(stderr, "[fx] model with a UnkStruct_61 list (not swapped)\n");
}

static void fx_camera(void)
{
    s32 vp = (s32)rd32(0x18);
    if (vp > 0)
        mark(vp, 16, U_H);
}

static void fx_scene(void)
{
    s32 n49 = (s32)rd32(0x00), n68 = (s32)rd32(0x10), n38 = (s32)rd32(0x14);
    s32 o49 = (s32)rd32(0x18), o68 = (s32)rd32(0x34), o38 = (s32)rd32(0x38), i;
    for (i = 0; i < n49 && o49 > 0; i++) {
        mark(o49 + 0x40 * i, 2, U_H); /* u16 */
        mark(o49 + 0x40 * i + 2, 2, U_B); /* u8, u8 (count of the child list) */
    }
    for (i = 0; i < n68 && o68 > 0; i++)
        mark(o68 + 8 * i + 4, 4, U_H);
    if (o38 > 0 && n38 > 0)
        mark(o38, 2 * n38, U_H);
    if (rd32(0x1C) != 0 && sErr++ < 20)
        fprintf(stderr, "[fx] scene with a UnkStruct_54 block (not swapped)\n");
}

static void fx_anim(void)
{
    s32 n51 = (s32)rd32(0x04), o51 = (s32)rd32(0x08), i, j, k;
    for (i = 0; i < n51 && o51 > 0; i++) {
        s32 e = o51 + 0x24 * i, n52 = (s32)rd32(e + 0x18), o52 = (s32)rd32(e + 0x1C);
        for (j = 0; j < n52 && o52 > 0; j++) {
            s32 f = o52 + 0x14 * j, last = (s32)(s16)rd16(f + 4), o53 = (s32)rd32(f + 0xC);
            mark(f + 4, 4, U_H);
            for (k = 0; o53 > 0 && k < 100000; k++) {
                s32 g = o53 + 8 * k;
                if (g + 8 > sN)
                    break;
                mark(g, 4, U_H);
                if ((s32)(s16)rd16(g) == last)
                    break;
            }
        }
    }
}

/* data: the asset's ROM part (after the header the game keeps in front of it), n bytes, still big-endian */
void hh_fx_swap(int type, void *data, int n)
{
    sD = data;
    sN = n;
    sU = calloc((size_t)n / 2 + 2, 1);
    if (sU == NULL)
        return;
    switch (type) {
    case 1: fx_texture(); break;
    case 2: fx_model(); break;
    case 3: fx_camera(); break;
    case 4: break;
    case 5: fx_scene(); break;
    case 6: fx_anim(); break;
    default:
        if (sErr++ < 20)
            fprintf(stderr, "[fx] asset type %d: left big-endian\n", (int)type);
        free(sU);
        return;
    }
    apply();
    free(sU);
}
