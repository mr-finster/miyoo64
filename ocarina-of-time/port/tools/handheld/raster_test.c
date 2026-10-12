/* raster_test.c: the rasterizer's row kinds through the real renderer, variant against variant, byte for byte
 * (2026-10-01, the FPS hunt; a sibling of water_test.c). Includes src/port/hh/gfx/gfx_soft.c and drives raster_tri
 * through gfx_soft_draw_triangles (and the 2D sprites through gfx_scegu_draw_triangles_2d) with random draws over
 * the modes the game uses: opaque and blended, alpha-tested foliage (texture-edge alphas, z written), fogged with and
 * without the constant combiner alpha, the sky (texture only, no z), the HUD's UI triangles (nearest, no z, the
 * (A - K) * TEXEL0 + K tints), 32-bit, expanded and 16-bit textures, repeating, clamped and odd sizes, bilinear and
 * nearest, flat and smooth, decals, the detail tile, slivers and near-eye-plane vertices; and sequences (seq) of a sky
 * between its tags and the draws after it, for the kept sky (OOT_SKYDEFER). Every variant draws every frame from the
 * same start; colour and depth must match the first variant's byte for byte.
 *
 *   raster_test <mode> [seed] [frames]
 *   mode = cmp       : every variant over random frames (exit 1 on any difference)
 *          bench:<v> : one variant only (instruction counts under qemu: tools/qprof/qlogcount.py)
 *          nearz [seed] [n] : OOT_NEARW's span depth (span_zdq + span_zq_far, 2026-10-04) against the exact depth: n
 *                     random spans (start -1e4..1e4, slope -1e3..1e3, 1-16 pixels, a third in [-1, 2]), every pixel's
 *                     16-bit depth from the 24.8 steps against clamp(floor(exact * 65535)) in double, at most 1
 *                     apart; with OOT_NEARW=0 the count of the old path's misses (the bug)
 *   env  : H_NEARZ=1 near-eye depths (OOT_NEARW): a quarter of the vertices at z -700..-1 with 1/w up to 64 (faces
 *          within 5 units of the eye, as OoT's near plane of 10 gives them), every row loop must agree there too;
 *          H_KIND=<k> one kind only (opq alpha sky ui gen 2d seq lerp); H_PSHADE0=1 OOT_PSHADE=0; H_ZPASS=1 every z test passes;
 *          H_EDGENOTE=1 the OOT_EDGETINT rows (a TEX_EDGE pixel over any depth-less pixel written unblended and noted);
 *          H_INSIDE=1 clamped / odd-sized textures sampled mostly inside, see-through texels (OOT_CLAMPTAP, OOT_ALPHASKIP);
 *          H_VARS=base,new only those variants (ARM); bench knobs: H_RR32=1 (32-bit repeating textures, opaque: no
 *          detail or decal), H_TRISZ=<n> (triangles up to n px across, on screen), H_UIIN=1 (texture coordinates
 *          inside the texture, as the HUD's quads have them)
 *
 * Variants: base = today's defaults with every switch below off; new = all of them on; one = each on alone;
 * old = the lean rows off too; scal / scalnew = the scalar loops (OOT_CMNEON=0), the switches off / on. x86 has only the scalar loops: its frame hash
 * (colour + depth of the first variant) must equal ARM's for a seed. Build and run: tools/handheld/raster_test.sh. */
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

static int gNearZ; /* H_NEARZ */
static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
static uint32_t gFB0[W * H], gFBr[W * H];
static uint16_t gZB0[W * H], gZBr[W * H];

/* the switches under test (gfx_soft.c statics; each 0 = the path before it). NEON-only ones get a stand-in on x86. */
#if !defined(HH_CM_NEON)
static int sAlphaLean, sSkyLean, sLeanZ, sSkyDefer, sLerpLean, sDetClamp, sDetLerp, sNoZLean, sClampTap, sAlphaSkip;
#endif
#if !defined(HH_2D_NEON)
static int sMagBil2D;
#endif
#define RT_SWITCHES X(sPsVcvt) X(sAlphaLean) X(sSkyLean) X(sUiLoop) X(sLeanZ) X(s2dGen) X(s2dLerp) X(sNearIn) X(sUiNeon) X(sSkyDefer) X(sLerpLean) X(sPld) \
                    X(sDetClamp) X(sMagBil2D) X(sDetLerp) X(sNoZLean) X(sClampTap) X(sAlphaSkip)
#define X(s) +1
enum { NSW = 0 RT_SWITCHES };
#undef X
#define X(s) #s,
static const char *kSwName[] = { RT_SWITCHES };
#undef X
static int *const kSw[] = {
#define X(s) &s,
    RT_SWITCHES
#undef X
};

