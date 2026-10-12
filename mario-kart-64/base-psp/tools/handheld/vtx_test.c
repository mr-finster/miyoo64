/* vtx_test.c: MK64_VTXFAST (2026-10-04, the final speed sweep) through the real renderer:
 * includes base-psp/src/port/gfx/gfx_pc.c (its statics reachable) and links src/port/sdl/gfx_soft.c and
 * src/port/gfx/gfx_cc.c as built for the game; no ROM, no SDL.
 *
 *   vtx_test <seed> <frames> <w> <h> [int_w int_h]
 *
 * Builds race-like display lists every frame and runs them through gfx_run, as the game's frames do: the sky
 * gradient (ortho, before any 3D), a perspective projection with a lookAt multiplied in (G_MTX_PROJECTION
 * | G_MTX_MUL, as MK64), the course as textured meshes of shared vertices (32-vertex loads, G_TRI2 strips, back
 * faces culled, half of them fogged in 2-cycle mode, some with the LOD fraction in the combiner (it reads the
 * view-space depth), meshes reaching behind the eye and past every side so the clipper runs), objects under pushed modelview matrices (some lit, with normals; some blended), a second
 * viewport for part of the frame and back (the cache's generation), and a race HUD pass (HUD tags, ortho
 * textured quads: the HUD placement rewrites clip x after the load). Prints one hash of every float, colour and
 * count gfx_pc hands the rasterizer (draw_triangles) and one of every finished frame's pixels (the native
 * framebuffer after the composite), plus totals. MK64_VTXFAST chooses the path (0, 1, ab: VT_ABEVERY=<n> flips
 * the A/B phase every n frames). The two hashes must not depend on the switch, and must equal the tree before it
 * (tools/handheld/vtx_test.sh builds this against both). */
#include "gfx_pc.c"
#include <stdarg.h>
#include <PR/mbi.h> /* _SHIFTL for the gbi macros */

/* what gfx_pc.c and gfx_soft.c reach outside the renderer */
s32 gGamestate = 4; /* racing: mp_update's unsquished 3D runs as in a race */
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
char _ftext[16], _fbss[16], __assets_start[16], __assets_end[16];
u32 gPortProfFrame[PROF_COUNT];
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
void port_gfx_overlay(void) {}
void *port_seg_to_ptr(uintptr_t a) { return (void *)a; }
u32 port_time_us(void) { return 0; }
int gPortAbPhase = 1;
int port_switch_mode(const char *env, int cfg, int dflt) {
    const char *e = getenv(env);
    if (e && e[0]) {
        if (e[0] == 'a') return 2;
        if (e[0] == '0' || e[0] == '1') return e[0] - '0';
    }
    return cfg >= 0 ? cfg : dflt;
}
int handheld_cfg_unsquish3d(void) { return 1; }
int handheld_cfg_nearw(void) { return -1; }
int handheld_cfg_fillrule(void) { return -1; }
int handheld_cfg_hud(void) { return -1; }
int handheld_cfg_neon(void) { return -1; }
int handheld_cfg_vtxfast(void) { return -1; }
int handheld_cfg_spancvt(void) { return -1; }

extern struct GfxRenderingAPI gfx_soft_api;
extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);

static uint32_t sW = 240, sH = 240;
static void wm_init(const char *n, bool f) { (void)n; (void)f; }
static void wm_kb(bool (*a)(int), bool (*b)(int), void (*c)(void)) { (void)a; (void)b; (void)c; }
static void wm_fsc(void (*a)(bool)) { (void)a; }
static void wm_fs(bool e) { (void)e; }
static void wm_loop(void (*r)(void)) { r(); }
static void wm_dim(uint32_t *w, uint32_t *h) { *w = sW; *h = sH; }
static void wm_ev(void) {}
static bool wm_start(void) { return true; }
static void wm_swap(void) {}
static double wm_time(void) { return 0.0; }
static struct GfxWindowManagerAPI sWm = { wm_init, wm_kb, wm_fsc, wm_fs, wm_loop, wm_dim, wm_ev, wm_start, wm_swap, wm_swap, wm_time };

