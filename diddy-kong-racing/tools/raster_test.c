/* raster_test.c: the 3D rows through the real renderer, variant against variant (2026-10-04, the final speed sweep;
 * after the OoT port's tools/handheld/raster_test.c and the GoldenEye port's rast_test.c). Includes port/sdl/gfx_soft.c
 * and draws random frames through gfx_soft_draw_triangles, the way gfx_pc drives it: a frame opens (start_frame),
 * batches of triangles with their state, the frame ends (end_frame, the deferred sky settled).
 *
 * Kinds of frame (argument 3, or all mixed):
 *   0 world   z-tested opaque texture x shade (decals, z writes on and off, flat and smooth, clamped / odd sizes)
 *   1 alpha   blended and alpha-tested (every alpha source, texture-edge alphas, opaque edges, z write on and off)
 *   2 water   z-tested, no z write, blended by the shade or texel alpha (Whale Bay's water: the branch-free blend)
 *   3 sky     a no-z opaque sky opening the frame (the deferred sky), then world and blended draws over it
 *   4 fog     per-pixel fog (the factor in the vertex alpha), with and without a constant alpha (amode 3)
 *   5 sprite  blended sprites (the span skip: alpha-0 borders), with and without z
 *   6 mixed   all of the above plus the scalar-only combines (tint, add, decal), nearest, flat fills
 *   7 small   kind 6 with small triangles (the per-row and per-span work)
 * Variants (each frame drawn from the same start by each; colour and depth compared byte for byte):
 *   base  the defaults (every switch on)
 *   old   the switches this round added off (the rows as before)
 *   scal  the scalar loops (DKR_CMNEON=0: what the x86 proxy runs)
 *   and one variant per new switch alone off.
 *   raster_test [seed] [frames] [kind]   prints the per-variant difference counts and the frame hash; exit 1 if any
 *                                        variant differs. RT_PROF=<variant name>: draw only that variant (profiling).
 * Build and run: tools/raster_test.sh (ARM under qemu-arm and x86 -m32; the hashes must agree). */
#include "gfx_soft.c"

#include <stdarg.h>

u32 gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
int gfx_trace_frames;
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
u32 port_time_us(void) { return 0; }
int gfx_hud_exact_on(void) { return 1; }
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }

static uint32_t rs = 1; /* (one rnd() per full expression: argument order differs between ARM and x86 gcc) */
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static int chance(int pct) { return (int)(rnd() % 100) < pct; }

#define W 240
#define H 240

/* ---- textures: 32-bit ARGB (DKR keeps none 16-bit), power-of-two or odd, repeat or clamp, every alpha kind */
#define NTEX 14
static uint32_t gTexId[NTEX];
static int gTexA[NTEX]; /* 0 opaque, 1 cut-out (0/255), 2 smooth, 3 sprite (alpha-0 border), 4 water (alpha everywhere) */
static void make_tex(int i, int w, int h, int akind, int cms, int cmt) {
    static uint8_t buf[128 * 128 * 4];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = &buf[(y * w + x) * 4];
            p[0] = (uint8_t)rnd(); p[1] = (uint8_t)rnd(); p[2] = (uint8_t)rnd();
            int a = 255;
            if (akind == 1) a = chance(40) ? 0 : 255;
            else if (akind == 2) a = (int)(rnd() & 255);
            else if (akind == 3) { const int cx = x * 2 - w, cy = y * 2 - h; a = (cx * cx + cy * cy < (w * w) / 3) ? (int)(rnd() & 255) : 0; }
            else if (akind == 4) a = 60 + (int)(rnd() % 140);
            p[3] = (uint8_t)a;
        }
    gTexId[i] = gfx_soft_new_texture(); gTexA[i] = akind;
    gfx_soft_select_texture(0, gTexId[i]);
    gfx_soft_upload_texture(buf, w, h, GU_PSM_8888);
    gfx_soft_set_sampler_parameters(0, true, cms ? G_TX_CLAMP : 0, cmt ? G_TX_CLAMP : 0);
}
static void make_textures(void) {
    make_tex(0, 32, 32, 0, 0, 0); make_tex(1, 64, 32, 0, 0, 0); make_tex(2, 16, 64, 0, 0, 0);
    make_tex(3, 32, 32, 0, 1, 0); make_tex(4, 24, 20, 0, 0, 0); make_tex(5, 32, 16, 0, 0, 1);
    make_tex(6, 32, 32, 1, 0, 0); make_tex(7, 32, 32, 2, 0, 0); make_tex(8, 64, 64, 3, 1, 1);
    make_tex(9, 32, 32, 4, 0, 0); make_tex(10, 64, 64, 3, 0, 0); make_tex(11, 8, 8, 0, 0, 0);
    make_tex(12, 128, 64, 0, 0, 0); make_tex(13, 20, 32, 2, 1, 0);
}
static int pick_tex(int akind) {
    for (int k = 0; k < 64; k++) { const int i = (int)(rnd() % NTEX); if (akind < 0 || gTexA[i] == akind) return i; }
    return 0;
}

