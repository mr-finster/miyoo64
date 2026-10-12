/* halfwater_test.c: the half-resolution water layer (OOT_HALFWATER, 2026-10-02, a test switch) through the real renderer.
 * Includes src/port/hh/gfx/gfx_soft.c and draws synthetic scenes through gfx_soft_draw_triangles (and one 2D sprite
 * through gfx_scegu_draw_triangles_2d), as the game's display lists reach it: opaque walls and floor writing z, then
 * the see-through draws, then solid things drawn after them.
 * ARM (qemu-arm) only: the layer lives in the NEON section (the desktop proxy's scalar build has none).
 *
 *   halfwater_test scene <water|flat> <outdir> <variant>...   PPMs <outdir>/<scene>_<variant>.ppm + stats
 *   halfwater_test check [seed] [frames]                      the invariants below (exit 1 on a failure)
 *   halfwater_test hashes [seed] [frames]                     each random frame's hash, off and water (tree A/B)
 *   halfwater_test bench <scene> <variant> <reps>             the scene drawn reps times (qemu counts)
 *   variants: off, water (OOT_HALFWATER=1), waterpx (the same, the composite pixel by pixel)
 *
 * check, over random scenes (seeded) of water-like draws (fogged, a constant alpha, no texel alpha; the other combines,
 * fetches, the grain tile, blended twice): (1) a frame whose see-through draws all fail the qualifying tests (z
 * written, decals, texture edges, UI, texel alpha, no constant alpha) is identical with the switch on; (2) an opaque
 * quad drawn after the water, nearer than everything, is identical on all its pixels (the layer composited before it);
 * (3) an opaque quad drawn before water that is all behind it (by more than the tolerance) is identical on all its
 * pixels; (4) a flat water (one colour, one alpha) over a flat wall is within 2 levels of the full-res blend away from
 * its edges; (5) the layer is empty after every frame; (6) the composite's 8-pixel NEON path draws what its
 * pixel-by-pixel path draws. The file builds against any tree with the layer (HW_TREE=<tree> in the script): hashes
 * from two trees show a change to the layer kept the water's pixels. */
#include "gfx_soft.c"

#include <math.h>
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

/* ---- textures ---- */
enum { TX_BRICK, TX_FLOOR, TX_SMOKE, TX_FIRE, TX_SPARK, TX_GLOW, TX_WGRAIN, TX_WDETAIL, TX_WPLAIN, TX_SKY, TX_CRATE,
       TX_FLAT, TX_HUD, TX_N };
static uint32_t gTex[TX_N];
static void upload32(int slot, const uint32_t *argb, int w, int h, int clamp) {
    static uint8_t buf[128 * 128 * 4];
    for (int i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = (uint8_t)(argb[i] >> 16); buf[i * 4 + 1] = (uint8_t)(argb[i] >> 8);
        buf[i * 4 + 2] = (uint8_t)argb[i]; buf[i * 4 + 3] = (uint8_t)(argb[i] >> 24);
    }
    if (!gTex[slot]) gTex[slot] = gfx_soft_new_texture();
    gfx_soft_select_texture(0, gTex[slot]);
    gfx_soft_upload_texture(buf, w, h, GU_PSM_8888);
    gfx_soft_set_sampler_parameters(0, true, clamp ? G_TX_CLAMP : 0, clamp ? G_TX_CLAMP : 0);
}
static uint32_t px_argb(int a, int r, int g, int b) {
    a = a < 0 ? 0 : a > 255 ? 255 : a; r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}
