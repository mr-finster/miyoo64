/*
 * hh_interp.c -- frame interpolation: Ocarina of Time's 20 game updates a second shown at 30 frames a second.
 * Written for the Miyoo Mini port (cacuracaptors/oot-miyoomini, MIT) after Ship of Harkinian's approach
 * (soh/frame_interpolation.cpp); this code is the port's own.
 *
 * While the game builds a frame's display list (Play_Draw), every matrix that becomes a Mtx is reported here
 * (Matrix_ToMtx, Matrix_MtxFToMtx, SkinMatrix_MtxFToMtx), filed in a tree: each actor, each OPEN_DISPS block (file,
 * line), the camera, each effect and each light halo opens a child with a key. With the last frame's recording and
 * this one's, each Mtx of this frame is matched with the same one of the last (the same place in the tree), and an
 * in-between matrix is computed; the renderer draws this frame's display list again with those Mtx swapped in
 * (gfx_pc.c). As in SoH only the FINAL matrices are blended; an actor's are kept relative to its position and
 * rotation (Actor_Draw's Matrix_SetTranslateRotateYXZ), which are blended as numbers, so a fast turn doesn't
 * squash the model. Also blended: the skinned models' CPU vertices (Epona), the scrolling textures' tile sizes,
 * the dialog box's colour and rectangle, the letterbox bars and their scissor.
 *
 * Recording runs on the game thread inside Play_Draw; blending runs on the same thread later, from closed
 * recordings (3 in turn). All of it lies outside the save states (SAVESTATE_EXCLUDE); a state load marks the next
 * frame as not to be mixed (gfx_savestate_fixup).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hh_interp.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE */

