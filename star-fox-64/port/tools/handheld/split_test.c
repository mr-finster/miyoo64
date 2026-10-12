/* split_test.c: the Miyoo Mini's second core in the renderer (gfx_soft.c raster_tri's row split, MIYOO), through the
 * real renderer and the real worker (src/port/sdl/miyoo_worker.c, a thread on another core under qemu-arm too).
 * Includes gfx_soft.c and miyoo_worker.c, built with -DHANDHELD -DMIYOO, and draws random frames through
 * gfx_soft_draw_triangles: random textures (RGBA, alpha 0 / 255 / anything, clamp or repeat, power-of-two and not),
 * random state per batch (depth test and write, decal bias, alpha test and blend, the texture-edge modes, texel
 * lerp, ADD, decal, per-pixel fog with and without its constant alpha, flat and smooth shade, perspective or not,
 * bilinear with the magnified-only gate on and off, the owned-edge rule), big and small triangles, either winding,
 * on the 320x240 native target; and the deferred backdrops' replays (SF64_NOZDEFER=2: an opaque no-depth backdrop
 * queued, blended draws over it set off partial replays, then the full one) on an internal 272x204 target.
 * After every frame the colour and depth buffers go into one FNV hash.
 *
 *   split_test [seed] [frames]   prints "hash <h> split <triangles shared> tris <all> live <replay spans>"
 * Run it with SF64_THREADS=1 (one core: nothing shared), with the defaults, and with every triangle of 2+ rows
 * shared (SF64_SPLITPX=0 SF64_SPLITROWS=2): the hashes must agree (port/tools/handheld/split_test.sh). Needs no ROM. */
#define _GNU_SOURCE /* miyoo_worker.c: CPU_SET (it defines it itself, too late once gfx_soft.c has pulled the headers in) */
#include "gfx_soft.c"
#include "miyoo_worker.c"

#include <stdarg.h>

u32 gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 320, 240, 4.0f / 3.0f };
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
u32 port_time_us(void) { return 0; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }
unsigned char gPortSceneRam[16];
void gfx_sdl12_native_to_panel(float x, float y, float *px, float *py) { *px = x; *py = y; }
const unsigned char *port_vi_gamma8(void) { return NULL; }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 320
#define H 240
#define NTEX 8
static struct ShaderProgram gProg;
static uint32_t gTex[NTEX];
static uint32_t hsum = 2166136261u;

static void make_textures(void) {
    static uint8_t buf[64 * 64 * 4];
    static const int dims[NTEX][2] = { { 32, 32 }, { 64, 32 }, { 16, 16 }, { 64, 64 }, { 8, 8 }, { 24, 16 }, { 32, 8 }, { 16, 64 } };
    for (int t = 0; t < NTEX; t++) {
        const int w = dims[t][0], h = dims[t][1], amode = t % 4;
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = (uint8_t)rnd(); buf[i * 4 + 1] = (uint8_t)rnd(); buf[i * 4 + 2] = (uint8_t)rnd();
            /* alpha: opaque, cut-out (0 / 255 runs: the cell skip), smooth, anything */
            buf[i * 4 + 3] = amode == 0 ? 255 : amode == 1 ? (((i / 7) ^ (i / (w * 3))) & 1 ? 255 : 0)
                           : amode == 2 ? (uint8_t)(i * 255 / (w * h)) : (uint8_t)rnd();
        }
        gTex[t] = gfx_soft_new_texture();
        gfx_soft_select_texture(0, gTex[t]);
        gfx_soft_upload_texture(buf, w, h, GU_PSM_8888);
        gfx_soft_set_sampler_parameters(0, true, (rnd() & 1) ? G_TX_CLAMP : 0, (rnd() & 1) ? G_TX_CLAMP : 0);
    }
}

