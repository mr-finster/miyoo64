/* rast_test.c: GoldenEye's rasterizer (port/hh/gfx/gfx_soft.c, included whole) driven with crafted triangles,
 * variant against variant, byte for byte -- colour and depth (2026-10-01, the FPS hunt; modelled on the OoT port's
 * tools/handheld/water_test.c). No ROM: every frame is random textures (32-bit, 16-bit kept 16-bit, 16-bit expanded,
 * mipmapped, power-of-two or not, repeat or clamp) and random perspective triangles in the world's modes -- opaque
 * z-tested texture x shade (hot_row), K + (S - K) * T (hot_row_vk), blended, decals, no z write, no z test, flat,
 * near-eye-plane and sliver vertices, magnified and minified (every mip level, trilinear) -- through raster_tri.
 *
 *   rast_test cmp [seed] [frames]   every variant over random frames; prints mismatches, exit 1 on any
 *   rast_test bench:<v> [seed] [frames]  one variant only (instruction counts under qemu: qemu -d in_asm,exec,nochain
 *                                   + the OoT port tools/handheld/qprof/qlogcount.py; a hint only, the device Bench decides)
 *   rast_test ops                   the lean rows' instruction swaps against what they replace, over all inputs
 *   rast_test mipx                  lean_mip_lf against the float mip level, all 2^32 inputs (run on the desktop)
 *   rast_test fill:<0|1>            100 full-screen solid blended rectangles, GE_2DFILL off / on (hash; qemu counts)
 *   rast_test edge [seed] [n]       GE_EDGESKIP: every skipped right-hand solve leaves xe at maxx (random and adversarial rows)
 *   rast_test nearz [seed] [n]      GE_NEARW's span depth (span_zdq + span_zq_far, 2026-10-04) against the exact depth: n
 *                                   random spans (start -1e4..1e4, slope -1e3..1e3, 1-16 pixels, a third in [-1, 2]), every
 *                                   pixel's 16-bit depth from the 24.8 steps against clamp(floor(exact * 65535)) in double,
 *                                   at most 1 apart; with GE_NEARW=0 the count of the old path's misses (the bug)
 *   env: R_KIND=<k>  only one triangle kind (0 hot, 1 vk, 2 blended, 3 any, 4 scene: a Dam-like ground + walls,
 *                    5 small: hot and vk triangles of ~70 px, the Dam's average; 6 smoke: blended puffs;
 *                    7 explosions: explosion.c's billboard quads, 56x56 repeating sprites, flat, blended, no z write;
 *                    8 the same quads Gouraud-shaded with sloped depth: GE_LEANASM's non-flat loop)
 *        R_T16=1 every texture 16-bit and kept 16-bit (64x128, 128x64, 128x128)
 *        R_DEFER=1 a third of the triangles first as opaque no-depth backgrounds, everything through
 *                  gfx_soft_draw_triangles: the deferred backgrounds' marks, covers and resolves (def_row)
 *        R_PSHADE0=1 GE_PSHADE=0;  R_TRILIN0=1 GE_TRILIN=0;  R_ZPASS=1 every pixel passes the depth test
 *        R_ZPLANE=1 the stored depth a gentle random plane (+0..7 noise) instead of random per pixel: whole spans lie
 *                   behind it, so GE_ZSKIP's skip runs (2026-10-07)
 *        R_CLAMP=1 every texture clamped on one axis or both, coordinates mostly inside the image (GE_LEANCLAMP, 2026-10-04)
 *        R_LOD=1 the RDP's LOD on for three triangles in four, each with its own last level 0-7 (gfx_pc passes them per
 *                draw): GE_MIPLOD's level pick and two-level filter (tribil_texel) in every row loop; R_MIPLOD=<m> GE_MIPLOD
 *                (default the build's). Without R_LOD every draw is a non-LOD draw: the old pick, whatever GE_MIPLOD.
 *   rast_test lod [seed] [frames]   GE_MIPLOD's level choice (mip_pick_n64, a span at a time) against the RDP's rule
 *                                   per pixel (angrylion tclod_2cycle: 10.5 coordinates, the next pixel and the next
 *                                   line, lodfrac_lodtile_signals), and n64_lod_tiles against a transcription of
 *                                   lodfrac_lodtile_signals for every lod and last level (2026-10-03)
 *        R_NEARZ=1 near-eye depths (GE_NEARW, 2026-10-04): a quarter of the vertices at z -8000..-1 (z/w of faces
 *                  within 2 units of the eye) with 1/w up to 64, the rest as usual: every row loop must agree there too
 *        R_OWN=1 GE_FILLRULE's owned edges on for the blended triangles (as gfx_pc sets them for a blended draw
 *                without AA_EN): every row loop must draw the same pixels with them; with R_DEFER=1 R_ZPASS=1 also the
 *                frame drawn without the deferral (nodefer: the opaque backgrounds resolved under a blended draw keep
 *                the inclusive rule they were marked with, RastState.own; R_ZPASS: the deferral's own premise, depth
 *                at 0xFFFF where it defers -- random depth breaks it with or without R_OWN) (2026-10-03)
 *
 * Variants (gfx_soft.c's static switches): base = the build's defaults; scal = the scalar span loops (no NEON:
 * the reference, = the desktop's); nohot = GE_HOTROW=0; lean = GE_LEANROW (hot_row's and hot_row_vk's lean rows,
 * the no-depth and the blended CM rows, a call a row: GE_LEANTRI=0); leancm = the lean hot_row only (GE_LEANVK=0,
 * GE_LEANNOZ=0, GE_LEANBLEND=0, GE_LEANTRI=0); leantri = all of them with the row loop in hot_tri_lean.
 * nofast, noclamp, edgeflip, noempty: the defaults with the named switches flipped (see set_variant); nospan
 * (2026-10-05): GE_TBILSPAN, GE_BILSPAN, GE_LODLEAN and GE_CLAMPSPAN off; noround (2026-10-07): GE_FASTROWS, GE_SPANVEC
 * and GE_ZSKIP off, nosvec and nozskip each alone, and frowsflip GE_FASTROWS the other way (default off since 2026-10-10,
 * so it runs the fast rows; was nofrows).
 * The frame hash (colour + depth of the first variant) must be the same on ARM (NEON) and x86 (scalar). */
#ifndef RT_NO_LEAN /* -DRT_NO_LEAN: a tree before the 2026-10-01 switches */
#define HAVE_LEANROW 1
#endif
#include "gfx_soft.c"

#include <stdarg.h>

uint32_t gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }
int bossGetStageNum(void) { return 33; }
int gfx_pc_frame_w(void) { return 320; }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
#define NT 400
static uint32_t gFB0[W * H], gFBr[W * H];
static uint16_t gZB0[W * H], gZBr[W * H];

typedef struct {
    int w, h, fmt, clamp; /* fmt 0 = 8888, 1 = 5551 */
    uint32_t px[128 * 128];
} Tex;
typedef struct {
    int nt;
    SoftVtx v[3 * NT];
    int tex[NT];          /* texture slot */
    uint8_t mode[NT];     /* bit 0 alpha, 1 ztest, 2 dmask, 3 decal, 4 a_tex, 5 a_in, 6 texa, 7 edge */
    uint8_t ui[NT], nobil[NT], rgbin[NT];
    Tex t[4];
} Frame;

static int gKind = -1;
static int gNearZ; /* R_NEARZ */
static void make_tex(Tex *t) {
    static const int kSz[][2] = { { 32, 32 }, { 64, 64 }, { 64, 128 }, { 128, 64 }, { 16, 16 }, { 32, 64 },
                                  { 8, 8 },   { 48, 40 }, { 33, 33 }, { 65, 65 }, { 128, 128 }, { 64, 32 } };
    const int k = rnd() % (int)(sizeof(kSz) / sizeof(kSz[0]));
    t->w = kSz[k][0]; t->h = kSz[k][1];
    t->fmt = rnd() & 1;
    if (getenv("R_T16")) { /* 16-bit textures kept 16-bit (more than TEX32_SMALL texels), repeating */
        static const int kBig[][2] = { { 64, 128 }, { 128, 64 }, { 128, 128 } };
        const int b = rnd() % 3;
        t->w = kBig[b][0]; t->h = kBig[b][1]; t->fmt = 1;
    }
    t->clamp = (rnd() % 5 == 0) ? 1 + rnd() % 3 : 0;
    if (getenv("R_CLAMP")) t->clamp = 1 + rnd() % 3; /* R_CLAMP (2026-10-04, GE_LEANCLAMP): every texture clamps on one axis or both */
    if (gKind == 7 || gKind == 8) { t->w = t->h = 56; t->clamp = 0; } /* explosion.c's sprites: 56x56, repeating (round 13) */
    const int opaque = gKind != 6 && rnd() % 3 == 0, smooth = rnd() & 1;
    for (int i = 0; i < t->w * t->h; i++) {
        uint32_t c = rnd();
        if (smooth && i) c = (t->px[i - 1] & 0xF0F0F0F0u) | (c & 0x0F0F0F0Fu);
        if (opaque) c |= 0xFF000000u;
        else if (rnd() % 4 == 0) c &= 0x00FFFFFFu;
        if (gKind == 6 || gKind == 7 || gKind == 8) { /* a puff: alpha 0 outside a circle */
            const int x = i % t->w, y = i / t->w, dx = 2 * x - t->w, dy = 2 * y - t->h;
            if (dx * dx + dy * dy > t->w * t->w * 3 / 4) c &= 0x00FFFFFFu;
        }
        t->px[i] = c;
    }
}