/* the game's math.h (include/libc) has no lrintf / fminf / fmaxf / fabsf */
static inline int fi_round(float x) {
    return (int)(x >= 0.0f ? x + 0.5f : x - 0.5f);
}
static inline float fi_abs(float x) {
    return x < 0.0f ? -x : x;
}
static inline float fi_clamp(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

#define FI_MAX_PATHS 8192
#define FI_MAX_ITEMS 32768
#define FI_MAX_MTX 12288
#define FI_MAX_TRS 2048
#define FI_HASH 32768
#define FI_DEPTH 96
#define FI_REC_SKIN 256
#define FI_MAX_SCROLL 1024
#define FI_MAX_CMD 256
#define FI_REC_SKINVTX 16384

enum { IT_CHILD, IT_TRS, IT_TOMTX, IT_MTXF, IT_SCROLL, IT_CMD };

typedef struct {
    const void* ka;
    int kb, occ;
    int parent;
    int first, last;
    int ord[6];
    int id;
} FiPath;

typedef struct {
    uint8_t type;
    int ord;
    int data;
    int next;
} FiItem;

typedef struct {
    float x, y, z;
    int16_t rot[3];
    uint8_t hasMtx;
} FiTrs;

typedef struct {
    const void* dest;
    float m[16];
    uint8_t adjusted;
} FiMtx;

typedef struct {
    uint32_t gen;
    int path;
    const void* ka;
    int kb, occ;
    int val;
} FiHashE;

typedef struct {
    uint32_t id;
    int valid;
    int used;
    int noInterp;
    int rate;
    const void* tag;
    uint32_t camEpoch;
    int nPaths, nItems, nMtx, nTrs;
    uint32_t gen;
    FiPath paths[FI_MAX_PATHS];
    FiItem items[FI_MAX_ITEMS];
    FiMtx mtx[FI_MAX_MTX];
    FiTrs trs[FI_MAX_TRS];
    FiHashE hChild[FI_HASH];
    FiHashE hItem[FI_HASH];
    FiHashE hCount[FI_HASH];

    int nSkin, nSkinVtx;
    struct {
        const void* key;
        const void* dest;
        int count, off;
    } skin[FI_REC_SKIN];
    uint8_t skinVtx[FI_REC_SKINVTX * 16];

    int nScroll;
    struct {
        const void* cmd;
        uint32_t w0, w1;
    } scroll[FI_MAX_SCROLL];

    int nCmd;
    struct {
        const void* cmd;
        int kind;
        uint32_t w[4];
    } cmds[FI_MAX_CMD];
} FiRec;

#define FI_SLOTS 3

static FiRec* g_rec SAVESTATE_EXCLUDE;
static FiRec* g_cur SAVESTATE_EXCLUDE;
static uint32_t g_recId SAVESTATE_EXCLUDE;
static int g_dontFrame SAVESTATE_EXCLUDE;
static int g_rate SAVESTATE_EXCLUDE = 3;

void Port_FiRate(int rate) {
    g_rate = rate;
}
static int g_stack[FI_DEPTH] SAVESTATE_EXCLUDE;
static int g_sp SAVESTATE_EXCLUDE;
static int g_overflow SAVESTATE_EXCLUDE;
FiDiag port_fi_diag SAVESTATE_EXCLUDE;
const FiRepl* gHhFiRepl SAVESTATE_EXCLUDE; /* the renderer's swaps for the frame being drawn (hh_sched.c sets it) */
#define OVF(k) (g_overflow = 1, port_fi_diag.ovf[k]++)

int port_fi_enabled SAVESTATE_EXCLUDE = 0; /* OOT_FPS30 / fps30 in debug.cfg (hh_sched.c) */
static int g_fiSkip SAVESTATE_EXCLUDE;

static int g_nextActor SAVESTATE_EXCLUDE;
static int g_hasInvActor SAVESTATE_EXCLUDE;
static float g_invActor[16] SAVESTATE_EXCLUDE;
static int g_invActorDepth SAVESTATE_EXCLUDE;
static int g_mtxDepth SAVESTATE_EXCLUDE;
static int g_newMtxDepth SAVESTATE_EXCLUDE = -1;

static uint32_t g_camEpoch SAVESTATE_EXCLUDE, g_prevCamEpoch SAVESTATE_EXCLUDE;

#define FI_DONT 1024
static struct {
    const void* key;
    uint32_t rec0, rec1;
} g_dont[FI_DONT] SAVESTATE_EXCLUDE;

static uint32_t hkey(int path, const void* ka, int kb, int occ) {
    uint32_t h = (uint32_t)path * 0x9E3779B1u;
    h ^= (uint32_t)(uintptr_t)ka * 0x85EBCA77u;
    h ^= (uint32_t)kb * 0xC2B2AE3Du;
    h ^= (uint32_t)occ * 0x27D4EB2Fu;
    h ^= h >> 15;
    return h & (FI_HASH - 1);
}

static FiHashE* hfind(FiHashE* t, uint32_t gen, int path, const void* ka, int kb, int occ, int create) {
    uint32_t i = hkey(path, ka, kb, occ);
    int n;
    for (n = 0; n < FI_HASH; n++, i = (i + 1) & (FI_HASH - 1)) {
        FiHashE* e = &t[i];
        if (e->gen != gen) {
            if (!create) {
                return NULL;
            }
            e->gen = gen;
            e->path = path;
            e->ka = ka;
            e->kb = kb;
            e->occ = occ;
            e->val = 0;
            return e;
        }
        if (e->path == path && e->ka == ka && e->kb == kb && e->occ == occ) {
            return e;
        }
    }
    return NULL;
}

static int new_path(FiRec* r, int parent, const void* ka, int kb, int occ) {
    FiPath* p;
    if (r->nPaths >= FI_MAX_PATHS) {
        OVF(0);
        return -1;
    }
    p = &r->paths[r->nPaths];
    memset(p, 0, sizeof *p);
    p->ka = ka;
    p->kb = kb;
    p->occ = occ;
    p->parent = parent;
    p->first = p->last = -1;
    return r->nPaths++;
}

static FiItem* add_item(FiRec* r, int type, int data) {
    int pi = g_stack[g_sp - 1];
    FiPath* p;
    FiItem* it;
    FiHashE* e;
    if (pi < 0 || r->nItems >= FI_MAX_ITEMS) {
        OVF(1);
        return NULL;
    }
    p = &r->paths[pi];
    it = &r->items[r->nItems];
    it->type = (uint8_t)type;
    it->ord = p->ord[type]++;
    it->data = data;
    it->next = -1;
    if (p->last >= 0) {
        r->items[p->last].next = r->nItems;
    } else {
        p->first = r->nItems;
    }
    p->last = r->nItems;
    r->nItems++;
    if (type != IT_CHILD) {
        e = hfind(r->hItem, r->gen, pi, NULL, type, it->ord, 1);
        if (e == NULL) {
            OVF(5);
            return NULL;
        }
        e->val = data;
    }
    return it;
}

void Port_FiStartRecord(void) {
    FiRec* r;
    if (!port_fi_enabled) {
        g_cur = NULL;
        return;
    }
    if (g_rec == NULL) {
        g_rec = (FiRec*)calloc(FI_SLOTS, sizeof(FiRec));
        if (g_rec == NULL) {
            fprintf(stderr, "interp: no memory for the recordings (%u KB): 30 fps off\n", (unsigned)(FI_SLOTS * sizeof(FiRec) / 1024));
            port_fi_enabled = 0;
            g_cur = NULL;
            return;
        }
    }
    g_recId++;
    r = &g_rec[g_recId % FI_SLOTS];
    r->valid = 0;
    r->used = 0;
    r->rate = g_rate;
    r->noInterp = __atomic_exchange_n(&g_dontFrame, 0, __ATOMIC_ACQ_REL);
    r->id = g_recId;
    r->nPaths = r->nItems = r->nMtx = r->nTrs = 0;
    r->nSkin = r->nSkinVtx = 0;
    r->nScroll = 0;
    r->nCmd = 0;
    r->gen++;
    if (r->gen == 0) {
        memset(r->hChild, 0, sizeof r->hChild);
        memset(r->hItem, 0, sizeof r->hItem);
        memset(r->hCount, 0, sizeof r->hCount);
        r->gen = 1;
    }
    g_overflow = 0;
    g_sp = 0;
    g_stack[g_sp++] = new_path(r, -1, NULL, 0, 0);
    g_mtxDepth = 0;
    g_newMtxDepth = -1;
    g_hasInvActor = 0;
    g_nextActor = 0;
    g_fiSkip = 0;
    g_cur = r;
}

void Port_FiStopRecord(const void* tag) {
    FiRec* r = g_cur;
    g_prevCamEpoch = g_camEpoch;
    if (r == NULL) {
        return;
    }
    r->tag = tag;
    r->camEpoch = g_camEpoch;
    port_fi_diag.recs++;
    port_fi_diag.recBad += g_overflow != 0;
    __atomic_store_n(&r->valid, !g_overflow, __ATOMIC_RELEASE);
    g_cur = NULL;
}

void Port_FiOpenChild(const void* a, int b) {
    FiRec* r = g_cur;
    FiHashE* cnt;
    FiItem* it;
    int parent, child, occ;
    if (r == NULL) {
        return;
    }
    if (g_sp >= FI_DEPTH) {

        OVF(4);
        g_cur = NULL;
        return;
    }
    parent = g_stack[g_sp - 1];
    if (parent < 0) {
        g_stack[g_sp++] = -1;
        return;
    }
    cnt = hfind(r->hCount, r->gen, parent, a, b, -1, 1);
    occ = cnt ? cnt->val++ : 0;
    child = new_path(r, parent, a, b, occ);
    if (child >= 0) {
        r->paths[child].id = (a != NULL && b == 0) ? *(const int16_t*)a : -1;
        FiHashE* e = hfind(r->hChild, r->gen, parent, a, b, occ, 1);
        if (e != NULL) {
            e->val = child;
        }
        it = add_item(r, IT_CHILD, child);
        (void)it;
    }
    g_stack[g_sp++] = child;
}

void Port_FiCloseChild(void) {
    if (g_cur == NULL || g_sp <= 1) {
        return;
    }
    if (g_hasInvActor && g_sp == g_invActorDepth) {
        g_hasInvActor = 0;
        g_newMtxDepth = -1;
    }
    g_sp--;
}

void Port_FiActorPosRot(void) {
    if (g_cur != NULL) {
        g_nextActor = 1;
    }
}

void Port_FiPush(void) {
    if (g_cur != NULL) {
        g_mtxDepth++;
    }
}

void Port_FiPop(void) {
    if (g_cur != NULL) {
        g_mtxDepth--;
        if (g_mtxDepth < g_newMtxDepth) {
            g_newMtxDepth = -1;
        }
    }
}

void Port_FiMode(int mode) {
    if (g_cur != NULL && mode == 0 && g_newMtxDepth < 0) {
        g_newMtxDepth = g_mtxDepth;
    }
}

static int invert4(const float* m, float* out);
typedef struct FiCtx FiCtx;
static void skin_build(FiCtx* c);

void Port_FiSetTRYXZ(float x, float y, float z, const int16_t* rot, const float* cur) {
    FiRec* r = g_cur;
    FiTrs* t;
    if (r == NULL) {
        return;
    }
    if (r->nTrs >= FI_MAX_TRS) {
        OVF(3);
        return;
    }
    t = &r->trs[r->nTrs];
    t->x = x;
    t->y = y;
    t->z = z;
    t->rot[0] = rot[0];
    t->rot[1] = rot[1];
    t->rot[2] = rot[2];
    t->hasMtx = 0;
    if (g_nextActor) {
        t->hasMtx = 1;
        g_hasInvActor = invert4(cur, g_invActor);
        g_nextActor = 0;
        g_invActorDepth = g_sp;
        g_newMtxDepth = -1;
    } else {
        Port_FiMode(0);
    }
    add_item(r, IT_TRS, r->nTrs++);
}

static void mtxf_mult(const float* a, const float* b, float* d) {
    float t[16];
    int r, c, k;
    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) {
                s += a[k * 4 + r] * b[c * 4 + k];
            }
            t[c * 4 + r] = s;
        }
    }
    memcpy(d, t, sizeof t);
}

