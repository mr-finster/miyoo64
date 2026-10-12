/* dl_test.c: the display-list front end (src/port/hh/gfx/gfx_pc.c) on synthetic input, old path against new,
 * byte for byte (2026-10-01, the FPS hunt, frame side). Includes gfx_pc.c itself (its statics reachable) and links
 * the real gfx_cc.c, gfx_soft.c and hh_trig.c; no ROM, no SDL. Build and run: tools/handheld/dl_test.sh.
 *
 *   dl_test vtx [seed] [n]    gSPVertex loads: the old loop (gfx_sp_vertex) and OOT_VTXFAST's two passes
 *                             (gfx_sp_vertex_v2) from the same random RSP state (matrices, ortho and perspective,
 *                             0..7 lights + lookat, lighting / texgen / linear texgen / fog, texture scales, clip
 *                             ratio, NoN and near-clip switches, the UI / squash / pan widening, square and 320x240 outputs,
 *                             special floats in the matrices); every loaded vertex and the RSP state after compared
 *                             with memcmp. Prints the mismatches and exits 1 on any.
 *   dl_test dl [seed] [frames] whole frames: random F3DEX2 display lists (matrices, lights, fog, textures of every
 *                             format through LoadBlock / LoadTile / TLUTs, 1- and 2-cycle combiners, render modes,
 *                             G_TRI1 / G_TRI2 (doubled halves too) / G_QUAD, G_CULLDL, texrects, fill rects)
 *                             through gfx_start_frame / gfx_run / gfx_end_frame and the real rasterizer; prints one
 *                             hash per frame of every draw_triangles buffer and the final framebuffer. Run it with
 *                             OOT_<switch>=0 and =1 and compare the outputs (dl_test.sh does).
 *   dl_test vc [seed] [n]     the vertex combiner (gfx_vc_eval) scalar against NEON (OOT_VCNEON) on random programs;
 *                             and C's x / 255 against the NEON form for every product (see vc_mode).
 *   dl_test flags [seed] [n]  the clip flags, float compares against OOT_VTXFLAGS's integer keys (see flags_mode).
 *   dl_test nearw [seed] [n]  the near-eye clip (OOT_NEARW): the port's flags, trigger and clipper on triangles passing
 *                             the eye against the N64's NoN rule, point by point (see nearw_mode).
 *   dl_test vbench <0|1> <lit> [loads]   one vertex path only, for qemu instruction counts (qprof/qlogcount.py).
 */
#include "gfx_pc.c"

#include <stdarg.h>
#include <sys/mman.h>

/* ---- the rest of the port the three files call ---- */
char _ftext[16] __attribute__((aligned(16)));
extern char _fbss[] __attribute__((alias("_ftext")));          /* the static-texel range: empty */
char __assets_start[16] __attribute__((aligned(16)));
extern char __assets_end[] __attribute__((alias("__assets_start")));
uint32_t gPortProfFrame[PROF_COUNT];
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
void port_gfx_overlay(void) {}
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
void *port_seg_to_ptr(uintptr_t a) { return (void *)a; }
uint32_t port_time_us(void) { return 0; }

static uint64_t sH;
static void hb(const void *p, size_t n) { /* FNV-1a 64 */
    const uint8_t *b = p;
    while (n--) sH = (sH ^ *b++) * 0x100000001b3ull;
}

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static int irnd(int n) { return (int)(rnd() % (uint32_t)n); }

/* ---- window manager: square (or 320x240), no events, every frame drawn ---- */
static uint32_t sDimW = 240, sDimH = 240;
static void wm_init(const char *n, bool f) { (void)n; (void)f; }
static void wm_cb(bool (*a)(int), bool (*b)(int), void (*c)(void)) { (void)a; (void)b; (void)c; }
static void wm_fs_cb(void (*a)(bool)) { (void)a; }
static void wm_fs(bool e) { (void)e; }
static void wm_loop(void (*f)(void)) { f(); }
static void wm_dims(uint32_t *w, uint32_t *h) { *w = sDimW; *h = sDimH; }
static void wm_events(void) {}
static bool wm_start(void) { return true; }
static void wm_swap(void) {}
static double wm_time(void) { return 0.0; }
static struct GfxWindowManagerAPI sWm = { wm_init, wm_cb, wm_fs_cb, wm_fs, wm_loop, wm_dims, wm_events, wm_start, wm_swap, wm_swap, wm_time };

extern struct GfxRenderingAPI gfx_soft_api;
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
static struct GfxRenderingAPI sApi;
static void (*sRealDraw)(float *, size_t, size_t);
static unsigned long sDraws, sTris;
static int sTrX = -1, sTrY = -1; /* seam: SEAM_PIX=x,y prints the triangles whose box holds that pixel */
static void hook_draw(float buf[], size_t len, size_t n) {
    if (sTrX >= 0) {
        const psp_fast_t *p = (const psp_fast_t *)buf;
        for (size_t t = 0; t < n; t++) {
            const psp_fast_t *a = &p[t * 3], *b = a + 1, *c = a + 2;
            float x0 = fminf(a->x, fminf(b->x, c->x)), x1 = fmaxf(a->x, fmaxf(b->x, c->x));
            float y0 = fminf(a->y, fminf(b->y, c->y)), y1 = fmaxf(a->y, fmaxf(b->y, c->y));
            if (sTrX + 0.5f >= x0 - 1 && sTrX + 0.5f <= x1 + 1 && sTrY + 0.5f >= y0 - 1 && sTrY + 0.5f <= y1 + 1)
                printf("  tri (%.9g, %.9g w %.6g) (%.9g, %.9g w %.6g) (%.9g, %.9g w %.6g)\n", a->x, a->y, 1.0f / a->invw,
                       b->x, b->y, 1.0f / b->invw, c->x, c->y, 1.0f / c->invw);
        }
    }
    hb(&n, sizeof(n));
    hb(buf, n * 3 * sizeof(psp_fast_t));
    hb(buf_tri_twice, n);
    sDraws++;
    sTris += n;
    sRealDraw(buf, len, n);
}