/* R_KIND=7: explosions as explosion.c draws them -- billboard quads (two triangles, 0-1-2 and 0-2-3) with texture
 * coordinates 0..55 of a 56x56 repeating texture, one colour and alpha a quad (flat), one depth and 1/w a quad, rotated,
 * 10 to 260 px across and piled up; texture x shade on colour and alpha, blended, z-tested, no z write (round 13) */
static void make_boom(Frame *f) {
    f->nt = 2 * (8 + rnd() % 40);
    for (int q = 0; q < f->nt / 2; q++) {
        const float cx = frnd(-20, 260), cy = frnd(-20, 260), r = (rnd() % 4) ? frnd(5, 60) : frnd(60, 130);
        const float z = frnd(0.0f, 1.0f), iw = frnd(0.05f, 2.0f);
        /* the rotation without sinf/cosf (libm differs between the ARM and x86 builds; sqrtf is exact on both) */
        const float co = frnd(-1.0f, 1.0f), si = (rnd() & 1 ? 1.0f : -1.0f) * sqrtf(1.0f - co * co);
        const uint8_t cr = rnd(), cg = rnd(), cb = rnd(), ca = (rnd() % 3) ? rnd() : 255;
        /* explosion.c's 0..55; a quarter of the quads a little past it (the taps then wrap at the edges) */
        const float lo = (rnd() % 4) ? 0.0f : frnd(-3, 1) / 56, hi = (rnd() % 4) ? 55.0f / 56 : frnd(54, 59) / 56;
        const float uu[4] = { hi, lo, lo, hi }, vv[4] = { lo, lo, hi, hi };
        const float ox[4] = { 1, -1, -1, 1 }, oy[4] = { -1, -1, 1, 1 };
        SoftVtx c[4];
        memset(c, 0, sizeof(c));
        for (int k = 0; k < 4; k++) {
            c[k].x = cx + r * (ox[k] * co - oy[k] * si);
            c[k].y = cy + r * (ox[k] * si + oy[k] * co);
            if (rnd() % 8 == 0) { c[k].x = floorf(c[k].x); c[k].y = floorf(c[k].y); }
            c[k].z = z; c[k].invw = iw; c[k].u = uu[k]; c[k].v = vv[k];
            c[k].r = cr; c[k].g = cg; c[k].b = cb; c[k].a = ca;
            if (gKind == 8) { /* R_KIND=8: the same quads Gouraud-shaded, depth sloped (GE_LEANASM's non-flat loop) */
                c[k].r = rnd(); c[k].g = rnd(); c[k].b = rnd(); c[k].a = (rnd() % 3) ? rnd() : 255;
                c[k].z = z + frnd(-0.05f, 0.05f);
            }
        }
        static const int kTri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
        const int tex = rnd() % 4;
        for (int h = 0; h < 2; h++) {
            const int t = 2 * q + h;
            for (int k = 0; k < 3; k++) f->v[t * 3 + k] = c[kTri[h][k]];
            f->tex[t] = tex;
            f->mode[t] = 1 | 2 | 16 | 32 | (rnd() % 16 == 0 ? 4 : 0); /* blended, z test, a_tex x a_in (some z write) */
            if (gKind == 8 && rnd() % 6 == 0) f->mode[t] |= 8; /* a decal's depth bias */
            f->rgbin[t] = 1;
            f->nobil[t] = rnd() % 32 == 0;
        }
    }
}

static void make_frame(Frame *f) {
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < 4; i++) make_tex(&f->t[i]);
    f->nt = 1 + rnd() % (gKind == 5 ? NT : 96);
    if (gKind == 7 || gKind == 8) { make_boom(f); return; }
    const int scene = gKind == 4;
    for (int t = 0; t < f->nt; t++) {
        const int kind = gKind >= 0 && gKind < 4 ? gKind : gKind == 6 ? 2 : scene || gKind == 5 ? (rnd() % 8 == 0 ? 1 : 0) : (int)(rnd() % 4);
        uint8_t m = 0;
        if (kind == 0) m = 2 | 4 | (rnd() % 12 == 0 ? 8 : 0) | (rnd() % 16 == 0 ? 0 : 0);
        else if (kind == 1) m = (rnd() % 6 ? 2 : 0) | (rnd() % 8 ? 4 : 0) | (rnd() % 4 == 0 ? 1 : 0) | (rnd() % 3 ? 16 : 0);
        else if (kind == 2) m = 1 | (rnd() % 6 ? 2 : 0) | (rnd() & 4) | (rnd() % 8 == 0 ? 8 : 0) | (rnd() & 1 ? 16 : 0) |
                                (rnd() % 4 ? 32 : 0) | (rnd() % 4 == 0 ? 128 : 0);
        else m = (uint8_t)rnd();
        if (gKind == 6) /* smoke: blended sprites, the texture's alpha (x shade), z-tested, mostly no z write */
            m = 1 | 2 | (rnd() % 6 == 0 ? 4 : 0) | (rnd() % 10 == 0 ? 8 : 0) | (rnd() % 5 ? 16 : 0) | (rnd() % 4 ? 32 : 0) |
                (rnd() % 6 == 0 ? 128 : 0);
        if (kind == 0 && rnd() % 20 == 0) m &= (uint8_t)~4; /* an opaque draw that leaves z */
        if (kind == 0 && rnd() % 8 == 0) m = (uint8_t)(rnd() & 12); /* opaque without the depth test (the gun, the sea) */
        if (getenv("R_DEFER") && t < f->nt / 3) m = 0; /* the backgrounds first: opaque, no depth (deferred) */
        f->mode[t] = m;
        f->ui[t] = kind == 3 && rnd() % 8 == 0;
        f->nobil[t] = rnd() % 16 == 0;
        f->rgbin[t] = kind == 3 ? (rnd() % 6 != 0) : 1;
        f->tex[t] = rnd() % 4;
        /* the screen triangle and its depth / 1/w */
        float cx, cy, sz;
        if (scene && t < 6) { cx = frnd(0, 240); cy = frnd(120, 240); sz = frnd(150, 400); }
        else if (gKind == 5) { cx = frnd(0, 240); cy = frnd(0, 240); sz = frnd(3, 14); } /* the Dam's size: ~70 px a triangle */
        else { cx = frnd(-40, 280); cy = frnd(-40, 280); sz = (rnd() % 4) ? frnd(2, 60) : frnd(60, 300); }
        float tpp = (rnd() % 3 == 0) ? frnd(0.02f, 1.0f) : frnd(1.0f, 24.0f); /* texels a pixel, roughly */
        const float tw = (float)f->t[f->tex[t]].w, th = (float)f->t[f->tex[t]].h;
        float u0 = frnd(-4, 4), v0 = frnd(-4, 4);
        if (getenv("R_CLAMP") && rnd() % 4) { /* R_CLAMP: mostly inside the image (a clamped wall, a frond), spans to its edges */
            u0 = frnd(-0.1f, 1.1f); v0 = frnd(-0.1f, 1.1f); tpp = frnd(0.05f, 3.0f);
        }
        const float flatw = (rnd() % 6 == 0) ? frnd(0.05f, 2.0f) : 0.0f; /* affine-ish: one 1/w */
        for (int k = 0; k < 3; k++) {
            SoftVtx *a = &f->v[t * 3 + k];
            a->x = cx + frnd(-sz, sz); a->y = cy + frnd(-sz, sz);
            a->z = (rnd() % 8) ? frnd(0.0f, 1.0f) : frnd(-0.2f, 1.2f);
            a->invw = flatw > 0.0f ? flatw : (rnd() % 24) ? frnd(0.02f, 3.0f) : frnd(-0.5f, 0.02f);
            a->u = u0 + (a->x - cx) * tpp / tw * frnd(0.5f, 1.5f);
            a->v = v0 + (a->y - cy) * tpp / th * frnd(0.5f, 1.5f);
            if (rnd() % 64 == 0) a->u += frnd(-4000, 4000); /* far coordinates: wrap_far, to_fix16's clamp */
            a->r = rnd(); a->g = rnd(); a->b = rnd(); a->a = rnd();
            if (kind == 1 || (kind == 3 && rnd() % 3 == 0)) {
                a->kr = rnd(); a->kg = rnd(); a->kb = rnd(); a->ka = rnd();
                if (rnd() % 4 == 0) a->kr = a->kg = a->kb = a->ka = 0;
            }
        }
        if (rnd() % 12 == 0) { const int k = rnd() % 3; f->v[t * 3 + k].invw = frnd(1e-9f, 1e-4f); } /* near the eye plane */
        if (gNearZ) /* R_NEARZ: faces passing the eye (GE_NEARW's depths) */
            for (int k = 0; k < 3; k++) if (rnd() % 4 == 0) { f->v[t * 3 + k].z = frnd(-8000.0f, -1.0f); f->v[t * 3 + k].invw = frnd(2.0f, 64.0f); }
        if (rnd() % 12 == 0) { /* a sliver */
            SoftVtx *a = &f->v[t * 3], *b = &f->v[t * 3 + 1], *c = &f->v[t * 3 + 2];
            const float k = frnd(-0.5f, 1.5f), e = frnd(-0.05f, 0.05f);
            c->x = a->x + (b->x - a->x) * k + e; c->y = a->y + (b->y - a->y) * k - e;
        }
        if (rnd() % 6 == 0) /* flat */
            for (int k = 1; k < 3; k++) {
                SoftVtx *a = &f->v[t * 3 + k], *b = &f->v[t * 3];
                a->r = b->r; a->g = b->g; a->b = b->b; a->a = b->a;
            }
        if (rnd() % 10 == 0) /* whole-pixel corners (shared edges, inclusive rules) */
            for (int k = 0; k < 3; k++) { f->v[t * 3 + k].x = floorf(f->v[t * 3 + k].x); f->v[t * 3 + k].y = floorf(f->v[t * 3 + k].y); }
    }
}