static void make_textures(void) {
    static uint32_t t[128 * 128];
    uint32_t save = rs; rs = 777;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* bricks */
        const int mortar = (y % 8 == 0) || ((x + ((y / 8) & 1) * 8) % 16 == 0);
        const int n = (int)(rnd() % 30);
        t[y * 32 + x] = mortar ? px_argb(255, 90 + n, 85 + n, 80 + n) : px_argb(255, 150 + n, 70 + n / 2, 50 + n / 2);
    }
    upload32(TX_BRICK, t, 32, 32, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* floor tiles */
        const int c = ((x / 8) + (y / 8)) & 1, n = (int)(rnd() % 24);
        t[y * 32 + x] = c ? px_argb(255, 120 + n, 110 + n, 90 + n) : px_argb(255, 70 + n, 75 + n, 60 + n);
    }
    upload32(TX_FLOOR, t, 32, 32, 0);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { /* a soft, lumpy puff */
        const float dx = (x - 31.5f) / 32.0f, dy = (y - 31.5f) / 32.0f, r2 = dx * dx + dy * dy;
        const float lump = 0.75f + 0.25f * sinf(x * 0.45f) * cosf(y * 0.37f) + 0.1f * frnd(-1, 1);
        float a = r2 < 1.0f ? powf(1.0f - r2, 1.5f) * lump : 0.0f;
        const int g = 170 + (int)(60 * lump);
        t[y * 64 + x] = px_argb((int)(a * 255.0f), g, g, g);
    }
    upload32(TX_SMOKE, t, 64, 64, 1);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* flame: intensity, a tongue shape */
        const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f;
        const float wdt = 0.75f * (1.0f - (dy + 1.0f) * 0.4f);
        float i = 1.0f - fabsf(dx) / (wdt > 0.05f ? wdt : 0.05f);
        i = i < 0 ? 0 : i * (0.7f + 0.3f * frnd(0, 1)) * (dy > 0.8f ? 0.0f : 1.0f);
        const int v = (int)(i * 255.0f);
        t[y * 32 + x] = px_argb(v, v, v, v);
    }
    upload32(TX_FIRE, t, 32, 32, 1);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { /* spark */
        const float r = sqrtf((x - 3.5f) * (x - 3.5f) + (y - 3.5f) * (y - 3.5f)) / 4.0f;
        const int a = r < 1.0f ? (int)(255.0f * (1.0f - r * r)) : 0;
        t[y * 8 + x] = px_argb(a, 255, 255, 220);
    }
    upload32(TX_SPARK, t, 8, 8, 1);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* glow (Navi) */
        const float r = sqrtf((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f)) / 16.0f;
        const int a = r < 1.0f ? (int)(255.0f * powf(1.0f - r, 2.0f)) : 0;
        t[y * 32 + x] = px_argb(a, 255, 255, 255);
    }
    upload32(TX_GLOW, t, 32, 32, 1);
    for (int i = 0; i < 32 * 32; i++) { const int n = (int)(rnd() % 50); t[i] = px_argb(255, 30 + n, 110 + n, 120 + n); }
    upload32(TX_WGRAIN, t, 32, 32, 0);
    for (int i = 0; i < 64 * 64; i++) { const int n = (int)(rnd() % 120); t[i] = px_argb(255, 60 + n, 140 + n / 2, 150 + n / 2); }
    upload32(TX_WDETAIL, t, 64, 64, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* the plain layer's light streaks */
        const int s = (int)(40.0f * sinf((x + y * 0.5f) * 0.4f)) + (int)(rnd() % 20);
        t[y * 32 + x] = px_argb(255, 40 + s, 120 + s, 130 + s);
    }
    upload32(TX_WPLAIN, t, 32, 32, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) t[y * 32 + x] = px_argb(255, 110 + y, 150 + y, 230);
    upload32(TX_SKY, t, 32, 32, 0);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
        const int edge = x < 2 || y < 2 || x > 29 || y > 29 || x == y || x == 31 - y;
        t[y * 32 + x] = edge ? px_argb(255, 80, 50, 20) : px_argb(255, 170 + (int)(rnd() % 20), 120, 60);
    }
    upload32(TX_CRATE, t, 32, 32, 0);
    for (int i = 0; i < 16 * 16; i++) t[i] = px_argb(255, 200, 120, 40);
    upload32(TX_FLAT, t, 16, 16, 0);
    for (int i = 0; i < 16 * 16; i++) t[i] = px_argb((i % 16) < 2 || (i / 16) < 2 ? 255 : 160, 255, 230, 80);
    upload32(TX_HUD, t, 16, 16, 1);
    rs = save;
}