enum { K_OPQ, K_ALPHA, K_SKY, K_UI, K_GEN, K_2D, K_SEQ, K_LERP, K_N };
static const char *kKind[K_N] = { "opq", "alpha", "sky", "ui", "gen", "2d", "seq", "lerp" };

typedef struct {
    int kind, nt;
    SoftVtx v[3 * 64];
    Vtx2D s2[2 * 32];
    uint32_t tp[128 * 128], td[64 * 64];
    int pw, ph, pfmt, pclamp, dw, dh, es, et; int32_t offs, offt; uint16_t dw4[4];
    bool detail;
    int fga, fogr, fogg, fogb;
    bool bilin, alpha, ztest, dmask, decal, fogpx, edge, edgeop, ui;
    bool rgb_tex, rgb_in, a_tex, a_in, rgb_texa, texlerp, texadd, forcein; int lk[3]; int ninputs;
    int inside; /* H_INSIDE: 1, or 2 with every vertex alpha 255 */
    bool hastwice; uint8_t twice[64]; /* G_TRI2 doubled triangles (gfx_soft_tri_twice: the blend-twice fold) */
} Frame;

static const uint8_t kAEdge[] = { 0, 7, 8, 9, 84, 85, 86, 127, 128, 253, 254, 255 }; /* the alpha tests' edges */
static int pick_size(void) { static const int s[] = { 8, 16, 32, 32, 64, 64, 128, 24, 48, 12 }; return s[rnd() % 10]; }

static void make_frame(Frame *f, int kind);
static Frame gSub[6]; static int gNSub; static bool gSubTag[6], gRewrite; static uint32_t gSubTex[6];
/* K_SEQ: a sky batch between the sky's tags, then draws that keep it (opaque rows writing z, no decal) or draw it
 * (anything else), sometimes another sky batch, sometimes the sky's texels rewritten in place after it; each batch
 * with its own texture, uploaded before the frame */