static struct ShaderProgram gProg;
static uint32_t gId[4];

static void upload(uint32_t id, const Tex *t) {
    static uint8_t buf[128 * 128 * 4];
    if (t->fmt == 0) {
        for (int i = 0; i < t->w * t->h; i++) {
            buf[i * 4 + 0] = (uint8_t)(t->px[i] >> 16); buf[i * 4 + 1] = (uint8_t)(t->px[i] >> 8);
            buf[i * 4 + 2] = (uint8_t)t->px[i]; buf[i * 4 + 3] = (uint8_t)(t->px[i] >> 24);
        }
    } else {
        uint16_t *b16 = (uint16_t *)buf;
        for (int i = 0; i < t->w * t->h; i++) b16[i] = (uint16_t)t->px[i];
    }
    gfx_soft_select_texture(0, id);
    gfx_soft_upload_texture(buf, t->w, t->h, t->fmt == 0 ? GU_PSM_8888 : GU_PSM_5551);
    gfx_soft_set_sampler_parameters(0, true, (t->clamp & 1) ? G_TX_CLAMP : 0, (t->clamp & 2) ? G_TX_CLAMP : 0);
}

/* the variants: each sets gfx_soft.c's switches from the build's defaults */
enum { V_BASE, V_SCAL, V_NOHOT, V_LEAN, V_LEANCM, V_LEANTRI, V_NOASM, V_NODEFER, V_NOFAST, V_NOCLAMP, V_NOEDGE, V_NOEMPTY, V_NOSPAN,
       V_NOROUND, V_NOFROWS, V_NOSVEC, V_NOZSKIP, V_N };
static const char *kName[V_N] = { "base", "scal", "nohot", "lean", "leancm", "leantri", "noasm", "nodefer", "nofast", "noclamp", "edgeflip", "noempty", "nospan",
                                  "noround", "frowsflip", "nosvec", "nozskip" };
static int sRtDef[16]; /* the defaults, captured at the start */

static void set_variant(int v) {
    /* nofast (2026-10-04): the build's defaults with GE_LODFAST, GE_TBILPAIR, GE_BILPAIR, GE_OWNFAST and
     * GE_TBILNEON off (the code before them); on x86 the scalar loops with them off */
    { /* the switches' build defaults (or GE_LODFAST=0 etc. from the environment), off in nofast */
        static int d[5] = { -1, -1, -1, -1, -1 };
        if (d[0] < 0) { static const char *k[5] = { "GE_LODFAST", "GE_TBILPAIR", "GE_BILPAIR", "GE_OWNFAST", "GE_TBILNEON" };
                        for (int i = 0; i < 5; i++) d[i] = !(getenv(k[i]) && getenv(k[i])[0] == '0'); }
        const int on = v != V_NOFAST;
        sLodFast = on && d[0]; sTbilPair = on && d[1]; sBilPair = on && d[2]; sOwnFast = on && d[3];
#if defined(HH_CM_NEON)
        sTbilNeon = on && d[4];
#endif
    }
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
    sLeanClamp = v == V_NOCLAMP ? 0 : sRtDef[7]; /* noclamp (2026-10-04): the defaults with GE_LEANCLAMP=0 */
#endif
    { /* edgeflip (2026-10-05, was noedge): the build's defaults with GE_EDGESKIP the other way (default off since the
       * device Bench 2 of 2026-10-05, so this variant now runs the skip on) */
        static int de = -1;
        if (de < 0) de = getenv("GE_EDGESKIP") && getenv("GE_EDGESKIP")[0] == '1'; /* default off since 2026-10-05 */
        sEdgeSkip = v == V_NOEDGE ? !de : de;
        /* noempty (2026-10-05): GE_EMPTYTRI=0 (the triangle's whole setup before its first row) */
        static int dm = -1;
        if (dm < 0) dm = !(getenv("GE_EMPTYTRI") && getenv("GE_EMPTYTRI")[0] == '0');
        sEmptyTri = v == V_NOEMPTY ? 0 : dm;
    }
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
    { /* nospan (2026-10-05): the build's defaults with GE_TBILSPAN, GE_BILSPAN, GE_LODLEAN and GE_CLAMPSPAN off (the
       * lean rows' loops LEAN_TBIL32P / LEAN_BIL32P, mip_pick_fast and the span functions, as before them) */
        static int d[4] = { -1, -1, -1, -1 };
        if (d[0] < 0) { static const char *k[4] = { "GE_TBILSPAN", "GE_BILSPAN", "GE_LODLEAN", "GE_CLAMPSPAN" };
                        for (int i = 0; i < 4; i++) d[i] = !(getenv(k[i]) && getenv(k[i])[0] == '0'); }
        const int on = v != V_NOSPAN;
        sTbilSpan = on && d[0]; sBilSpan = on && d[1]; sLodLean = on && d[2]; sClampSpan = on && d[3];
    }
#endif
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
    { /* noround (2026-10-07): the build's defaults with GE_FASTROWS, GE_SPANVEC and GE_ZSKIP off (raster_tri_body's row
       * loop, the scalar span set-up, no span skip); nosvec, nozskip: each of them off alone; frowsflip: GE_FASTROWS the
       * other way (default off since the device Benches of 2026-10-10, so this variant runs the fast rows) */
        static int d[3] = { -1, -1, -1 };
        if (d[0] < 0) { static const char *k[3] = { "GE_FASTROWS", "GE_SPANVEC", "GE_ZSKIP" };
                        for (int i = 0; i < 3; i++) d[i] = !(getenv(k[i]) && getenv(k[i])[0] == '0');
                        d[0] = getenv(k[0]) && getenv(k[0])[0] == '1'; } /* GE_FASTROWS: default off */
        sFastRows = v == V_NOROUND ? 0 : v == V_NOFROWS ? !d[0] : d[0];
        sSpanVec = v == V_NOROUND || v == V_NOSVEC ? 0 : d[1];
        sZSkip = v == V_NOROUND || v == V_NOZSKIP ? 0 : d[2];
    }
#endif
    if (v == V_NOFAST || v == V_NOCLAMP || v == V_NOEDGE || v == V_NOEMPTY || v == V_NOSPAN || v == V_NOROUND || v == V_NOFROWS ||
        v == V_NOSVEC || v == V_NOZSKIP) v = V_BASE;
#if defined(HH_CM_NEON)
    sCmNeon = v != V_SCAL; sMipNeon = v != V_SCAL; sVkNeon = v != V_SCAL; sVkaNeon = v != V_SCAL;
    sCmbNeon = v != V_SCAL; sCmNoZ = v != V_SCAL;
    sHotRow = v != V_NOHOT && v != V_SCAL;
#ifdef HAVE_LEANROW
    sLeanRow = v == V_LEAN || v == V_LEANCM || v == V_LEANTRI ? 1 : v == V_BASE ? sRtDef[0] : 0;
    sLeanVk = v == V_LEAN || v == V_LEANTRI ? 1 : v == V_LEANCM ? 0 : v == V_BASE ? sRtDef[1] : 0;
    sLeanTri = v == V_LEANTRI ? 1 : v == V_BASE ? sRtDef[2] : 0;
    sLeanNoz = v == V_LEAN || v == V_LEANTRI ? 1 : v == V_BASE ? sRtDef[3] : 0;
    sLeanBlend = v == V_LEAN || v == V_LEANTRI ? 1 : v == V_BASE ? sRtDef[4] : 0;
    sLeanNp2 = v == V_LEAN || v == V_LEANTRI ? 1 : v == V_BASE ? sRtDef[5] : 0;
#ifndef RT_NO_ASM
    sLeanAsm = v == V_LEAN || v == V_LEANTRI ? 1 : v == V_BASE ? sRtDef[6] : 0;
#endif
    if (v == V_NOASM) { /* the build's defaults with GE_LEANASM=0: the C loop */
        sLeanRow = sRtDef[0]; sLeanVk = sRtDef[1]; sLeanTri = sRtDef[2]; sLeanNoz = sRtDef[3]; sLeanBlend = sRtDef[4];
        sLeanNp2 = sRtDef[5];
    }
#endif
#endif
    (void)v;
}