/* ---- matrices ---- */
static void f2l(const float m[4][4], int32_t *out) { /* guMtxF2L */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const int32_t a = (int32_t)(m[i][j] * 65536.0f), b = (int32_t)(m[i][j + 1] * 65536.0f);
            out[i * 2 + j / 2] = (int32_t)(((uint32_t)a & 0xFFFF0000u) | ((uint32_t)b >> 16));
            out[8 + i * 2 + j / 2] = (int32_t)(((uint32_t)a << 16) | ((uint32_t)b & 0xFFFFu));
        }
    }
}
static void mul(float r[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(r, t, sizeof(t));
}
static void persp(float m[4][4], float fovy, float aspect, float n, float f) { /* guPerspectiveF, scale 1 */
    const float c = 1.0f / tanf(fovy * 3.14159265f / 360.0f);
    memset(m, 0, 64);
    m[0][0] = c / aspect; m[1][1] = c; m[2][2] = (n + f) / (n - f); m[2][3] = -1; m[3][2] = 2 * n * f / (n - f);
}
static void ortho(float m[4][4], float l, float r, float b, float t, float n, float f) {
    memset(m, 0, 64);
    m[0][0] = 2 / (r - l); m[1][1] = 2 / (t - b); m[2][2] = -2 / (f - n);
    m[3][0] = -(r + l) / (r - l); m[3][1] = -(t + b) / (t - b); m[3][2] = -(f + n) / (f - n); m[3][3] = 1;
}
static void rotxyz(float m[4][4], float s) {
    const float a = frnd(-3.2f, 3.2f), b = frnd(-3.2f, 3.2f), c = frnd(-3.2f, 3.2f);
    float x[4][4] = { { 1, 0, 0, 0 }, { 0, cosf(a), sinf(a), 0 }, { 0, -sinf(a), cosf(a), 0 }, { 0, 0, 0, 1 } };
    float y[4][4] = { { cosf(b), 0, -sinf(b), 0 }, { 0, 1, 0, 0 }, { sinf(b), 0, cosf(b), 0 }, { 0, 0, 0, 1 } };
    float z[4][4] = { { cosf(c), sinf(c), 0, 0 }, { -sinf(c), cosf(c), 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    mul(m, x, y);
    mul(m, m, z);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] *= s;
}

/* ---- the arena: every pointer the display lists carry is >= PORT_REAL_PTR_MIN, as on the device ---- */
#define ARENA ((uint8_t *)0x50000000u)
#define ARENA_SIZE (8u << 20)
static uint8_t *sArenaPos;
static void *alloc(size_t n) {
    uint8_t *p = (uint8_t *)(((uintptr_t)sArenaPos + 15) & ~(uintptr_t)15);
    sArenaPos = p + n;
    if (sArenaPos > ARENA + ARENA_SIZE) { fprintf(stderr, "arena full\n"); exit(2); }
    return p;
}

static void rand_vtx(Vtx *v, int n, int lit, float spread) {
    for (int i = 0; i < n; i++) {
        Vtx_t *t = &v[i].v;
        const int k = irnd(16);
        const float s = k == 0 ? 32000.0f : spread;
        t->ob[0] = (short)frnd(-s, s); t->ob[1] = (short)frnd(-s, s); t->ob[2] = (short)frnd(-s, s);
        if (k == 1) { t->ob[0] = t->ob[1] = t->ob[2] = 0; }
        t->flag = (unsigned short)rnd();
        t->tc[0] = (short)rnd(); t->tc[1] = (short)rnd();
        if (irnd(2)) { t->tc[0] = (short)frnd(-2048, 2048); t->tc[1] = (short)frnd(-2048, 2048); }
        for (int c = 0; c < 4; c++) t->cn[c] = (unsigned char)rnd();
        if (lit && irnd(8) == 0) { v[i].n.n[0] = v[i].n.n[1] = v[i].n.n[2] = 0; }
    }
}

static void rand_light(Light_t *l) {
    for (int c = 0; c < 3; c++) { l->col[c] = l->colc[c] = (unsigned char)rnd(); l->dir[c] = (signed char)rnd(); }
    if (irnd(10) == 0) l->dir[0] = l->dir[1] = l->dir[2] = 0;
}

/* ================================================================== vtx: old loop vs two passes ==== */
static int vtx_mode(int seed, int n) {
    rs = (uint32_t)seed * 2654435761u + 1;
    static Vtx vin[MAX_VERTICES] __attribute__((aligned(16)));
    static struct RSP s0, sA, sB;
    int bad = 0;
    for (int it = 0; it < n; it++) {
        /* the state */
        float P[4][4], M[4][4];
        sDimW = irnd(3) ? 240 : 320; sDimH = 240;
        gfx_current_dimensions.width = sDimW; gfx_current_dimensions.height = sDimH;
        if (irnd(4)) persp(P, frnd(20, 120), irnd(2) ? 4.0f / 3.0f : 1.0f, frnd(1, 30), frnd(500, 13000));
        else ortho(P, frnd(-200, 0), frnd(1, 320), frnd(-200, 0), frnd(1, 240), frnd(-2, 2), frnd(3, 2000));
        rotxyz(M, irnd(4) ? frnd(0.01f, 2.0f) : frnd(0.0001f, 0.05f));
        M[3][0] = frnd(-3000, 3000); M[3][1] = frnd(-3000, 3000); M[3][2] = frnd(-3000, 3000); M[0][3] = M[1][3] = M[2][3] = 0; M[3][3] = 1;
        if (irnd(20) == 0) M[irnd(4)][irnd(4)] = irnd(2) ? 0.0f : -0.0f;
        if (irnd(40) == 0) { /* the special floats a corrupted matrix can carry */
            static const uint32_t sp[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u };
            uint32_t b = sp[irnd(7)]; memcpy(&M[irnd(4)][irnd(4)], &b, 4);
        }
        memset(&rsp, 0, sizeof(rsp));
        rsp.modelview_matrix_stack_size = 1 + irnd(3);
        memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], M, 64);
        memcpy(rsp.P_matrix, P, 64);
        mul(rsp.MP_matrix, M, P);
        { const float wx = P[0][3], wy = P[1][3], wz = P[2][3]; rsp.is_persp = (wx * wx + wy * wy + wz * wz) > 0.01f; }
        if (irnd(10) == 0) rsp.is_persp = !rsp.is_persp;
        rsp.current_num_lights = 1 + irnd(8);
        for (int l = 0; l <= MAX_LIGHTS; l++) rand_light(&rsp.current_lights[l]);
        rand_light(&rsp.current_lookat[0]); rand_light(&rsp.current_lookat[1]);
        rsp.lights_changed = irnd(4) != 0;
        for (int l = 0; l < MAX_LIGHTS; l++) for (int c = 0; c < 3; c++) rsp.current_lights_coeffs[l][c] = frnd(-1, 1);
        for (int l = 0; l < 2; l++) for (int c = 0; c < 3; c++) rsp.current_lookat_coeffs[l][c] = frnd(-1, 1);
        rsp.geometry_mode = rnd() & (G_LIGHTING | G_FOG | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_ZBUFFER | G_CULL_BOTH | G_SHADE);
        rsp.fog_mul = (int16_t)(irnd(4) ? frnd(0, 30000) : (int)(int16_t)rnd());
        rsp.fog_offset = (int16_t)(irnd(4) ? frnd(-30000, 500) : (int)(int16_t)rnd());
        rsp.texture_scaling_factor.s = (uint16_t)(irnd(3) ? 0xFFFF : rnd());
        rsp.texture_scaling_factor.t = (uint16_t)(irnd(3) ? 0xFFFF : rnd());
        for (int i = 0; i < MAX_VERTICES; i++) { uint8_t *p = (uint8_t *)&rsp.loaded_vertices[i]; for (size_t k = 0; k < sizeof(struct LoadedVertex); k++) p[k] = (uint8_t)rnd(); }
        gfx_clip_ratio = 1 + irnd(6);
        gfx_ui_mode = irnd(6) == 0;
        gfx_jpeg_room_squash = irnd(6) == 0;
        gfx_jpeg_room_pan = irnd(4) == 0 ? frnd(-40, 40) : 0.0f;
        sNoN = irnd(4) ? 1 : 0; sNearClip = irnd(6) ? 1 : 0;
        const int nv = 1 + irnd(MAX_VERTICES);
        const int dest = irnd(MAX_VERTICES - nv + 1);
        rand_vtx(vin, nv, (rsp.geometry_mode & G_LIGHTING) != 0, irnd(3) ? 2000.0f : 200.0f);
        /* old, then new, from the same state */
        s0 = rsp;
        gfx_sp_vertex(nv, dest, vin);
        sA = rsp;
        rsp = s0;
        gfx_sp_vertex_v2(nv, dest, vin);
        sB = rsp;
        if (memcmp(&sA, &sB, sizeof(sA)) != 0) {
            if (bad++ < 10) {
                printf("vtx mismatch: it %d nv %d dest %d gm %08X persp %d lights %d\n", it, nv, dest, (unsigned)s0.geometry_mode, s0.is_persp, s0.current_num_lights);
                for (int i = 0; i < MAX_VERTICES; i++) {
                    if (memcmp(&sA.loaded_vertices[i], &sB.loaded_vertices[i], sizeof(struct LoadedVertex)) != 0) {
                        const struct LoadedVertex *a = &sA.loaded_vertices[i], *b = &sB.loaded_vertices[i];
                        printf("  v%d old %g %g %g %g | %g %g %g %g | %g %g | %02X%02X%02X%02X | %X\n", i, a->x, a->y, a->z, a->w, a->_x, a->_y, a->_z, a->_w, a->u, a->v, a->color.r, a->color.g, a->color.b, a->color.a, a->clip_rej);
                        printf("  v%d new %g %g %g %g | %g %g %g %g | %g %g | %02X%02X%02X%02X | %X\n", i, b->x, b->y, b->z, b->w, b->_x, b->_y, b->_z, b->_w, b->u, b->v, b->color.r, b->color.g, b->color.b, b->color.a, b->clip_rej);
                        break;
                    }
                }
            }
        }
        hb(&sA, sizeof(sA));
    }
    printf("vtx seed %d: %d loads, %d mismatches, hash %016llx\n", seed, n, bad, (unsigned long long)sH);
    return bad != 0;
}

/* ================================================================== dl: whole frames ==== */
static void rand_combine(Gfx **pg) {
    Gfx *g = *pg;
    static const int sets[][16] = {
        /* TEXEL0*SHADE, TEXEL0 alpha (MODULATEIDECALA) */
        { G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_TEXEL0,
          G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_TEXEL0 },
        /* (TEXEL0*SHADE) then *PRIM (Link's tunic) */
        { G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0, G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_SHADE, G_ACMUX_0,
          G_CCMUX_COMBINED, G_CCMUX_0, G_CCMUX_PRIMITIVE, G_CCMUX_0, G_ACMUX_COMBINED, G_ACMUX_0, G_ACMUX_PRIMITIVE, G_ACMUX_0 },
        /* shade only */
        { G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_SHADE, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_SHADE,
          G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_SHADE, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_SHADE },
        /* prim colour, env alpha */
        { G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_PRIMITIVE, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_ENVIRONMENT,
          G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_COMBINED, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_COMBINED },
        /* the two-texture lerp by prim LOD fraction, then x shade */
        { G_CCMUX_TEXEL1, G_CCMUX_TEXEL0, G_CCMUX_PRIM_LOD_FRAC, G_CCMUX_TEXEL0, G_ACMUX_TEXEL1, G_ACMUX_TEXEL0, G_ACMUX_PRIM_LOD_FRAC, G_ACMUX_TEXEL0,
          G_CCMUX_COMBINED, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0, G_ACMUX_COMBINED, G_ACMUX_0, G_ACMUX_SHADE, G_ACMUX_0 },
        /* the decal: texel only */
        { G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_TEXEL0,
          G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_TEXEL0 },
        /* env lerp: (PRIM - ENV) * TEXEL0 + ENV, alpha TEXEL0 * PRIM */
        { G_CCMUX_PRIMITIVE, G_CCMUX_ENVIRONMENT, G_CCMUX_TEXEL0, G_CCMUX_ENVIRONMENT, G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_PRIMITIVE, G_ACMUX_0,
          G_CCMUX_COMBINED, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0, G_ACMUX_COMBINED, G_ACMUX_0, G_ACMUX_SHADE, G_ACMUX_0 },
    };
    int m[16];
    if (irnd(4)) {
        memcpy(m, sets[irnd((int)(sizeof(sets) / sizeof(sets[0])))], sizeof(m));
    } else { /* any muxes */
        for (int c = 0; c < 2; c++) {
            m[c * 8 + 0] = irnd(16); m[c * 8 + 1] = irnd(16); m[c * 8 + 2] = irnd(32); m[c * 8 + 3] = irnd(8);
            m[c * 8 + 4] = irnd(8); m[c * 8 + 5] = irnd(8); m[c * 8 + 6] = irnd(8); m[c * 8 + 7] = irnd(8);
        }
    }
    /* gDPSetCombineLERP's words from the mux values (the macro pastes names) */
    gDPSetCombine(g++, GCCc0w0(m[0], m[2], m[4], m[6]) | GCCc1w0(m[8], m[10]),
                  GCCc0w1(m[1], m[3], m[5], m[7]) | GCCc1w1(m[9], m[12], m[14], m[11], m[13], m[15]));
    *pg = g;
}