/* every buffer gfx_pc hands the rasterizer, bit for bit */
static uint32_t sTriHash = 2166136261u;
static uint64_t sTris, sBatches;
static void (*sRealDraw)(float *, size_t, size_t);
static void hash_words(uint32_t *h, const void *p, size_t n) {
    const uint32_t *w = (const uint32_t *)p;
    for (size_t i = 0; i < n / 4; i++) *h = (*h ^ w[i]) * 16777619u;
}
static void draw_wrap(float buf[], size_t len, size_t ntris) {
    hash_words(&sTriHash, buf, len);
    hash_words(&sTriHash, &ntris, sizeof(ntris));
    sTris += ntris;
    sBatches++;
    sRealDraw(buf, len, ntris);
}

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

/* sine and cosine by a fixed polynomial, so the scenes are bit for bit the same on ARM (musl) and x86 (glibc) */
static float vsin(float x) {
    while (x > 3.14159265f) x -= 6.28318531f;
    while (x < -3.14159265f) x += 6.28318531f;
    const float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}
static float vcos(float x) { return vsin(x + 1.57079633f); }
/* guMtxF2L's s15.16 layout */
static void f2l(Mtx *out, const float m[4][4]) {
    int32_t *o = (int32_t *)out;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j += 2) {
        const int32_t a = (int32_t)(m[i][j] * 65536.0f), b = (int32_t)(m[i][j + 1] * 65536.0f);
        o[i * 2 + j / 2] = (int32_t)(((uint32_t)a & 0xffff0000u) | ((uint32_t)b >> 16));
        o[8 + i * 2 + j / 2] = (int32_t)(((uint32_t)a << 16) | ((uint32_t)b & 0xffffu));
    }
}
static void persp(float m[4][4], float fovy, float aspect, float n, float f) {
    const float h = fovy * 3.14159265358979f / 360.0f, c = vcos(h) / vsin(h);
    memset(m, 0, 64);
    m[0][0] = c / aspect; m[1][1] = c; m[2][2] = (n + f) / (n - f); m[2][3] = -1; m[3][2] = 2 * n * f / (n - f);
}
static void ortho(float m[4][4], float l, float r, float b, float t, float n, float f) {
    memset(m, 0, 64);
    m[0][0] = 2 / (r - l); m[1][1] = 2 / (t - b); m[2][2] = -2 / (f - n);
    m[3][0] = -(r + l) / (r - l); m[3][1] = -(t + b) / (t - b); m[3][2] = -(f + n) / (f - n); m[3][3] = 1;
}
static float sEye[3], sFwd[3];
static void lookat(float m[4][4]) { /* a rigid view: the camera somewhere on a course, looking roughly level */
    const float a = frnd(-3.2f, 3.2f), b = frnd(-0.35f, 0.15f);
    sEye[0] = frnd(-2000, 2000); sEye[1] = frnd(0, 300); sEye[2] = frnd(-2000, 2000);
    const float fx = vcos(b) * vsin(a), fy = vsin(b), fz = vcos(b) * vcos(a);
    sFwd[0] = fx; sFwd[1] = fy; sFwd[2] = fz;
    float rx = fz, ry = 0, rz = -fx; const float rl = sqrtf(rx * rx + rz * rz); rx /= rl; rz /= rl;
    const float ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
    const float r[3][3] = { { rx, ux, -fx }, { ry, uy, -fy }, { rz, uz, -fz } };
    memset(m, 0, 64);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = r[i][j];
    m[3][0] = -(sEye[0] * rx + sEye[1] * ry + sEye[2] * rz); m[3][1] = -(sEye[0] * ux + sEye[1] * uy + sEye[2] * uz);
    m[3][2] = sEye[0] * fx + sEye[1] * fy + sEye[2] * fz; m[3][3] = 1;
}
static void model(float m[4][4], float x, float y, float z, float ang, float sc) {
    memset(m, 0, 64);
    m[0][0] = vcos(ang) * sc; m[0][2] = -vsin(ang) * sc; m[1][1] = sc; m[2][0] = vsin(ang) * sc; m[2][2] = vcos(ang) * sc;
    m[3][0] = x; m[3][1] = y; m[3][2] = z; m[3][3] = 1;
}

/* per-frame pools: display list, matrices, vertices, viewports */
#define DL_MAX 65536
static Gfx sDl[DL_MAX];
static Mtx sMtx[512];
static Vtx sVtx[64 * 1024];
static Vp sVp[4];
static uint16_t sTex[4][32 * 32] __attribute__((aligned(8)));
static Lights1 sLight;
static int sNM, sNV;