static void run_frame(const Frame *f, int v) {
    set_variant(v == V_NODEFER ? V_BASE : v);
    memcpy(sFB, gFB0, sizeof(gFB0)); memcpy(sZB, gZB0, sizeof(gZB0));
    const int defer = getenv("R_DEFER") != NULL; /* through gfx_soft_draw_triangles: the deferred backgrounds */
    const int own = getenv("R_OWN") != NULL;     /* GE_FILLRULE: owned edges for the blended triangles */
    gfx_soft_defer = v != V_NODEFER;
    gfx_soft_depth_written = false; sDefRest = 0;
    gfx_soft_bilinear = 1;
    sCurShader = &gProg;
    for (int t = 0; t < f->nt; t++) {
        const uint8_t m = f->mode[t];
        sUseAlpha = m & 1; sDepthTest = (m >> 1) & 1; sDepthMask = (m >> 2) & 1; sZDecal = (m >> 3) & 1;
        gProg.a_tex = (m >> 4) & 1; gProg.a_in = (m >> 5) & 1; gProg.rgb_texa = (m >> 6) & 1;
        gProg.cc.opt_texture_edge = (m >> 7) & 1;
        gProg.rgb_in = f->rgbin[t];
        sUiMode = f->ui[t];
        gfx_soft_bilinear = !f->nobil[t];
        sBoundTex[0] = gId[f->tex[t]];
        gfx_soft_set_own_edges(own && (m & 1));
        if (getenv("R_LOD")) { /* not from rnd(): the frames stay those of a run without R_LOD */
            const uint32_t hsh = (uint32_t)t * 2654435761u;
            sTexLod = (hsh >> 7) % 4 != 0;
            sMaxLevel = sTexLod ? (int)((hsh >> 11) % 8) : 0;
        }
        if (defer) { gfx_soft_draw_triangles((float *)&f->v[t * 3], 3 * sizeof(SoftVtx) / 4, 1); continue; }
        const int px = raster_tri(&f->v[t * 3], &f->v[t * 3 + 1], &f->v[t * 3 + 2], true);
        if (px > 0) gfx_soft_span_px += (uint32_t)px;
    }
    if (defer) def_resolve_all();
    gfx_soft_set_own_edges(false);
}

#ifdef HAVE_LEANROW
/* the mip block's float level and fraction (raster_tri / hot_row_body), for lean_mip_lf */
static void mip_lf_ref(uint32_t u, int *plvl, uint32_t *pfr) {
    const int e = (int)((u >> 23) & 0xff) - 127;
    const float l2 = ((float)e + (float)(u & 0x7fffffu) * (1.0f / 8388608.0f)) * 0.5f;
    int lvl = 0; uint32_t fr = 0;
    if (l2 > 0.0f) { lvl = l2 < 7.0f ? (int)l2 : 7; fr = (uint32_t)((l2 - (float)lvl) * 256.0f); }
    *plvl = lvl; *pfr = fr;
}
static long mip_lf_check(uint32_t u) {
    int l0, l1; uint32_t f0, f1;
    mip_lf_ref(u, &l0, &f0); lean_mip_lf(u, &l1, &f1);
    return l0 != l1 || f0 != f1;
}
#endif
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
/* mode "ops": the lean rows' swaps against what they replace, every input */
static int ops_check(void) {
    long bad = 0;
    for (int i = 0; i < 4000000; i++) { /* NEON vcvtq_s32_f32 = sat_i32 (NaN, infinities, the int32 limits, denormals) */
        static const uint32_t e[] = { 0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF7FFFFFu,
                                      0x4EFFFFFFu, 0x4F000000u, 0xCF000000u, 0xCF000001u, 0x7F800000u, 0xFF800000u,
                                      0x7FC00000u, 0xFFC00001u, 0x7F800001u, 0x00000001u, 0x80000001u, 0x007FFFFFu,
                                      0x477FFF80u, 0xC7800060u };
        const uint32_t u = i < 20 ? e[i] : rnd();
        float f; memcpy(&f, &u, 4);
        if (vgetq_lane_s32(vcvtq_s32_f32(vdupq_n_f32(f)), 0) != sat_i32(f)) bad++;
        /* the depth clamp: vmax/vmin then the NEON multiply and convert = the scalar ternaries, multiply and cast */
        const float zc = f < -1.0f ? -1.0f : f > 2.0f ? 2.0f : f;
        const int32_t zs = (int32_t)(zc * (65535.0f * 256.0f));
        const int32_t zn = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(vmin_f32(vmax_f32(vdup_n_f32(f), vdup_n_f32(-1.0f)),
                                                                          vdup_n_f32(2.0f)), 65535.0f * 256.0f)), 0);
        if (zs != zn) bad++;
    }
    for (int i = 0; i < 4000000; i++) { /* vqshrun #16 = vqmovun(vshr #16) */
        static const int32_t e[] = { 0, -1, 65535 << 16, 65536 << 16, INT32_MAX, INT32_MIN, 255 << 16, -65536, 256 << 16, (256 << 16) - 1 };
        const int32_t a = i < 10 ? e[i] : (int32_t)rnd();
        const int32x4_t q = vdupq_n_s32(a);
        if (vget_lane_u16(vqshrun_n_s32(q, 16), 0) != vget_lane_u16(vqmovun_s32(vshrq_n_s32(q, 16)), 0)) bad++;
    }
    for (int n = 1; n <= PERSP_SPAN; n++) { volatile float fn = (float)n; if (kLeanRcp[n] != 1.0f / fn) bad++; } /* the table */
    for (int i = 0; i < 6000000; i++) { /* the edge solve's clamp + ceil / floor, and the 1/w choice */
        static const uint32_t e[] = { 0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u,
                                      0x49742400u, 0x49742401u, 0x4974233Fu, 0xCF000000u, 0xCF000001u, 0x4F000000u,
                                      0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x2B8CBCCCu, 0x2B8CBCCDu,
                                      0x2B8CBCCBu, 0x00000001u, 0x80000001u, 0x3727C5ACu, 0xB727C5ACu, 0x4B000001u };
        uint32_t u = i < 24 ? e[i] : rnd();
        if (i >= 24 && (i & 3) == 0) u = (u & 0x807FFFFFu) | ((uint32_t)(100 + (u >> 27)) << 23); /* near 1 .. 2^31 */
        float q; memcpy(&q, &u, 4);
        const int nd = (u & 0x7F800000u) == 0 && (u & 0x7FFFFFu) != 0; /* denormal: never an edge q (see lean_iceil_clamp) */
        const float qc = q > 1e6f ? 1e6f : q;
        if (!nd && (lean_iceil_clamp(q) != iceilf(qc) || lean_ifloor_clamp(q) != ifloorf(qc))) bad++;
        const float r0 = (q > 1e-12f) ? 1.0f / q : 0.0f, r1 = lean_rcp_w(q);
        if (memcmp(&r0, &r1, 4) != 0) bad++;
    }
    /* lean_mip_lf = the float code: every exponent with edge and random mantissas, and every mantissa of the
     * exponents where the sum rounds (e 2, 3, 9: the ties that carry into the fraction's bits); all 2^32 inputs
     * on the desktop ("mipx", the same IEEE single operations) */
    for (uint32_t i = 0; i < 4000000; i++)
        bad += mip_lf_check(i < 256 * 64 ? ((i >> 6) << 23) | (i & 32 ? 0x7fffffu - (i & 31) : (i & 31)) : rnd());
    for (uint32_t e = 129; e <= 136; e += 7)
        for (uint32_t m = 0; m < 0x800000u; m++) bad += mip_lf_check(e << 23 | m);
    for (int i = 0; i < 20000000; i++) { /* lean_np2_inside: true only when every tap the span reads is inside */
        const int tw = 2 + (int)(rnd() % 80), th = 2 + (int)(rnd() % 80), n = 1 + (int)(rnd() % 16);
        int32_t uf, vf, duf, dvf;
        if (i & 1) { /* near the edges, small steps */
            uf = (int32_t)(rnd() % (unsigned)((tw + 2) << 16)) - 0x10000; vf = (int32_t)(rnd() % (unsigned)((th + 2) << 16)) - 0x10000;
            duf = (int32_t)(rnd() % 0x40000) - 0x20000; dvf = (int32_t)(rnd() % 0x40000) - 0x20000;
        } else { uf = (int32_t)rnd(); vf = (int32_t)rnd(); duf = (int32_t)rnd() >> (rnd() % 31); dvf = (int32_t)rnd() >> (rnd() % 31); }
        if (!lean_np2_inside(uf, vf, duf, dvf, n, tw, th)) continue;
        int32_t u = (int32_t)((uint32_t)uf - 0x8000u), v = (int32_t)((uint32_t)vf - 0x8000u); /* the loop's wrapping steps */
        for (int k = 0; k < n; k++, u = (int32_t)((uint32_t)u + (uint32_t)duf), v = (int32_t)((uint32_t)v + (uint32_t)dvf))
            if ((u >> 16) < 0 || (u >> 16) + 1 > tw - 1 || (v >> 16) < 0 || (v >> 16) + 1 > th - 1) { bad++; break; }
    }
    { /* GE_2DFILL: blend_solid_row's NEON lanes = its scalar loop, every colour, destination and alpha per channel */
        static uint32_t a[256], b[256];
        for (int ca = 0; ca < 256; ca++)
            for (int c = 0; c < 256; c++) {
                for (int j = 0; j < 256; j++) a[j] = b[j] = 0xFF000000u | (uint32_t)j << 16 | (uint32_t)((j * 7 + c) & 255) << 8 | (uint32_t)(255 - j);
                s2dFill = 1; blend_solid_row(a, 256, c, 255 - c, c ^ 0x55, ca);
                s2dFill = 0; blend_solid_row(b, 256, c, 255 - c, c ^ 0x55, ca);
                bad += memcmp(a, b, sizeof(a)) != 0;
            }
        s2dFill = -1;
    }
    for (uint32_t m = 0; m < 65536; m++) /* lean_div255 = cm_div255 */
        if (vget_lane_u16(lean_div255(vdup_n_u16((uint16_t)m)), 0) != vget_lane_u16(cm_div255(vdup_n_u16((uint16_t)m)), 0)) bad++;
    for (uint32_t x = 0; x < 256; x++) /* the one-multiply lerp = the two-multiply lerp, every x, y, f */
        for (uint32_t y = 0; y < 256; y++)
            for (uint32_t f = 0; f <= 256; f++) {
                const uint16_t one = (uint16_t)((uint16_t)(x << 8) + (uint16_t)((uint16_t)(y - x) * (uint16_t)f)) >> 8;
                const uint16_t two = (uint16_t)((x * (256 - f) + y * f) >> 8);
                if (one != two) bad++;
            }
    printf("ops: %ld differ\n", bad);
    return bad != 0;
}
#endif