void Port_FiToMtx(const float* cur, void* dest) {
    FiRec* r = g_cur;
    FiMtx* m;
    if (r == NULL) {
        return;
    }
    if (r->nMtx >= FI_MAX_MTX) {
        OVF(2);
        return;
    }
    m = &r->mtx[r->nMtx];
    m->dest = dest;
    if (g_hasInvActor && g_newMtxDepth < 0) {
        m->adjusted = 1;
        mtxf_mult(g_invActor, cur, m->m);
    } else {
        m->adjusted = 0;
        memcpy(m->m, cur, sizeof m->m);
    }
    add_item(r, IT_TOMTX, r->nMtx++);
}

void Port_FiMtxF(const float* src, void* dest) {
    FiRec* r = g_cur;
    FiMtx* m;
    if (r == NULL || g_fiSkip > 0) {
        return;
    }
    if (r->nMtx >= FI_MAX_MTX) {
        OVF(2);
        return;
    }
    m = &r->mtx[r->nMtx];
    m->dest = dest;
    m->adjusted = 0;
    memcpy(m->m, src, sizeof m->m);
    add_item(r, IT_MTXF, r->nMtx++);
}

void Port_FiSkinVtx(const void* key, const void* buf, int count) {
    FiRec* r = g_cur;
    if (r == NULL || buf == NULL || count <= 0) {
        return;
    }
    if (r->nSkin >= FI_REC_SKIN || r->nSkinVtx + count > FI_REC_SKINVTX) {
        return;
    }
    r->skin[r->nSkin].key = key;
    r->skin[r->nSkin].dest = buf;
    r->skin[r->nSkin].count = count;
    r->skin[r->nSkin].off = r->nSkinVtx;
    memcpy(r->skinVtx + (size_t)r->nSkinVtx * 16u, buf, (size_t)count * 16u);
    r->nSkin++;
    r->nSkinVtx += count;
}

void Port_FiTexScroll(const void* cmd) {
    FiRec* r = g_cur;
    const uint32_t* w = (const uint32_t*)cmd;
    if (r == NULL || g_fiSkip > 0 || r->nScroll >= FI_MAX_SCROLL) {
        return;
    }
    r->scroll[r->nScroll].cmd = cmd;
    r->scroll[r->nScroll].w0 = w[0];
    r->scroll[r->nScroll].w1 = w[1];
    add_item(r, IT_SCROLL, r->nScroll++);
}