/* ---- triangles */
static SoftVtx gV[3 * 64];
static int gNT;
static int gSmall;
static uint8_t gFlat[4];
static SoftVtx vtx(float cx, float cy, float r, bool flat, int persp, float z0) {
    SoftVtx v; memset(&v, 0, sizeof(v));
    v.x = cx + frnd(-r, r); v.y = cy + frnd(-r, r);
    if (chance(3)) { v.x = floorf(v.x) + (chance(50) ? 0.5f : 0.0f); v.y = floorf(v.y) + (chance(50) ? 0.5f : 0.0f); }
    v.z = z0 + frnd(-0.1f, 0.1f);
    if (chance(2)) v.z = frnd(-1.5f, 2.5f);  /* past the depth clamps */
    v.invw = persp == 2 ? frnd(0.02f, 0.3f) : persp == 1 ? frnd(0.3f, 3.0f) : (chance(50) ? 0.0f : -frnd(0.1f, 1.0f));
    if (persp && chance(2)) v.invw = frnd(1e-6f, 1e-3f); /* near the eye plane */
    v.u = frnd(-1.5f, 2.5f); v.v = frnd(-1.5f, 2.5f);
    if (chance(5)) { v.u = frnd(0.0f, 1.0f); v.v = frnd(0.0f, 1.0f); }
    if (flat) { v.r = gFlat[0]; v.g = gFlat[1]; v.b = gFlat[2]; v.a = gFlat[3]; }
    else { v.r = (uint8_t)rnd(); v.g = (uint8_t)rnd(); v.b = (uint8_t)rnd(); v.a = (uint8_t)rnd(); }
    return v;
}
static void make_tris(int n, float rmin, float rmax, int alpha_lo, int alpha_hi) {
    gNT = n;
    const bool flat = chance(30);
    gFlat[0] = (uint8_t)rnd(); gFlat[1] = (uint8_t)rnd(); gFlat[2] = (uint8_t)rnd();
    gFlat[3] = (uint8_t)(alpha_lo + (int)(rnd() % (unsigned)(alpha_hi - alpha_lo + 1)));
    const int persp = (int)(rnd() % 10) < 7 ? 1 : (chance(50) ? 2 : 0);
    for (int t = 0; t < n; t++) {
        const float r = gSmall ? frnd(1.0f, 12.0f) : frnd(rmin, rmax);
        const float cx = frnd(-40.0f, W + 40.0f), cy = frnd(-40.0f, H + 40.0f), z0 = frnd(0.0f, 1.0f);
        for (int k = 0; k < 3; k++) {
            gV[t * 3 + k] = vtx(cx, cy, r, flat, persp, z0);
            if (!flat) gV[t * 3 + k].a = (uint8_t)(alpha_lo + (int)(rnd() % (unsigned)(alpha_hi - alpha_lo + 1)));
        }
        if (chance(10)) { /* a shared edge with the previous triangle (strips) */
            if (t > 0) { gV[t * 3] = gV[t * 3 - 1]; gV[t * 3 + 1] = gV[t * 3 - 2]; }
        }
    }
}