static void make_seq(Frame *f) {
    memset(f, 0, sizeof(*f));
    f->kind = K_SEQ;
    gNSub = 2 + rnd() % 5;
    gRewrite = rnd() % 4 == 0;
    for (int i = 0; i < gNSub; i++) {
        const int r = rnd() % 10;
        const int k = i == 0 ? K_SKY : r < 5 ? K_OPQ : r < 6 ? K_SKY : r < 7 ? K_ALPHA : r < 8 ? K_2D : r < 9 ? K_UI : K_GEN;
        make_frame(&gSub[i], k);
        gSubTag[i] = k == K_SKY && rnd() % 8 != 0;
        if (k == K_SKY && (i == 0 || rnd() % 2)) { gSub[i].pw = 32; gSub[i].ph = 32; gSub[i].pfmt = 0; gSub[i].pclamp = 0; gSub[i].bilin = 1; gSub[i].ztest = 0; }
        if (k == K_OPQ && rnd() % 4) { gSub[i].dmask = 1; gSub[i].decal = 0; gSub[i].detail = rnd() % 4 == 0; gSub[i].bilin = 1; }
        if (k == K_OPQ && rnd() % 2) { gSub[i].pclamp = 0; if (gSub[i].pw != 32 && gSub[i].pw != 64 && gSub[i].pw != 16) gSub[i].pw = 32; if (gSub[i].ph != 32 && gSub[i].ph != 64 && gSub[i].ph != 16 && gSub[i].ph != 128) gSub[i].ph = 16; }
    }
}
static void make_frame(Frame *f, int kind) {
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->nt = 1 + rnd() % 40;
    /* texture: 32-bit, 16-bit expanded at upload (<= TEX32_SMALL texels), 16-bit kept (bigger) */
    f->pw = pick_size(); f->ph = pick_size();
    if (kind == K_SKY && rnd() % 2) { f->pw = 32; f->ph = 32; }
    f->pfmt = rnd() % 3 == 0 ? 0 : 1;
    if (rnd() % 4 == 0) { f->pw = 64 << (rnd() % 2); f->ph = 128; f->pfmt = 1; } /* > 4096 texels: stays 16-bit */
    f->pclamp = rnd() % 3 == 0 ? (int)(1 + rnd() % 3) : 0;
    if (getenv("H_RR32")) { f->pw = 32; f->ph = 32; f->pfmt = 0; f->pclamp = 0; } /* bench: 32-bit repeating only */
    for (int i = 0; i < f->pw * f->ph; i++) f->tp[i] = rnd();
    const int am = rnd() % 5; /* texel alphas: random, all opaque, cut-out (0 / 255), or the tests' edges */
    for (int i = 0; i < f->pw * f->ph; i++) {
        if (am == 1) f->tp[i] |= 0xFF000000u;
        else if (am == 2) f->tp[i] = (f->tp[i] & 0xFFFFFFu) | ((rnd() & 1) ? 0xFF000000u : 0u);
        else if (am == 3) f->tp[i] = (f->tp[i] & 0xFFFFFFu) | ((uint32_t)kAEdge[rnd() % 12] << 24);
    }
    f->fogr = rnd() & 255; f->fogg = rnd() & 255; f->fogb = rnd() & 255;
    f->fga = -1;
    f->bilin = 1; f->rgb_tex = 1; f->rgb_in = 1; f->a_tex = 0; f->a_in = 1;
    f->ninputs = 1;
    switch (kind) {
    case K_OPQ: /* the world's opaque rows */
        f->ztest = 1; f->dmask = rnd() % 8 != 0; f->decal = rnd() % 6 == 0; f->fogpx = rnd() % 2;
        f->detail = rnd() % 4 == 0; f->bilin = rnd() % 8 != 0;
        if (getenv("H_RR32")) { f->detail = 0; f->bilin = 1; f->decal = 0; }
        break;
    case K_ALPHA: /* blended and alpha-tested: foliage, fences, water, smoke */
        f->ztest = 1; f->alpha = 1; f->dmask = rnd() % 2; f->decal = rnd() % 8 == 0; f->fogpx = rnd() % 2;
        f->a_tex = rnd() % 4 != 0; f->a_in = rnd() % 3 != 0;
        f->edge = rnd() % 3 == 0; f->edgeop = rnd() % 2;
        if (f->fogpx && rnd() % 3 == 0) { /* the fogged constant alpha: amode 3 (no texel alpha) or 4 */
            f->fga = (rnd() % 4) ? (int)(rnd() % 256) : (rnd() & 1) * 255;
            f->a_tex = rnd() & 1;
        }
        f->detail = rnd() % 6 == 0; f->bilin = rnd() % 8 != 0;
        break;
    case K_SKY: /* texture only, no z, opaque */
        f->rgb_in = 0; f->a_tex = rnd() % 2; f->a_in = rnd() % 2; f->bilin = rnd() % 6 != 0;
        if (rnd() % 8 == 0) f->ztest = 1; /* a z-tested texture-only draw (scalar loops) */
        break;
    case K_UI: /* HUD triangles: UI mode (nearest, box-filtered when squeezed), mostly no z, blended */
        f->ui = 1; f->ztest = rnd() % 4 == 0; f->dmask = 0; f->alpha = rnd() % 6 != 0;
        f->a_tex = rnd() % 4 != 0; f->a_in = rnd() % 3 != 0; f->edge = rnd() % 4 == 0;
        { const int c = rnd() % 6;
          if (c == 0 || c == 1) f->texlerp = 1;           /* (A - K) * TEXEL0 + K */
          else if (c == 2) f->texadd = 1;
          else if (c == 3) f->rgb_in = 0;                 /* the texel alone */
          else if (c == 4) f->rgb_texa = rnd() % 2; }     /* TEXEL0_ALPHA as colour */
        if (rnd() % 4 == 0) f->rgb_tex = 0;
        f->lk[0] = rnd() & 255; f->lk[1] = rnd() & 255; f->lk[2] = rnd() & 255;
        break;
    case K_LERP: /* z-tested (A - K) * TEXEL0 + K tints (OOT_LERPLEAN): opaque and alpha-tested / blended, fogged with
                  * and without the constant alpha, decals, z writes, and around them (texadd too, rgb_texa, no rgb_in,
                  * the detail tile, nearest, UI mode: those stay on the scalar loops) */
        f->ztest = rnd() % 16 != 0; f->dmask = rnd() % 3 != 0; f->alpha = rnd() % 3 != 0; f->decal = rnd() % 6 == 0;
        f->fogpx = rnd() % 3 == 0;
        if (f->fogpx && rnd() % 2) { f->fga = (rnd() % 4) ? (int)(rnd() % 256) : (rnd() & 1) * 255; }
        f->a_tex = rnd() % 4 != 0; f->a_in = rnd() % 3 != 0; f->edge = rnd() % 4 == 0; f->edgeop = rnd() & 1;
        f->texlerp = rnd() % 10 != 0; f->texadd = rnd() % 8 == 0; f->rgb_texa = rnd() % 12 == 0;
        f->rgb_in = rnd() % 12 != 0; f->rgb_tex = rnd() % 12 != 0; f->forcein = rnd() % 12 == 0;
        f->bilin = rnd() % 10 != 0; f->detail = rnd() % 10 == 0; f->ui = rnd() % 10 == 0;
        f->lk[0] = rnd() & 255; f->lk[1] = rnd() & 255; f->lk[2] = rnd() & 255;
        if (rnd() % 4 == 0) { const int k = rnd() % 3; f->lk[k] = rnd() % 2 ? 0 : 255; }
        f->hastwice = rnd() % 3 == 0;
        for (int t = 0; t < f->nt; t++) f->twice[t] = (uint8_t)(rnd() & 1);
        break;
    case K_2D:
        f->ui = 1; f->alpha = rnd() % 4 != 0; f->a_tex = rnd() % 4 != 0; f->a_in = rnd() % 3 != 0; f->edge = rnd() % 4 == 0;
        { const int c = rnd() % 6;
          if (c == 0) f->texlerp = 1; else if (c == 1) f->texadd = 1; else if (c == 2) f->rgb_in = 0;
          else if (c == 3) f->rgb_texa = rnd() % 2; }
        f->lk[0] = rnd() & 255; f->lk[1] = rnd() & 255; f->lk[2] = rnd() & 255;
        f->ninputs = rnd() % 4 ? 1 : 0;
        break;
    default: /* anything */
        f->ztest = rnd() % 4 != 0; f->dmask = rnd() & 1; f->alpha = rnd() & 1; f->decal = rnd() % 6 == 0;
        f->fogpx = rnd() % 3 == 0; if (f->fogpx && rnd() % 2) f->fga = (int)(rnd() % 256);
        f->a_tex = rnd() & 1; f->a_in = rnd() % 4 != 0; f->edge = rnd() % 4 == 0; f->edgeop = rnd() & 1;
        f->rgb_in = rnd() % 5 != 0; f->rgb_texa = rnd() % 8 == 0; f->texlerp = rnd() % 8 == 0; f->texadd = rnd() % 10 == 0;
        f->forcein = rnd() % 10 == 0; f->ui = rnd() % 6 == 0; f->bilin = rnd() % 4 != 0; f->detail = rnd() % 6 == 0;
        f->lk[0] = rnd() & 255; f->lk[1] = rnd() & 255; f->lk[2] = rnd() & 255;
        break;
    }
    if (getenv("H_NOZ") && kind == K_ALPHA && rnd() % 4) { f->ztest = 0; if (rnd() % 2) f->fogpx = 0; } /* OOT_NOZLEAN: blends without z */
    if (getenv("H_DETL") && kind == K_LERP && rnd() % 4) { f->detail = 1; f->bilin = 1; } /* OOT_DETLERP: the tint over two tiles */
    if (getenv("H_DETC") && (kind == K_OPQ || kind == K_ALPHA)) { /* OOT_DETCLAMP: the detail tile over a clamped or odd tile 0 */
        f->detail = 1; f->bilin = 1;
        if (rnd() % 3) f->pclamp = 1 + rnd() % 3; else { f->pw = 24 + 24 * (rnd() % 2); f->ph = 12 + 36 * (rnd() % 2); }
        if (rnd() % 4 == 0) f->pfmt = 0;
        for (int i = 0; i < f->pw * f->ph; i++) f->tp[i] = rnd() | (kind == K_OPQ || rnd() % 2 ? 0xFF000000u : 0u);
    }
    if (getenv("H_INSIDE") && kind != K_UI && kind != K_2D && rnd() % 4) {
        /* OOT_CLAMPTAP / OOT_ALPHASKIP (2026-10-04, the flame overlays): tile 0 clamped or odd-sized (wrapf's taps), the
         * texture coordinates mostly inside it (below: the inside taps and the edges around them), the texel alphas
         * mostly see-through or at the tests' edges (the flames' cut-outs) */
        if (rnd() % 3) f->pclamp = 1 + rnd() % 3; else { f->pw = 24 + 24 * (rnd() % 2); f->ph = 12 + 36 * (rnd() % 2); f->pclamp = rnd() % 4; }
        if (rnd() % 3 == 0) { f->pw = 64; f->ph = 128; f->pclamp = 3; }
        f->bilin = 1;
        const int am = rnd() % 4;
        const uint32_t ra1 = rnd() % 4; /* am 3: one alpha over the whole texture, mostly an alpha test's threshold */
        const uint32_t a1 = ra1 == 0 ? 8 : ra1 == 1 ? 0x55 : kAEdge[rnd() % 12];
        for (int i = 0; i < f->pw * f->ph; i++) {
            const uint32_t r = rnd() % 8;
            const uint32_t a = am == 0 ? (r < 5 ? 0 : r < 7 ? kAEdge[rnd() % 12] : rnd() & 255) : am == 1 ? kAEdge[rnd() % 12]
                             : am == 2 ? (r < 4 ? 0 : 255) : a1;
            f->tp[i] = (f->tp[i] & 0xFFFFFFu) | (a << 24);
        }
        if (am == 3) f->pfmt = 0; /* (a 16-bit texel's alpha is 0 or 255) */
        f->inside = rnd() % 2 ? 1 : 2; /* 2: every vertex alpha 255 (the texel alpha reaches the test unscaled) */
    }
    f->dw = 1 << (3 + rnd() % 4); f->dh = 1 << (3 + rnd() % 4);
    for (int i = 0; i < f->dw * f->dh; i++) f->td[i] = rnd();
    f->es = (int)(rnd() % 5) - 1; f->et = (int)(rnd() % 5) - 1;
    if (f->es <= 0 && f->et <= 0) f->es = 1;
    f->offs = (int32_t)rnd(); f->offt = (int32_t)rnd();
    for (int k = 0; k < 4; k++) f->dw4[k] = (uint16_t)(rnd() % 257);
    const int nofog = rnd() % 5 == 0; /* every vertex alpha 0: a fogged draw's triangles without fog */
    const bool persp = kind != K_UI || rnd() % 2; /* the HUD's ortho quads: w = 1 */
    const bool uiin = kind == K_UI && rnd() % 2;   /* coordinates inside the texture, as the HUD's quads have them */
    const int aedge = rnd() % 3 == 0 ? (int)kAEdge[rnd() % 12] : -1; /* every vertex alpha at an alpha test's edge */
    const float tsc = kind == K_UI ? (float)(rnd() % 3 + 1) : 6.0f;
    if (kind == K_2D) {
        f->nt = 1 + rnd() % 24;
        for (int s = 0; s < f->nt; s++) {
            Vtx2D *a = &f->s2[s * 2], *b = &f->s2[s * 2 + 1];
            const int x = (int)(rnd() % 260) - 10, y = (int)(rnd() % 260) - 10, w = 1 + rnd() % 120, h = 1 + rnd() % 60;
            a->x = (uint16_t)(x < 0 ? 0 : x); a->y = (uint16_t)(y < 0 ? 0 : y);
            b->x = (uint16_t)(a->x + w); b->y = (uint16_t)(a->y + h);
            const int sq = rnd() % 3; /* 1:1, squeezed (box filter), stretched */
            const int tw = sq == 0 ? w : sq == 1 ? w * 4 / 3 : w / 2 + 1;
            a->u = (int16_t)(rnd() % 64 - 8); a->v = (int16_t)(rnd() % 64 - 8);
            b->u = (int16_t)(a->u + tw); b->v = (int16_t)(a->v + (rnd() % 2 ? h : h / 2 + 1));
            a->z = b->z = (rnd() % 6 == 0 || (getenv("H_MAG") && rnd() % 4)) ? 5 : 0; /* H_MAG: mostly magnified (OOT_MAGBIL2D) */
            if (a->z == 5) { a->u *= 32; a->v *= 32; b->u *= 32; b->v *= 32; }
            b->r = rnd(); b->g = rnd(); b->b = rnd(); b->a = rnd() % 3 ? (uint8_t)rnd() : 255;
            if (rnd() % 3 == 0) { b->r = b->g = b->b = b->a = 255; }
        }
        return;
    }
    for (int t = 0; t < f->nt; t++) {
        float cx = frnd(-40, 280), cy = frnd(-40, 280), sz = (rnd() % 4) ? frnd(2, 60) : frnd(60, 300);
        if (getenv("H_TRISZ")) { sz = frnd(2, (float)atoi(getenv("H_TRISZ"))); cx = frnd(0, 240); cy = frnd(0, 240); } /* bench */
        for (int k = 0; k < 3; k++) {
            SoftVtx *a = &f->v[t * 3 + k];
            a->x = cx + frnd(-sz, sz); a->y = cy + frnd(-sz, sz);
            a->z = (rnd() % 8) ? frnd(0.0f, 1.0f) : frnd(-0.2f, 1.2f);
            a->invw = !persp ? 1.0f : (rnd() % 16) ? frnd(0.02f, 3.0f) : frnd(-0.5f, 0.02f);
            a->u = frnd(-tsc, tsc); a->v = frnd(-tsc, tsc);
            if (f->inside) { a->u = frnd(-0.15f, 1.15f); a->v = frnd(-0.15f, 1.15f); } /* H_INSIDE */
            if (uiin || getenv("H_UIIN")) { a->u = frnd(0.0f, 0.999f); a->v = frnd(0.0f, 0.999f); }
            a->r = rnd(); a->g = rnd(); a->b = rnd(); a->a = nofog && f->fogpx ? 0 : rnd();
            if (aedge >= 0) a->a = (uint8_t)aedge;
            if (f->inside == 2) a->a = 255; /* H_INSIDE */
        }
        if (rnd() % 10 == 0) { const int k = rnd() % 3; f->v[t * 3 + k].invw = frnd(1e-9f, 1e-4f); }
        if (gNearZ) /* H_NEARZ: faces passing the eye (OOT_NEARW's depths: z/w * 0.5 + 0.5 = 1 - 10 / w, w down to 1/64) */
            for (int k = 0; k < 3; k++) if (rnd() % 4 == 0) { f->v[t * 3 + k].z = frnd(-700.0f, -1.0f); f->v[t * 3 + k].invw = frnd(0.2f, 64.0f); }
        if (rnd() % 10 == 0) {
            SoftVtx *a = &f->v[t * 3], *b = &f->v[t * 3 + 1], *c = &f->v[t * 3 + 2];
            const float k = frnd(-0.5f, 1.5f), e = frnd(-0.05f, 0.05f);
            c->x = a->x + (b->x - a->x) * k + e; c->y = a->y + (b->y - a->y) * k - e;
        }
        if (rnd() % 10 == 0) { /* an axis-aligned quad half (the sky's faces, the HUD's quads) */
            SoftVtx *a = &f->v[t * 3], *b = &f->v[t * 3 + 1], *c = &f->v[t * 3 + 2];
            b->y = a->y; c->x = a->x;
        }
        if (rnd() % 5 == 0) { /* flat */
            for (int k = 1; k < 3; k++) {
                f->v[t * 3 + k].r = f->v[t * 3].r; f->v[t * 3 + k].g = f->v[t * 3].g;
                f->v[t * 3 + k].b = f->v[t * 3].b; f->v[t * 3 + k].a = f->v[t * 3].a;
            }
        }
    }
}

