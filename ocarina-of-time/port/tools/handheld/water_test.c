/* water_test.c: the Deku Tree basement water's draws through the real rasterizer, variant against variant, byte for
 * byte (2026-10-01, the basement water audit). Includes src/port/hh/gfx/gfx_soft.c and drives raster_tri through
 * gfx_soft_draw_triangles with crafted triangles in the water's mode -- a "grain" layer (32x32, the detail tile, fog,
 * constant combiner alpha fga 127, blended, no z write, every triangle doubled as the G_TRI2 fold flags it) then a
 * "plain" layer on the same positions -- and, for the guards and the opaque rows, the modes around it (opaque fogged
 * or not with z writes, 16-bit and expanded textures, clamps, nearest, decals, texture-edge alpha, other alphas).
 * cm_row_neon / cm_row_lean on ARM, the scalar span loops on x86. Build and run: tools/handheld/water_test.sh.
 *
 *   water_test <mode> [seed] [frames]
 *   mode = cmp      : every variant below over random frames; prints mismatches (exit 1 on any)
 *          bench:<v>: one variant only (instruction counts under qemu: tools/qprof/qlogcount.py)
 *   env  : H_PURE=1   only the water's own mode (two layers, fga 127, every triangle doubled)
 *          H_OPAQUE=1 only opaque frames (fogged or not, z written); H_P32 / H_P16: the plain texture 32x32 32-bit /
 *                     64x128 16-bit
 *          H_LAYER=g|p only the grain (detail) layer / only the plain layer
 *          H_ZPASS=1  every pixel passes the depth test
 *          H_PSHADE0=1 OOT_PSHADE=0;  H_FAST=1 OOT_FASTWATER=1 (+ H_FWTINT=<n>)
 *
 * Variants (gfx_soft.c's static switches):
 *   ref   : NEON rows, constant-folded (CMSPEC), blend-twice fold      -- the device's exact path
 *   scal  : the scalar span loops (OOT_CMNEON=0), the copy drawn again  -- the reference path
 *   gen   : NEON generic rows (OOT_CMSPEC=0)
 *   dup   : each doubled triangle sent as two triangles (OOT_BLENDTWICE=0)
 *   wlean : OOT_WATERLEAN=1;  olean : OOT_OPAQUELEAN=1;  both : both
 * The frame hash (colour + depth of the first variant) is the same on ARM (NEON) and x86 (scalar) for a seed.
 * -DWT_NO_LEAN: for a tree before the lean rows. */
#ifndef WT_NO_LEAN
#define HAVE_LEAN 1
#define HAVE_OLEAN 1
#endif
#include "gfx_soft.c"

#include <stdarg.h>

uint32_t gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
bool gfx_depth_full_range(void) { return false; }
int gfx_depth_read_x(int x, int w, int h) { (void)h; return x * w / 320; }
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
static uint32_t gFB0[W * H], gFBr[W * H];
static uint16_t gZB0[W * H], gZBr[W * H];

typedef struct {
    int nt;                  /* triangles per layer */
    SoftVtx g[3 * 64], p[3 * 64]; /* the grain layer, the plain layer (same positions, own u/v and colours) */
    uint8_t twice[64];
    uint32_t tg[32 * 32], tp[64 * 128], td[64 * 64];
    int pw, ph, pfmt, pclamp;   /* the plain layer's texture: size, 0 = 8888, 1 = 5551; clamp bits (1 s, 2 t) */
    int dw, dh, es, et; int32_t offs, offt; uint16_t dw4[4];
    int fga, fogr, fogg, fogb;
    bool bilin, alpha, ztest, dmask, decal, fogpx, grain_detail, a_tex, a_in, edgeop;
    int scene;
} Frame;