static const uint32_t kRm[][2] = { /* (cycle 1, cycle 2) render modes */
    { G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2 }, { G_RM_FOG_SHADE_A, G_RM_AA_ZB_XLU_SURF2 },
    { G_RM_FOG_SHADE_A, G_RM_AA_ZB_TEX_EDGE2 }, { G_RM_PASS, G_RM_AA_ZB_OPA_SURF2 },
    { G_RM_PASS, G_RM_AA_ZB_XLU_SURF2 }, { G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2 },
    { G_RM_AA_ZB_XLU_DECAL, G_RM_AA_ZB_XLU_DECAL2 }, { G_RM_ZB_OVL_SURF, G_RM_ZB_OVL_SURF2 },
    { G_RM_XLU_SURF, G_RM_XLU_SURF2 }, { G_RM_AA_ZB_XLU_INTER, G_RM_AA_ZB_XLU_INTER2 },
    { G_RM_FOG_PRIM_A, G_RM_AA_ZB_OPA_SURF2 }, { G_RM_OPA_SURF, G_RM_OPA_SURF2 },
};

static void rand_texture(Gfx **pg, int tile1) {
    Gfx *g = *pg;
    const int fmtsel = irnd(7);
    static const int fmts[7][2] = { { G_IM_FMT_RGBA, G_IM_SIZ_16b }, { G_IM_FMT_RGBA, G_IM_SIZ_32b }, { G_IM_FMT_CI, G_IM_SIZ_4b },
                                    { G_IM_FMT_CI, G_IM_SIZ_8b }, { G_IM_FMT_IA, G_IM_SIZ_8b }, { G_IM_FMT_I, G_IM_SIZ_4b },
                                    { G_IM_FMT_IA, G_IM_SIZ_16b } };
    const int fmt = fmts[fmtsel][0], siz = fmts[fmtsel][1];
    const int w = 8 << irnd(4), h = 8 << irnd(4);
    const int bits = siz == G_IM_SIZ_4b ? 4 : siz == G_IM_SIZ_8b ? 8 : siz == G_IM_SIZ_16b ? 16 : 32;
    if (w * h * bits / 8 > 4096) { *pg = g; return; }
    uint8_t *tex = alloc((size_t)w * h * bits / 8 + 16);
    for (int i = 0; i < w * h * bits / 8; i++) tex[i] = (uint8_t)rnd();
    if (irnd(3) == 0) for (int i = 0; i < w * h * bits / 8; i++) tex[i] |= 0x01; /* opaque 5551 alphas often */
    const int cms = irnd(4), cmt = irnd(4), ms = irnd(2) ? 0 : 3 + irnd(4), mt = irnd(2) ? 0 : 3 + irnd(4);
    if (fmt == G_IM_FMT_CI) {
        uint16_t *tl = alloc(512);
        for (int i = 0; i < 256; i++) tl[i] = (uint16_t)rnd();
        gDPLoadTLUT_pal256(g++, tl);
        gDPSetTextureLUT(g++, G_TT_RGBA16);
    } else {
        gDPSetTextureLUT(g++, G_TT_NONE);
    }
    const int ms2 = ms ? ms : 0, mt2 = mt ? mt : 0;
    if (!tile1) {
        if (irnd(3)) {
            switch (siz) {
                case G_IM_SIZ_4b: gDPLoadTextureBlock_4b(g++, tex, fmt, w, h, 0, cms, cmt, ms2, mt2, irnd(3), irnd(3)); break;
                case G_IM_SIZ_8b: gDPLoadTextureBlock(g++, tex, fmt, G_IM_SIZ_8b, w, h, 0, cms, cmt, ms2, mt2, irnd(3), irnd(3)); break;
                case G_IM_SIZ_16b: gDPLoadTextureBlock(g++, tex, fmt, G_IM_SIZ_16b, w, h, 0, cms, cmt, ms2, mt2, irnd(3), irnd(3)); break;
                default: gDPLoadTextureBlock(g++, tex, fmt, G_IM_SIZ_32b, w, h, 0, cms, cmt, ms2, mt2, irnd(3), irnd(3)); break;
            }
        } else if (siz == G_IM_SIZ_16b || siz == G_IM_SIZ_8b) {
            const int ul = irnd(w / 2), ut = irnd(h / 2);
            if (siz == G_IM_SIZ_16b)
                gDPLoadTextureTile(g++, tex, fmt, G_IM_SIZ_16b, w, h, ul, ut, w - 1, h - 1, 0, cms, cmt, ms2, mt2, 0, 0);
            else
                gDPLoadTextureTile(g++, tex, fmt, G_IM_SIZ_8b, w, h, ul, ut, w - 1, h - 1, 0, cms, cmt, ms2, mt2, 0, 0);
        } else {
            gDPLoadTextureBlock(g++, tex, fmt, G_IM_SIZ_16b, w / 2 > 0 ? w / 2 : 1, h, 0, cms, cmt, ms2, mt2, 0, 0);
        }
    } else if (siz == G_IM_SIZ_16b || siz == G_IM_SIZ_32b || siz == G_IM_SIZ_8b) { /* tile 1 at TMEM 256 (OoT's two-texture draws) */
        const int line = (w * bits / 8 + 7) / 8;
        gDPSetTextureImage(g++, fmt, siz == G_IM_SIZ_32b ? G_IM_SIZ_32b : siz, w, tex);
        gDPSetTile(g++, fmt, siz, line, 256, G_TX_LOADTILE, 0, cmt, mt2, 0, cms, ms2, 0);
        gDPLoadSync(g++);
        gDPLoadTile(g++, G_TX_LOADTILE, 0, 0, (w - 1) << 2, (h - 1) << 2);
        gDPPipeSync(g++);
        gDPSetTile(g++, fmt, siz, line, 256, 1, 0, cmt, mt2, irnd(3), cms, ms2, irnd(3));
        gDPSetTileSize(g++, 1, irnd(4) << 2, irnd(4) << 2, (w - 1) << 2, (h - 1) << 2);
    }
    if (irnd(4) == 0) gDPSetTileSize(g++, G_TX_RENDERTILE, irnd(64), irnd(64), ((w - 1) << 2) + irnd(64), ((h - 1) << 2) + irnd(64));
    *pg = g;
}

