/*
 * hh_interp.h -- frame interpolation (hh_interp.c)
 *
 * Copyright (c) 2026 cacuracaptors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef HH_INTERP_H
#define HH_INTERP_H

#include <stdint.h>

/* called by the game (the hooks in src/code, include/gfx.h has the prototypes too) */
void Port_FiStartRecord(void);
void Port_FiStopRecord(const void* tag);
void Port_FiOpenChild(const void* a, int b);
void Port_FiCloseChild(void);
void Port_FiActorPosRot(void);
void Port_FiPush(void);
void Port_FiPop(void);
void Port_FiMode(int mode);
void Port_FiSetTRYXZ(float x, float y, float z, const int16_t* rot, const float* cur);
void Port_FiToMtx(const float* cur, void* dest);
void Port_FiMtxF(const float* src, void* dest);
void Port_FiSkip(int d);
void Port_FiDontInterpolateCamera(void);
void Port_FiDontInterpolateFrame(void);
int Port_FiCameraEpoch(void);
void Port_FiCameraCheck(const float* eye, const float* at, const float* up, int pauseMode);
void Port_FiDontInterpolateChild(const void* key);
void Port_FiSkinVtx(const void* key, const void* buf, int count);
void Port_FiTexScroll(const void* cmd);
void Port_FiCmd(const void* cmd, int kind); /* 0 = G_SETPRIMCOLOR, 1 = G_TEXRECT (+ RDPHALF_1/2), 2 = G_FILLRECT / G_SETSCISSOR */

/* used by the renderer (gfx_pc.c) and the scheduler (hh_sched.c) */
#define FI_REPL_BITS 14
#define FI_REPL_HASH (1 << FI_REPL_BITS)
#define FI_REPL_MAX 12288
#define FI_SKIN_MAX 256   /* limbs with skin vertices a frame */
#define FI_SKIN_VTX 16384 /* skin vertices a frame */
#define FI_TILE_HASH 2048 /* scrolling textures a frame (a power of 2) */
#define FI_CMD_HASH 512   /* 2D commands swapped a frame (the dialog box...) */
typedef struct {
    int n;
    const void* key[FI_REPL_HASH];
    int idx[FI_REPL_HASH];
    float m[FI_REPL_MAX][16];
    const void* seen[FI_REPL_HASH]; /* diagnostics: every Mtx recorded this frame */
    /* blended skin vertices: a G_VTX reading [dest, dest + 16 * count) reads vtx + 16 * off instead */
    int nSkin;
    uintptr_t skinLo, skinHi;
    struct {
        uintptr_t dest;
        unsigned len, off;
    } skin[FI_SKIN_MAX];
    uint8_t vtx[FI_SKIN_VTX * 16];
    /* scrolling textures: the G_SETTILESIZE at tkey[i] uses tw0/tw1 instead */
    int nTile;
    const void* tkey[FI_TILE_HASH];
    uint32_t tw0[FI_TILE_HASH], tw1[FI_TILE_HASH];
    /* 2D commands (Port_FiCmd): the command at ckey[i] uses cw[i] instead */
    int nCmd;
    const void* ckey[FI_CMD_HASH];
    uint32_t cw[FI_CMD_HASH][4];
} FiRepl;

extern int port_fi_enabled;
int port_fi_pair(const void* tag, int* curSlot, int* prevSlot);
void port_fi_build(int curSlot, int prevSlot, float step, FiRepl* out);
const float* port_fi_lookup(const FiRepl* r, const void* dest);
const void* port_fi_vtx_lookup(const FiRepl* r, const void* src, unsigned bytes);
int port_fi_tile_lookup(const FiRepl* r, const void* cmd, uint32_t* w0, uint32_t* w1);
const uint32_t* port_fi_cmd_lookup(const FiRepl* r, const void* cmd);

/* diagnostics: what was left unblended */
typedef struct {
    unsigned recs, recBad, camCuts, builds, mtxCur, mtxRepl, mtxNoItem, adjMis, angSkip;
    unsigned chNew, mtxNew, chDont, mtxDont, skinCur, skinRepl, skinVtx, scrCur, scrRepl, scrSkip;
    unsigned ovf[6]; /* overflows: paths, items, matrices, trs, depth, hash */
    float cut[4][4]; /* up to 4 camera cuts: eyeD lookD upD dirD */
    int nCut;
    struct {
        const void* ka;
        int kb;
        int id;
        unsigned mtx, n;
    } top[24];
} FiDiag;
extern FiDiag port_fi_diag;
int port_fi_seen(const FiRepl* r, const void* dest); /* was this Mtx recorded this frame? */

/* The renderer's swaps for the frame being drawn (NULL: none; hh_sched.c sets it around a redraw, gfx_pc.c reads it) */
extern const FiRepl* gHhFiRepl;

#endif