static void make_frame(Frame *f) {
    memset(f, 0, sizeof(*f));
    const int pure = getenv("H_PURE") != NULL, opq = getenv("H_OPAQUE") != NULL;
    /* 0: the water's mode, 1: opaque (fogged or not), 2: anything */
    const int kind = pure ? 0 : opq ? 1 : (int)(rnd() % 3);
    f->nt = 1 + rnd() % 40;
    f->fga = kind == 2 ? (int)(rnd() % 256) : (rnd() % 4 ? 127 : (int)(rnd() % 256));
    if (rnd() % 16 == 0) f->fga = (rnd() & 1) ? 255 : (int)(rnd() % 8);
    if (kind == 2 && rnd() % 4 == 0) f->fga = -1;
    if (pure) f->fga = 127;
    f->fogr = rnd() & 255; f->fogg = rnd() & 255; f->fogb = rnd() & 255;
    f->bilin = kind == 2 ? (rnd() % 4 != 0) : 1;
    f->alpha = kind == 0 ? 1 : kind == 1 ? 0 : (rnd() & 1);
    f->ztest = kind == 2 ? (rnd() % 8 != 0) : 1;
    f->dmask = kind == 0 ? 0 : kind == 1 ? (rnd() % 8 != 0) : (rnd() & 1);
    f->decal = kind == 0 ? 0 : (rnd() % 6) == 0;
    f->fogpx = kind == 0 ? 1 : (rnd() % 4 != 0);
    if (kind == 1) f->fga = -1;
    f->a_tex = kind == 2 ? (rnd() & 1) : 0;
    f->a_in = kind == 2 ? (rnd() % 4 != 0) : 1;
    f->edgeop = kind == 2 ? (rnd() & 1) : 0;
    f->grain_detail = kind == 2 ? (rnd() % 4 != 0) : 1;
    f->scene = (rnd() % 4) ? -1 : 0;
    if (pure) f->scene = -1;
    f->pw = 32; f->ph = 32; f->pfmt = 0; f->pclamp = 0;
    if (!pure) {
        const int r = rnd() % 4;
        if (r == 1) { f->pw = 64; f->ph = 128; f->pfmt = 1; }      /* 16-bit, kept 16-bit (> TEX32_SMALL texels) */
        else if (r == 2) { f->pfmt = 1; }                          /* 16-bit, expanded at upload */
        if (rnd() % 6 == 0) f->pclamp = 1 + rnd() % 3;
    }
    if (getenv("H_P32")) { f->pw = 32; f->ph = 32; f->pfmt = 0; f->pclamp = 0; }
    if (getenv("H_P16")) { f->pw = 64; f->ph = 128; f->pfmt = 1; f->pclamp = 0; }
    for (int i = 0; i < 32 * 32; i++) f->tg[i] = rnd();
    for (int i = 0; i < f->pw * f->ph; i++) f->tp[i] = rnd();
    if (rnd() % 3 == 0) {
        for (int i = 0; i < 32 * 32; i++) f->tg[i] |= 0xFF000000u;
        for (int i = 0; i < f->pw * f->ph; i++) f->tp[i] |= 0xFF000000u;
    }
    f->dw = 1 << (3 + rnd() % 4); f->dh = 1 << (3 + rnd() % 4);
    for (int i = 0; i < f->dw * f->dh; i++) f->td[i] = rnd();
    f->es = (int)(rnd() % 5) - 1; f->et = (int)(rnd() % 5) - 1;
    if (f->es <= 0 && f->et <= 0) f->es = 1;
    f->offs = (int32_t)rnd(); f->offt = (int32_t)rnd();
    for (int k = 0; k < 4; k++) f->dw4[k] = (uint16_t)(rnd() % 257);
    if (rnd() & 1) { const uint16_t kk = (uint16_t)(rnd() % 256); f->dw4[0] = f->dw4[1] = f->dw4[2] = kk; }
    const int nofog = kind != 0 && rnd() % 5 == 0; /* every vertex alpha 0: no fog on the triangle (fga -1) */
    for (int t = 0; t < f->nt; t++) {
        const float cx = frnd(-40, 280), cy = frnd(-40, 280), sz = (rnd() % 4) ? frnd(2, 60) : frnd(60, 300);
        for (int k = 0; k < 3; k++) {
            SoftVtx *a = &f->g[t * 3 + k], *b = &f->p[t * 3 + k];
            a->x = cx + frnd(-sz, sz); a->y = cy + frnd(-sz, sz);
            a->z = (rnd() % 8) ? frnd(0.0f, 1.0f) : frnd(-0.2f, 1.2f);
            a->invw = (rnd() % 16) ? frnd(0.02f, 3.0f) : frnd(-0.5f, 0.02f);
            a->u = frnd(-6, 6); a->v = frnd(-6, 6);
            a->r = rnd(); a->g = rnd(); a->b = rnd(); a->a = nofog ? 0 : rnd();
            *b = *a;
            b->u = frnd(-6, 6); b->v = frnd(-6, 6);
            if (rnd() & 1) { b->r = rnd(); b->g = rnd(); b->b = rnd(); }
        }
        if (rnd() % 10 == 0) { /* a vertex nearly at the eye plane: huge c/w, saturating colour steps */
            const int k = rnd() % 3; f->g[t * 3 + k].invw = f->p[t * 3 + k].invw = frnd(1e-9f, 1e-4f);
        }
        if (rnd() % 10 == 0) { /* a sliver: the third vertex near the line through the other two */
            SoftVtx *a = &f->g[t * 3], *b = &f->g[t * 3 + 1], *c = &f->g[t * 3 + 2];
            const float k = frnd(-0.5f, 1.5f), e = frnd(-0.05f, 0.05f);
            c->x = a->x + (b->x - a->x) * k + e; c->y = a->y + (b->y - a->y) * k - e;
            f->p[t * 3 + 2].x = c->x; f->p[t * 3 + 2].y = c->y;
        }
        if (rnd() % 6 == 0) { /* flat */
            for (int k = 1; k < 3; k++) {
                f->g[t * 3 + k].r = f->g[t * 3].r; f->g[t * 3 + k].g = f->g[t * 3].g; f->g[t * 3 + k].b = f->g[t * 3].b; f->g[t * 3 + k].a = f->g[t * 3].a;
                f->p[t * 3 + k].r = f->p[t * 3].r; f->p[t * 3 + k].g = f->p[t * 3].g; f->p[t * 3 + k].b = f->p[t * 3].b; f->p[t * 3 + k].a = f->p[t * 3].a;
            }
        }
        f->twice[t] = (rnd() % 8) ? 1 : 0;
        if (pure) f->twice[t] = 1;
        if (kind == 1) f->twice[t] = 0;
    }
}