void Port_FiCmd(const void* cmd, int kind) {
    FiRec* r = g_cur;
    const uint32_t* w = (const uint32_t*)cmd;
    if (r == NULL || g_fiSkip > 0 || r->nCmd >= FI_MAX_CMD) {
        return;
    }
    r->cmds[r->nCmd].cmd = cmd;
    r->cmds[r->nCmd].kind = kind;
    r->cmds[r->nCmd].w[0] = w[0];
    r->cmds[r->nCmd].w[1] = w[1];

    r->cmds[r->nCmd].w[2] = kind == 1 ? w[3] : 0;
    r->cmds[r->nCmd].w[3] = kind == 1 ? w[5] : 0;
    add_item(r, IT_CMD, r->nCmd++);
}

void Port_FiSkip(int d) {
    g_fiSkip += d;
}

void Port_FiDontInterpolateFrame(void) {
    __atomic_store_n(&g_dontFrame, 1, __ATOMIC_RELEASE);
}

void Port_FiDontInterpolateCamera(void) {
    if (getenv("OOT_FIDEBUG") != NULL) {
        fprintf(stderr, "interp: camera cut (rec %u)\n", (unsigned)g_recId);
    }
    g_camEpoch = g_prevCamEpoch + 1;
    port_fi_diag.camCuts++;
}

int Port_FiCameraEpoch(void) {
    return (int)g_camEpoch;
}

void Port_FiCameraCheck(const float* eye, const float* at, const float* up, int pauseMode) {
    static float oe[3] SAVESTATE_EXCLUDE, oa[3] SAVESTATE_EXCLUDE, ou[3] SAVESTATE_EXCLUDE;
    static int have SAVESTATE_EXCLUDE;
    float d[3], od[3], dl, odl, eyeD, lookD, upD, dirD;
    int k, dont = 0;
    if (g_cur == NULL) {
        return;
    }
    if (have) {
        for (k = 0; k < 3; k++) {
            d[k] = eye[k] - at[k];
            od[k] = oe[k] - oa[k];
        }
        dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        odl = sqrtf(od[0] * od[0] + od[1] * od[1] + od[2] * od[2]);
        eyeD = lookD = upD = dirD = 0.0f;
        for (k = 0; k < 3; k++) {
            float a = dl > 0 ? d[k] / dl : 0, b = odl > 0 ? od[k] / odl : 0;
            eyeD += (eye[k] - oe[k]) * (eye[k] - oe[k]);
            lookD += (at[k] - oa[k]) * (at[k] - oa[k]);
            upD += (up[k] - ou[k]) * (up[k] - ou[k]);
            dirD += (a - b) * (a - b);
        }
        eyeD = sqrtf(eyeD);
        lookD = sqrtf(lookD);
        upD = sqrtf(upD);
        dirD = sqrtf(dirD);
        if (upD < 0.01f && dirD < 0.01f) {
            dont = eyeD + lookD > 300.0f;
        } else {
            dont = eyeD >= 400.0f || lookD >= 100.0f || upD >= 1.5f || (dirD >= 1.414f && lookD >= 15.0f) ||
                   (dirD >= 1.414f && upD >= 0.31f && lookD >= 1.0f && eyeD >= 300.0f) ||
                   (dirD >= 0.5f && upD >= 0.31f && lookD >= 3.0f && eyeD >= 170.0f) || (lookD >= 52.0f && eyeD >= 52.0f) ||
                   (lookD >= 30.0f && eyeD >= 90.0f);
        }
        if (dont && pauseMode == 0) {
            if (port_fi_diag.nCut < 4) {
                float* cv = port_fi_diag.cut[port_fi_diag.nCut++];
                cv[0] = eyeD;
                cv[1] = lookD;
                cv[2] = upD;
                cv[3] = dirD;
            }
            Port_FiDontInterpolateCamera();
        }
    }
    memcpy(oe, eye, sizeof oe);
    memcpy(oa, at, sizeof oa);
    memcpy(ou, up, sizeof ou);
    have = 1;
}

void Port_FiDontInterpolateChild(const void* key) {
    uint32_t i = ((uint32_t)(uintptr_t)key * 0x9E3779B1u) >> 22;
    if (key == NULL) {
        return;
    }

    g_dont[i].rec0 = 0;
    g_dont[i].key = key;
    g_dont[i].rec1 = g_recId + 1;
    g_dont[i].rec0 = g_cur != NULL ? g_recId : g_recId + 1;
}

static int dont_interp(const void* key, uint32_t rec) {
    uint32_t i = ((uint32_t)(uintptr_t)key * 0x9E3779B1u) >> 22;
    return key != NULL && g_dont[i].key == key && rec >= g_dont[i].rec0 && rec <= g_dont[i].rec1;
}

extern float Math_SinS(int16_t angle);
extern float Math_CosS(int16_t angle);

struct FiCtx {
    float step, w;
    int diag;
    const FiRec *o, *n;
    float actor[16];
    FiRepl* out;
};

static int16_t interp_angle(const FiCtx* c, int16_t os, int16_t ns) {
    int o = (uint16_t)os, n = (uint16_t)ns, diff;
    if (os == ns) {
        return ns;
    }
    diff = o - n;
    if (!(-0x8000 <= diff && diff <= 0x8000)) {
        if (o < n) {
            o += 0x10000;
        } else {
            n += 0x10000;
        }
        diff = o - n;
    }
    if (diff < -0x4000 || diff > 0x4000) {
        port_fi_diag.angSkip++;
        return ns;
    }
    return (int16_t)(uint16_t)(int)(c->w * o + c->step * n);
}