/* mode "lod": angrylion-rdp-plus lodfrac_lodtile_signals and tclod_2cycle's tile pair, transcribed (no sharpen,
 * no detail, min level 0), against n64_lod_tiles; then mip_pick_n64's span choice against that rule per pixel */
static void lod_ref(int32_t lod, int max_level, int *t1, int *t2, uint32_t *lf) {
    static int log2table[256];
    if (!log2table[2]) for (int i = 0; i < 256; i++) for (int k = 7; k > 0; k--) if ((i >> k) & 1) { log2table[i] = k; break; }
    uint32_t ltil, dis, mag; int32_t f;
    if (lod & 0x4000) lod = 0x7fff;
    mag = lod < 32;
    ltil = log2table[(lod >> 5) & 0xff];
    dis = ((lod & 0x6000) || ltil >= (uint32_t)max_level) ? 1 : 0;
    f = ((lod << 3) >> ltil) & 0xff;
    if (dis) f = 0xff; else if (mag) f = 0;
    if (dis) ltil = (uint32_t)max_level;
    *t1 = (int)ltil;
    *t2 = (dis || mag) ? (int)ltil : (int)ltil + 1;
    *lf = (uint32_t)f;
}
static int lod_check(int frames) {
    long bad = 0;
    for (int ml = 0; ml < 8; ml++)
        for (int32_t lod = 0; lod < 0x8000; lod++) {
            int a1, a2, b1, b2; uint32_t af, bf;
            lod_ref(lod, ml, &a1, &a2, &af); n64_lod_tiles(lod, ml, &b1, &b2, &bf);
            bad += a1 != b1 || a2 != b2 || af != bf;
        }
    printf("lod: n64_lod_tiles vs lodfrac_lodtile_signals, 8 x 2^15 inputs: %ld differ\n", bad);
    { /* GE_LODLEAN (2026-10-05): n64_lod_tiles_lean = n64_lod_tiles for every lod the pick makes (0..0x3fff, 0x7fff) */
        long lbad = 0, ln = 0;
        for (int ml = 0; ml < 8; ml++)
            for (int32_t lod = 0; lod < 0x8000; lod++) {
                if (lod >= 0x4000 && lod != 0x7fff) continue;
                int a1, a2, b1, b2; uint32_t af, bf;
                n64_lod_tiles(lod, ml, &a1, &a2, &af); n64_lod_tiles_lean(lod, ml, &b1, &b2, &bf);
                ln++; lbad += a1 != b1 || a2 != b2 || af != bf;
            }
        printf("lod: n64_lod_tiles_lean vs n64_lod_tiles, %ld inputs: %ld differ\n", ln, lbad);
        bad += lbad;
    }
    { /* GE_LODFAST: mip_pick_fast = mip_pick_n64, every mode, brilinear on and off, bias scales, last levels, level
       * counts; random spans (plain, wide-range and bit-random floats) with zeros of both signs, infinities, NaNs,
       * denormals and huge values mixed in (2026-10-04) */
        static const uint32_t sp[] = { 0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00001u,
                                       0x7f800001u, 0x00000001u, 0x807fffffu, 0x00400000u, 0x7f7fffffu, 0xff7fffffu,
                                       0x3f800000u, 0xbf800000u, 0x43ffffffu, 0x44000000u, 0x3d000000u, 0x2b8cbccdu };
        static const float lin[] = { 1.0f, 1.18920712f, 0.84089642f, 1.41421356f, 0.70710678f, 2.0f, 0.5f };
        static SoftTex t2;
        const int savelod = sMipLod, savetri = sTrilin, savemax = sMaxLevel; const float savelin = sMipLin;
        long fbad = 0, n = 0, nan = 0, nl2 = 0, lbad2 = 0; (void)nl2; (void)lbad2;
        for (int i = 0; i < 6000000; i++) {
            float a[10];
            for (int k = 0; k < 10; k++) {
                const uint32_t r = rnd();
                if (r % 23 == 0) { const uint32_t b = sp[rnd() % (sizeof(sp) / 4)]; memcpy(&a[k], &b, 4); }
                else if (r % 7 == 0) { uint32_t b = rnd(); memcpy(&a[k], &b, 4); }
                else if (r % 3 == 0) a[k] = frnd(-1.0f, 1.0f) * (float)(1u << (rnd() % 24)) / (float)(1u << (rnd() % 24));
                else a[k] = frnd(-2.0f, 2.0f);
            }
            const float fspan = (float)(1 + rnd() % PERSP_SPAN);
            const int tw = 1 << (rnd() % 8), th = 1 << (rnd() % 8);
            sMipLod = 1 + (int)(rnd() % 3); sTrilin = (int)(rnd() & 1); sMipLin = lin[rnd() % 7];
            sMaxLevel = (int)(rnd() % 8); t2.nlev = 1 + (int)(rnd() % 8);
            int l0, l1; uint32_t f0, f1; bool q0, q1;
            mip_pick_n64(&t2, tw, th, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], fspan, &l0, &f0, &q0);
            mip_pick_fast(&t2, tw, th, (float)tw, (float)th, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], fspan,
                          &l1, &f1, &q1);
            n++; nan += a[0] != a[0] || a[1] != a[1] || a[6] != a[6];
            if (l0 != l1 || q0 != q1 || (q0 && f0 != f1)) fbad++;
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
            { /* GE_LODLEAN: mip_pick_lean = mip_pick_n64 (the switches from a MipSet, GE_MIPFAR's level too) */
                sMipFar = (rnd() % 3) ? 99 : 1 + (int)(rnd() % 4);
                mip_pick_n64(&t2, tw, th, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], fspan, &l0, &f0, &q0);
                MipSet ms; mip_set_get(&ms);
                int l2; uint32_t f2; bool q2;
                mip_pick_lean(&t2, tw, th, (float)tw, (float)th, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], fspan,
                              &ms, &l2, &f2, &q2);
                nl2++;
                if (l0 != l2 || f0 != f2 || q0 != q2) lbad2++;
                sMipFar = 99;
            }
#endif
            if (l0 != l1 || f0 != f1 || q0 != q1) bad++;
        }
        sMipLod = savelod; sTrilin = savetri; sMaxLevel = savemax; sMipLin = savelin;
        printf("lod: mip_pick_fast vs mip_pick_n64, %ld spans (%ld with a NaN input): %ld differ\n", n, nan, fbad);
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
        printf("lod: mip_pick_lean vs mip_pick_n64, %ld spans: %ld differ\n", nl2, lbad2);
        bad += lbad2;
