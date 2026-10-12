/* libultra's matrix routines that the decomp keeps as MIPS assembly (src/os/gu*.s), in C for the native port (M2).
 * The assembly converts with trunc.w.s (toward zero) and multiplies in single precision, as these casts and floats do;
 * guMtxCatF sums each product left to right from 0, as its unrolled loop does. The Mtx layout is the N64's: the
 * integer halves of the 16 fixed-point elements in the first 8 words, the fractions in the last 8, two elements to a
 * 32-bit word (the renderer reads it word by word, so it holds on a little-endian host). */
#include "ultra64.h"

#define FTOFIX32(x) ((s32) ((x) * (f32) 0x00010000))
#define FIX32TOF(x) ((f32) (x) * (1.0f / (f32) 0x00010000))

void guMtxIdentF(f32 mf[4][4]) {
    s32 i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            mf[i][j] = i == j ? 1.0f : 0.0f;
}

void guMtxF2L(f32 mf[4][4], Mtx *m) {
    u32 *ai = (u32 *) &m->m[0][0], *af = (u32 *) &m->m[2][0];
    s32 i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            const s32 e1 = FTOFIX32(mf[i][j * 2]), e2 = FTOFIX32(mf[i][j * 2 + 1]);
            *ai++ = ((u32) e1 & 0xFFFF0000u) | (((u32) e2 >> 16) & 0xFFFFu);
            *af++ = (((u32) e1 << 16) & 0xFFFF0000u) | ((u32) e2 & 0xFFFFu);
        }
    }
}

void guMtxL2F(f32 mf[4][4], Mtx *m) {
    u32 *ai = (u32 *) &m->m[0][0], *af = (u32 *) &m->m[2][0];
    s32 i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            const u32 e1 = (*ai & 0xFFFF0000u) | ((*af >> 16) & 0xFFFFu);
            const u32 e2 = ((*ai << 16) & 0xFFFF0000u) | (*af & 0xFFFFu);
            ai++, af++;
            mf[i][j * 2] = FIX32TOF((s32) e1);
            mf[i][j * 2 + 1] = FIX32TOF((s32) e2);
        }
    }
}

void guMtxIdent(Mtx *m) {
    f32 mf[4][4];
    guMtxIdentF(mf);
    guMtxF2L(mf, m);
}

void guMtxCatF(f32 mf[4][4], f32 nf[4][4], f32 res[4][4]) {
    f32 temp[4][4];
    s32 i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            f32 sum = 0.0f;
            for (k = 0; k < 4; k++)
                sum += mf[i][k] * nf[k][j];
            temp[i][j] = sum;
        }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            res[i][j] = temp[i][j];
}

void guScaleF(f32 mf[4][4], f32 x, f32 y, f32 z) {
    guMtxIdentF(mf);
    mf[0][0] = x;
    mf[1][1] = y;
    mf[2][2] = z;
    mf[3][3] = 1.0f;
}

void guScale(Mtx *m, f32 x, f32 y, f32 z) {
    f32 mf[4][4];
    guScaleF(mf, x, y, z);
    guMtxF2L(mf, m);
}

void guTranslateF(f32 mf[4][4], f32 x, f32 y, f32 z) {
    guMtxIdentF(mf);
    mf[3][0] = x;
    mf[3][1] = y;
    mf[3][2] = z;
}

void guTranslate(Mtx *m, f32 x, f32 y, f32 z) {
    f32 mf[4][4];
    guTranslateF(mf, x, y, z);
    guMtxF2L(mf, m);
}

void guNormalize(f32 *x, f32 *y, f32 *z) {
    const f32 m = 1.0f / __builtin_sqrtf(*x * *x + *y * *y + *z * *z);
    *x *= m;
    *y *= m;
    *z *= m;
}