static void set_trs(float* m, float tx, float ty, float tz, const int16_t* rot) {

    float t1 = Math_SinS(rot[1]), t2 = Math_CosS(rot[1]), s, co;
    m[0] = t2;
    m[2] = -t1;
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
    if (rot[0] != 0) {
        s = Math_SinS(rot[0]);
        co = Math_CosS(rot[0]);
        m[10] = t2 * co;
        m[6] = t2 * s;
        m[8] = t1 * co;
        m[4] = t1 * s;
        m[9] = -s;
        m[5] = co;
    } else {
        m[10] = t2;
        m[8] = t1;
        m[9] = 0.0f;
        m[6] = 0.0f;
        m[4] = 0.0f;
        m[5] = 1.0f;
    }
    if (rot[2] != 0) {
        float a, b;
        s = Math_SinS(rot[2]);
        co = Math_CosS(rot[2]);
        a = m[0];
        b = m[4];
        m[0] = a * co + b * s;
        m[4] = b * co - a * s;
        a = m[2];
        b = m[6];
        m[2] = a * co + b * s;
        m[6] = b * co - a * s;
        b = m[5];
        m[1] = b * s;
        m[5] = b * co;
    } else {
        m[1] = 0.0f;
    }
}

static void lerp16(const FiCtx* c, const float* a, const float* b, float* d) {
    int i;
    for (i = 0; i < 16; i++) {
        d[i] = c->w * a[i] + c->step * b[i];
    }
}

static void repl_put(FiRepl* r, const void* dest, const float* m) {
    uint32_t i;
    int n;
    if (dest == NULL || r->n >= FI_REPL_MAX) {
        return;
    }
    i = ((uint32_t)(uintptr_t)dest * 0x9E3779B1u) >> (32 - FI_REPL_BITS);
    for (n = 0; n < FI_REPL_HASH; n++, i = (i + 1) & (FI_REPL_HASH - 1)) {
        if (r->key[i] == NULL || r->key[i] == dest) {
            if (r->key[i] == NULL) {
                r->key[i] = dest;
                r->idx[i] = r->n++;
            }
            memcpy(r->m[r->idx[i]], m, sizeof r->m[0]);
            return;
        }
    }
}

const float* port_fi_lookup(const FiRepl* r, const void* dest) {
    uint32_t i;
    int n;
    if (r == NULL || r->n == 0) {
        return NULL;
    }
    i = ((uint32_t)(uintptr_t)dest * 0x9E3779B1u) >> (32 - FI_REPL_BITS);
    for (n = 0; n < FI_REPL_HASH; n++, i = (i + 1) & (FI_REPL_HASH - 1)) {
        if (r->key[i] == dest) {
            return r->m[r->idx[i]];
        }
        if (r->key[i] == NULL) {
            return NULL;
        }
    }
    return NULL;
}

static unsigned count_mtx(const FiRec* n, int np) {
    unsigned k = 0;
    int ii;
    for (ii = n->paths[np].first; ii >= 0; ii = n->items[ii].next) {
        const FiItem* it = &n->items[ii];
        if (it->type == IT_CHILD) {
            k += count_mtx(n, it->data);
        } else if (it->type == IT_TOMTX || it->type == IT_MTXF) {
            k++;
        }
    }
    return k;
}

static void diag_top(const FiPath* p, unsigned m) {
    FiDiag* d = &port_fi_diag;
    int i, lo = 0;
    for (i = 0; i < 24; i++) {
        if (d->top[i].n != 0 && d->top[i].ka == p->ka && d->top[i].kb == p->kb) {
            d->top[i].mtx += m;
            d->top[i].n++;
            return;
        }
        if (d->top[i].mtx < d->top[lo].mtx) {
            lo = i;
        }
    }
    if (d->top[lo].n == 0 || d->top[lo].mtx < m) {
        d->top[lo].ka = p->ka;
        d->top[lo].kb = p->kb;
        d->top[lo].id = p->id;
        d->top[lo].mtx = m;
        d->top[lo].n = 1;
    }
}

static int scroll_coord(const FiCtx* c, int o, int n) {
    int p = 4, d = n - o, best;
    const int m = o > n ? o : n;
    while (p <= m) {
        p <<= 1;
    }
    best = d;
    if (abs(d - p) < abs(best)) {
        best = d - p;
    }
    if (abs(d + p) < abs(best)) {
        best = d + p;
    }
    if (abs(best) > p / 4) {
        return -1;
    }
    {
        int v = o + (int)fi_round(c->step * (float)best);
        v %= p;
        if (v < 0) {
            v += p;
        }
        return v;
    }
}

static void scroll_put(FiCtx* c, const void* os, const void* ns) {
    typedef struct {
        const void* cmd;
        uint32_t w0, w1;
    } Sc;
    const Sc *a = (const Sc*)os, *b = (const Sc*)ns;
    FiRepl* out = c->out;

    const int aus = (int)((a->w0 >> 12) & 0xFFF), aut = (int)(a->w0 & 0xFFF);
    const int bus = (int)((b->w0 >> 12) & 0xFFF), but = (int)(b->w0 & 0xFFF);
    const int bls = (int)((b->w1 >> 12) & 0xFFF), blt = (int)(b->w1 & 0xFFF);
    int us, ut;
    uint32_t i;
    if (c->diag) {
        port_fi_diag.scrCur++;
    }
    if (a->w0 == b->w0 && a->w1 == b->w1) {
        return;
    }
    if ((a->w1 >> 24) != (b->w1 >> 24) || ((a->w1 >> 12) & 0xFFF) - aus != (uint32_t)(bls - bus) ||
        (a->w1 & 0xFFF) - aut != (uint32_t)(blt - but)) {
        port_fi_diag.scrSkip += c->diag;
        return;
    }
    us = scroll_coord(c, aus, bus);
    ut = scroll_coord(c, aut, but);
    if (us < 0 || ut < 0 || us + (bls - bus) > 0xFFF || ut + (blt - but) > 0xFFF) {
        port_fi_diag.scrSkip += c->diag;
        return;
    }
    i = ((uint32_t)(uintptr_t)b->cmd * 0x9E3779B1u) >> (32 - 11);
    for (;;) {
        if (out->tkey[i] == NULL || out->tkey[i] == b->cmd) {
            break;
        }
        i = (i + 1) & (FI_TILE_HASH - 1);
    }
    if (out->tkey[i] == NULL) {
        if (out->nTile >= FI_TILE_HASH / 2) {
            return;
        }
        out->nTile++;
    }
    out->tkey[i] = b->cmd;
    out->tw0[i] = (b->w0 & 0xFF000000u) | ((uint32_t)us << 12) | (uint32_t)ut;
    out->tw1[i] = (b->w1 & 0xFF000000u) | ((uint32_t)(us + bls - bus) << 12) | (uint32_t)(ut + blt - but);
    port_fi_diag.scrRepl += c->diag;
}

