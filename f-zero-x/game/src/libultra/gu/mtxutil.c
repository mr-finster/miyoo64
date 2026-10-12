/**************************************************************************
 *									  *
 *		 Copyright (C) 1994, Silicon Graphics, Inc.		  *
 *									  *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright law.  They  may  not be disclosed  *
 *  to  third  parties  or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *									  *
 **************************************************************************/

#include <PR/guint.h>

#ifdef HANDHELD
// The game writes and reads Mtx through its u16 i[4][4] / f[4][4] view (math.c,
// camera.c), whose halves sit in the other order inside each 32-bit word on a
// little-endian CPU. The port keeps that view as the one layout: gu writes it too,
// and the renderer reads it (gfx_pc.c gfx_sp_matrix).
void guMtxF2L(float mf[4][4], Mtx* m) {
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int e = FTOFIX32(mf[i][j]);
            m->u.i[i][j] = (u16) ((u32) e >> 16);
            m->u.f[i][j] = (u16) e;
        }
    }
}

void guMtxL2F(float mf[4][4], Mtx* m) {
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            mf[i][j] = (s32) (((u32) m->u.i[i][j] << 16) | m->u.f[i][j]) / 65536.0f;
        }
    }
}
#else
void guMtxF2L(float mf[4][4], Mtx* m) {
    int i, j;
    int e1, e2;
    int *ai, *af;

    ai = (int*) &m->m[0][0];
    af = (int*) &m->m[2][0];

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            e1 = FTOFIX32(mf[i][j * 2]);
            e2 = FTOFIX32(mf[i][j * 2 + 1]);
            *(ai++) = (e1 & 0xffff0000) | ((e2 >> 16) & 0xffff);
            *(af++) = ((e1 << 16) & 0xffff0000) | (e2 & 0xffff);
        }
    }
}

void guMtxL2F(float mf[4][4], Mtx* m) {
    int i, j;
    unsigned int e1, e2;
    unsigned int *ai, *af;
    int q1, q2;

    ai = (unsigned int*) &m->m[0][0];
    af = (unsigned int*) &m->m[2][0];

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            e1 = (*ai & 0xffff0000) | ((*af >> 16) & 0xffff);
            e2 = ((*(ai++) << 16) & 0xffff0000) | (*(af++) & 0xffff);
            q1 = *((int*) &e1);
            q2 = *((int*) &e2);

            mf[i][j * 2] = FIX32TOF(q1);
            mf[i][j * 2 + 1] = FIX32TOF(q2);
        }
    }
}
#endif

void guMtxIdentF(float mf[4][4]) {
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (i == j) {
                mf[i][j] = 1.0;
            } else {
                mf[i][j] = 0.0;
            }
        }
    }
}

void guMtxIdent(Mtx* m) {
    float mf[4][4];

    guMtxIdentF(mf);

    guMtxF2L(mf, m);
}