#endif
    }
    /* per pixel: s, t (texels, perspective-divided) at the pixel, the next pixel, the next line, in 10.5 (tcdiv);
     * lod = the largest difference (tclod_4x17_to_15); the span: mip_pick_n64 from the span's ends as raster_tri */
    sMipLod = 1; sMipLin = 1.0f; sTrilin = 1;
    static SoftTex tx; tx.nlev = 8;
    long n = 0, same = 0, near = 0, lvl1 = 0; double fd = 0;
    for (int fr = 0; fr < frames; fr++) {
        const float tw = (float)(16 << (rnd() % 4)), th = (float)(16 << (rnd() % 4));
        sMaxLevel = 1 + (int)(rnd() % 7);
        /* a plane: u/w, v/w, 1/w linear on screen (a wall or floor seen in perspective) */
        const float iw0 = frnd(0.05f, 1.0f), iwx = frnd(-0.004f, 0.004f), iwy = frnd(-0.004f, 0.004f);
        const float k = frnd(0.002f, 0.2f);
        const float uw0 = frnd(-1, 1), uwx = k * frnd(-1, 1), uwy = k * frnd(-1, 1);
        const float vw0 = frnd(-1, 1), vwx = k * frnd(-1, 1), vwy = k * frnd(-1, 1);
        for (int y = 0; y < 64; y++) {
            for (int xs = 0; xs < 64; xs += PERSP_SPAN) {
                const float px = xs + 0.5f, py = y + 0.5f;
                const float sw = iw0 + iwx * px + iwy * py, uw = uw0 + uwx * px + uwy * py, vw = vw0 + vwx * px + vwy * py;
                if (sw <= 0.01f || sw + iwx * PERSP_SPAN <= 0.01f || sw + iwy <= 0.01f) continue;
                const float rw = 1.0f / sw, sw_b = sw + iwx * PERSP_SPAN, rw_b = 1.0f / sw_b;
                const float u_a = uw * rw, v_a = vw * rw;
                const float du = ((uw + uwx * PERSP_SPAN) * rw_b - u_a) * (1.0f / PERSP_SPAN);
                const float dv = ((vw + vwx * PERSP_SPAN) * rw_b - v_a) * (1.0f / PERSP_SPAN);
                int pl; uint32_t pf; bool ptri;
                mip_pick_n64(&tx, (int)tw, (int)th, du, dv, u_a, v_a, uwy, vwy, iwy, rw, rw_b, (float)PERSP_SPAN, &pl, &pf, &ptri);
                const int p2 = ptri ? pl + 1 : pl;
                for (int i = 0; i < PERSP_SPAN; i++) {
                    double S[3], T[3];
                    for (int q = 0; q < 3; q++) {
                        const double X = px + i + (q == 1), Y = py + (q == 2);
                        const double Wq = iw0 + iwx * X + iwy * Y;
                        S[q] = floor((uw0 + uwx * X + uwy * Y) / Wq * tw * 32.0);
                        T[q] = floor((vw0 + vwx * X + vwy * Y) / Wq * th * 32.0);
                    }
                    double m = fabs(S[1] - S[0]);
                    if (fabs(T[1] - T[0]) > m) m = fabs(T[1] - T[0]);
                    if (fabs(S[2] - S[0]) > m) m = fabs(S[2] - S[0]);
                    if (fabs(T[2] - T[0]) > m) m = fabs(T[2] - T[0]);
                    const int32_t lod = m >= 16384.0 ? 0x7fff : (int32_t)m;
                    int r1, r2; uint32_t rf;
                    lod_ref(lod, sMaxLevel, &r1, &r2, &rf);
                    if (rf == 0) r2 = r1; /* TRILERP by 0: level r1 alone */
                    if (r2 == r1) rf = 0; /* one level (magnified, past the last): the fraction reads nothing */
                    const uint32_t pfe = ptri ? pf : 0;
                    n++;
                    /* the same texel mix: the same two levels, or one level both ways */
                    if (r1 == pl && r2 == p2) { same++; fd += fabs((double)rf - (double)pfe); if (abs((int)rf - (int)pfe) <= 32) near++; }
                    else if (abs(r1 - pl) <= 1) lvl1++;
                }
            }
        }
    }
    printf("lod: %ld pixels: %.2f%% the same two levels (%.2f%% of all also within 1/8 in LOD_FRACTION, mean |diff| %.1f/256), "
           "%.2f%% one level off\n", n, 100.0 * same / n, 100.0 * near / n, same ? fd / same : 0.0, 100.0 * lvl1 / n);
    return bad != 0 || same < n * 90 / 100;
}

/* mode "own" (2026-10-04): GE_OWNFAST's own_pxf (the row's term once, the test on the bits) against own_px, for random
 * triangles -- pixel-scale, huge, tiny, whole and half-pixel corners, degenerate, with zeros, infinities, NaNs and
 * denormals mixed in -- at rows and pixels on and around them, both windings; then own_row against own_px's loops */
static float own_rv(void) {
    static const uint32_t sp[] = { 0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00000001u,
                                   0x807fffffu, 0x7f7fffffu, 0x4f000000u, 0xcf000000u, 0x3f000000u };
    const uint32_t r = rnd();
    if (r % 41 == 0) { float f; const uint32_t b = sp[rnd() % (sizeof(sp) / 4)]; memcpy(&f, &b, 4); return f; }
    if (r % 5 == 0) return (float)((int)(rnd() % 600) - 180) * 0.5f;
    if (r % 7 == 0) return frnd(-1e9f, 1e9f);
    return frnd(-180.0f, 420.0f);
}
static int own_check(int n) {
    long bad = 0, pass = 0, rows = 0;
    for (int i = 0; i < n; i++) {
        SoftVtx v[3];
        memset(v, 0, sizeof(v));
        for (int k = 0; k < 3; k++) { v[k].x = own_rv(); v[k].y = own_rv(); }
        if (rnd() % 4 == 0) v[2] = v[rnd() % 2]; /* a degenerate one */
        if (rnd() % 16 == 0) { /* an edge a few denormals long at the origin: products that round to zeros of either sign */
            static const float z[2] = { 0.0f, -0.0f };
            const uint32_t bx = (1u + rnd() % 4u) | ((rnd() & 1u) << 31), by = (1u + rnd() % 4u) | ((rnd() & 1u) << 31);
            const int a = (int)(rnd() % 3), b = (a + 1 + (int)(rnd() % 2)) % 3;
            v[a].x = z[rnd() & 1]; v[a].y = z[rnd() & 1];
            memcpy(&v[b].x, &bx, 4); memcpy(&v[b].y, &by, 4);
        }
        const float s = (rnd() & 1) ? 1.0f : -1.0f;
        OwnTri t;
        own_edge(&t.e[0], &v[1], &v[2], s); own_edge(&t.e[1], &v[2], &v[0], s); own_edge(&t.e[2], &v[0], &v[1], s);
        if (rnd() % 4 == 0) { /* one edge alone: the other two a zero edge every pixel passes (e = 0, a tie edge) */
            const int k = (int)(rnd() % 3);
            for (int j = 0; j < 3; j++) if (j != k) own_edge(&t.e[j], &v[k], &v[k], 1.0f);
            for (int j = 0; j < 3; j++) if (j != k) { t.e[j].px = t.e[j].py = 0.0f; t.e[j].tie = true; t.e[j].lo = 0u; }
        }
        for (int r = 0; r < 4; r++) {
            const int y = (rnd() % 3) ? (int)floorf(v[rnd() % 3].y) + (int)(rnd() % 5) - 2 : (int)(rnd() % 600) - 180;
            const float py = (float)y + 0.5f;
            const float rb[3] = { (py - t.e[0].py) * t.e[0].sex, (py - t.e[1].py) * t.e[1].sex, (py - t.e[2].py) * t.e[2].sex };
            for (int q = 0; q < 8; q++) {
                const int x = (q < 4) ? (int)floorf(v[rnd() % 3].x) + (int)(rnd() % 5) - 2 : (int)(rnd() % 600) - 180;
                const bool a = own_px(&t, x, py), b = own_pxf(&t, rb, x);
                bad += a != b; pass += a;
            }
            int xs = (int)(rnd() % 300) - 30, xe = xs + (int)(rnd() % 300), xs2 = xs, xe2 = xe;
            while (xs < xe && !own_px(&t, xs, py)) xs++;
            while (xe > xs && !own_px(&t, xe - 1, py)) xe--;
            own_row(&t, py, &xs2, &xe2);
            bad += xs != xs2 || xe != xe2; rows++;
        }
    }
    printf("own: %d triangles, %ld rows, %ld pixels owned: %ld differ\n", n, rows, pass, bad);
    return bad != 0;
}

/* GE_EDGESKIP (2026-10-05): every row edge_end_inside lets skip must be one where raster_tri_body's right-hand solve
 * gives xe >= maxx (no change). Rows of random triangles (the solve's own row bases, screen boxes) and adversarial rows
 * (B within a few ulps of -n D, tiny and huge slopes, subnormals, infinities, NaN). Prints skipped / checked / wrong. */