int port_fi_tile_lookup(const FiRepl* r, const void* cmd, uint32_t* w0, uint32_t* w1) {
    uint32_t i;
    if (r == NULL || r->nTile == 0) {
        return 0;
    }
    i = ((uint32_t)(uintptr_t)cmd * 0x9E3779B1u) >> (32 - 11);
    while (r->tkey[i] != NULL) {
        if (r->tkey[i] == cmd) {
            *w0 = r->tw0[i];
            *w1 = r->tw1[i];
            return 1;
        }
        i = (i + 1) & (FI_TILE_HASH - 1);
    }
    return 0;
}

static uint32_t mix_field(const FiCtx* c, uint32_t a, uint32_t b, int sh, int bits, int sgn) {
    const uint32_t m = (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u));
    int va = (int)((a >> sh) & m), vb = (int)((b >> sh) & m), v;
    if (sgn && bits < 32) {
        va = (va ^ (1 << (bits - 1))) - (1 << (bits - 1));
        vb = (vb ^ (1 << (bits - 1))) - (1 << (bits - 1));
    }
    v = (int)fi_round(c->w * (float)va + c->step * (float)vb);
    return ((uint32_t)v & m) << sh;
}

static void cmd_put(FiCtx* c, const void* os, const void* ns) {
    typedef struct {
        const void* cmd;
        int kind;
        uint32_t w[4];
    } Cm;
    const Cm *a = (const Cm*)os, *b = (const Cm*)ns;
    FiRepl* out = c->out;
    uint32_t w[4], i;
    if (a->kind != b->kind || (a->w[0] >> 24) != (b->w[0] >> 24)) {
        return;
    }
    if (a->w[0] == b->w[0] && a->w[1] == b->w[1] && a->w[2] == b->w[2] && a->w[3] == b->w[3]) {
        return;
    }
    if (b->kind == 0) {

        w[0] = b->w[0];
        w[1] = mix_field(c, a->w[1], b->w[1], 24, 8, 0) | mix_field(c, a->w[1], b->w[1], 16, 8, 0) |
               mix_field(c, a->w[1], b->w[1], 8, 8, 0) | mix_field(c, a->w[1], b->w[1], 0, 8, 0);
        w[2] = w[3] = 0;
    } else {

        if ((a->w[1] >> 24) != (b->w[1] >> 24)) {
            return;
        }
        w[0] = (b->w[0] & 0xFF000000u) | mix_field(c, a->w[0], b->w[0], 12, 12, 0) | mix_field(c, a->w[0], b->w[0], 0, 12, 0);
        w[1] = (b->w[1] & 0xFF000000u) | mix_field(c, a->w[1], b->w[1], 12, 12, 0) | mix_field(c, a->w[1], b->w[1], 0, 12, 0);
        w[2] = mix_field(c, a->w[2], b->w[2], 16, 16, 1) | mix_field(c, a->w[2], b->w[2], 0, 16, 1);
        w[3] = mix_field(c, a->w[3], b->w[3], 16, 16, 1) | mix_field(c, a->w[3], b->w[3], 0, 16, 1);
    }
    i = ((uint32_t)(uintptr_t)b->cmd * 0x9E3779B1u) >> (32 - 9);
    while (out->ckey[i] != NULL && out->ckey[i] != b->cmd) {
        i = (i + 1) & (FI_CMD_HASH - 1);
    }
    if (out->ckey[i] == NULL) {
        if (out->nCmd >= FI_CMD_HASH / 2) {
            return;
        }
        out->nCmd++;
    }
    out->ckey[i] = b->cmd;
    memcpy(out->cw[i], w, sizeof w);
}

const uint32_t* port_fi_cmd_lookup(const FiRepl* r, const void* cmd) {
    uint32_t i;
    if (r == NULL || r->nCmd == 0) {
        return NULL;
    }
    i = ((uint32_t)(uintptr_t)cmd * 0x9E3779B1u) >> (32 - 9);
    while (r->ckey[i] != NULL) {
        if (r->ckey[i] == cmd) {
            return r->cw[i];
        }
        i = (i + 1) & (FI_CMD_HASH - 1);
    }
    return NULL;
}