/* ---- draw state ---- */
static struct ShaderProgram gProg;
typedef struct {
    bool ztest, zwrite, alpha, decal, edge, ui, fog;
    int fga, fogr, fogg, fogb;
    bool rgb_tex, rgb_in, a_tex, a_in, texlerp; int lk[3];
    int tex, detail; int es, et; uint16_t dw4[4];
} Mode;
static void set_mode(const Mode *m) {
    gHhSceneId = -1;
    gfx_soft_bilinear = 1;
    sDepthTest = m->ztest; sDepthMask = m->zwrite; sZDecal = m->decal; sUseAlpha = m->alpha;
    sEdgeOpaque = false;
    gfx_soft_set_fog(m->fog, m->fogr, m->fogg, m->fogb, m->fga);
    gProg.rgb_tex = m->rgb_tex; gProg.rgb_in = m->rgb_in; gProg.a_tex = m->a_tex; gProg.a_in = m->a_in;
    gProg.rgb_texa = false; gProg.cc.opt_texture_edge = m->edge; gProg.cc.num_inputs = 1;
    gProg.cc.used_textures[0] = true;
    sCurShader = &gProg;
    gfx_scegu_set_tex_lerp(m->texlerp, (uint8_t)m->lk[0], (uint8_t)m->lk[1], (uint8_t)m->lk[2]);
    sTexAdd = false; sForceIn = false;
    gfx_scegu_set_ui_mode(m->ui);
    sBoundTex[0] = gTex[m->tex];
    gfx_soft_set_detail(m->detail ? gTex[m->detail] : 0, m->es, m->et, 0x12345, 0x6789, m->dw4);
}
static Mode mode_opaque(int tex) { Mode m; memset(&m, 0, sizeof(m)); m.ztest = m.zwrite = 1; m.rgb_tex = m.rgb_in = 1; m.a_in = 1; m.fga = -1; m.tex = tex; return m; }
static Mode mode_xlu(int tex) { Mode m = mode_opaque(tex); m.zwrite = 0; m.alpha = 1; m.a_tex = 1; return m; }

#define MAXV (3 * 2048)
static SoftVtx gV[MAXV]; static uint8_t gTw[MAXV / 3]; static int gNV;
static void begin(void) { gNV = 0; }
static void vtx(float x, float y, float z, float iw, float u, float v, int r, int g, int b, int a) {
    SoftVtx *p = &gV[gNV++];
    p->x = x; p->y = y; p->z = z; p->invw = iw; p->u = u; p->v = v;
    p->r = (uint8_t)r; p->g = (uint8_t)g; p->b = (uint8_t)b; p->a = (uint8_t)a;
}
/* a screen quad x0..x1, y0..y1, its top edge at z0 / 1/w iw0, its bottom at z1 / iw1 (u, v times 1/w: perspective) */
static void quadp(float x0, float y0, float x1, float y1, float z0, float z1, float iw0, float iw1, float u0, float v0,
                  float u1, float v1, int r, int g, int b, int a0, int a1) {
    vtx(x0, y0, z0, iw0, u0, v0, r, g, b, a0); vtx(x1, y0, z0, iw0, u1, v0, r, g, b, a0); vtx(x1, y1, z1, iw1, u1, v1, r, g, b, a1);
    vtx(x0, y0, z0, iw0, u0, v0, r, g, b, a0); vtx(x1, y1, z1, iw1, u1, v1, r, g, b, a1); vtx(x0, y1, z1, iw1, u0, v1, r, g, b, a1);
}
static void quad(float x0, float y0, float x1, float y1, float z, int r, int g, int b, int a) {
    quadp(x0, y0, x1, y1, z, z, 1.0f, 1.0f, 0, 0, 1, 1, r, g, b, a, a);
}
/* a billboard rotated by rot (radians) about its centre */
static void bill(float cx, float cy, float s, float rot, float z, int r, int g, int b, int a) {
    const float c = cosf(rot) * s * 0.5f, sn = sinf(rot) * s * 0.5f;
    const float px[4] = { cx - c + sn, cx + c + sn, cx + c - sn, cx - c - sn }, py[4] = { cy - sn - c, cy + sn - c, cy + sn + c, cy - sn + c };
    const float uu[4] = { 0, 1, 1, 0 }, vv[4] = { 0, 0, 1, 1 };
    const int k[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) vtx(px[k[i]], py[k[i]], z, 1.0f, uu[k[i]], vv[k[i]], r, g, b, a);
}
static void flush_tris(bool twice) {
    const int nt = gNV / 3;
    for (int t = 0; t < nt; t++) gTw[t] = twice;
    gfx_soft_tri_twice = twice ? gTw : NULL;
    gfx_soft_draw_triangles((float *)gV, (size_t)gNV * sizeof(SoftVtx) / 4, (size_t)nt);
    gfx_soft_tri_twice = NULL;
    gNV = 0;
}
static void draw_sky(void) {
    Mode m = mode_opaque(TX_SKY); m.ztest = m.zwrite = 0; m.rgb_in = 0;
    set_mode(&m);
    gfx_scegu_set_sky_pass(true);
    begin(); quadp(0, 0, 240, 60, 0, 0, 1, 1, 0, 0, 3, 1, 255, 255, 255, 255, 255); flush_tris(false);
    gfx_scegu_set_sky_pass(false);
}
static void hud_sprite(void) {
    Mode m = mode_xlu(TX_HUD); m.ztest = 0; m.ui = 1; set_mode(&m);
    Vtx2D s[2]; memset(s, 0, sizeof(s));
    s[0].x = 8; s[0].y = 8; s[1].x = 24; s[1].y = 24; s[0].u = 0; s[0].v = 0; s[1].u = 16; s[1].v = 16;
    s[1].r = s[1].g = s[1].b = s[1].a = 255;
    gfx_scegu_draw_triangles_2d((float *)s, 2 * sizeof(Vtx2D) / 4, 1);
}