/* ---- state, as gfx_pc sets it */
static struct ShaderProgram gProg;
static void set_shader(bool tex, bool rgb_tex, bool rgb_in, bool a_tex, bool a_in, bool edge) {
    memset(&gProg, 0, sizeof(gProg));
    gProg.rgb_tex = rgb_tex; gProg.rgb_in = rgb_in; gProg.a_tex = a_tex; gProg.a_in = a_in;
    gProg.cc.num_inputs = 1; gProg.cc.used_textures[0] = tex; gProg.cc.opt_texture_edge = edge;
    sCurShader = &gProg;
}
static void reset_state(void) {
    sDepthTest = true; sDepthMask = true; sZDecal = false; sUseAlpha = false; sEdgeOpaque = false;
    gfx_soft_set_fog(false, 0, 0, 0, -1);
    gfx_scegu_set_tex_lerp(false, 0, 0, 0);
    sTexAdd = false; sForceIn = false; sTex2 = false;
    gfx_soft_bilinear = 1;
    gfx_soft_set_own_edges(false);
    set_shader(true, true, true, true, true, false);
}
static void draw(void) {
    gfx_soft_draw_triangles((float *)gV, (size_t)gNT * 3 * sizeof(SoftVtx) / 4, (size_t)gNT);
}
static void bind(int ti) { sBoundTex[0] = gTexId[ti]; }
/* the same triangles again a few depth steps behind, as a decal (the z bias decides) */
static void redraw_decal(void) {
    if (!chance(30)) return;
    const float dz = (float)(1 + (int)(rnd() % 6)) / 65535.0f;
    for (int i = 0; i < gNT * 3; i++) gV[i].z += dz;
    sZDecal = true;
    bind(pick_tex(-1));
    draw();
}

static void batch_world(void) {
    reset_state();
    const int a = (int)(rnd() % 4); /* the combine's alpha: tex x shade, tex, shade (opaque: unused) */
    set_shader(true, true, true, a != 2, a != 1, false);
    bind(chance(80) ? pick_tex(0) : pick_tex(-1));
    sZDecal = chance(10);
    sDepthMask = chance(85);
    if (chance(3)) gfx_soft_bilinear = 0;
    if (chance(5)) gfx_soft_set_own_edges(true);
    make_tris(1 + (int)(rnd() % 6), 10.0f, 90.0f, 0, 255);
    draw();
    redraw_decal();
}
static void batch_alpha(void) {
    reset_state();
    const int a = (int)(rnd() % 3);
    const bool edge = chance(40);
    set_shader(true, true, chance(90), a != 2, a != 1, edge);
    sUseAlpha = true;
    sEdgeOpaque = edge && chance(50);
    sDepthMask = chance(50);
    sDepthTest = chance(90);
    sZDecal = chance(5);
    if (chance(3)) gfx_soft_bilinear = 0;
    if (chance(30)) gfx_soft_set_own_edges(true);
    bind(pick_tex(edge ? 1 : (int)(rnd() % 5)));
    { const int n = 1 + (int)(rnd() % 5), lo = chance(20) ? 255 : 0; make_tris(n, 10.0f, 80.0f, lo, 255); }
    draw();
    redraw_decal();
}
static void batch_water(void) {
    reset_state();
    const int a = (int)(rnd() % 3);
    set_shader(true, true, true, a != 2, a != 1, false);
    sUseAlpha = true; sDepthMask = false;
    if (chance(20)) gfx_soft_set_own_edges(true);
    bind(chance(50) ? pick_tex(4) : pick_tex(0));
    make_tris(2 + (int)(rnd() % 6), 30.0f, 140.0f, 40, 230);
    draw();
}
static void batch_sky(void) {
    reset_state();
    set_shader(true, true, true, true, true, false);
    sDepthTest = false; sDepthMask = false;
    bind(pick_tex(0));
    make_tris(2 + (int)(rnd() % 6), 60.0f, 200.0f, 255, 255);
    draw();
}
static void batch_fog(void) {
    reset_state();
    const int a = (int)(rnd() % 3);
    set_shader(true, true, true, a != 2, a != 1, false);
    const int fga = chance(40) ? (int)(rnd() % 256) : -1;
    { const int r = (int)(rnd() & 255), g = (int)(rnd() & 255), b = (int)(rnd() & 255); gfx_soft_set_fog(true, r, g, b, fga); }
    sUseAlpha = fga >= 0 ? chance(70) : chance(30);
    sDepthMask = chance(70);
    sDepthTest = chance(65); /* no z: the fogged sky (the scalar loops before DKR_LEANF) */
    if (chance(15)) gfx_soft_set_own_edges(true);
    bind(pick_tex(sUseAlpha ? -1 : 0));
    make_tris(1 + (int)(rnd() % 5), 10.0f, 90.0f, 0, 255);
    draw();
}
static void batch_sprite(void) {
    reset_state();
    const int a = (int)(rnd() % 2);
    set_shader(true, chance(80), true, true, a == 0, false);
    sUseAlpha = true; sDepthMask = chance(20); sDepthTest = chance(60);
    bind(pick_tex(3));
    make_tris(1 + (int)(rnd() % 4), 15.0f, 80.0f, 0, 255);
    draw();
}
static void batch_other(void) {
    reset_state();
    const int k = (int)(rnd() % 6);
    if (k == 0) { set_shader(true, true, true, true, true, false); { const uint8_t r = (uint8_t)rnd(), g = (uint8_t)rnd(), b = (uint8_t)rnd(); gfx_scegu_set_tex_lerp(true, r, g, b); } }
    else if (k == 1) { set_shader(true, true, true, true, true, false); sTexAdd = true; }
    else if (k == 2) { set_shader(false, false, true, false, true, false); } /* shade only */
    else if (k == 3) { set_shader(true, true, false, true, false, false); }  /* texture only */
    else if (k == 4) { set_shader(true, true, true, true, true, false); sForceIn = true; }
    else { set_shader(false, false, true, false, true, false); sDepthTest = false; sDepthMask = false; }
    sUseAlpha = chance(40); sDepthMask = sDepthMask && chance(70);
    bind(pick_tex(-1));
    make_tris(1 + (int)(rnd() % 4), 10.0f, 80.0f, 0, 255);
    draw();
}