static void walk(FiCtx* c, int op, int np) {
    const FiRec *o = c->o, *n = c->n;
    int ii;
    for (ii = n->paths[np].first; ii >= 0; ii = n->items[ii].next) {
        const FiItem* it = &n->items[ii];
        const FiHashE* e;
        if (it->type == IT_CHILD) {
            const FiPath* ch = &n->paths[it->data];
            if (op >= 0 && !dont_interp(ch->ka, n->id)) {

                e = hfind((FiHashE*)o->hChild, o->gen, op, ch->ka, ch->kb, ch->occ, 0);
                if (e != NULL) {
                    walk(c, e->val, it->data);
                } else if (c->diag) {
                    unsigned m = count_mtx(n, it->data);
                    port_fi_diag.chNew++;
                    port_fi_diag.mtxNew += m;
                    if (m > 0) {
                        diag_top(ch, m);
                    }
                }
            } else if (op >= 0 && c->diag) {
                port_fi_diag.chDont++;
                port_fi_diag.mtxDont += count_mtx(n, it->data);
            }

            continue;
        }
        if (op < 0) {
            continue;
        }
        e = hfind((FiHashE*)o->hItem, o->gen, op, NULL, it->type, it->ord, 0);
        if (e == NULL) {
            port_fi_diag.mtxNoItem += c->diag && (it->type == IT_TOMTX || it->type == IT_MTXF);
            continue;
        }
        if (it->type == IT_SCROLL) {
            scroll_put(c, &o->scroll[e->val], &n->scroll[it->data]);
            continue;
        }
        if (it->type == IT_CMD) {
            cmd_put(c, &o->cmds[e->val], &n->cmds[it->data]);
            continue;
        }
        if (it->type == IT_TRS) {
            const FiTrs *to = &o->trs[e->val], *tn = &n->trs[it->data];
            if (to->hasMtx && tn->hasMtx) {
                int16_t r3[3];
                r3[0] = interp_angle(c, to->rot[0], tn->rot[0]);
                r3[1] = interp_angle(c, to->rot[1], tn->rot[1]);
                r3[2] = interp_angle(c, to->rot[2], tn->rot[2]);
                set_trs(c->actor, c->w * to->x + c->step * tn->x, c->w * to->y + c->step * tn->y,
                        c->w * to->z + c->step * tn->z, r3);
            }
        } else {
            const FiMtx *mo = &o->mtx[e->val], *mn = &n->mtx[it->data];
            float t[16];
            if (mo->adjusted && mn->adjusted) {
                lerp16(c, mo->m, mn->m, t);
                mtxf_mult(c->actor, t, t);
                repl_put(c->out, mn->dest, t);
            } else if (!mo->adjusted && !mn->adjusted) {
                lerp16(c, mo->m, mn->m, t);
                repl_put(c->out, mn->dest, t);
            } else {
                port_fi_diag.adjMis += c->diag;
            }
        }
    }
}

int port_fi_pair(const void* tag, int* curSlot, int* prevSlot) {
    int i, best = -1;
    uint32_t bid = 0;
    if (g_rec == NULL) {
        return 0;
    }
    uint32_t last = __atomic_load_n(&g_recId, __ATOMIC_ACQUIRE);
    for (i = 0; i < FI_SLOTS; i++) {
        const FiRec* r = &g_rec[i];

        if (__atomic_load_n(&r->valid, __ATOMIC_ACQUIRE) && !r->used && r->tag == tag && r->id + 1 >= last &&
            (best < 0 || r->id > bid)) {
            best = i;
            bid = r->id;
        }
    }
    if (best < 0) {
        return 0;
    }
    if (g_rec[best].rate != 3) {

        g_rec[best].used = 1;
        return 0;
    }
    if (g_rec[best].noInterp) {

        *curSlot = best;
        *prevSlot = best;
        g_rec[best].used = 1;
        return 1;
    }
    for (i = 0; i < FI_SLOTS; i++) {
        const FiRec* r = &g_rec[i];
        if (__atomic_load_n(&r->valid, __ATOMIC_ACQUIRE) && r->id + 1 == bid) {
            *curSlot = best;
            *prevSlot = r->rate == 3 ? i : best;
            g_rec[best].used = 1;
            return 1;
        }
    }
    return 0;
}

void port_fi_build(int curSlot, int prevSlot, float step, FiRepl* out) {
    FiCtx c;
    memset(out->key, 0, sizeof out->key);
    out->n = 0;
    c.step = step;
    c.w = 1.0f - step;
    c.o = &g_rec[prevSlot];
    c.n = &g_rec[curSlot];
    c.out = out;
    memset(c.actor, 0, sizeof c.actor);
    c.actor[0] = c.actor[5] = c.actor[10] = c.actor[15] = 1.0f;
    c.diag = step < 0.5f;
    memset(out->tkey, 0, sizeof out->tkey);
    out->nTile = 0;
    memset(out->ckey, 0, sizeof out->ckey);
    out->nCmd = 0;
    if (c.diag) {

        int i;
        memset(out->seen, 0, sizeof out->seen);
        for (i = 0; i < c.n->nMtx; i++) {
            const void* dest = c.n->mtx[i].dest;
            uint32_t h = ((uint32_t)(uintptr_t)dest * 0x9E3779B1u) >> (32 - FI_REPL_BITS);
            int k;
            for (k = 0; k < 64 && out->seen[h] != NULL && out->seen[h] != dest; k++) {
                h = (h + 1) & (FI_REPL_HASH - 1);
            }
            out->seen[h] = dest;
        }
        port_fi_diag.builds++;
        port_fi_diag.mtxCur += (unsigned)c.n->nMtx;
    }
    walk(&c, 0, 0);
    if (c.diag) {
        port_fi_diag.mtxRepl += (unsigned)out->n;
    }
    skin_build(&c);
}