static struct ShaderProgram gProg;
static uint32_t gIdG, gIdP, gIdD;

static void upload(uint32_t id, const uint32_t *argb32, int w, int h, int fmt, int clamp) {
    static uint8_t buf[64 * 128 * 4];
    if (fmt == 0) {
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = (uint8_t)(argb32[i] >> 16); buf[i * 4 + 1] = (uint8_t)(argb32[i] >> 8);
            buf[i * 4 + 2] = (uint8_t)argb32[i]; buf[i * 4 + 3] = (uint8_t)(argb32[i] >> 24);
        }
    } else {
        uint16_t *b16 = (uint16_t *)buf;
        for (int i = 0; i < w * h; i++) b16[i] = (uint16_t)argb32[i];
    }
    gfx_soft_select_texture(0, id);
    gfx_soft_upload_texture(buf, w, h, fmt == 0 ? GU_PSM_8888 : GU_PSM_5551);
    gfx_soft_set_sampler_parameters(0, true, (clamp & 1) ? G_TX_CLAMP : 0, (clamp & 2) ? G_TX_CLAMP : 0);
}

enum { V_REF, V_SCAL, V_GEN, V_DUP, V_WLEAN, V_OLEAN, V_BOTH, V_N };
static const char *kName[V_N] = { "ref", "scal", "gen", "dup", "wlean", "olean", "both" };

static void set_variant(int v) {
#if defined(HH_CM_NEON)
    sCmNeon = v != V_SCAL;
    sCmSpec = v != V_GEN;
#ifdef HAVE_LEAN
    sWaterLean = v == V_WLEAN || v == V_BOTH;
#endif
#ifdef HAVE_OLEAN
    sOpaqueLean = v == V_OLEAN || v == V_BOTH;
#endif
#endif
}

static void draw_layer(const SoftVtx *vt, const uint8_t *tw, int nt, bool fold) {
    static SoftVtx buf[3 * 128]; static uint8_t tf[128];
    int n = 0;
    for (int t = 0; t < nt; t++) {
        memcpy(&buf[n * 3], &vt[t * 3], 3 * sizeof(SoftVtx));
        if (tw[t] && !fold) { tf[n++] = 0; memcpy(&buf[n * 3], &vt[t * 3], 3 * sizeof(SoftVtx)); tf[n++] = 0; }
        else tf[n++] = tw[t];
    }
    gfx_soft_tri_twice = tf;
    gfx_soft_draw_triangles((float *)buf, (size_t)n * 3 * sizeof(SoftVtx) / 4, (size_t)n);
    gfx_soft_tri_twice = NULL;
}