static Gfx *build_frame(int frame) {
    const size_t cap = 60000;
    Gfx *dl = alloc(cap * sizeof(Gfx)), *g = dl;
    Vp *vp = alloc(sizeof(Vp));
    vp->vp.vscale[0] = 640; vp->vp.vscale[1] = 480; vp->vp.vscale[2] = 511; vp->vp.vscale[3] = 0;
    vp->vp.vtrans[0] = 640; vp->vp.vtrans[1] = 480; vp->vp.vtrans[2] = 511; vp->vp.vtrans[3] = 0;
    gSPViewport(g++, vp);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    gDPSetFogColor(g++, rnd() & 255, rnd() & 255, rnd() & 255, 255);
    gSPFogPosition(g++, irnd(990), 996 + irnd(4));
    float P[4][4];
    int depth = 0;
    const int nbatch = 4 + irnd(16);
    for (int b = 0; b < nbatch && g - dl < (ptrdiff_t)cap - 400; b++) {
        if (b == 0 || irnd(6) == 0) {
            int32_t *m = alloc(64);
            if (irnd(5)) persp(P, frnd(30, 90), 4.0f / 3.0f, frnd(5, 20), frnd(2000, 12000));
            else ortho(P, 0, 320, 0, 240, -1, 1);
            f2l(P, m);
            gSPMatrix(g++, m, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
            if (irnd(3) == 0) { if (irnd(2)) gSPClipRatio(g++, FRUSTRATIO_1); else gSPClipRatio(g++, FRUSTRATIO_2); }
        }
        {
            float M[4][4];
            int32_t *m = alloc(64);
            rotxyz(M, irnd(5) ? frnd(0.3f, 2.0f) : frnd(0.005f, 0.05f));
            M[3][0] = frnd(-600, 600); M[3][1] = frnd(-600, 600); M[3][2] = frnd(-1500, 50);
            M[0][3] = M[1][3] = M[2][3] = 0; M[3][3] = 1;
            f2l(M, m);
            const int push = depth < 8 && irnd(2);
            gSPMatrix(g++, m, G_MTX_MODELVIEW | (irnd(3) ? G_MTX_LOAD : G_MTX_MUL) | (push ? G_MTX_PUSH : G_MTX_NOPUSH));
            depth += push;
        }
        const uint32_t geo = (irnd(5) ? G_ZBUFFER : 0) | G_SHADE | (irnd(2) ? G_SHADING_SMOOTH : 0) | (irnd(3) ? G_CULL_BACK : irnd(2) ? G_CULL_FRONT : 0) |
                             (irnd(3) == 0 ? G_LIGHTING : 0) | (irnd(3) == 0 ? G_FOG : 0);
        const uint32_t tg = (geo & G_LIGHTING) && irnd(4) == 0 ? (G_TEXTURE_GEN | (irnd(2) ? G_TEXTURE_GEN_LINEAR : 0)) : 0;
        gSPLoadGeometryMode(g++, geo | tg);
        if (geo & G_LIGHTING) {
            const int nl = 1 + irnd(7);
            Light_t *ls = alloc(sizeof(Light_t) * 8);
            for (int l = 0; l <= nl; l++) rand_light(&ls[l]);
            gSPNumLights(g++, nl);
            for (int l = 0; l < nl; l++) gSPLight(g++, (Light *)&ls[l], l + 1);
            gSPLight(g++, (Light *)&ls[nl], nl + 1);
            if (tg && irnd(2)) {
                LookAt *la = alloc(sizeof(LookAt));
                rand_light((Light_t *)&la->l[0]); rand_light((Light_t *)&la->l[1]);
                gSPLookAt(g++, la);
            }
        }
        gSPTexture(g++, irnd(3) ? 0xFFFF : rnd() & 0xFFFF, irnd(3) ? 0xFFFF : rnd() & 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        const int twocyc = irnd(2);
        const int rm = irnd((int)(sizeof(kRm) / sizeof(kRm[0])));
        gDPPipeSync(g++);
        gDPSetCycleType(g++, twocyc ? G_CYC_2CYCLE : G_CYC_1CYCLE);
        if (twocyc) gDPSetRenderMode(g++, kRm[rm][0], kRm[rm][1]);
        else gDPSetRenderMode(g++, kRm[rm][1] & ~0u, kRm[rm][1]);
        gDPSetTextureFilter(g++, irnd(4) ? G_TF_BILERP : G_TF_POINT);
        if (irnd(8) == 0) gDPSetAlphaCompare(g++, G_AC_THRESHOLD);
        rand_combine(&g);
        gDPSetPrimColor(g++, 0, rnd() & 255, rnd() & 255, rnd() & 255, rnd() & 255, irnd(3) ? 255 : rnd() & 255);
        gDPSetEnvColor(g++, rnd() & 255, rnd() & 255, rnd() & 255, irnd(3) ? 255 : rnd() & 255);
        rand_texture(&g, 0);
        if (irnd(3) == 0) rand_texture(&g, 1);
        /* vertices and triangles */
        const int loads = 1 + irnd(4);
        for (int ld = 0; ld < loads; ld++) {
            const int nv = 3 + irnd(30), v0 = irnd(32 - (nv > 32 ? 32 : nv) + 1);
            Vtx *v = alloc(sizeof(Vtx) * (size_t)nv);
            rand_vtx(v, nv, (geo & G_LIGHTING) != 0, irnd(3) ? 400.0f : 60.0f);
            gSPVertex(g++, v, nv, v0);
            if (irnd(8) == 0) gSPCullDisplayList(g++, v0, v0 + nv - 1);
            const int nt = 1 + irnd(20);
            for (int t = 0; t < nt; t++) {
                const int a = v0 + irnd(nv), bb = v0 + irnd(nv), c = v0 + irnd(nv);
                switch (irnd(6)) {
                    case 0: case 1: gSP1Triangle(g++, a, bb, c, 0); break;
                    case 2: gSP2Triangles(g++, a, bb, c, 0, v0 + irnd(nv), v0 + irnd(nv), v0 + irnd(nv), 0); break;
                    case 3: gSP2Triangles(g++, a, bb, c, 0, a, bb, c, 0); break; /* the doubled halves (the basement water) */
                    case 4: gSP1Quadrangle(g++, a, bb, c, v0 + irnd(nv), 0); break;
                    default: gSP1Triangle(g++, a, c, bb, 0); break;
                }
            }
        }
        if (depth > 0 && irnd(3) == 0) { gSPPopMatrix(g++, G_MTX_MODELVIEW); depth--; } /* matched, as the game's */
        if (irnd(10) == 0) { /* 2D on top */
            gDPPipeSync(g++);
            gDPSetCycleType(g++, G_CYC_1CYCLE);
            gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
            gSPTextureRectangle(g++, irnd(200) << 2, irnd(200) << 2, (100 + irnd(200)) << 2, (100 + irnd(130)) << 2, G_TX_RENDERTILE,
                                irnd(256), irnd(256), irnd(2) ? 1 << 10 : irnd(4096), irnd(2) ? 1 << 10 : irnd(4096));
        }
        if (irnd(16) == 0) {
            gDPPipeSync(g++);
            gDPSetCycleType(g++, G_CYC_FILL);
            gDPSetFillColor(g++, (rnd() & 0xFFFEu) * 0x10001u);
            gDPFillRectangle(g++, irnd(160), irnd(120), 160 + irnd(159), 120 + irnd(119));
        }
    }
    gSPEndDisplayList(g++);
    (void)frame;
    return dl;
}

static int dl_mode(int seed, int frames) {
    rs = (uint32_t)seed * 2654435761u + 7;
    sApi = gfx_soft_api;
    sRealDraw = sApi.draw_triangles;
    sApi.draw_triangles = hook_draw;
    gfx_init(&sWm, &sApi, "dl_test", false);
    for (int f = 0; f < frames; f++) {
        sArenaPos = ARENA;
        Gfx *dl = build_frame(f);
        sH = 0xcbf29ce484222325ull;
        sDraws = sTris = 0;
        gfx_start_frame();
        gfx_run(dl);
        gfx_end_frame();
        int w = 0, h = 0;
        const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
        const uint64_t hdraw = sH;
        if (fb) hb(fb, (size_t)w * h * 4);
        printf("frame %d draws %lu tris %lu drawhash %016llx fbhash %016llx\n", f, sDraws, sTris, (unsigned long long)hdraw, (unsigned long long)sH);
        if (fb && getenv("DL_DUMP")) { /* DL_DUMP=<dir>: each frame's pixels, raw 32-bit (pixel diffs between trees) */
            char path[512]; snprintf(path, sizeof(path), "%s/dl_%d_%d.raw", getenv("DL_DUMP"), seed, f);
            FILE *o = fopen(path, "wb"); if (o) { fwrite(fb, 4, (size_t)w * h, o); fclose(o); }
        }
    }
    return 0;
}

/* ================================================================== vc: the vertex combiner, scalar vs NEON ==== */
/* dl_test vc [seed] [programs]: random combiner programs (any cycle-1 id, any cycle-2 muxes, prim / env / LOD
 * fraction, one or two cycles) compiled by gfx_vc_compile, each evaluated on random shades and LODs (and every
 * corner byte) with OOT_VCNEON off and on: outputs and the source table after compared. Also checks C's x / 255
 * against the NEON form sign * ((|x| * 0x8081) >> 23) for every |x| <= 65025. On x86 only the division check
 * runs (the NEON path is ARM's). */
static int vc_mode(int seed, int n) {
    int bad = 0;
    for (int32_t x = -65025; x <= 65025; x++) {
        const uint32_t ax = (uint32_t)(x < 0 ? -x : x);
        const int32_t q = (int32_t)((ax * 0x8081u) >> 23);
        if ((x < 0 ? -q : q) != x / 255) { if (bad++ < 5) printf("vcdiv: %d\n", (int)x); }
    }
    printf("vcdiv: %s\n", bad ? "FAIL" : "all 130051 products agree");
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    rs = (uint32_t)seed * 2654435761u + 3;
    static const uint8_t corner[] = { 0, 1, 127, 128, 254, 255 };
    unsigned long evals = 0;
    for (int it = 0; it < n; it++) {
        const uint32_t cc_id = rnd() & (0xFFFFFFu | CC_FLAG_RGB_A_ONE | CC_FLAG_ALPHA_A_ONE | CC_FLAG_RGB_D_ONE | CC_FLAG_ALPHA_D_ONE);
        for (int k = 0; k < 8; k++) rdp.cc2[k] = (uint8_t)(k < 4 ? (k == 2 ? irnd(32) : irnd(16)) : irnd(8));
        rdp.prim_color.r = rnd(); rdp.prim_color.g = rnd(); rdp.prim_color.b = rnd(); rdp.prim_color.a = rnd();
        rdp.env_color.r = rnd(); rdp.env_color.g = rnd(); rdp.env_color.b = rnd(); rdp.env_color.a = rnd();
        rdp.prim_lod_frac = rnd();
        tri_state.cc2 = irnd(2);
        gfx_vc_compile(cc_id);
        for (int e = 0; e < 64; e++) {
            struct RGBA sh, o0, o1;
            int lod;
            if (e < 36) { sh.r = corner[e % 6]; sh.g = corner[e / 6]; sh.b = corner[(e * 5) % 6]; sh.a = corner[(e * 7) % 6]; lod = corner[e % 6]; }
            else { sh.r = rnd(); sh.g = rnd(); sh.b = rnd(); sh.a = rnd(); lod = irnd(256); }
            uint8_t srcA[VC_N], srcB[VC_N];
            uint8_t save[VC_N];
            memcpy(save, sVc.src, VC_N);
            sVcNeon = 0; gfx_vc_eval(&sh, lod, &o0); memcpy(srcA, sVc.src, VC_N);
            memcpy(sVc.src, save, VC_N);
            sVcNeon = 1; gfx_vc_eval(&sh, lod, &o1); memcpy(srcB, sVc.src, VC_N);
            evals++;
            if (memcmp(&o0, &o1, 4) != 0 || memcmp(srcA, srcB, VC_N) != 0) {
                if (bad++ < 10) printf("vc mismatch: cc %08X cc2 %d shade %02X%02X%02X%02X lod %d: %02X%02X%02X%02X vs %02X%02X%02X%02X\n",
                                       (unsigned)cc_id, (int)tri_state.cc2, sh.r, sh.g, sh.b, sh.a, lod,
                                       o0.r, o0.g, o0.b, o0.a, o1.r, o1.g, o1.b, o1.a);
            }
        }
    }
    printf("vc seed %d: %d programs, %lu evaluations, %d mismatches\n", seed, n, evals, bad);
#else
    (void)seed; (void)n;
#endif
    return bad != 0;
}

/* ================================================================== flags: clip flags, float vs integer keys ==== */
/* dl_test flags [seed] [n]: vtx_clip_flags with the float compares (the old loop's) against OOT_VTXFLAGS's integer
 * keys: every combination of x y z w from a pool of special values (+-0, the smallest and largest denormals,
 * FLT_MIN, 1/64 and 2 (W_MIN with OOT_NEARW on / off), 1, 3, values one ulp around them, the largest float, infinities, NaNs) for every clip ratio
 * and both NoN / near-clip settings, then n random bit patterns and n random nearby values (equalities). */
static float bitsf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static int flags_mode(int seed, int n) {
    static const uint32_t pos[] = { 0x00000000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3F7FFFFFu, 0x3F800000u,
                                    0x3F800001u, 0x40000000u, 0x40400000u, 0x40C00000u, 0x7F7FFFFFu, 0x7F800000u,
                                    0x7FC00000u, 0x7F800001u, 0x3C800000u /* 1/64: OOT_NEARW's W_MIN */ };
    float pool[2 * (sizeof(pos) / sizeof(pos[0]))];
    int np = 0, bad = 0;
    unsigned long calls = 0;
    for (size_t i = 0; i < sizeof(pos) / sizeof(pos[0]); i++) { pool[np++] = bitsf(pos[i]); pool[np++] = bitsf(pos[i] | 0x80000000u); }
    for (int ix = 0; ix < np; ix++) for (int iy = 0; iy < np; iy++) for (int iz = 0; iz < np; iz++) for (int iw = 0; iw < np; iw++)
        for (int r = 1; r <= 6; r++) for (int m = 0; m < 4; m++) {
            const float x = pool[ix], y = pool[iy], z = pool[iz], w = pool[iw];
            const uint32_t a = vtx_clip_flags(x, y, z, w, (float)r, m & 1, m >> 1, 0), b = vtx_clip_flags(x, y, z, w, (float)r, m & 1, m >> 1, 1);
            calls++;
            if (a != b && bad++ < 10) printf("flags: %08X %08X %08X %08X r %d m %d: %X vs %X\n", (unsigned)pos[ix / 2], (unsigned)pos[iy / 2], (unsigned)pos[iz / 2], (unsigned)pos[iw / 2], r, m, a, b);
        }
    rs = (uint32_t)seed * 2654435761u + 5;
    for (int i = 0; i < n; i++) {
        float v[4];
        if (i & 1) { for (int k = 0; k < 4; k++) v[k] = bitsf(rnd()); }
        else { /* nearby values: equal, one ulp apart, scaled by the bounds' factors */
            const float base = frnd(-2000, 2000);
            for (int k = 0; k < 4; k++) {
                const int sel = irnd(6);
                float t = sel == 0 ? base : sel == 1 ? -base : sel == 2 ? base * 3.0f : sel == 3 ? -base * 2.0f : sel == 4 ? base * (float)(1 + irnd(6)) : frnd(-2000, 2000);
                uint32_t b2; memcpy(&b2, &t, 4); b2 += (uint32_t)(irnd(3) - 1); memcpy(&t, &b2, 4);
                v[k] = t;
            }
        }
        const int r = 1 + irnd(6), m = irnd(4);
        const uint32_t a = vtx_clip_flags(v[0], v[1], v[2], v[3], (float)r, m & 1, m >> 1, 0);
        const uint32_t b = vtx_clip_flags(v[0], v[1], v[2], v[3], (float)r, m & 1, m >> 1, 1);
        calls++;
        if (a != b && bad++ < 10) printf("flags: %g %g %g %g r %d m %d: %X vs %X\n", v[0], v[1], v[2], v[3], r, m, a, b);
    }
    printf("flags seed %d: %lu cases, %d mismatches\n", seed, calls, bad);
    return bad != 0;
}

/* ================================================================== vbench: one path, for instruction counts ==== */
/* dl_test vbench <0|1> <lit 0|1|2> [loads]: a perspective scene's 32-vertex loads through the old loop (0) or the
 * two passes (1); lit 1 = 2 lights, 2 = 2 lights + texgen; fog on. For qemu -d counts (qprof/qlogcount.py). */
static int vbench_mode(int which, int lit, int n) {
    static Vtx vin[32] __attribute__((aligned(16)));
    float P[4][4], M[4][4];
    rs = 99;
    persp(P, 60, 4.0f / 3.0f, 10, 12800);
    rotxyz(M, 1.0f);
    M[3][0] = 100; M[3][1] = -50; M[3][2] = -900; M[0][3] = M[1][3] = M[2][3] = 0; M[3][3] = 1;
    memset(&rsp, 0, sizeof(rsp));
    rsp.modelview_matrix_stack_size = 1;
    memcpy(rsp.modelview_matrix_stack[0], M, 64);
    memcpy(rsp.P_matrix, P, 64);
    mul(rsp.MP_matrix, M, P);
    rsp.is_persp = true;
    rsp.current_num_lights = 3;
    for (int l = 0; l <= MAX_LIGHTS; l++) rand_light(&rsp.current_lights[l]);
    rsp.lights_changed = true;
    rsp.geometry_mode = G_ZBUFFER | G_SHADE | G_FOG | (lit ? G_LIGHTING : 0) | (lit == 2 ? G_TEXTURE_GEN : 0);
    rsp.fog_mul = 2000; rsp.fog_offset = -1500;
    rsp.texture_scaling_factor.s = rsp.texture_scaling_factor.t = 0xFFFF;
    gfx_current_dimensions.width = 240; gfx_current_dimensions.height = 240;
    rand_vtx(vin, 32, lit, 300.0f);
    uint32_t acc = 0;
    for (int it = 0; it < n; it++) {
        if (which) gfx_sp_vertex_v2(32, 0, vin); else gfx_sp_vertex(32, 0, vin);
        acc += rsp.loaded_vertices[it & 31].clip_rej;
    }
    printf("vbench %d lit %d: %d loads, acc %u\n", which, lit, n, acc);
    return 0;
}

/* ================================================================== seam: shared floor edges near the eye ==== */
/* dl_test seam [seed] [cases] (2026-10-02, the user's dashed line along a floor edge out of Kokiri): a floor of
 * quads (two triangles each, every corner loaded with the same Vtx so shared edges are bit-identical in the cache)
 * seen from an OoT camera (guPerspective 60 deg, 4:3, near 10, far 12800; height 8..150, pitch -60..+5 deg, any yaw,
 * quads 40..600 units, some floors jittered so they are not one plane), drawn through gfx_pc + gfx_soft as the
 * game's opaque floor (TEXEL0 x SHADE, a 32x32 RGBA16 repeating texture, bilinear, z, G_RM_AA_ZB_OPA_SURF: the lean
 * opaque rows), after a sky (SKYB/SKYE tags, texture only, no z: the deferred sky's kept rows) when SEAM_SKY=1. The
 * floor's texel and shade are one colour, so a pixel that is not that colour with the floor's colour within 3 px
 * above and below in its column is a hole. Then the same floor without z, blended at half alpha (G_RM_XLU_SURF,
 * shade only) on black: a pixel brighter than one layer was covered twice. Prints per case "holes n twice n" and the
 * totals; SEAM_DUMP=<dir> writes each case with holes as a PPM. */
uint32_t gfx_soft_crack_fills __attribute__((weak)); /* gfx_soft.c's (OOT_CRACKFILL); weak: a tree without it links */
static int seam_mode(int seed, int n) {
    rs = (uint32_t)seed * 2654435761u + 11;
    sApi = gfx_soft_api;
    sRealDraw = sApi.draw_triangles;
    sApi.draw_triangles = hook_draw;
    gfx_init(&sWm, &sApi, "dl_test", false);
    const char *e = getenv("SEAM_SKY");
    const int sky = e && e[0] == '1';
    const char *dump = getenv("SEAM_DUMP");
    const int tj = getenv("SEAM_TJ") && getenv("SEAM_TJ")[0] == '1';
    long tot_holes = 0, tot_twice = 0, cases_holes = 0;
    uint64_t hall = 0;
    for (int c = 0; c < n; c++) {
        const float h = irnd(3) ? frnd(8, 60) : frnd(60, 150), pitch = frnd(-60, 5) * 3.14159265f / 180.0f;
        const float yaw = frnd(0, 6.2831853f), q = (float)(irnd(2) ? 40 + irnd(160) : 200 + irnd(400));
        const float ox = frnd(0, q), oz = frnd(0, q);
        const int jit = irnd(3) == 0, cells = 3000 / (int)q < 24 ? 3000 / (int)q : 24, diag = irnd(3);
        int holes = 0, twice = 0, floorpx = 0;
        const uint32_t fills0 = gfx_soft_crack_fills;
        const char *ec = getenv("SEAM_CASE");
        if (ec && atoi(ec) == c && getenv("SEAM_PIX")) sscanf(getenv("SEAM_PIX"), "%d,%d", &sTrX, &sTrY); else sTrX = -1;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) sTrX = -1;
            sArenaPos = ARENA;
            Gfx *dl = alloc(200000 * sizeof(Gfx)), *g = dl;
            Vp *vp = alloc(sizeof(Vp));
            vp->vp.vscale[0] = 640; vp->vp.vscale[1] = 480; vp->vp.vscale[2] = 511; vp->vp.vscale[3] = 0;
            vp->vp.vtrans[0] = 640; vp->vp.vtrans[1] = 480; vp->vp.vtrans[2] = 511; vp->vp.vtrans[3] = 0;
            gSPViewport(g++, vp);
            gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
            float P[4][4], V[4][4], T[4][4];
            persp(P, 60, 4.0f / 3.0f, 10, 12800);
            int32_t *pm = alloc(64); f2l(P, pm);
            gSPMatrix(g++, pm, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
            gSPClipRatio(g++, FRUSTRATIO_2);
            /* view = translate(-eye) * rotY(yaw) * rotX(pitch) (row vectors, as the RSP's) */
            memset(T, 0, 64); T[0][0] = T[1][1] = T[2][2] = T[3][3] = 1; T[3][0] = -ox; T[3][1] = -h; T[3][2] = -oz;
            const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
            float ry[4][4] = { { cy, 0, sy, 0 }, { 0, 1, 0, 0 }, { -sy, 0, cy, 0 }, { 0, 0, 0, 1 } };
            float rx[4][4] = { { 1, 0, 0, 0 }, { 0, cp, -sp, 0 }, { 0, sp, cp, 0 }, { 0, 0, 0, 1 } };
            mul(V, T, ry); mul(V, V, rx);
            if (sky && pass == 0) { /* a sky quad far ahead, no z, texture only, between the tags */
                float I[4][4]; memset(I, 0, 64); I[0][0] = I[1][1] = I[2][2] = I[3][3] = 1;
                int32_t *mi = alloc(64); f2l(I, mi);
                gSPMatrix(g++, mi, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
                gSPLoadGeometryMode(g++, G_SHADE);
                gDPPipeSync(g++);
                gDPSetCycleType(g++, G_CYC_1CYCLE);
                gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
                gDPSetTextureFilter(g++, G_TF_BILERP);
                gDPSetCombineLERP(g++, 0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
                uint16_t *st = alloc(32 * 32 * 2);
                for (int i = 0; i < 32 * 32; i++) st[i] = (uint16_t)((((i >> 5) & 31) << 1) | 0xC001);
                gDPSetTextureLUT(g++, G_TT_NONE);
                gDPLoadTextureBlock(g++, st, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
                Vtx *sv = alloc(sizeof(Vtx) * 4);
                const short sx[4] = { -3000, 3000, 3000, -3000 }, sy2[4] = { -3000, -3000, 3000, 3000 };
                for (int i = 0; i < 4; i++) {
                    memset(&sv[i], 0, sizeof(Vtx));
                    sv[i].v.ob[0] = sx[i]; sv[i].v.ob[1] = sy2[i]; sv[i].v.ob[2] = -5000;
                    sv[i].v.tc[0] = (short)(i == 1 || i == 2 ? 32 << 6 : 0); sv[i].v.tc[1] = (short)(i >= 2 ? 32 << 6 : 0);
                    sv[i].v.cn[0] = sv[i].v.cn[1] = sv[i].v.cn[2] = sv[i].v.cn[3] = 255;
                }
                gDPNoOpTag(g++, HH_TAG_SKY_BEGIN);
                gSPVertex(g++, sv, 4, 0);
                gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
                gDPNoOpTag(g++, HH_TAG_SKY_END);
            }
            if (tj) { /* floor coordinates in half units (T-junction offsets of half a unit, as rounding gives) */
                float S[4][4]; memset(S, 0, 64); S[0][0] = S[1][1] = S[2][2] = 0.5f; S[3][3] = 1;
                mul(V, S, V);
            }
            int32_t *mv = alloc(64); f2l(V, mv);
            gSPMatrix(g++, mv, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
            gDPPipeSync(g++);
            gDPSetCycleType(g++, G_CYC_1CYCLE);
            if (pass == 0) {
                gSPLoadGeometryMode(g++, G_ZBUFFER | G_SHADE);
                gDPSetRenderMode(g++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
                gDPSetTextureFilter(g++, G_TF_BILERP);
                gDPSetCombineLERP(g++, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
                uint16_t *ft = alloc(32 * 32 * 2);
                for (int i = 0; i < 32 * 32; i++) ft[i] = (uint16_t)((16u << 11) | (20u << 6) | (12u << 1) | 1u);
                gDPSetTextureLUT(g++, G_TT_NONE);
                gDPLoadTextureBlock(g++, ft, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
            } else {
                gSPLoadGeometryMode(g++, G_SHADE);
                gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
                gDPSetCombineLERP(g++, 0, 0, 0, SHADE, 0, 0, 0, SHADE, 0, 0, 0, SHADE, 0, 0, 0, SHADE);
                gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
            }
            /* the floor: 2 cells x 2 cells quads of q around the eye (corners at y 0, or jittered by a hash) */
            const int c0x = (int)(ox / q) - cells, c0z = (int)(oz / q) - cells;
            for (int i = 0; i < 2 * cells; i++) {
                for (int j = 0; j < 2 * cells; j++) {
                    Vtx *v = alloc(sizeof(Vtx) * 5);
                    for (int k = 0; k < 4; k++) {
                        const int gx = c0x + i + (k == 1 || k == 2), gz = c0z + j + (k >= 2);
                        memset(&v[k], 0, sizeof(Vtx));
                        int x = (int)(gx * q), z = (int)(gz * q), y = 0;
                        if (jit) { const uint32_t hh = (uint32_t)(gx * 73856093) ^ (uint32_t)(gz * 19349663); y = (int)(hh % 41) - 20; x += (int)((hh >> 8) % 21) - 10; z += (int)((hh >> 16) % 21) - 10; }
                        if (tj) { x *= 2; y *= 2; z *= 2; }
                        v[k].v.ob[0] = (short)x; v[k].v.ob[1] = (short)y; v[k].v.ob[2] = (short)z;
                        v[k].v.tc[0] = (short)(((gx & 7) * 32) << 5); v[k].v.tc[1] = (short)(((gz & 7) * 32) << 5);
                        v[k].v.cn[0] = v[k].v.cn[1] = v[k].v.cn[2] = 255; v[k].v.cn[3] = pass ? 128 : 255;
                    }
                    if (tj && ((i + j) & 1)) { /* SEAM_TJ: a vertex M on the far edge (2-3), half a unit inside: a T-junction */
                        v[4] = v[2];
                        v[4].v.ob[0] = (short)((v[2].v.ob[0] + v[3].v.ob[0]) / 2); v[4].v.ob[2] = (short)((v[2].v.ob[2] + v[3].v.ob[2]) / 2 - 1);
                        v[4].v.ob[1] = (short)((v[2].v.ob[1] + v[3].v.ob[1]) / 2);
                        gSPVertex(g++, v, 5, 0);
                        gSP2Triangles(g++, 0, 1, 4, 0, 1, 2, 4, 0);
                        gSP1Triangle(g++, 0, 4, 3, 0);
                        continue;
                    }
                    gSPVertex(g++, v, 4, 0);
                    const int d = diag == 2 ? ((i + j) & 1) : diag;
                    if (d) gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
                    else gSP2Triangles(g++, 0, 1, 3, 0, 1, 2, 3, 0);
                }
            }
            gSPEndDisplayList(g++);
            gfx_start_frame();
            gfx_run(dl);
            gfx_end_frame();
            int w = 0, hh = 0;
            const uint32_t *fb = gfx_soft_get_framebuffer(&w, &hh);
            sH = hall; hb(fb, (size_t)w * hh * 4); hall = sH;
            if (pass == 0) {
                const uint32_t F = fb[(hh - 1) * w + w / 2] & 0xFFFFFFu; /* the floor's colour (bottom middle) */
                for (int i = 0; i < w * hh; i++) floorpx += (fb[i] & 0xFFFFFFu) == F;
                for (int y = 3; y < hh - 3; y++)
                    for (int x = 0; x < w; x++) {
                        if ((fb[y * w + x] & 0xFFFFFFu) == F) continue;
                        int up = 0, dn = 0;
                        for (int k = 1; k <= 3; k++) { up |= (fb[(y - k) * w + x] & 0xFFFFFFu) == F; dn |= (fb[(y + k) * w + x] & 0xFFFFFFu) == F; }
                        if (up && dn) holes++;
                    }
                if (dump && (holes || getenv("SEAM_DUMPALL"))) {
                    char path[512]; snprintf(path, sizeof(path), "%s/seam_%d_%d.ppm", dump, seed, c);
                    FILE *f = fopen(path, "wb");
                    if (f) { fprintf(f, "P6 %d %d 255\n", w, hh);
                        for (int i = 0; i < w * hh; i++) { const uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] }; fwrite(px, 1, 3, f); }
                        fclose(f); }
                }
            } else {
                for (int i = 0; i < w * hh; i++) if (((fb[i] >> 8) & 0xFF) > 160) twice++;
            }
        }
        printf("case %d h %.1f pitch %.1f yaw %.2f q %d jit %d diag %d: floor %d holes %d twice %d fills %u\n", c, h,
               pitch * 180.0f / 3.14159265f, yaw, (int)q, jit, diag, floorpx, holes, twice, (unsigned)(gfx_soft_crack_fills - fills0));
        tot_holes += holes; tot_twice += twice; cases_holes += holes > 0;
    }
    printf("seam seed %d sky %d tj %d: %d cases, %ld with holes, holes %ld, twice %ld, hash %016llx\n", seed, sky, tj, n, cases_holes,
           tot_holes, tot_twice, (unsigned long long)hall);
    return 0;
}

/* ================================================================== edge: cut-out rims (TEX_EDGE) ==== */
/* dl_test edge <dir> [shot] (2026-10-02, the user's yellow flowers with a blue rim outside Hyrule Castle): a
 * near-camera cut-out quad -- a 32x32 CI8 flower, its transparent texels (palette index 0) blue -- drawn with
 * G_RM_FOG_SHADE_A + G_RM_AA_ZB_TEX_EDGE2, TEXEL0 x SHADE, alpha TEXEL0 (OoT's foliage), over a grass ground, and a
 * fence (RGBA16 bars with one-texel transparent slits) against the clear colour. Order 0: the cut-outs before the
 * ground (over the clear: round 49's case), 1: after. Writes <dir>/edge_<order>.ppm, and for the N64 estimate
 * (tools: the RDP writes the filtered colour unblended with coverage = alpha x 8, the VI filter pulls a partly
 * covered pixel toward its neighbours) the same view's filtered texel colours (n64_c.ppm, unlit, every pixel of
 * the quads), alphas (n64_a.ppm) and the ground and clear alone (n64_g.ppm). shot: the camera's x offset. */
static int edge_mode(const char *dir, int shot) {
    sApi = gfx_soft_api;
    sRealDraw = sApi.draw_triangles;
    sApi.draw_triangles = hook_draw;
    gfx_init(&sWm, &sApi, "dl_test", false);
    /* textures */
    static uint8_t ci[32 * 32] __attribute__((aligned(16)));
    static uint16_t tl[256] __attribute__((aligned(16)));
    static uint16_t fence[32 * 32] __attribute__((aligned(16)));
    static uint16_t grass[32 * 32] __attribute__((aligned(16)));
#define P5551N(r, g, b, a) (uint16_t)((((r) >> 3) << 11) | (((g) >> 3) << 6) | (((b) >> 3) << 1) | (a))
#define P5551(r, g, b, a) (uint16_t)((P5551N(r, g, b, a) >> 8) | ((P5551N(r, g, b, a) & 0xFF) << 8)) /* big-endian in memory, as the ROM's */
    tl[0] = P5551(48, 64, 248, 0);   /* the transparent texels' colour: blue */
    tl[1] = P5551(232, 200, 40, 1);  /* petal */
    tl[2] = P5551(200, 150, 24, 1);  /* petal shade */
    tl[3] = P5551(120, 72, 16, 1);   /* centre */
    for (int i = 4; i < 256; i++) tl[i] = tl[1];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const float dx = x - 15.5f, dy = y - 15.5f, r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
            const float pr = 9.0f + 6.0f * cosf(5.0f * a); /* five petals */
            ci[y * 32 + x] = r < 3.5f ? 3 : r < pr ? (r > pr - 2.5f ? 2 : 1) : 0;
        }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) fence[y * 32 + x] = (x % 4 == 3 || y < 2 || y > 29) ? P5551(96, 64, 32, 0) /* a slit */
                                                           : P5551(160, 120, 72, 1);
    for (int i = 0; i < 32 * 32; i++) { const uint32_t h = (uint32_t)i * 2654435761u; grass[i] = P5551(40 + (h >> 27), 88 + ((h >> 22) & 31), 24, 1); }
    char path[512];
    for (int pass = 0; pass < 5; pass++) { /* 0,1: the orders; 2: n64 colour, 3: n64 alpha, 4: ground alone */
        sArenaPos = ARENA;
        Gfx *dl = alloc(4000 * sizeof(Gfx)), *g = dl;
        Vp *vp = alloc(sizeof(Vp));
        vp->vp.vscale[0] = 640; vp->vp.vscale[1] = 480; vp->vp.vscale[2] = 511; vp->vp.vscale[3] = 0;
        vp->vp.vtrans[0] = 640; vp->vp.vtrans[1] = 480; vp->vp.vtrans[2] = 511; vp->vp.vtrans[3] = 0;
        gSPViewport(g++, vp);
        gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
        void *zimg = alloc(320 * 240 * 2), *cimg = alloc(320 * 240 * 2);
        gDPSetDepthImage(g++, zimg);
        gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, cimg);
        uint8_t *ci_a = alloc(sizeof(ci)); memcpy(ci_a, ci, sizeof(ci));
        uint16_t *tl_a = alloc(sizeof(tl)); memcpy(tl_a, tl, sizeof(tl));
        uint16_t *fence_a = alloc(sizeof(fence)); memcpy(fence_a, fence, sizeof(fence));
        uint16_t *grass_a = alloc(sizeof(grass)); memcpy(grass_a, grass, sizeof(grass));
        gDPPipeSync(g++);
        gDPSetCycleType(g++, G_CYC_FILL); /* the frame's clear: the fog colour (OoT's Gfx_SetupFrame) */
        gDPSetFillColor(g++, (pass == 3 ? 0u : (uint32_t)P5551N(200, 200, 150, 1)) * 0x10001u);
        gDPFillRectangle(g++, 0, 0, 319, 239);
        float P[4][4], V[4][4];
        persp(P, 60, 4.0f / 3.0f, 10, 12800);
        int32_t *pm = alloc(64); f2l(P, pm);
        gSPMatrix(g++, pm, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
        memset(V, 0, 64); V[0][0] = V[1][1] = V[2][2] = V[3][3] = 1; V[3][0] = (float)-shot;
        int32_t *mv = alloc(64); f2l(V, mv);
        gSPMatrix(g++, mv, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
        Vtx *gv = alloc(sizeof(Vtx) * 4), *fv = alloc(sizeof(Vtx) * 4), *ev = alloc(sizeof(Vtx) * 4);
        const short gx[4] = { -2000, 2000, 2000, -2000 }, gz[4] = { -12, -12, -3000, -3000 };
        for (int k = 0; k < 4; k++) {
            memset(&gv[k], 0, sizeof(Vtx)); gv[k].v.ob[0] = gx[k]; gv[k].v.ob[1] = -40; gv[k].v.ob[2] = gz[k];
            gv[k].v.tc[0] = (short)((gx[k] / 8) << 5); gv[k].v.tc[1] = (short)((gz[k] / 8) << 5);
            gv[k].v.cn[0] = gv[k].v.cn[1] = gv[k].v.cn[2] = gv[k].v.cn[3] = 255;
        }
        const short fx[4] = { -62, -2, -2, -62 }, fy[4] = { -40, -40, 20, 20 };
        const short ex[4] = { 6, 66, 66, 6 }, ey[4] = { -30, -30, 30, 30 };
        for (int k = 0; k < 4; k++) {
            memset(&fv[k], 0, sizeof(Vtx)); fv[k].v.ob[0] = fx[k]; fv[k].v.ob[1] = fy[k]; fv[k].v.ob[2] = -110;
            fv[k].v.tc[0] = (short)(k == 1 || k == 2 ? 32 << 5 : 0); fv[k].v.tc[1] = (short)(k >= 2 ? 0 : 32 << 5);
            fv[k].v.cn[0] = fv[k].v.cn[1] = fv[k].v.cn[2] = fv[k].v.cn[3] = 255;
            ev[k] = fv[k]; ev[k].v.ob[0] = ex[k]; ev[k].v.ob[1] = ey[k]; ev[k].v.ob[2] = -200;
        }
        gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        gDPSetTextureFilter(g++, G_TF_BILERP);
        for (int step = 0; step < 2; step++) {
            const int ground = (pass == 1) ? step == 0 : (pass == 4 || pass < 2) ? step == 1 || pass == 4 : 0;
            if (ground) {
                if (pass == 4 && step == 1) break;
                gDPPipeSync(g++);
                gDPSetCycleType(g++, G_CYC_1CYCLE);
                gSPLoadGeometryMode(g++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
                gDPSetRenderMode(g++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
                gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
                gDPSetTextureLUT(g++, G_TT_NONE);
                gDPLoadTextureBlock(g++, grass_a, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
                gSPVertex(g++, gv, 4, 0);
                gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
                continue;
            }
            if (pass == 4) continue;
            /* the cut-outs */
            gDPPipeSync(g++);
            gSPLoadGeometryMode(g++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
            if (pass < 2) {
                gDPSetCycleType(g++, G_CYC_2CYCLE);
                gDPSetRenderMode(g++, G_RM_PASS, G_RM_AA_ZB_TEX_EDGE2);
                gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_PASS2);
            } else {
                gDPSetCycleType(g++, G_CYC_1CYCLE);
                gSPLoadGeometryMode(g++, G_SHADE | G_SHADING_SMOOTH);
                gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
                if (pass == 2) gDPSetCombineLERP(g++, 0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1);
                else { /* the alpha as a grey: white blended over black by it */
                    gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
                    gDPSetPrimColor(g++, 0, 0, 255, 255, 255, 255);
                    gDPSetCombineLERP(g++, 0, 0, 0, PRIMITIVE, 0, 0, 0, TEXEL0, 0, 0, 0, PRIMITIVE, 0, 0, 0, TEXEL0);
                }
            }
            gDPLoadTLUT_pal256(g++, tl_a);
            gDPSetTextureLUT(g++, G_TT_RGBA16);
            gDPLoadTextureBlock(g++, ci_a, G_IM_FMT_CI, G_IM_SIZ_8b, 32, 32, 0, G_TX_CLAMP, G_TX_CLAMP, 5, 5, 0, 0);
            gSPVertex(g++, fv, 4, 0);
            gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
            gDPPipeSync(g++);
            gDPSetTextureLUT(g++, G_TT_NONE);
            gDPLoadTextureBlock(g++, fence_a, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
            gSPVertex(g++, ev, 4, 0);
            gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
        }
        gSPEndDisplayList(g++);
        gfx_start_frame();
        gfx_run(dl);
        gfx_end_frame();
        int w = 0, h = 0;
        const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
        static const char *names[5] = { "edge_0", "edge_1", "n64_c", "n64_a", "n64_g" };
        snprintf(path, sizeof(path), "%s/%s.ppm", dir, names[pass]);
        FILE *f = fopen(path, "wb");
        if (!f) return 1;
        fprintf(f, "P6 %d %d 255\n", w, h);
        for (int i = 0; i < w * h; i++) { const uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] }; fwrite(px, 1, 3, f); }
        fclose(f);
    }
#undef P5551
#undef P5551N
    printf("edge: wrote %s/edge_0.ppm edge_1.ppm n64_c.ppm n64_a.ppm n64_g.ppm\n", dir);
    return 0;
}

/* ================================================================== nearw: the near-eye clip against the N64 ==== */
/* dl_test nearw [seed] [n] (2026-10-04, OOT_NEARW): n random triangles passing the eye (OoT's camera: guPerspective
 * 60 deg, 4:3, near 10, far 12800; vertices from 3 units behind the eye to 40 in front, half of them within 4; side
 * extents around the guard band; clip ratio 1 or 2), through the port's own decisions -- the vertex flags
 * (vtx_clip_flags), the shared-flag reject and the clipper's trigger of gfx_sp_tri1, gfx_clip_single_vert and its fan
 * -- against the N64's NoN rule (F3DZEX2 NoN: drawn where w > 0 and z <= w; the guard band only trims what is off the
 * screen). 48 points a triangle (barycentric, away from its own edges); each on the screen and in front of the eye is
 * drawn by the N64 or not, and by the port when it lies in one of the triangles the port emits (projected; within an
 * eighth of a pixel, NEARW_EPS: a sliver's clipped edge moves with float rounding). Points
 * within 1e-4 of a plane are left out. Prints the disagreements nearer than W_MIN (the sliver the port gives up:
 * w < 1/64 with OOT_NEARW, w < 2 without) and beyond it (must be 0); exit 1 on any beyond. */
/* nearw: the point (px, py) inside the projected triangle or within NEARW_EPS of it (NDC; 1/1024 is an eighth of a
 * pixel at 240): the clipper's vertices are floats, and a sliver's edge moves by their rounding */
#define NEARW_EPS (1.0 / 1024.0)
static bool in_tri_ndc(double px, double py, double x0, double y0, double x1, double y1, double x2, double y2) {
    const double a = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (a == 0.0) return false;
    const double sg = a > 0.0 ? 1.0 : -1.0, vx[3] = { x0, x1, x2 }, vy[3] = { y0, y1, y2 };
    for (int e = 0; e < 3; e++) {
        const double ax = vx[e], ay = vy[e], bx = vx[(e + 1) % 3], by = vy[(e + 1) % 3];
        const double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        if (len == 0.0) return false;
        if (sg * ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / len < -NEARW_EPS) return false;
    }
    return true;
}
static int nearw_mode(int seed, int n) {
    float P[4][4];
    persp(P, 60.0f, 4.0f / 3.0f, 10.0f, 12800.0f);
    sNoN = 1; sNearClip = 1; sClipEdges = 2;
    const float wmin = W_MIN;
    rs = (uint32_t)seed * 2654435761u + 7;
    long tris = 0, clipped = 0, rejected = 0, samples = 0, n64 = 0, near_bad = 0, far_bad = 0, near_pts = 0;
    for (int it = 0; it < n; it++) {
        gfx_clip_ratio = 1 + irnd(2);
        const float r = (float)gfx_clip_ratio;
        struct LoadedVertex v[3];
        memset(v, 0, sizeof(v));
        for (int k = 0; k < 3; k++) {
            const float w = irnd(2) ? frnd(-3.0f, 4.0f) : frnd(-3.0f, 40.0f), ze = -w;
            const float s = r * (fabsf(ze) + 0.5f) * (4.0f / 3.0f) / P[1][1];
            const float e[4] = { frnd(-1.4f, 1.4f) * s, frnd(-1.4f, 1.4f) * s * 0.75f, ze, 1.0f };
            float c[4];
            for (int j = 0; j < 4; j++) c[j] = e[0] * P[0][j] + e[1] * P[1][j] + e[2] * P[2][j] + e[3] * P[3][j];
            v[k].x = e[0]; v[k].y = e[1]; v[k].z = e[2]; v[k].w = 1.0f;
            v[k]._x = c[0]; v[k]._y = c[1]; v[k]._z = c[2]; v[k]._w = c[3];
            v[k].u = frnd(0, 64); v[k].v = frnd(0, 64);
            v[k].clip_rej = vtx_clip_flags(c[0], c[1], c[2], c[3], r, gfx_non_on(), gfx_nearclip_on(), 0);
        }
        tris++;
        /* the port's triangles: gfx_sp_tri1's shared-flag reject, then its trigger and the clipper */
        static struct LoadedVertex out[32];
        size_t nout = 0;
        struct LoadedVertex *va[3] = { &v[0], &v[1], &v[2] };
        if (v[0].clip_rej & v[1].clip_rej & v[2].clip_rej & ~(XY_OUT | GB_OUT)) { rejected++; nout = 0; }
        else if ((v[0].clip_rej | v[1].clip_rej | v[2].clip_rej) & (Z_POS | Z_NEG | GB_OUT)) {
            gfx_clip_single_vert(out, &nout, va); clipped++;
        } else { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; nout = 3; }
        float sx[32], sy[32];
        for (size_t i = 0; i < nout; i++) { sx[i] = out[i]._x / out[i]._w; sy[i] = out[i]._y / out[i]._w; }
        for (int m = 0; m < 48; m++) {
            float a = frnd(0.002f, 1.0f), b = frnd(0.002f, 1.0f), c = frnd(0.002f, 1.0f);
            const float t = a + b + c; a /= t; b /= t; c /= t;
            if (a < 0.001f || b < 0.001f || c < 0.001f) continue;
            float p[4];
            for (int j = 0; j < 4; j++) p[j] = a * (&v[0]._x)[j] + b * (&v[1]._x)[j] + c * (&v[2]._x)[j];
            const float x = p[0], y = p[1], z = p[2], w = p[3];
            if (!(w > 0.0f)) continue;
            const float eps = 1e-4f * (w > 1.0f ? w : 1.0f);
            if (fabsf(x) > w - eps || fabsf(y) > w - eps) continue;                /* off the screen (or on its edge) */
            if (fabsf(z - w) < eps || fabsf(w - wmin) < 1e-4f * wmin || fabsf(w - 2.0f) < 2e-4f) continue; /* on a plane */
            samples++;
            const bool nd = z <= w;                                                 /* the N64 (NoN): w > 0, z <= w */
            n64 += nd;
            bool pd = false;
            const float nx = x / w, ny = y / w;
            for (size_t i = 0; i + 2 < nout && !pd; i += 3) {
                const float x0 = sx[i], y0 = sy[i], x1 = sx[i + 1], y1 = sy[i + 1], x2 = sx[i + 2], y2 = sy[i + 2];
                pd = in_tri_ndc(nx, ny, x0, y0, x1, y1, x2, y2);
            }
            if (w < 2.0f) near_pts++;
            if (pd != nd) { if (w < wmin) near_bad++; else { if (far_bad < 5) printf("  tri %d: point w %g z %g x %g y %g: N64 %d, port %d\n", it, w, z, x, y, nd, pd); far_bad++; } }
        }
    }
    printf("nearw: OOT_NEARW=%d, W_MIN %g: %ld triangles (%ld clipped, %ld rejected), %ld points on the screen in front of the "
           "eye (%ld within 2 units), the N64 draws %ld; the port differs at %ld nearer than W_MIN, %ld beyond\n",
           gfx_soft_nearw_on(), wmin, tris, clipped, rejected, samples, near_pts, n64, near_bad, far_bad);
    return far_bad != 0;
}

int main(int argc, char **argv) {
    setenv("OOT_HALFWATER", "0", 1); /* the renderer at full resolution: the water layer is ARM-only and has its own test (halfwater_test) */
    if (mmap(ARENA, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)ARENA) {
        fprintf(stderr, "mmap at %p failed\n", (void *)ARENA);
        return 2;
    }
    const char *mode = argc > 1 ? argv[1] : "vtx";
    const int seed = argc > 2 ? atoi(argv[2]) : 1, n = argc > 3 ? atoi(argv[3]) : 2000;
    sH = 0xcbf29ce484222325ull;
    if (!strcmp(mode, "vtx")) return vtx_mode(seed, n);
    if (!strcmp(mode, "dl")) return dl_mode(seed, n);
    if (!strcmp(mode, "vc")) return vc_mode(seed, n);
    if (!strcmp(mode, "flags")) return flags_mode(seed, n);
    if (!strcmp(mode, "seam")) return seam_mode(seed, n);
    if (!strcmp(mode, "nearw")) return nearw_mode(seed, n);
    if (!strcmp(mode, "edge")) return edge_mode(argc > 2 ? argv[2] : ".", argc > 3 ? atoi(argv[3]) : 0);
    if (!strcmp(mode, "vbench")) return vbench_mode(seed, argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 1000);
    fprintf(stderr, "usage: dl_test vtx|dl [seed] [n]\n");
    return 2;
}