static Mtx *mtx_of(const float m[4][4]) { f2l(&sMtx[sNM], m); return &sMtx[sNM++]; }
static void vp_set(Vp *v, int x, int y, int w, int h) { /* the N64's viewport in quarter pixels (320x240 layout) */
    v->vp.vscale[0] = (short)(w * 2); v->vp.vscale[1] = (short)(h * 2); v->vp.vscale[2] = G_MAXZ / 2; v->vp.vscale[3] = 0;
    v->vp.vtrans[0] = (short)((x + w / 2) * 4); v->vp.vtrans[1] = (short)((y + h / 2) * 4); v->vp.vtrans[2] = G_MAXZ / 2; v->vp.vtrans[3] = 0;
}

/* a mesh of 8 x 4 shared vertices (one 32-vertex load) on a random plane around a point, 42 triangles */
static Gfx *mesh(Gfx *p, float cx, float cy, float cz, float size, int lit, int tile_uv) {
    Vtx *v = &sVtx[sNV];
    const float ax = frnd(-1, 1), ay = frnd(-0.3f, 0.3f), az = frnd(-1, 1), bx = frnd(-1, 1), by = frnd(-1, 1), bz = frnd(-1, 1);
    for (int j = 0; j < 4; j++) for (int i = 0; i < 8; i++) {
        Vtx *o = &v[j * 8 + i];
        const float s = (i / 7.0f - 0.5f) * size, t = (j / 3.0f - 0.5f) * size * 0.6f;
        float x = cx + ax * s + bx * t + frnd(-3, 3), y = cy + ay * s + by * t * 0.3f, z = cz + az * s + bz * t;
        if (x > 32000) x = 32000; if (x < -32000) x = -32000; if (z > 32000) z = 32000; if (z < -32000) z = -32000;
        o->v.ob[0] = (short)x; o->v.ob[1] = (short)y; o->v.ob[2] = (short)z; o->v.flag = 0;
        o->v.tc[0] = (short)(i * (tile_uv ? 512 : 128) + (rnd() & 31)); o->v.tc[1] = (short)(j * (tile_uv ? 512 : 128));
        if (lit) { o->n.n[0] = (signed char)(rnd() % 255 - 127); o->n.n[1] = (signed char)(rnd() % 127); o->n.n[2] = (signed char)(rnd() % 255 - 127); o->n.a = 255; }
        else { o->v.cn[0] = rnd() & 255; o->v.cn[1] = rnd() & 255; o->v.cn[2] = rnd() & 255; o->v.cn[3] = 128 + (rnd() & 127); }
    }
    sNV += 32;
    gSPVertex(p++, v, 32, 0);
    for (int j = 0; j < 3; j++) for (int i = 0; i < 7; i++) {
        const int a = j * 8 + i;
        gSP2Triangles(p++, a, a + 1, a + 9, 0, a, a + 9, a + 8, 0);
    }
    return p;
}