static void skin_build(FiCtx* c) {
    const FiRec *o = c->o, *n = c->n;
    FiRepl* out = c->out;
    int i, j, k, used = 0, hint = 0;
    out->nSkin = 0;
    out->skinLo = ~(uintptr_t)0;
    out->skinHi = 0;
    for (i = 0; i < n->nSkin; i++) {
        const int cnt = n->skin[i].count;
        int pj = -1;

        for (k = 0; k < o->nSkin; k++) {
            j = (hint + k) % o->nSkin;
            if (o->skin[j].key == n->skin[i].key && o->skin[j].count == cnt) {
                pj = j;
                break;
            }
        }
        if (c->diag) {
            port_fi_diag.skinCur++;
        }
        if (pj < 0 || dont_interp(n->skin[i].key, n->id) || out->nSkin >= FI_SKIN_MAX || used + cnt > FI_SKIN_VTX) {
            continue;
        }
        hint = pj + 1;
        {
            const uint8_t* a = o->skinVtx + (size_t)o->skin[pj].off * 16u;
            const uint8_t* b = n->skinVtx + (size_t)n->skin[i].off * 16u;
            uint8_t* d = out->vtx + (size_t)used * 16u;
            const uintptr_t dest = (uintptr_t)n->skin[i].dest;
            memcpy(d, b, (size_t)cnt * 16u);
            for (k = 0; k < cnt; k++, a += 16, b += 16, d += 16) {
                int16_t oa[3], ob[3], od[3];
                float nx, ny, nz, l2;
                memcpy(oa, a, 6);
                memcpy(ob, b, 6);
                for (j = 0; j < 3; j++) {
                    od[j] = (int16_t)fi_round(c->w * (float)oa[j] + c->step * (float)ob[j]);
                }
                memcpy(d, od, 6);

                nx = c->w * (float)(int8_t)a[12] + c->step * (float)(int8_t)b[12];
                ny = c->w * (float)(int8_t)a[13] + c->step * (float)(int8_t)b[13];
                nz = c->w * (float)(int8_t)a[14] + c->step * (float)(int8_t)b[14];
                l2 = nx * nx + ny * ny + nz * nz;
                if (l2 > 1.0f) {
                    const float bx = (float)(int8_t)b[12], by = (float)(int8_t)b[13], bz = (float)(int8_t)b[14];
                    const float f = sqrtf((bx * bx + by * by + bz * bz) / l2);
                    d[12] = (uint8_t)(int8_t)fi_round(fi_clamp(nx * f, -128.0f, 127.0f));
                    d[13] = (uint8_t)(int8_t)fi_round(fi_clamp(ny * f, -128.0f, 127.0f));
                    d[14] = (uint8_t)(int8_t)fi_round(fi_clamp(nz * f, -128.0f, 127.0f));
                }
            }
            out->skin[out->nSkin].dest = dest;
            out->skin[out->nSkin].len = (unsigned)cnt * 16u;
            out->skin[out->nSkin].off = (unsigned)used;
            out->nSkin++;
            used += cnt;
            if (dest < out->skinLo) {
                out->skinLo = dest;
            }
            if (dest + (uintptr_t)cnt * 16u > out->skinHi) {
                out->skinHi = dest + (uintptr_t)cnt * 16u;
            }
            if (c->diag) {
                port_fi_diag.skinRepl++;
                port_fi_diag.skinVtx += (unsigned)cnt;
            }
        }
    }
}

const void* port_fi_vtx_lookup(const FiRepl* r, const void* src, unsigned bytes) {
    const uintptr_t a = (uintptr_t)src;
    int i;
    if (r == NULL || r->nSkin == 0 || a < r->skinLo || a >= r->skinHi) {
        return NULL;
    }
    for (i = 0; i < r->nSkin; i++) {
        const uintptr_t off = a - r->skin[i].dest;
        if (off < r->skin[i].len) {
            if (off + bytes > r->skin[i].len) {
                return NULL;
            }
            return r->vtx + (size_t)r->skin[i].off * 16u + off;
        }
    }
    return NULL;
}

int port_fi_seen(const FiRepl* r, const void* dest) {
    uint32_t h = ((uint32_t)(uintptr_t)dest * 0x9E3779B1u) >> (32 - FI_REPL_BITS);
    int k;
    for (k = 0; k < 64 && r->seen[h] != NULL; k++) {
        if (r->seen[h] == dest) {
            return 1;
        }
        h = (h + 1) & (FI_REPL_HASH - 1);
    }
    return 0;
}

static int invert4(const float* m, float* invOut) {
    /* Gauss-Jordan elimination with partial pivoting on [m | I] (4x4, column-major as MtxF's mf[col][row]); 0 if m is
     * singular */
    float a[4][8];
    int r, c, k;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            a[r][c] = m[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0f : 0.0f;
        }
    }
    for (c = 0; c < 4; c++) {
        int p = c;
        float best = fi_abs(a[c][c]), f;
        for (r = c + 1; r < 4; r++) {
            if (fi_abs(a[r][c]) > best) {
                best = fi_abs(a[r][c]);
                p = r;
            }
        }
        if (best < 1e-12f) {
            return 0;
        }
        if (p != c) {
            for (k = 0; k < 8; k++) {
                const float t = a[c][k];
                a[c][k] = a[p][k];
                a[p][k] = t;
            }
        }
        f = 1.0f / a[c][c];
        for (k = 0; k < 8; k++) {
            a[c][k] *= f;
        }
        for (r = 0; r < 4; r++) {
            if (r != c && a[r][c] != 0.0f) {
                const float g = a[r][c];
                for (k = 0; k < 8; k++) {
                    a[r][k] -= g * a[c][k];
                }
            }
        }
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            invOut[c * 4 + r] = a[r][c + 4];
        }
    }
    return 1;
}