static struct ShaderProgram gProg;
static uint32_t gIdP, gIdD;

static void upload(uint32_t id, const uint32_t *argb32, int w, int h, int fmt, int clamp) {
    static uint8_t buf[128 * 128 * 4];
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

/* variants: 0 base, 1 new, 2..: each switch alone, then old, scal */
#define V_BASE 0
#define V_NEW 1
#define V_ONE 2
#define V_OLD (V_ONE + NSW)
#define V_SCAL (V_OLD + 1)
#define V_SCALNEW (V_SCAL + 1)
#define V_N (V_SCALNEW + 1)
static const char *vname(int v) {
    static char b[64];
    if (v == V_BASE) return "base";
    if (v == V_NEW) return "new";
    if (v == V_OLD) return "old";
    if (v == V_SCAL) return "scal";
    if (v == V_SCALNEW) return "scalnew";
    snprintf(b, sizeof(b), "%s", kSwName[v - V_ONE]);
    return b;
}

static void set_variant(int v) {
    for (int i = 0; i < NSW; i++) *kSw[i] = v == V_NEW || v == V_SCALNEW || v == V_ONE + i;
#if defined(HH_CM_NEON)
    sCmNeon = v != V_SCAL && v != V_SCALNEW;
    sWaterLean = sOpaqueLean = v != V_OLD;
#endif
}

static void draw_sub(const Frame *f, uint32_t texid, bool tag);
static void upload(uint32_t id, const uint32_t *argb32, int w, int h, int fmt, int clamp);
static void run_frame(const Frame *f, int v) {
    set_variant(v);
    memcpy(sFB, gFB0, sizeof(gFB0)); memcpy(sZB, gZB0, sizeof(gZB0));
    if (f->kind == K_SEQ) { /* the sky's rows kept (OOT_SKYDEFER) across the draws after them */
        for (int i = 0; i < gNSub; i++) /* the textures afresh (the in-place rewrite below changes one) */
            upload(gSubTex[i], gSub[i].tp, gSub[i].pw, gSub[i].ph, gSub[i].pfmt, gSub[i].pclamp);
        for (int i = 0; i < gNSub; i++) {
            draw_sub(&gSub[i], gSubTex[i], gSubTag[i]);
            if (i == 0 && gRewrite) { /* the sky's texels rewritten in place (the blend cache's way) */
                static const uint8_t k3[3] = { 0, 0, 0 };
                gfx_soft_blend_textures(gSubTex[0], gSubTex[1], gSubTex[1], gSub[0].pw, gSub[0].ph, 0, 0, 0, 0, 0, 0, 1, 1,
                                        0, k3, 0, 0, false, 0);
            }
        }
        sky_settle(); /* gfx_soft_end_frame's */
        return;
    }
    draw_sub(f, gIdP, false);
    sky_settle();
}
static void draw_sub(const Frame *f, uint32_t texid, bool tag) {
    gHhSceneId = -1;
    gfx_soft_bilinear = f->bilin;
    sDepthTest = f->ztest; sDepthMask = f->dmask; sZDecal = f->decal; sUseAlpha = f->alpha;
    sEdgeOpaque = f->edgeop;
    gfx_soft_set_fog(f->fogpx, f->fogr, f->fogg, f->fogb, f->fga);
    gProg.rgb_tex = f->rgb_tex; gProg.rgb_in = f->rgb_in; gProg.a_tex = f->a_tex; gProg.a_in = f->a_in;
    gProg.rgb_texa = f->rgb_texa;
    gProg.cc.opt_texture_edge = f->edge;
    gProg.cc.num_inputs = (uint8_t)f->ninputs;
    sCurShader = &gProg;
    gfx_scegu_set_tex_lerp(f->texlerp, (uint8_t)f->lk[0], (uint8_t)f->lk[1], (uint8_t)f->lk[2]);
    sTexAdd = f->texadd; sForceIn = f->forcein;
    gfx_scegu_set_ui_mode(f->ui);
    sBoundTex[0] = texid;
    gfx_soft_set_detail(f->detail ? gIdD : 0, f->es, f->et, f->offs, f->offt, f->dw4);
    if (f->kind == K_2D) {
        gfx_scegu_draw_triangles_2d((float *)f->s2, (size_t)f->nt * 2 * sizeof(Vtx2D) / 4, (size_t)f->nt);
        sNatStale = false;
        return;
    }
    if (tag) gfx_scegu_set_sky_pass(true);
    gfx_soft_tri_twice = f->hastwice ? f->twice : NULL;
    gfx_soft_draw_triangles((float *)f->v, (size_t)f->nt * 3 * sizeof(SoftVtx) / 4, (size_t)f->nt);
    gfx_soft_tri_twice = NULL;
    if (tag) gfx_scegu_set_sky_pass(false);
}

int main(int argc, char **argv) {
    setenv("OOT_HALFWATER", "0", 1); /* the renderer at full resolution: the water layer is ARM-only and has its own test (halfwater_test) */
    const char *mode = argc > 1 ? argv[1] : "cmp";
    rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
    if (!rs) rs = 1;
#ifndef RT_OLDTREE /* (built against the tree before OOT_NEARW: raster_test.sh's OLD check) */
    if (!strcmp(mode, "nearz")) { /* OOT_NEARW's span depth against the exact one (see the header) */
        const long n = argc > 3 ? atol(argv[3]) : 1000000;
        long bad = 0, npx = 0, outside = 0;
        for (long i = 0; i < n; i++) {
            const int span = 1 + (int)(rnd() % 16);
            float zf = (rnd() % 3 == 0) ? frnd(-1.0f, 2.0f) : (rnd() & 1) ? frnd(-1e4f, 1e4f) : frnd(-30.0f, 30.0f);
            float zdx = (rnd() & 1) ? frnd(-2.0f, 2.0f) : (rnd() & 1) ? frnd(-30.0f, 30.0f) : frnd(-1e3f, 1e3f);
            if (rnd() % 8 == 0) zdx = 0.0f;
            const int32_t zdq = span_zdq(zdx);
            const int32_t zq = span_z_out(zf) ? span_zq_far(zf, zdx, span, zdq)
                                              : (int32_t)((zf < -8.0f ? -8.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            outside += zf < -1.0f || zf > 2.0f;
            for (int k = 0; k < span; k++) {
                const int ours = clamp16_sh8(zq + k * zdq);
                const double ez = ((double)zf + (double)k * (double)zdx) * 65535.0;
                const int ex = ez < 0.0 ? 0 : ez > 65535.0 ? 65535 : (int)ez;
                npx++;
                if (ours - ex > 1 || ex - ours > 1) {
                    if (bad < 5) printf("  span %d zf %g zdx %g pixel %d: %d, exact %d\n", span, zf, zdx, k, ours, ex);
                    bad++;
                }
            }
        }
        printf("nearz: OOT_NEARW=%d, %ld spans (%ld starting outside [-1, 2]), %ld px, %ld differ by more than 1\n",
               gfx_soft_nearw_on(), n, outside, npx, bad);
        return 0;
    }
#endif
    const int frames = argc > 3 ? atoi(argv[3]) : 200;
    gNearZ = getenv("H_NEARZ") != NULL;
    if (getenv("H_PSHADE0")) setenv("OOT_PSHADE", "0", 1);
    gfx_soft_init();
    if (getenv("H_EDGENOTE")) { sEnMark = calloc(W * H, 1); sEnMarkSz = W * H; } /* OOT_EDGETINT's rows (edge_note) */
    select_target(true);
    sNatStale = false;
    gProg.cc.used_textures[0] = true;
    gIdP = gfx_soft_new_texture(); gIdD = gfx_soft_new_texture();
    int kind_only = -1;
    if (getenv("H_KIND")) for (int k = 0; k < K_N; k++) if (!strcmp(getenv("H_KIND"), kKind[k])) kind_only = k;
    long bad = 0, px = 0; uint32_t hsum = 2166136261u;
    int vlist[V_N], nv = 0;
    if (strncmp(mode, "bench:", 6) == 0) {
        for (int v = 0; v < V_N; v++) if (!strcmp(mode + 6, vname(v))) vlist[nv++] = v;
    } else {
        for (int v = 0; v < V_N; v++) vlist[nv++] = v;
    }
    if (getenv("H_VARS")) { /* H_VARS=base,new: only those variants */
        int k = 0; char want[256]; snprintf(want, sizeof(want), ",%s,", getenv("H_VARS"));
        for (int i = 0; i < nv; i++) { char nm[80]; snprintf(nm, sizeof(nm), ",%s,", vname(vlist[i])); if (strstr(want, nm)) vlist[k++] = vlist[i]; }
        nv = k;
    }
#if !defined(HH_CM_NEON)
    if (strncmp(mode, "bench:", 6) != 0) { nv = 0; vlist[nv++] = V_BASE; vlist[nv++] = V_NEW; for (int i = 0; i < NSW; i++) vlist[nv++] = V_ONE + i; }
#endif
    long kfr[K_N] = { 0 };
    static Frame f;
    const int zpass = getenv("H_ZPASS") != NULL;
    for (int fr = 0; fr < frames; fr++) {
        const int kind = kind_only >= 0 ? kind_only : (int)(rnd() % K_N);
        kfr[kind]++;
        if (kind == K_SEQ) make_seq(&f); else make_frame(&f, kind);
        for (int i = 0; i < W * H; i++) {
            gFB0[i] = rnd() | 0xFF000000u;
            if (rnd() % 8 == 0) gFB0[i] = sUnified ? (sNatFill | 0xFF000000u) : 0xFF000000u; /* the clear (TEX_EDGE's test) */
            gZB0[i] = (rnd() % 16) ? (uint16_t)rnd() : 0xFFFF;
            if (zpass) gZB0[i] = 0xFFFF;
        }
        upload(gIdP, f.tp, f.pw, f.ph, f.pfmt, f.pclamp); upload(gIdD, f.td, f.dw, f.dh, 0, 0);
        if (kind == K_SEQ)
            for (int i = 0; i < gNSub; i++) gSubTex[i] = gfx_soft_new_texture();
        for (int k = 0; k < nv; k++) {
            sEnN = 0; sEnFB = NULL;
            run_frame(&f, vlist[k]);
            if (k == 0) { memcpy(gFBr, sFB, sizeof(gFBr)); memcpy(gZBr, sZB, sizeof(gZBr)); continue; }
            long d = 0;
            for (int i = 0; i < W * H; i++) d += (sFB[i] != gFBr[i]) || (sZB[i] != gZBr[i]);
            if (d) {
                bad += d;
                printf("frame %d (%s): %s vs %s: %ld px differ (tex %dx%d fmt %d clamp %d bilin %d alpha %d z %d dmask %d fog %d fga %d ui %d detail %d)\n",
                       fr, kKind[kind], vname(vlist[k]), vname(vlist[0]), d, f.pw, f.ph, f.pfmt, f.pclamp, f.bilin, f.alpha,
                       f.ztest, f.dmask, f.fogpx, f.fga, f.ui, f.detail);
            }
        }
        for (int i = 0; i < W * H; i++) { hsum = (hsum ^ gFBr[i]) * 16777619u; hsum = (hsum ^ gZBr[i]) * 16777619u; }
        for (int i = 0; i < W * H; i++) px += gFBr[i] != gFB0[i];
        sArenaUsed = 0; sNextTexId = 1; memset(sTex, 0, sizeof(sTex));
        gIdP = gfx_soft_new_texture(); gIdD = gfx_soft_new_texture();
    }
    printf("%s: %d frames (", mode, frames);
    for (int k = 0; k < K_N; k++) printf("%s%s %ld", k ? ", " : "", kKind[k], kfr[k]);
    printf("), %d variants (", nv);
    for (int k = 0; k < nv; k++) printf("%s%s", k ? " " : "", vname(vlist[k]));
    printf("), %ld px changed by the draws, %ld px differ, hash %08x\n", px, bad, (unsigned)hsum);
    return bad != 0;
}