static void frame_begin(uint32_t clear) {
    for (int i = 0; i < W * H; i++) { sFB[i] = clear | 0xFF000000u; sZB[i] = 0xFFFF; }
    sNatStale = false;
}
static void frame_end(void) { sky_settle(); /* gfx_soft_end_frame's */ }

/* the world: sky band, back wall, floor in perspective, a pillar crossing the view (a wall edge the water passes behind) */
static void draw_world(bool pillar) {
    draw_sky();
    Mode m = mode_opaque(TX_BRICK); set_mode(&m);
    begin(); quadp(0, 40, 240, 132, 0.95f, 0.95f, 0.2f, 0.2f, 0, 0, 6, 2.5f, 255, 255, 255, 255, 255); flush_tris(false);
    m = mode_opaque(TX_FLOOR); set_mode(&m);
    begin(); quadp(0, 130, 240, 240, 0.95f, 0.55f, 0.2f, 1.0f, 0, 0, 1.6f, 1.2f, 230, 230, 230, 255, 255); flush_tris(false);
    if (pillar) {
        m = mode_opaque(TX_BRICK); set_mode(&m);
        begin(); quadp(98, 20, 134, 240, 0.6f, 0.6f, 1, 1, 0, 0, 1, 6, 200, 200, 200, 255, 255); flush_tris(false);
    }
}

/* the Deku Tree basement water, synthetic: a floor and a ledge under / through a two-layer water plane (the grain
 * layer with the detail tile, then the plain layer; fogged at the constant alpha 127, every triangle drawn twice) */
static void scene_water(void) {
    frame_begin(0x102020);
    Mode m = mode_opaque(TX_BRICK); set_mode(&m);
    begin(); quadp(0, 0, 240, 112, 0.97f, 0.97f, 0.15f, 0.15f, 0, 0, 7, 3, 150, 150, 150, 255, 255); flush_tris(false);
    m = mode_opaque(TX_FLOOR); set_mode(&m);
    begin(); quadp(0, 100, 240, 240, 0.97f, 0.63f, 0.15f, 1.0f, 0, 0, 2.2f, 1.6f, 160, 160, 140, 255, 255); flush_tris(false);
    m = mode_opaque(TX_CRATE); set_mode(&m); /* a ledge rising through the water */
    begin(); quadp(150, 128, 214, 186, 0.70f, 0.80f, 1, 1, 0, 0, 2, 2, 230, 230, 230, 255, 255); flush_tris(false);
    for (int layer = 0; layer < 2; layer++) {
        Mode w = mode_xlu(layer == 0 ? TX_WGRAIN : TX_WPLAIN);
        w.a_tex = 0; w.fog = 1; w.fga = 127; w.fogr = 20; w.fogg = 50; w.fogb = 60;
        if (layer == 0) { w.detail = TX_WDETAIL; w.es = 1; w.et = 1; w.dw4[0] = w.dw4[1] = w.dw4[2] = w.dw4[3] = 110; }
        set_mode(&w); begin();
        const int NX = 6, NY = 5;
        for (int gy = 0; gy < NY; gy++) for (int gx = 0; gx < NX; gx++) {
            const float y0 = 110 + 130.0f * gy / NY, y1 = 110 + 130.0f * (gy + 1) / NY;
            const float t0 = (float)gy / NY, t1 = (float)(gy + 1) / NY;
            const float z0 = 0.945f - 0.34f * t0, z1 = 0.945f - 0.34f * t1, iw0 = 0.15f + 0.85f * t0, iw1 = 0.15f + 0.85f * t1;
            const float x0 = 240.0f * gx / NX, x1 = 240.0f * (gx + 1) / NX;
            const float u0 = 3.0f * gx / NX + layer * 0.3f, u1 = 3.0f * (gx + 1) / NX + layer * 0.3f;
            const float sh = layer ? 0.17f : 0.0f;
            quadp(x0, y0, x1, y1, z0, z1, iw0, iw1, u0, 2.0f * t0 + sh, u1, 2.0f * t1 + sh, 220, 230, 230,
                  (int)(120 - 100 * t0), (int)(120 - 100 * t1));
        }
        flush_tris(true);
    }
    frame_end();
}
/* a flat water: one colour, one constant alpha (fogged, the shade's alpha 0: no fog), over a flat wall (the composite
 * against the full-res blend) */