static float fbits(uint32_t u) { union { uint32_t u; float f; } c; c.u = u; return c.f; }
static uint32_t ubits(float f) { union { uint32_t u; float f; } c; c.f = f; return c.u; }
static int edge_solve_ref(float B, float D, int minx) { /* raster_tri_body's K < 0 solve */
    float q = -B / D + 1e-5f; if (q > 1e6f) q = 1e6f;
    return minx + 1 + ifloorf(q);
}
static long sEdgeRows, sEdgeSkips, sEdgeBad;
static void edge_one(float B, float D, int minx, int maxx) {
    float P, M;
    edge_end_setup(maxx - 1 - minx, D, &P, &M);
    sEdgeRows++;
    if (!edge_end_inside(B, P, M)) return;
    sEdgeSkips++;
    if (edge_solve_ref(B, D, minx) < maxx) {
        if (sEdgeBad < 5) printf("  B %a D %a minx %d maxx %d: solve %d\n", B, D, minx, maxx, edge_solve_ref(B, D, minx));
        sEdgeBad++;
    }
}
static int edge_check(long n) {
    SoftVtx v[3];
    memset(v, 0, sizeof(v));
    for (long i = 0; i < n; i++) {
        /* a random triangle: thin slivers, lattice bars, big polygons, sub-pixel ones */
        const int k = (int)(rnd() % 4);
        const float sc = k == 0 ? 2.0f : k == 1 ? 30.0f : k == 2 ? 300.0f : 3000.0f;
        const float cx = frnd(-40.0f, 280.0f), cy = frnd(-40.0f, 280.0f);
        for (int j = 0; j < 3; j++) { v[j].x = cx + frnd(-sc, sc); v[j].y = cy + frnd(-sc, sc); }
        if (rnd() % 3 == 0) { v[2].x = v[0].x + (v[1].x - v[0].x) * frnd(-2.0f, 2.0f) + frnd(-0.01f, 0.01f);
                              v[2].y = v[0].y + (v[1].y - v[0].y) * frnd(-2.0f, 2.0f) + frnd(-0.01f, 0.01f); } /* slivers */
        if (rnd() % 4 == 0) v[rnd() % 3].x = floorf(v[0].x) + (rnd() & 1) * 0.5f; /* vertices on pixel edges / centres */
        const float area = edge(&v[0], &v[1], v[2].x, v[2].y);
        if (area == 0.0f) continue;
        int minx = imax((int)fmin3(v[0].x, v[1].x, v[2].x), 0), maxx = imin(iceilf(fmax3(v[0].x, v[1].x, v[2].x)), 240);
        int miny = imax((int)fmin3(v[0].y, v[1].y, v[2].y), 0), maxy = imin(iceilf(fmax3(v[0].y, v[1].y, v[2].y)), 240);
        if (minx >= maxx || miny >= maxy) continue;
        const float inv_area = 1.0f / area;
        const float wdx[3] = { (v[2].y - v[1].y) * inv_area, (v[0].y - v[2].y) * inv_area, (v[1].y - v[0].y) * inv_area };
        const float fx = minx + 0.5f;
        const SoftVtx *ea[3] = { &v[1], &v[2], &v[0] }, *eb[3] = { &v[2], &v[0], &v[1] };
        for (int e = 0; e < 3; e++) {
            if (!(wdx[e] < 0.0f)) continue;
            const float c = (fx - ea[e]->x) * (eb[e]->y - ea[e]->y), ex = eb[e]->x - ea[e]->x, ey = ea[e]->y;
            for (int y = miny; y < maxy; y++) {
                const float py = y + 0.5f;
                edge_one((c - (py - ey) * ex) * inv_area, wdx[e], minx, maxx);
            }
        }
        /* adversarial rows: B a few ulps either side of the boundary -n D, any slope scale */
        for (int r = 0; r < 8; r++) {
            const int mn = (int)(rnd() % 240), mx = mn + 1 + (int)(rnd() % (240 - mn));
            const float D = -fbits((uint32_t)(rnd() % 0x7f000000u)); /* any finite negative incl. subnormal */
            const float B0 = -(float)(mx - 1 - mn) * D;
            const float B = fbits(ubits(B0) + (uint32_t)((int)(rnd() % 64) - 32));
            edge_one(B, D, mn, mx);
            edge_one(fbits(ubits(B0) + (uint32_t)(rnd() % 4)), D, mn, mx);
        }
    }
    /* the specials */
    const float sp[] = { 0.0f, -0.0f, 1e-45f, -1e-45f, 1e-30f, 1e30f, -1e30f, INFINITY, -INFINITY, NAN, 1.0f, -1.0f };
    for (unsigned a = 0; a < sizeof(sp) / sizeof(sp[0]); a++)
        for (unsigned b = 0; b < sizeof(sp) / sizeof(sp[0]); b++)
            if (sp[b] < 0.0f || sp[b] != sp[b])
                for (int mx = 1; mx < 240; mx += 37) edge_one(sp[a], sp[b], 0, mx);
    printf("edge: %ld rows, %ld skipped, %ld wrong\n", sEdgeRows, sEdgeSkips, sEdgeBad);
    return sEdgeBad != 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "cmp";
    if (!strcmp(mode, "own")) {
        rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
        return own_check(argc > 3 ? atoi(argv[3]) : 1000000);
    }
    if (!strcmp(mode, "edge")) { /* GE_EDGESKIP's row test against the solve (see edge_check) */
        rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
        return edge_check(argc > 3 ? atol(argv[3]) : 1000000);
    }
    if (!strcmp(mode, "nearz")) { /* GE_NEARW's span depth against the exact one (see the header) */
        rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
        if (!rs) rs = 1;
        const long n = argc > 3 ? atol(argv[3]) : 2000000;
        long bad = 0, px = 0, outside = 0;
        for (long i = 0; i < n; i++) {
            const int span = 1 + (int)(rnd() % 16);
            float zf = (rnd() % 3 == 0) ? frnd(-1.0f, 2.0f) : (rnd() & 1) ? frnd(-1e4f, 1e4f) : frnd(-30.0f, 30.0f);
            float zdx = (rnd() & 1) ? frnd(-2.0f, 2.0f) : (rnd() & 1) ? frnd(-30.0f, 30.0f) : frnd(-1e3f, 1e3f);
            if (rnd() % 8 == 0) zdx = 0.0f;
            const int32_t zdq = span_zdq(zdx);
            const int32_t zq = span_z_out(zf) ? span_zq_far(zf, zdx, span, zdq)
                                              : (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            outside += zf < -1.0f || zf > 2.0f;
            for (int k = 0; k < span; k++) {
                const int ours = clamp16_sh8(zq + k * zdq);
                const double ez = ((double)zf + (double)k * (double)zdx) * 65535.0;
                const int ex = ez < 0.0 ? 0 : ez > 65535.0 ? 65535 : (int)ez;
                px++;
                if (ours - ex > 1 || ex - ours > 1) {
                    if (bad < 5) printf("  span %d zf %g zdx %g pixel %d: %d, exact %d\n", span, zf, zdx, k, ours, ex);
                    bad++;
                }
            }
        }
        printf("nearz: GE_NEARW=%d, %ld spans (%ld starting outside [-1, 2]), %ld px, %ld differ by more than 1\n",
               gfx_soft_nearw_on(), n, outside, px, bad);
        return 0;
    }
    if (!strcmp(mode, "lod")) {
        rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
        return lod_check(argc > 3 ? atoi(argv[3]) : 200);
    }
#ifdef HAVE_LEANROW
    if (!strcmp(mode, "mipx")) { /* lean_mip_lf against the float code, all 2^32 inputs (the desktop: ~20 s) */
        long bad = 0;
        for (uint64_t u = 0; u <= 0xffffffffull; u++) bad += mip_lf_check((uint32_t)u);
        printf("mipx: %ld of 2^32 differ\n", bad);
        return bad != 0;
    }
#endif
    if (!strcmp(mode, "2d")) { /* GE_2DSKIP: random sprites in random states, skip on vs off, byte for byte */
        rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
        const int frames = argc > 3 ? atoi(argv[3]) : 200;
        gfx_soft_init(); select_target(false); sNatStale = false;
        static Frame f2;
        long bad = 0, skipped = 0, drawn = 0;
        for (int fr = 0; fr < frames; fr++) {
            make_frame(&f2);
            for (int i = 0; i < 4; i++) { gId[i] = gfx_soft_new_texture(); upload(gId[i], &f2.t[i]); }
            for (int i = 0; i < W * H; i++) gFB0[i] = rnd() | 0xFF000000u;
            static Vtx2D q[2 * 64];
            const int ns = 1 + rnd() % 64;
            for (int k = 0; k < ns; k++) {
                Vtx2D *a = &q[2 * k], *b = &q[2 * k + 1];
                memset(a, 0, 2 * sizeof(*a));
                a->x = (uint16_t)(rnd() % 260); a->y = (uint16_t)(rnd() % 260);
                b->x = (uint16_t)(a->x + rnd() % 80); b->y = (uint16_t)(a->y + rnd() % 80);
                a->u = (int16_t)(rnd() % 64 - 16); a->v = (int16_t)(rnd() % 64 - 16);
                b->u = (int16_t)(a->u + rnd() % 160 - 40); b->v = (int16_t)(a->v + rnd() % 160 - 40);
                b->r = rnd(); b->g = rnd(); b->b = rnd(); b->a = (rnd() & 1) ? rnd() % 90 : rnd();
            }
            const uint32_t st = rnd(), tex = rnd() % 4;
            for (int v = 0; v < 2; v++) {
                memcpy(sFB, gFB0, sizeof(gFB0));
                s2dSkip = v; s2dFill = v;
                sUseAlpha = st & 1; gProg.cc.opt_texture_edge = (st >> 1) & 1; gProg.a_tex = (st >> 2) & 1;
                gProg.a_in = (st >> 3) & 1; gProg.rgb_tex = (st >> 4) & 1; gProg.rgb_in = (st >> 5) & 1;
                gProg.rgb_texa = ((st >> 6) & 7) == 0; sTexAdd = ((st >> 9) & 7) == 0; sTexLerp = ((st >> 12) & 7) == 0;
                gfx_soft_rect_flip = ((st >> 15) & 3) == 0; gProg.cc.used_textures[0] = ((st >> 17) & 7) != 0;
                gProg.cc.num_inputs = 1;
                sCurShader = &gProg; sBoundTex[0] = gId[tex];
                gfx_scegu_draw_triangles_2d((float *)q, (size_t)ns * 2 * sizeof(Vtx2D) / 4, (size_t)ns);
                gfx_soft_rect_flip = false;
                if (v == 0) { memcpy(gFBr, sFB, sizeof(gFBr)); continue; }
                long d = 0;
                for (int i = 0; i < W * H; i++) d += sFB[i] != gFBr[i];
                bad += d;
                for (int i = 0; i < W * H; i++) drawn += gFBr[i] != gFB0[i];
            }
            if (sUseAlpha && gProg.cc.used_textures[0] && (!gProg.a_tex || gProg.a_in)) skipped++;
            sArenaUsed = 0; sNextTexId = 1; memset(sTex, 0, sizeof(sTex));
        }
        gProg.cc.used_textures[0] = true;
        printf("2d: %d frames (%ld with skippable states), %ld px drawn, %ld px differ\n", frames, skipped, drawn, bad);
        return bad != 0;
    }
    if (!strncmp(mode, "fill:", 5)) { /* fill:<0|1>: 100 full-screen solid rectangles at alpha 128 (GE_2DFILL off / on) */
        gfx_soft_init(); select_target(false); sNatStale = false;
        static Vtx2D q[2];
        memset(q, 0, sizeof(q));
        q[1].x = W; q[1].y = H; q[1].r = 200; q[1].g = 30; q[1].b = 10; q[1].a = 128;
        for (int i = 0; i < W * H; i++) sFB[i] = rnd() | 0xFF000000u;
        sUseAlpha = 1; gProg.cc.used_textures[0] = false; gProg.cc.opt_texture_edge = 0; sCurShader = &gProg;
        s2dFill = mode[5] == '1';
        for (int k = 0; k < 100; k++) gfx_scegu_draw_triangles_2d((float *)q, 2 * sizeof(Vtx2D) / 4, 1);
        uint32_t h = 2166136261u;
        for (int i = 0; i < W * H; i++) h = (h ^ sFB[i]) * 16777619u;
        printf("%s: hash %08x\n", mode, (unsigned)h);
        return 0;
    }
    if (!strcmp(mode, "ops")) {
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
        return ops_check();
#else
        return 0;
#endif
    }
    rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const int frames = argc > 3 ? atoi(argv[3]) : 100;
    if (getenv("R_KIND")) gKind = atoi(getenv("R_KIND"));
    gNearZ = getenv("R_NEARZ") != NULL;
    if (getenv("R_PSHADE0")) setenv("GE_PSHADE", "0", 1);
    if (getenv("R_TRILIN0")) setenv("GE_TRILIN", "0", 1);
    if (getenv("R_MIPLOD")) setenv("GE_MIPLOD", getenv("R_MIPLOD"), 1);
    gfx_soft_init();
    select_target(true);
    sNatStale = false;
    gProg.cc.used_textures[0] = true;
    gProg.rgb_tex = true; gProg.rgb_in = true; gProg.a_tex = false; gProg.a_in = true;
#if defined(HH_CM_NEON) && defined(HAVE_LEANROW)
    if (sLeanRow < 0) { const char *e = getenv("GE_LEANROW"); sLeanRow = !(e && e[0] == '0'); }
    if (sLeanVk < 0) { const char *e = getenv("GE_LEANVK"); sLeanVk = !(e && e[0] == '0'); }
    if (sLeanTri < 0) { const char *e = getenv("GE_LEANTRI"); sLeanTri = e && e[0] == '1'; } /* default off (2026-10-02) */
    if (sLeanNoz < 0) { const char *e = getenv("GE_LEANNOZ"); sLeanNoz = !(e && e[0] == '0'); }
    if (sLeanBlend < 0) { const char *e = getenv("GE_LEANBLEND"); sLeanBlend = !(e && e[0] == '0'); }
    if (sLeanNp2 < 0) { const char *e = getenv("GE_LEANNP2"); sLeanNp2 = !(e && e[0] == '0'); }
    sRtDef[0] = sLeanRow; sRtDef[1] = sLeanVk; sRtDef[2] = sLeanTri; sRtDef[3] = sLeanNoz; sRtDef[4] = sLeanBlend;
    sRtDef[5] = sLeanNp2;
    if (sLeanClamp < 0) { const char *e = getenv("GE_LEANCLAMP"); sLeanClamp = !(e && e[0] == '0'); }
    sRtDef[7] = sLeanClamp;
#ifndef RT_NO_ASM
    if (sLeanAsm < 0) { const char *e = getenv("GE_LEANASM"); sLeanAsm = e && e[0] == '1'; } /* the build's default: off */
    sRtDef[6] = sLeanAsm;
#endif
#endif
    int vlist[V_N], nv = 0;
    if (strncmp(mode, "bench:", 6) == 0) {
        for (int v = 0; v < V_N; v++) if (!strcmp(mode + 6, kName[v])) vlist[nv++] = v;
    } else {
        for (int v = 0; v < V_N; v++)
            if (v != V_NODEFER || (getenv("R_DEFER") && getenv("R_OWN") && getenv("R_ZPASS"))) vlist[nv++] = v;
    }
#if !defined(HH_CM_NEON)
    nv = 0; vlist[nv++] = V_SCAL; vlist[nv++] = V_NOFAST; vlist[nv++] = V_NOEDGE; vlist[nv++] = V_NOEMPTY;
    if (getenv("R_DEFER") && getenv("R_OWN") && getenv("R_ZPASS")) vlist[nv++] = V_NODEFER;
#endif
#ifndef HAVE_LEANROW
    for (int i = 0; i < nv; i++) if (vlist[i] == V_LEAN || vlist[i] == V_LEANCM || vlist[i] == V_LEANTRI) { memmove(&vlist[i], &vlist[i + 1], (size_t)(nv - i - 1) * sizeof(int)); nv--; i--; }
#endif
    long bad = 0, px = 0; uint32_t hsum = 2166136261u;
    static Frame f;
    const int zpass = getenv("R_ZPASS") != NULL;
    for (int fr = 0; fr < frames; fr++) {
        make_frame(&f);
        for (int i = 0; i < W * H; i++) {
            gFB0[i] = rnd() | 0xFF000000u;
            gZB0[i] = (rnd() % 16) ? (uint16_t)rnd() : 0xFFFF;
            if (zpass) gZB0[i] = 0xFFFF;
        }
        if (getenv("R_ZPLANE")) { /* GE_ZSKIP (2026-10-07): a smooth stored depth, so whole spans lie behind it */
            const int base = (int)(rnd() % 60000u), dx = (int)(rnd() % 9u) - 4, dy = (int)(rnd() % 9u) - 4;
            for (int i = 0; i < W * H; i++) {
                const int z = base + dx * (i % W) + dy * (i / W) + (int)(rnd() % 8u);
                gZB0[i] = (uint16_t)(z < 0 ? 0 : z > 65535 ? 65535 : z);
            }
        }
        for (int i = 0; i < 4; i++) { gId[i] = gfx_soft_new_texture(); upload(gId[i], &f.t[i]); }
        for (int k = 0; k < nv; k++) {
            run_frame(&f, vlist[k]);
            if (k == 0) { memcpy(gFBr, sFB, sizeof(gFBr)); memcpy(gZBr, sZB, sizeof(gZBr)); continue; }
            long d = 0;
            for (int i = 0; i < W * H; i++) d += (sFB[i] != gFBr[i]) || (sZB[i] != gZBr[i]);
            if (d) {
                bad += d;
                printf("frame %d: %s vs %s: %ld px differ\n", fr, kName[vlist[k]], kName[vlist[0]], d);
            }
        }
        for (int i = 0; i < W * H; i++) { hsum = (hsum ^ gFBr[i]) * 16777619u; hsum = (hsum ^ gZBr[i]) * 16777619u; }
        long changed = 0;
        for (int i = 0; i < W * H; i++) changed += gFBr[i] != gFB0[i] || gZBr[i] != gZB0[i];
        px += changed;
        /* uploads bump-allocate: reset as texman_clear does between frames */
        sArenaUsed = 0; sNextTexId = 1; memset(sTex, 0, sizeof(sTex));
        memset(sARows, 0, sizeof(sARows));
    }
    printf("%s: %d frames, %d variants (", mode, frames, nv);
    for (int k = 0; k < nv; k++) printf("%s%s", k ? " " : "", kName[vlist[k]]);
    printf("), %ld px changed by the draws, %ld px differ, hash %08x, span px %llu, deferred px %u\n", px, bad, (unsigned)hsum,
           (unsigned long long)gfx_soft_span_px, (unsigned)gfx_soft_defer_px);
    return bad != 0;
}