static Gfx *frame_dl(int frame) {
    Gfx *p = sDl;
    float P[4][4], L[4][4], M[4][4], O[4][4];
    sNM = 0; sNV = 0;
    vp_set(&sVp[0], 0, 0, 320, 240);
    vp_set(&sVp[1], 40, 30, 240, 180);
    gSPViewport(p++, &sVp[0]);
    gDPPipeSync(p++);
    gDPSetCycleType(p++, G_CYC_1CYCLE);
    gSPClearGeometryMode(p++, 0xFFFFFFFF);
    gSPPerspNormalize(p++, 0xFFFF);
    /* the sky gradient: ortho, shaded, before any 3D (gfx_pc puts it on the 3D target after a 3D frame) */
    ortho(O, 0, 320, 0, 240, -1, 1);
    gSPMatrix(p++, mtx_of(O), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(p++, mtx_of(identity_matrix), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gDPSetRenderMode(p++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCombineMode(p++, G_CC_SHADE, G_CC_SHADE);
    gSPSetGeometryMode(p++, G_SHADE | G_SHADING_SMOOTH);
    {
        Vtx *v = &sVtx[sNV]; sNV += 4;
        const short xy[4][2] = { { 0, 0 }, { 320, 0 }, { 320, 240 }, { 0, 240 } };
        for (int k = 0; k < 4; k++) {
            v[k].v.ob[0] = xy[k][0]; v[k].v.ob[1] = xy[k][1]; v[k].v.ob[2] = 0; v[k].v.flag = 0; v[k].v.tc[0] = v[k].v.tc[1] = 0;
            v[k].v.cn[0] = 40 + frame % 50; v[k].v.cn[1] = k < 2 ? 200 : 90; v[k].v.cn[2] = 230; v[k].v.cn[3] = 255;
        }
        gSPVertex(p++, v, 4, 0);
        gSP2Triangles(p++, 0, 1, 2, 0, 0, 2, 3, 0);
    }
    /* the 3D: projection then the camera multiplied into it, as the game loads them */
    const float near = (rnd() & 3) == 0 ? 1.0f : (rnd() & 1) ? 2.0f : 9.0f;
    const float fovy = frnd(40, 60);
    persp(P, fovy, 4.0f / 3.0f, near, 4500.0f);
    lookat(L);
    gSPMatrix(p++, mtx_of(P), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(p++, mtx_of(L), G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
    gSPMatrix(p++, mtx_of(identity_matrix), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPTexture(p++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, 1);
    gDPSetTextureFilter(p++, G_TF_BILERP);
    for (int m = 0; m < 44; m++) {
        if (m == 30) gSPViewport(p++, &sVp[1]); /* a part of the frame in another viewport, then back */
        if (m == 34) gSPViewport(p++, &sVp[0]);
        const int fog = (m & 1) && m < 30, tile = m & 3;
        const float d = (m % 5 == 0) ? frnd(0, 40) : frnd(20, 2500), side = frnd(-1.2f, 1.2f) * (d + 50);
        const float cx = sEye[0] + sFwd[0] * d + sFwd[2] * side, cz = sEye[2] + sFwd[2] * d - sFwd[0] * side;
        gDPPipeSync(p++);
        gDPLoadTextureBlock(p++, sTex[tile], G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP | (m % 7 == 0 ? G_TX_MIRROR : 0),
                            G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
        if (fog) {
            gDPSetCycleType(p++, G_CYC_2CYCLE);
            gDPSetRenderMode(p++, G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2);
            gDPSetCombineMode(p++, G_CC_MODULATERGB, G_CC_PASS2);
            gSPSetGeometryMode(p++, G_FOG);
            gSPFogPosition(p++, 900, 1000);
            gDPSetFogColor(p++, 180, 200, 255, 255);
        } else {
            gDPSetCycleType(p++, G_CYC_1CYCLE);
            gDPSetRenderMode(p++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
            if (m % 4 == 2) { /* the LOD fraction in the combiner: it reads the view-space depth (d->w, gfx_sp_tri1) */
                gDPSetPrimColor(p++, 0, 0, 255, 40, 20, 255);
                gDPSetCombineLERP(p++, PRIMITIVE, SHADE, LOD_FRACTION, SHADE, 0, 0, 0, SHADE, PRIMITIVE, SHADE, LOD_FRACTION,
                                  SHADE, 0, 0, 0, SHADE);
            } else {
                gDPSetCombineMode(p++, G_CC_MODULATERGB, G_CC_MODULATERGB);
            }
            gSPClearGeometryMode(p++, G_FOG);
        }
        gSPSetGeometryMode(p++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH | ((m % 3) ? G_CULL_BACK : 0));
        const float cy = frnd(-20, 40), size = frnd(60, 600); /* one random draw at a time: argument order is the compiler's */
        p = mesh(p, cx, cy, cz, size, 0, m & 1);
    }
    gSPClearGeometryMode(p++, G_FOG);
    gDPSetCycleType(p++, G_CYC_1CYCLE);
    /* objects under their own modelview: lit ones, blended ones */
    for (int o = 0; o < 10; o++) {
        const float d = frnd(5, 600), side = frnd(-0.8f, 0.8f) * d;
        const float oy = frnd(0, 30), ang = frnd(0, 6.28f), sc = frnd(0.2f, 2.0f);
        model(M, sEye[0] + sFwd[0] * d + sFwd[2] * side, oy, sEye[2] + sFwd[2] * d - sFwd[0] * side, ang, sc);
        gSPMatrix(p++, mtx_of(M), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
        gDPPipeSync(p++);
        if (o & 1) {
            gSPSetLights1(p++, sLight);
            gSPSetGeometryMode(p++, G_LIGHTING);
            gDPSetRenderMode(p++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
            gDPSetCombineMode(p++, G_CC_SHADE, G_CC_SHADE);
        } else {
            gSPClearGeometryMode(p++, G_LIGHTING);
            gDPSetRenderMode(p++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
            gDPSetCombineMode(p++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
        }
        const float osize = frnd(5, 40);
        p = mesh(p, 0, 0, 0, osize, o & 1, 0);
        gSPPopMatrix(p++, G_MTX_MODELVIEW);
    }
    gSPClearGeometryMode(p++, G_LIGHTING | G_CULL_BACK | G_ZBUFFER);
    /* the race HUD: tags, ortho textured quads (gfx_pc places them and rewrites their clip x) */
    gDPNoOpTag(p++, PORT_HUD_TAG_ON);
    gSPMatrix(p++, mtx_of(O), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(p++, mtx_of(identity_matrix), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gDPSetRenderMode(p++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(p++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    gDPSetTextureFilter(p++, G_TF_POINT);
    gDPLoadTextureBlock(p++, sTex[3], G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_CLAMP, G_TX_CLAMP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    for (int h = 0; h < 6; h++) {
        Vtx *v = &sVtx[sNV]; sNV += 4;
        const short x0 = (short)(h < 3 ? 10 + h * 36 : 210 + (h - 3) * 36), y0 = (short)(h & 1 ? 20 : 190);
        const short xy[4][2] = { { x0, y0 }, { x0 + 32, y0 }, { x0 + 32, y0 + 32 }, { x0, y0 + 32 } };
        const short st[4][2] = { { 0, 0 }, { 32 << 6, 0 }, { 32 << 6, 32 << 6 }, { 0, 32 << 6 } };
        for (int k = 0; k < 4; k++) {
            v[k].v.ob[0] = xy[k][0]; v[k].v.ob[1] = xy[k][1]; v[k].v.ob[2] = 0; v[k].v.flag = 0;
            v[k].v.tc[0] = st[k][0]; v[k].v.tc[1] = st[k][1];
            v[k].v.cn[0] = v[k].v.cn[1] = v[k].v.cn[2] = 255; v[k].v.cn[3] = 200;
        }
        gSPVertex(p++, v, 4, 0);
        gSP2Triangles(p++, 0, 1, 2, 0, 0, 2, 3, 0);
    }
    gDPNoOpTag(p++, PORT_HUD_TAG_OFF);
    gSPEndDisplayList(p++);
    if (p - sDl > DL_MAX) { fprintf(stderr, "display list overflow\n"); exit(2); }
    return sDl;
}

int main(int argc, char **argv) {
    const int seed = argc > 1 ? atoi(argv[1]) : 1, frames = argc > 2 ? atoi(argv[2]) : 100;
    sW = argc > 3 ? (uint32_t)atoi(argv[3]) : 240; sH = argc > 4 ? (uint32_t)atoi(argv[4]) : 240;
    const int iw = argc > 5 ? atoi(argv[5]) : (int)sW, ih = argc > 6 ? atoi(argv[6]) : (int)sH;
    const char *abe = getenv("VT_ABEVERY");
    const int abevery = abe ? atoi(abe) : 0;
    rs = (uint32_t)seed * 2654435761u + 7;
    for (int t = 0; t < 4; t++) for (int i = 0; i < 32 * 32; i++) sTex[t][i] = (uint16_t)(rnd() | 1);
    sLight = (Lights1)gdSPDefLights1(60, 50, 40, 250, 240, 200, 40, 90, 60);
    static struct GfxRenderingAPI api;
    api = gfx_soft_api;
    sRealDraw = api.draw_triangles;
    api.draw_triangles = draw_wrap;
    gfx_soft_set_render_res((int)sW, (int)sH, iw, ih);
    gfx_init(&sWm, &api, "vtx_test", false);
    uint32_t fbh = 2166136261u;
    for (int f = 0; f < frames; f++) {
        if (abevery > 0 && f % abevery == 0) gPortAbPhase ^= 1;
        Gfx *dl = frame_dl(f);
        gfx_start_frame();
        gfx_run(dl);
        gfx_end_frame();
        int w = 0, h = 0;
        const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
        hash_words(&fbh, fb, (size_t)w * h * 4);
        if (getenv("VT_DUMP")) { /* VT_DUMP=<file>: the last frame's pixels, raw (debug) */
            FILE *df = fopen(getenv("VT_DUMP"), "wb");
            if (df) { fwrite(fb, 4, (size_t)w * h, df); fclose(df); }
        }
    }
    printf("vtx_test seed %d frames %d screen %ux%u 3d %dx%d: %llu triangles in %llu batches; draw hash %08x, frame hash %08x\n",
           seed, frames, sW, sH, iw, ih, (unsigned long long)sTris, (unsigned long long)sBatches, sTriHash, fbh);
    return 0;
}