static void run_frame(const Frame *f, int v) {
    set_variant(v);
    memcpy(sFB, gFB0, sizeof(gFB0)); memcpy(sZB, gZB0, sizeof(gZB0));
    gHhSceneId = f->scene;
    gfx_soft_bilinear = f->bilin;
    sDepthTest = f->ztest; sDepthMask = f->dmask; sZDecal = f->decal; sUseAlpha = f->alpha;
    sEdgeOpaque = f->edgeop;
    gfx_soft_set_fog(f->fogpx, f->fogr, f->fogg, f->fogb, f->fga);
    gProg.a_tex = f->a_tex; gProg.a_in = f->a_in;
    gProg.cc.opt_texture_edge = f->edgeop;
    sCurShader = &gProg;
    const char *lay = getenv("H_LAYER");
    if (!lay || lay[0] == 'g') {
        /* the grain layer: tile 0 + the detail tile */
        sBoundTex[0] = gIdG; sBoundTex[1] = gIdD;
        gfx_soft_set_detail(f->grain_detail ? gIdD : 0, f->es, f->et, f->offs, f->offt, f->dw4);
        draw_layer(f->g, f->twice, f->nt, v != V_DUP);
    }
    if (lay && lay[0] == 'g') return;
    /* the plain layer */
    sBoundTex[0] = gIdP;
    gfx_soft_set_detail(0, 0, 0, 0, 0, f->dw4);
    draw_layer(f->p, f->twice, f->nt, v != V_DUP);
}

#if defined(HH_CM_NEON)
/* mode "ops": the lean rows' instruction swaps against what they replace, over edge and random inputs:
 * vcvtq_s32_f32 vs sat_i32 (NaN, infinities, the int32 limits, denormals), vqshrun #16 vs vqmovun(vshr #16),
 * cm_div255_lean vs cm_div255 (every 16-bit input) */
static int ops_check(void) {
    long bad = 0;
    static const float edge[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.99999994f, -0.99999994f, 2147483520.0f,
                                  2147483648.0f, -2147483648.0f, -2147483904.0f, 3e9f, -3e9f, 1e30f, -1e30f, 1e-40f,
                                  -1e-40f, 1.17549435e-38f, 65535.5f, -65536.75f };
    for (int i = 0; i < 4000000; i++) {
        float f;
        if (i < (int)(sizeof(edge) / sizeof(edge[0]))) f = edge[i];
        else if (i < 30) { const uint32_t u[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x7F800001u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x4F000000u };
                           memcpy(&f, &u[(i - 21) % 9], 4); }
        else { const uint32_t u = rnd(); memcpy(&f, &u, 4); }
        const float32x4_t v = vdupq_n_f32(f);
        if (vgetq_lane_s32(vcvtq_s32_f32(v), 0) != sat_i32(f)) { if (bad++ < 5) printf("vcvt %08x\n", (unsigned)vgetq_lane_s32(vreinterpretq_s32_f32(v), 0)); }
    }
    for (int i = 0; i < 4000000; i++) {
        const int32_t a = i < 8 ? (int32_t[]){ 0, -1, 65535 << 16, 65536 << 16, INT32_MAX, INT32_MIN, 255 << 16, -65536 }[i] : (int32_t)rnd();
        const int32x4_t q = vdupq_n_s32(a);
        if (vget_lane_u16(vqshrun_n_s32(q, 16), 0) != vget_lane_u16(vqmovun_s32(vshrq_n_s32(q, 16)), 0)) bad++;
    }
    for (uint32_t m = 0; m < 65536; m++)
        if (vget_lane_u16(cm_div255_lean(vdup_n_u16((uint16_t)m)), 0) != vget_lane_u16(cm_div255(vdup_n_u16((uint16_t)m)), 0)) bad++;
    printf("ops: %ld differ\n", bad);
    return bad != 0;
}
#endif