static void scene_flat(void) {
    frame_begin(0x000000);
    Mode m = mode_opaque(TX_FLAT); set_mode(&m);
    begin(); quad(0, 0, 240, 240, 0.9f, 100, 160, 220, 255); flush_tris(false);
    m = mode_xlu(TX_FLAT); m.a_tex = 0; m.fog = 1; m.fga = 140; set_mode(&m);
    begin(); quad(40, 40, 200, 200, 0.5f, 255, 255, 255, 0); flush_tris(false);
    m.fga = 77; set_mode(&m);
    begin(); quad(60, 60, 180, 180, 0.5f, 90, 255, 30, 0); flush_tris(false);
    frame_end();
}

static const char *kScene[] = { "water", "flat" };
#define NSCENE 2
static void draw_scene(int s) { if (s == 0) scene_water(); else scene_flat(); }

static int set_variant(const char *v) {
    setenv("OOT_HALFWATER", "0", 1); /* (the default is on since 2026-10-03: "off" says so) */
    if (strcmp(v, "off") && strcmp(v, "water") && strcmp(v, "waterpx")) return 0;
    if (strcmp(v, "off")) setenv("OOT_HALFWATER", "1", 1);
    sHfxMode = -1; /* hfx_init reads the environment again */
    sHfxCompPx = !strcmp(v, "waterpx");
    return 1;
}

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { const uint8_t c[3] = { (uint8_t)(sFB[i] >> 16), (uint8_t)(sFB[i] >> 8), (uint8_t)sFB[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}

static bool layer_empty(void) {
    if (sHfxOpen) return false;
    for (int i = 0; sHfxCol && i < sHfxW * sHfxH; i++) if (sHfxCol[i] != 0 || sHfxZE[i] != 0xFFFF) return false;
    for (int i = 0; sHfxTile && i < sHfxTW * sHfxTH; i++) if (sHfxTile[i]) return false;
    return true;
}

/* ---- check: random scenes ---- */
static uint32_t gA[W * H], gB[W * H], gPre[W * H]; static uint16_t gZA[W * H];
static void rand_water(bool qualifying, float zlo, float zhi, float xlo, float xhi) {
    static const int kTex[] = { TX_WGRAIN, TX_WPLAIN, TX_SMOKE, TX_FIRE, TX_GLOW };
    const int n = 1 + rnd() % 30;
    for (int i = 0; i < n; i++) {
        Mode m = mode_xlu(kTex[rnd() % 5]);
        m.a_tex = 0; m.fog = 1; m.fga = (int)(rnd() % 256); m.fogr = rnd() & 255; m.fogg = rnd() & 255; m.fogb = rnd() & 255;
        const int k = rnd() % 6;
        if (k == 1) { m.texlerp = 1; m.lk[0] = rnd() & 255; m.lk[1] = rnd() & 255; m.lk[2] = rnd() & 255; }
        if (k == 2) { m.rgb_in = 0; }
        if (k == 3) { m.tex = rnd() & 1 ? TX_WGRAIN : TX_WPLAIN; m.detail = TX_WDETAIL; m.es = 1; m.et = 1;
                      m.dw4[0] = m.dw4[1] = m.dw4[2] = m.dw4[3] = (uint16_t)(rnd() % 256); }
        if (!qualifying) { /* each of the non-qualifying reasons */
            switch (rnd() % 6) {
            case 0: m.zwrite = 1; break; case 1: m.decal = 1; break; case 2: m.edge = 1; break; case 3: m.ui = 1; break;
            case 4: m.a_tex = 1; break; default: m.fga = -1; break;
            }
        }
        set_mode(&m); begin();
        const int nb = 1 + rnd() % 6;
        for (int b = 0; b < nb; b++)
            bill(frnd(xlo, xhi), frnd(-10, 250), frnd(2, 140), frnd(0, 6.3f), frnd(zlo, zhi), rnd() & 255, rnd() & 255, rnd() & 255, rnd() & 255);
        flush_tris(rnd() % 3 == 0);
    }
}
/* one random frame (kind 0-3, as check draws them) */
static void rand_frame(int kind) {
    frame_begin(rnd() & 0xFFFFFF);
    draw_world(rnd() & 1);
    if (kind == 0) { rand_water(false, 0.3f, 0.99f, -20, 260); }
    else if (kind == 1) { /* water, then an opaque quad nearer than everything */
        rand_water(true, 0.3f, 0.99f, -20, 260);
        if (rnd() & 1) rand_water(false, 0.3f, 0.99f, -20, 260);
        Mode m = mode_opaque(TX_CRATE); set_mode(&m);
        begin(); quad(frnd(0, 120), frnd(0, 120), frnd(120, 240), frnd(120, 240), 0.1f, 255, 255, 255, 255); flush_tris(false);
    } else if (kind == 2) { /* a near opaque quad, then water all behind it (clear of the tolerance) */
        Mode m = mode_opaque(TX_CRATE); set_mode(&m);
        begin(); quad(60, 30, 180, 210, 0.3f, 255, 255, 255, 255); flush_tris(false);
        rand_water(true, 0.35f, 0.99f, -20, 260);
    } else { rand_water(true, 0.2f, 0.99f, -20, 260); if (rnd() & 1) rand_water(false, 0.2f, 0.99f, -20, 260); }
    hud_sprite();
    frame_end();
}
static uint32_t frame_hash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < W * H; i++) h = (h ^ sFB[i]) * 16777619u;
    for (int i = 0; i < W * H; i++) h = (h ^ sZB[i]) * 16777619u;
    return h;
}
static int check(uint32_t seed, int frames) {
    int fails = 0;
    rs = seed;
    for (int fr = 0; fr < frames; fr++) {
        const int kind = fr % 4;
        const uint32_t fseed = rnd();
        int bad = 0; const char *what = "";
        for (int pass = 0; pass < 3; pass++) {
            set_variant(pass == 2 ? "waterpx" : pass ? "water" : "off");
            rs = fseed;
            rand_frame(kind);
            if (!layer_empty()) { bad = 1; what = "layer not empty after the frame"; }
            if (pass == 2) { /* the NEON composite against the pixel-by-pixel one */
                long dd = 0;
                for (int i = 0; i < W * H; i++) dd += gB[i] != sFB[i];
                if (dd) { bad = 1; what = "the 8-pixel composite differs from the pixel one"; printf("  (%ld px)\n", dd); }
                continue;
            }
            memcpy(pass ? gB : gA, sFB, sizeof(gA));
            if (!pass) memcpy(gZA, sZB, sizeof(gZA));
        }
        long d = 0;
        if (kind == 0) { for (int i = 0; i < W * H; i++) d += gA[i] != gB[i]; if (d) { bad = 1; what = "non-qualifying draws changed"; } }
        if (kind == 1) { for (int i = 0; i < W * H; i++) if (gZA[i] == clamp16_sh8((int32_t)(0.1f * 65535.0f * 256.0f))) d += gA[i] != gB[i]; if (d) { bad = 1; what = "a solid drawn after the water changed"; } }
        if (kind == 2) {
            const uint16_t zq = (uint16_t)clamp16_sh8((int32_t)(0.3f * 65535.0f * 256.0f));
            for (int i = 0; i < W * H; i++) if (gZA[i] == zq) d += gA[i] != gB[i];
            if (d) { bad = 1; what = "the water showed on a solid it is behind"; }
        }
        if (bad) { fails++; printf("check frame %d (kind %d): %s (%ld px)\n", fr, kind, what, d); }
    }
    { /* (4) the flat water */
        set_variant("off"); scene_flat(); memcpy(gA, sFB, sizeof(gA));
        set_variant("water"); scene_flat(); memcpy(gB, sFB, sizeof(gB));
        int mx = 0, mxe = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int dd = 0;
            for (int k = 0; k < 24; k += 8) { const int q = abs((int)((gA[y * W + x] >> k) & 255) - (int)((gB[y * W + x] >> k) & 255)); if (q > dd) dd = q; }
            const bool edge = abs(x - y) < 4 /* the quads' shared diagonal: blended twice at full res (inclusive edges) */ ||
                              (abs(x - 40) < 3 || abs(x - 199) < 3 || abs(y - 40) < 3 || abs(y - 199) < 3 ||
                               abs(x - 60) < 3 || abs(x - 179) < 3 || abs(y - 60) < 3 || abs(y - 179) < 3);
            if (edge) { if (dd > mxe) mxe = dd; } else if (dd > mx) mx = dd;
        }
        printf("flat water: max %d levels from the full-res blend away from its edges, %d at its edges\n", mx, mxe);
        if (mx > 2) { fails++; printf("check: flat water off by %d\n", mx); }
    }
    printf("check: %d frames, %d failures\n", frames, fails);
    return fails != 0;
}
static int hashes(uint32_t seed, int frames) {
    rs = seed;
    for (int fr = 0; fr < frames; fr++) {
        const uint32_t fseed = rnd();
        for (int pass = 0; pass < 2; pass++) {
            set_variant(pass ? "water" : "off");
            rs = fseed;
            rand_frame(fr % 4);
            printf("frame %d %s: %08x\n", fr, pass ? "water" : "off", (unsigned)frame_hash());
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "check";
    gfx_soft_init();
    select_target(true);
    sNatStale = false;
    sDepthTest = true;
    make_textures();
    if (!strcmp(mode, "scene") && argc > 4) {
        int s = -1;
        for (int k = 0; k < NSCENE; k++) if (!strcmp(argv[2], kScene[k])) s = k;
        if (s < 0) { fprintf(stderr, "unknown scene %s\n", argv[2]); return 2; }
        static uint32_t ref[W * H];
        for (int a = 4; a < argc; a++) {
            if (!set_variant(argv[a])) { fprintf(stderr, "unknown variant %s\n", argv[a]); return 2; }
            draw_scene(s);
            char path[512]; snprintf(path, sizeof(path), "%s/%s_%s.ppm", argv[3], kScene[s], argv[a]);
            write_ppm(path);
            const uint32_t h = frame_hash();
            if (a == 4) memcpy(ref, sFB, sizeof(ref));
            long n = 0, n8 = 0; double sum = 0; int mx = 0;
            for (int i = 0; i < W * H; i++) {
                int dd = 0;
                for (int k = 0; k < 24; k += 8) { const int q = abs((int)((ref[i] >> k) & 255) - (int)((sFB[i] >> k) & 255)); if (q > dd) dd = q; }
                n += dd > 0; n8 += dd > 8; sum += dd; if (dd > mx) mx = dd;
            }
            printf("%s %s: hash %08x; vs %s: %ld px differ, %ld by > 8 levels, mean %.2f, max %d; %u composites, %llu px composited\n",
                   kScene[s], argv[a], (unsigned)h, argv[4], n, n8, sum / (W * H), mx, (unsigned)gfx_soft_hfx_flushes,
                   (unsigned long long)gfx_soft_hfx_px);
            gfx_soft_hfx_flushes = 0; gfx_soft_hfx_px = 0;
        }
        return 0;
    }
    if (!strcmp(mode, "bench") && argc > 4) {
        int s = -1;
        for (int k = 0; k < NSCENE; k++) if (!strcmp(argv[2], kScene[k])) s = k;
        if (s < 0 || !set_variant(argv[3])) return 2;
        const int reps = atoi(argv[4]);
        for (int r = 0; r < reps; r++) draw_scene(s);
        return 0;
    }
    if (!strcmp(mode, "check")) return check(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u, argc > 3 ? atoi(argv[3]) : 200);
    if (!strcmp(mode, "hashes")) return hashes(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u, argc > 3 ? atoi(argv[3]) : 200);
    fprintf(stderr, "usage: halfwater_test scene <water|flat> <outdir> <variant>... | check [seed] [frames] | hashes [seed] [frames] | bench <scene> <variant> <reps>\n");
    return 2;
}