static void random_state(bool allow_alpha) {
    const bool textured = (rnd() % 5) != 0;
    gfx_soft_set_own_edges((rnd() % 4) == 0);
    gfx_soft_bilinear = (rnd() % 4) != 0;
    gfx_soft_magbilin = (rnd() & 1);
    sDepthTest = (rnd() % 4) != 0; sDepthMask = (rnd() % 3) != 0; sZDecal = (rnd() % 6) == 0;
    sUseAlpha = allow_alpha && (rnd() % 2) == 0;
    sEdgeOpaque = (int)(rnd() % 3);
    sTexAdd = (rnd() % 8) == 0; sForceIn = (rnd() % 8) == 0;
    if ((rnd() % 5) == 0) gfx_soft_set_fog(true, (int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255), (rnd() & 1) ? -1 : (int)(rnd() & 255));
    else gfx_soft_set_fog(false, 0, 0, 0, -1);
    gfx_scegu_set_tex_lerp((rnd() % 6) == 0, (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd());
    memset(&gProg, 0, sizeof(gProg));
    gProg.rgb_tex = textured && (rnd() % 6) != 0; gProg.rgb_in = (rnd() % 5) != 0;
    gProg.a_tex = textured && (rnd() & 1); gProg.a_in = (rnd() % 4) != 0;
    gProg.rgb_texa = textured && (rnd() % 10) == 0; gProg.rgb_decal = textured && (rnd() % 10) == 0;
    gProg.cc.num_inputs = 1; gProg.cc.used_textures[0] = textured;
    gProg.cc.opt_texture_edge = textured && (rnd() % 3) == 0;
    sCurShader = &gProg;
    sBoundTex[0] = gTex[rnd() % NTEX];
}

static SoftVtx vtx(float x, float y, bool flat, const uint8_t c[4]) {
    SoftVtx v; memset(&v, 0, sizeof(v));
    v.x = x; v.y = y; v.z = frnd(-0.2f, 1.1f); v.invw = (rnd() % 4) ? frnd(0.05f, 2.0f) : 1.0f;
    v.u = frnd(-1.5f, 2.5f); v.v = frnd(-1.5f, 2.5f);
    v.r = flat ? c[0] : (uint8_t)rnd(); v.g = flat ? c[1] : (uint8_t)rnd(); v.b = flat ? c[2] : (uint8_t)rnd(); v.a = flat ? c[3] : (uint8_t)rnd();
    return v;
}
static SoftVtx gT[3 * 64];
static int random_tris(float w, float h) {
    const int n = 1 + (int)(rnd() % 24);
    const bool flat = rnd() & 1;
    const uint8_t c[4] = { (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd() };
    for (int i = 0; i < n; i++) {
        const int kind = (int)(rnd() % 4); /* 0 small, 1 medium, 2 screen-sized, 3 past the screen (the guard band) */
        const float r = kind == 0 ? 6.0f : kind == 1 ? 40.0f : kind == 2 ? 160.0f : 400.0f;
        const float cx = frnd(-20, w + 20), cy = frnd(-20, h + 20);
        for (int k = 0; k < 3; k++) gT[i * 3 + k] = vtx(cx + frnd(-r, r), cy + frnd(-r, r), flat, c);
    }
    return n;
}

static void hash_target(void) {
    for (int i = 0; i < sW * sH; i++) hsum = (hsum ^ sFB[i]) * 16777619u;
    for (int i = 0; i < sW * sH; i++) hsum = (hsum ^ sZB[i]) * 16777619u;
}

#define IW 272
#define IH 204
int main(int argc, char **argv) {
    rs = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const int frames = argc > 2 ? atoi(argv[2]) : 300;
    miyoo_worker_start();
    gfx_soft_set_render_res(W, H, W, H);
    gfx_soft_init();
    ensure_targets();
    { extern int gfx_soft_hud_unsquish; gfx_soft_hud_unsquish = 0; }
    make_textures();
    long tris = 0;
    uint64_t live = 0;
    for (int fr = 0; fr < frames; fr++) { /* the native target: everything drawn directly */
        sNozDefer = 0;
        select_target(true);
        for (int i = 0; i < sW * sH; i++) { sFB[i] = 0xFF000000u | (rnd() & 0xFFFFFF); sZB[i] = (uint16_t)(0x8000 + (rnd() & 0x7FFF)); }
        sNatStale = false;
        const int nb = 2 + (int)(rnd() % 6);
        for (int b = 0; b < nb; b++) {
            random_state(true);
            const int n = random_tris((float)sW, (float)sH);
            gfx_soft_draw_triangles((float *)gT, (size_t)n * 3 * sizeof(SoftVtx) / 4, (size_t)n);
            tris += n;
        }
        hash_target();
    }
    gfx_soft_set_render_res(W, H, IW, IH); /* the internal target: the deferred backdrops and their replays */
    ensure_targets();
    for (int fr = 0; fr < frames / 2; fr++) {
        sNozDefer = 2; sDeferGate = true; sIntDrawn = false; sTagNext = 1; sDfNB = sDfNT = 0;
        select_target(true);
        if (sFB != sIntFB) { printf("split_test: not drawing to the internal target\n"); return 2; }
        for (int i = 0; i < sW * sH; i++) { sFB[i] = 0; sZB[i] = 0xFFFF; }
        sDfLive = 0;
        random_state(false); sUseAlpha = false; sDepthTest = false; sFogPx = false; /* an opaque no-depth backdrop: queued */
        int n = random_tris((float)sW, (float)sH);
        gfx_soft_draw_triangles((float *)gT, (size_t)n * 3 * sizeof(SoftVtx) / 4, (size_t)n);
        tris += n;
        const int nb = 1 + (int)(rnd() % 5);
        for (int b = 0; b < nb; b++) { /* blended draws over it: partial replays, the early flush */
            random_state(true); sUseAlpha = true;
            n = random_tris((float)sW, (float)sH);
            gfx_soft_draw_triangles((float *)gT, (size_t)n * 3 * sizeof(SoftVtx) / 4, (size_t)n);
            tris += n;
        }
        defer_flush();
        live += sDfLive; /* the replays' span count: the worker's share is added to it */
        hash_target();
    }
    unsigned jobs = 0, sleeps = 0; unsigned long long spins = 0;
    miyoo_worker_stats(&jobs, &sleeps, &spins);
    printf("hash %08x split %u tris %ld live %llu (worker %s, handoffs %u)\n", hsum, (unsigned)gfx_soft_split_tris, tris,
           (unsigned long long)live, miyoo_worker_ready() ? "on" : "off", jobs);
    return 0;
}