int main(int argc, char **argv) {
    setenv("OOT_HALFWATER", "0", 1); /* the renderer at full resolution: the water layer is ARM-only and has its own test (halfwater_test) */
    const char *mode = argc > 1 ? argv[1] : "cmp";
#if defined(HH_CM_NEON)
    if (!strcmp(mode, "ops")) return ops_check();
#endif
    rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const int frames = argc > 3 ? atoi(argv[3]) : 200;
    setenv("OOT_FASTWATER", getenv("H_FAST") ? "1" : "0", 1);
    if (getenv("H_FWTINT")) setenv("OOT_FWTINT", getenv("H_FWTINT"), 1);
    if (getenv("H_PSHADE0")) setenv("OOT_PSHADE", "0", 1);
    gfx_soft_init();
    select_target(true);
    sNatStale = false;
    gProg.cc.used_textures[0] = true;
    gProg.rgb_tex = true; gProg.rgb_in = true; gProg.a_tex = false; gProg.a_in = true;
    gIdG = gfx_soft_new_texture(); gIdP = gfx_soft_new_texture(); gIdD = gfx_soft_new_texture();
    long bad = 0, px = 0; uint32_t hsum = 2166136261u;
    int vlist[V_N], nv = 0;
    if (strncmp(mode, "bench:", 6) == 0) {
        for (int v = 0; v < V_N; v++) if (!strcmp(mode + 6, kName[v])) vlist[nv++] = v;
    } else {
        for (int v = 0; v < V_N; v++) vlist[nv++] = v;
    }
#if !defined(HH_CM_NEON)
    nv = 0; vlist[nv++] = V_SCAL; vlist[nv++] = V_DUP;
#endif
    for (int i = 0; i < nv; i++) {
        int drop = 0;
#ifndef HAVE_LEAN
        drop |= vlist[i] == V_WLEAN || vlist[i] == V_BOTH;
#endif
#ifndef HAVE_OLEAN
        drop |= vlist[i] == V_OLEAN || vlist[i] == V_BOTH;
#endif
        /* fastwater keys on the fold's flag: without it (dup) the doubled water is drawn as it is, by design */
        drop |= getenv("H_FAST") != NULL && vlist[i] == V_DUP;
        if (drop) { memmove(&vlist[i], &vlist[i + 1], (size_t)(nv - i - 1) * sizeof(int)); nv--; i--; }
    }
    static Frame f;
    const int zpass = getenv("H_ZPASS") != NULL;
    for (int fr = 0; fr < frames; fr++) {
        make_frame(&f);
        for (int i = 0; i < W * H; i++) {
            gFB0[i] = rnd() | 0xFF000000u;
            gZB0[i] = (rnd() % 16) ? (uint16_t)rnd() : 0xFFFF;
            if (zpass) gZB0[i] = 0xFFFF;
        }
        upload(gIdG, f.tg, 32, 32, 0, 0); upload(gIdP, f.tp, f.pw, f.ph, f.pfmt, f.pclamp); upload(gIdD, f.td, f.dw, f.dh, 0, 0);
        for (int k = 0; k < nv; k++) {
            run_frame(&f, vlist[k]);
            if (k == 0) { memcpy(gFBr, sFB, sizeof(gFBr)); memcpy(gZBr, sZB, sizeof(gZBr)); continue; }
            long d = 0;
            for (int i = 0; i < W * H; i++) d += (sFB[i] != gFBr[i]) || (sZB[i] != gZBr[i]);
            if (d) {
                bad += d;
                printf("frame %d: %s vs %s: %ld px differ (fga %d bilin %d alpha %d dmask %d decal %d fogpx %d scene %d es %d et %d tex %dx%d fmt %d clamp %d)\n", fr,
                       kName[vlist[k]], kName[vlist[0]], d, f.fga, f.bilin, f.alpha, f.dmask, f.decal, f.fogpx, f.scene, f.es, f.et,
                       f.pw, f.ph, f.pfmt, f.pclamp);
            }
        }
        for (int i = 0; i < W * H; i++) { hsum = (hsum ^ gFBr[i]) * 16777619u; hsum = (hsum ^ gZBr[i]) * 16777619u; }
        long changed = 0;
        for (int i = 0; i < W * H; i++) changed += gFBr[i] != gFB0[i];
        px += changed;
        /* uploads bump-allocate: reset like texman_clear between frames */
        sArenaUsed = 0; sNextTexId = 1; memset(sTex, 0, sizeof(sTex));
        gIdG = gfx_soft_new_texture(); gIdP = gfx_soft_new_texture(); gIdD = gfx_soft_new_texture();
    }
    printf("%s: %d frames, %d variants (", mode, frames, nv);
    for (int k = 0; k < nv; k++) printf("%s%s", k ? " " : "", kName[vlist[k]]);
    printf("), %ld px changed by the draws, %ld px differ, hash %08x, span px %llu\n", px, bad, (unsigned)hsum,
           (unsigned long long)gfx_soft_span_px);
    return bad != 0;
}