static void frame(int kind) {
    gfx_soft_start_frame();
    const int nb = gSmall ? 40 : 10 + (int)(rnd() % 10);
    if (kind == 3 || (kind >= 6 && chance(50))) for (int i = 1 + (int)(rnd() % 3); i > 0; i--) batch_sky();
    for (int b = 0; b < nb; b++) {
        int k = kind;
        if (kind == 3) k = chance(70) ? 0 : 1 + (int)(rnd() % 2);
        if (kind >= 6) k = (int)(rnd() % 7);
        switch (k) {
        case 0: batch_world(); break;
        case 1: batch_alpha(); break;
        case 2: batch_water(); break;
        case 3: batch_sky(); break;
        case 4: batch_fog(); break;
        case 5: batch_sprite(); break;
        default: batch_other(); break;
        }
    }
    gfx_soft_end_frame();
}

/* ---- variants */
typedef struct { const char *name; int cmneon; int lean; int leanz; int leana; int leans; int leanf; int fr; int sv; int zs; } Variant;
static const Variant kVar[] = {
    { "base", 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { "old",  1, 0, 0, 0, 0, 0, 1, 1, 1 },
    { "scal", 0, 0, 0, 0, 0, 0, 1, 1, 1 },
    { "nolz", 1, 1, 0, 1, 1, 1, 1, 1, 1 },
    { "nola", 1, 1, 1, 0, 1, 1, 1, 1, 1 },
    { "nols", 1, 1, 1, 1, 0, 1, 1, 1, 1 },
    { "nolf", 1, 1, 1, 1, 1, 0, 1, 1, 1 },
    { "lean", 1, 1, 0, 0, 0, 0, 1, 1, 1 },
    { "nofr", 1, 1, 1, 1, 1, 1, 0, 1, 1 }, /* the speed round (2026-10-07): DKR_FASTROWS=0 */
    { "nosv", 1, 1, 1, 1, 1, 1, 1, 0, 1 }, /* DKR_SPANVEC=0 */
    { "nozs", 1, 1, 1, 1, 1, 1, 1, 1, 0 }, /* DKR_ZSKIP=0 */
    { "rnd0", 1, 1, 1, 1, 1, 1, 0, 0, 0 }, /* the round off: the rows as before it */
    { "oldv", 1, 0, 0, 0, 0, 0, 0, 1, 1 }, /* the old rows through raster_tri's loop */
};
#define NVAR ((int)(sizeof(kVar) / sizeof(kVar[0])))
static void set_variant(const Variant *v) {
#if defined(HH_CM_NEON)
    sCmNeon = v->cmneon;
#ifdef RT_LEAN
    sLeanRows = v->lean; sLeanZ = v->leanz; sLeanA = v->leana; sLeanS = v->leans; sLeanF = v->leanf;
#endif
#ifdef RT_XSPEED
    sFastRows = v->fr; sSpanVec = v->sv; sZSkip = v->zs;
#endif
#endif
    (void)v;
}

static uint32_t hash_frame(uint32_t h) {
    for (int i = 0; i < W * H; i++) h = (h ^ sNatFB[i]) * 16777619u;
    for (int i = 0; i < W * H; i++) h = (h ^ sNatZB[i]) * 16777619u;
    return h;
}

int main(int argc, char **argv) {
    const uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u;
    const int frames = argc > 2 ? atoi(argv[2]) : 100;
    const int kind0 = argc > 3 ? atoi(argv[3]) : -1;
    const char *prof = getenv("RT_PROF");
    gfx_soft_set_render_res(W, H, W, H);
    rs = 12345; gfx_soft_init(); ensure_targets(); make_textures();
    static uint32_t ref[W * H]; static uint16_t refz[W * H];
    long diff[NVAR] = { 0 }, dframes[NVAR] = { 0 };
    uint32_t hsum = 2166136261u;
    for (int fr = 0; fr < frames; fr++) {
        const uint32_t fseed = (seed * 2654435761u) ^ (uint32_t)(fr * 40503u + 1);
        const int kind = kind0 >= 0 ? kind0 : (int)(fseed % 8);
        int first = 1;
        for (int vi = 0; vi < NVAR; vi++) {
            if (prof && strcmp(prof, kVar[vi].name)) continue;
#if !defined(HH_CM_NEON)
            if (vi != 0) continue; /* x86: one path, the scalar loops */
#endif
            set_variant(&kVar[vi]);
            rs = fseed ? fseed : 1; gSmall = kind == 7;
            frame(kind == 7 ? 6 : kind);
            if (first) {
                memcpy(ref, sNatFB, sizeof(ref)); memcpy(refz, sNatZB, sizeof(refz));
                hsum = hash_frame(hsum); first = 0;
            } else {
                long d = 0;
                for (int i = 0; i < W * H; i++) d += (sNatFB[i] != ref[i]) | (sNatZB[i] != refz[i]);
                if (d) { dframes[vi]++; diff[vi] += d; if (dframes[vi] <= 3) printf("frame %d (kind %d): %s differs from %s in %ld px\n", fr, kind, kVar[vi].name, prof ? prof : kVar[0].name, d); }
            }
        }
    }
    if (getenv("RT_DUMP")) { FILE *f = fopen(getenv("RT_DUMP"), "wb"); if (f) { fwrite(ref, 1, sizeof(ref), f); fwrite(refz, 1, sizeof(refz), f); fclose(f); } }
    if (prof) printf("span px %llu, triangles %llu\n", (unsigned long long)gfx_soft_span_px, (unsigned long long)gfx_soft_tris);
    set_variant(&kVar[0]);
    long bad = 0;
    printf("raster_test seed %u, %d frames, kind %d:", seed, frames, kind0);
    for (int vi = 1; vi < NVAR; vi++) { printf(" %s %ld/%ld", kVar[vi].name, diff[vi], dframes[vi]); bad += diff[vi]; }
    printf("; hash %08x\n", (unsigned)hsum);
    return bad ? 1 : 0;
}
