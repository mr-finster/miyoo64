/* lerprows_test.c: the texel-lerp NEON rows (SSB64_LERPROWS, gfx_soft.c cm_row_neon's TINT rows, 2026-10-03)
 * through the real renderer (gfx_soft.c, included whole) -- no ROM.
 *
 *   lerprows_test rand [frames]    random frames of 3D batches around the texel lerp ((A - K) * TEXEL0 + K: K
 *                                  any colour, z-tested or not, opaque / alpha-tested / blended, every alpha source,
 *                                  decals, z writes on and off, flat and smooth, perspective and affine, 32-bit and
 *                                  16-bit textures, repeating on a power of two or not, clamped; and around them the
 *                                  other combines the rows must leave alone: texture x shade, texadd, rgb_texa, the
 *                                  texel alone, untextured, fog, UI mode, nearest). Each frame is drawn from the same
 *                                  start with the rows off and on (and with the NEON rows off: the scalar loops):
 *                                  every colour and depth value must be equal. Prints an FNV hash of the frames
 *                                  (switch on): ARM (NEON) must equal x86 (scalar), and the build against the older
 *                                  gfx_soft.c (-DLR_OLDFILE) must give the same hash.
 *   lerprows_test scene [frames]   a synthetic fight frame (a backdrop rectangle, the stage's texel-lerp blended
 *                                  surfaces, two fighters (texture x shade), a few effects): for qemu instruction
 *                                  counts (a hint only), SSB64_LERPROWS=0/1 from the environment. Prints its hash.
 *   lerprows_test ppm <frame> <out.ppm>  one scene frame as a PPM.
 */
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
int hh_neon_enabled(void) { return 1; }
int gfx_fill_mode_get(void) { return 0; }
int gfx_movie_frame_on(void) { return 0; }
int hh_cfg_switch(const char *env, const char *key, int dflt) { (void)key; const char *e = getenv(env); return e && e[0] ? atoi(e) : dflt; }
#ifdef LR_OLDFILE
static int gfx_soft_lerp_rows; /* (the old renderer has no switch) */
#endif

#define W 240
#define H 240

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static int chance(int pct) { return (int)(rnd() % 100) < pct; }

enum { T_R32, T_C32, T_ODD, T_R16, T_C16, T_ODD16, T_STAGE, T_FACE, T_FIGHTER, T_FX, T_BACK, T_N };
static uint32_t gTex[T_N];
static int gTw[T_N], gTh[T_N];

static uint32_t pk(int a, int r, int g, int b) {
    return (uint32_t)(a & 255) << 24 | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
}
static void upload(int slot, int w, int h, const uint32_t *argbpx, bool clamp, bool as16) {
    static uint8_t buf[64 * 64 * 4];
    if (as16) { /* 5551 as gfx_pc.c hands them over (u16, r:5 g:5 b:5 a:1) */
        uint16_t *b16 = (uint16_t *)buf;
        for (int i = 0; i < w * h; i++) {
            const uint32_t c = argbpx[i];
            b16[i] = (uint16_t)((((c >> 19) & 31) << 11) | (((c >> 11) & 31) << 6) | (((c >> 3) & 31) << 1) | ((c >> 31) & 1));
        }
    } else {
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = (uint8_t)(argbpx[i] >> 16); buf[i * 4 + 1] = (uint8_t)(argbpx[i] >> 8);
            buf[i * 4 + 2] = (uint8_t)argbpx[i]; buf[i * 4 + 3] = (uint8_t)(argbpx[i] >> 24);
        }
    }
    gTex[slot] = gfx_soft_new_texture(); gTw[slot] = w; gTh[slot] = h;
    gfx_soft_select_texture(0, gTex[slot]);
    gfx_soft_upload_texture(buf, w, h, as16 ? GU_PSM_5551 : GU_PSM_8888);
    gfx_soft_set_sampler_parameters(0, true, clamp ? G_TX_CLAMP : 0, clamp ? G_TX_CLAMP : 0);
}
static void make_textures(void) {
    static uint32_t px[64 * 64];
    rs = 4242;
    const struct { int w, h; bool clamp, t16; } k[6] = {
        { 32, 32, false, false }, { 16, 32, true, false }, { 24, 20, false, false },
        { 32, 16, false, true }, { 16, 16, true, true }, { 20, 12, false, true },
    };
    for (int t = 0; t < 6; t++) {
        for (int i = 0; i < k[t].w * k[t].h; i++) { /* noise, with alpha 0 / 255 runs and everything between */
            const int a = chance(20) ? 0 : chance(30) ? 255 : (int)(rnd() & 255);
            const int r = (int)rnd(), g = (int)rnd(), b = (int)rnd(); /* (in order: not as arguments) */
            px[i] = pk(a, r, g, b);
        }
        upload(t, k[t].w, k[t].h, px, k[t].clamp, k[t].t16);
    }
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* stage top: grass, I-like (the tint colours it) */
        const int n = (int)(rnd() % 40), c = ((x / 8) ^ (y / 8)) & 1;
        px[y * 32 + x] = pk(255, 150 + n + 30 * c, 150 + n + 30 * c, 150 + n + 30 * c);
    }
    upload(T_STAGE, 32, 32, px, false, false);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* stage face, with see-through gaps */
        const int m = (y % 8 == 0) || ((x + (y / 8) * 4) % 8 == 0), n = (int)(rnd() % 20);
        px[y * 32 + x] = m ? pk(120, 90, 90, 90) : pk(255, 200 + n, 200 + n, 200 + n);
    }
    upload(T_FACE, 32, 32, px, false, false);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* fighter */
        const int s = (y / 4) & 1, n = (int)(rnd() % 16);
        px[y * 32 + x] = s ? pk(255, 200 + n, 40 + n, 40) : pk(255, 240, 220 - n, 60);
    }
    upload(T_FIGHTER, 32, 32, px, false, true);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* a soft ring */
        const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f, r = dx * dx + dy * dy;
        px[y * 32 + x] = pk(r >= 1.0f ? 0 : (int)(230.0f * (1.0f - r)), 255, 250, 200);
    }
    upload(T_FX, 32, 32, px, true, false);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) px[y * 64 + x] = pk(255, 80 + y, 130 + y / 2, 200 - y);
    upload(T_BACK, 64, 64, px, false, false);
}

#define MAXV (3 * 256)
static SoftVtx gV[MAXV];
static int gNV;
static struct ShaderProgram gSh;
static SoftVtx vx(float x, float y, float z, float iw, float u, float v, int r, int g, int b, int a) {
    SoftVtx s; memset(&s, 0, sizeof(s));
    s.x = x; s.y = y; s.z = z; s.invw = iw; s.u = u; s.v = v; s.r = (uint8_t)r; s.g = (uint8_t)g; s.b = (uint8_t)b; s.a = (uint8_t)a;
    return s;
}
static void draw3d(int tex) {
    gfx_scegu_set_phase(1);
    sBoundTex[0] = tex >= 0 ? gTex[tex] : 0;
    sCurShader = &gSh;
    gfx_soft_draw_triangles((float *)gV, (size_t)gNV * sizeof(SoftVtx) / 4, (size_t)gNV / 3);
    gNV = 0;
}
static void state_reset(void) {
    sUseAlpha = false; sDepthTest = true; sDepthMask = true; sZDecal = false; sEdgeOpaque = false;
    sTexAdd = false; sForceIn = false; sFogPx = false; sFogA = 0; sUiMode = false;
    gfx_scegu_set_tex_lerp(false, 0, 0, 0);
    memset(&gSh, 0, sizeof(gSh));
}

/* one random batch */
static void rand_batch(void) {
    state_reset();
    const int kind = (int)(rnd() % 10);
    int tex = (int)(rnd() % 6);
    gSh.cc.used_textures[0] = true;
    gSh.rgb_tex = true; gSh.rgb_in = true; gSh.a_tex = chance(80); gSh.a_in = chance(70);
    if (kind <= 5) { /* the texel lerp */
        int k[3]; for (int i = 0; i < 3; i++) k[i] = chance(15) ? 0 : chance(15) ? 255 : (int)(rnd() & 255);
        if (chance(5)) k[0] = k[1] = k[2] = 0; /* (gfx_pc never sets K = 0 with the lerp on; the renderer must not care) */
        gfx_scegu_set_tex_lerp(true, (uint8_t)k[0], (uint8_t)k[1], (uint8_t)k[2]);
        if (chance(10)) sTexAdd = true;   /* texlerp comes first */
        if (chance(8)) gSh.rgb_texa = true;  /* stays scalar */
        if (chance(5)) gSh.rgb_in = false;   /* the texel alone: not the lerp */
        if (chance(4)) sForceIn = true;
    } else if (kind == 6) { /* texture x shade (the existing rows) */
        if (chance(30)) sTexAdd = true;
    } else if (kind == 7) { /* untextured or no texel rgb */
        if (chance(50)) { gSh.cc.used_textures[0] = false; gSh.rgb_tex = false; gSh.a_tex = false; tex = -1; }
        else gSh.rgb_tex = false;
    } else if (kind == 8) { /* the texel alone (sky-like) */
        gSh.rgb_in = false; gSh.a_in = chance(50);
    } else { /* the lerp with fog, or in UI mode (both stay scalar or take the UI rules) */
        const uint8_t kr = (uint8_t)rnd(), kg = (uint8_t)rnd(), kb = (uint8_t)rnd();
        gfx_scegu_set_tex_lerp(true, kr, kg, kb);
        if (chance(50)) { sFogPx = true; sFogR = (int)(rnd() & 255); sFogG = (int)(rnd() & 255); sFogB = (int)(rnd() & 255); sFogA = (int)(rnd() & 255); }
        else sUiMode = true;
    }
    sUseAlpha = chance(65);
    sDepthTest = chance(75);
    sDepthMask = chance(60);
    sZDecal = chance(10);
    gSh.cc.opt_texture_edge = chance(25);
    sEdgeOpaque = chance(20);
    if (chance(10)) gfx_soft_bilinear = 0; /* nearest: stays scalar */
    const int ntri = 1 + (int)(rnd() % 6);
    const bool flat = chance(30), affine = chance(15);
    int cr = (int)(rnd() & 255), cg = (int)(rnd() & 255), cb = (int)(rnd() & 255), ca = (int)(rnd() & 255);
    for (int t = 0; t < ntri; t++) {
        const float cx = frnd(-40, 280), cy = frnd(-40, 280);
        const float sz = chance(20) ? frnd(60, 300) : chance(30) ? frnd(1, 6) : frnd(6, 60);
        for (int k = 0; k < 3; k++) {
            const float z = chance(10) ? 0.5f : frnd(-0.1f, 1.05f);
            const float iw = affine ? 1.0f : frnd(0.05f, 2.0f);
            const float u = chance(10) ? 0.25f : frnd(-1.5f, 2.5f), v = frnd(-1.5f, 2.5f);
            int r = cr, g = cg, b = cb, a = ca;
            if (!flat) { r = (int)(rnd() & 255); g = (int)(rnd() & 255); b = (int)(rnd() & 255); a = chance(30) ? 255 : (int)(rnd() & 255); }
            const float ox = frnd(-sz, sz), oy = frnd(-sz, sz);
            gV[gNV++] = vx(cx + ox, cy + oy, z, iw, u, v, r, g, b, a);
        }
    }
    draw3d(tex);
    gfx_soft_bilinear = 1;
}
static void rand_frame(uint32_t seed) {
    rs = seed;
    gfx_soft_start_frame();
    state_reset();
    /* a floor first, so the z and blend tests have something to work against */
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = true;
    gV[gNV++] = vx(0, 0, 0.7f, 1, 0, 0, 200, 180, 160, 255); gV[gNV++] = vx(240, 0, 0.3f, 1, 3, 0, 120, 140, 160, 255);
    gV[gNV++] = vx(240, 240, 0.6f, 1, 3, 3, 90, 200, 60, 255);
    gV[gNV++] = vx(0, 0, 0.7f, 1, 0, 0, 200, 180, 160, 255); gV[gNV++] = vx(240, 240, 0.6f, 1, 3, 3, 90, 200, 60, 255);
    gV[gNV++] = vx(0, 240, 0.9f, 1, 0, 3, 255, 255, 255, 255);
    if (chance(70)) draw3d(T_R32); else gNV = 0;
    const int nb = 20 + (int)(rnd() % 40);
    for (int i = 0; i < nb; i++) rand_batch();
    gfx_soft_end_frame();
}

/* ------------------------------------------------------------------ scene --- */
static void quad(SoftVtx a, SoftVtx b, SoftVtx c, SoftVtx d) {
    gV[gNV++] = a; gV[gNV++] = b; gV[gNV++] = c;
    gV[gNV++] = a; gV[gNV++] = c; gV[gNV++] = d;
}
static float pw(float z) { return 1.0f / (0.5f + 8.0f * z); }
static void rect2d(int tex, int x0, int y0, int x1, int y1, int u1, int v1) {
    Vtx2D q[2]; memset(q, 0, sizeof(q));
    q[0].x = (uint16_t)x0; q[0].y = (uint16_t)y0; q[1].x = (uint16_t)x1; q[1].y = (uint16_t)y1;
    q[1].u = (int16_t)u1; q[1].v = (int16_t)v1; q[1].r = q[1].g = q[1].b = q[1].a = 255;
    gfx_scegu_set_phase(0);
    state_reset(); sDepthTest = false; sDepthMask = false;
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = true; gSh.a_tex = true;
    sCurShader = &gSh; sBoundTex[0] = gTex[tex];
    gfx_scegu_draw_triangles_2d((float *)q, 0, 1);
}
static void draw_scene(int fr) {
    const float t = (float)fr * 0.2f;
    gfx_soft_start_frame();
    rect2d(T_BACK, 0, 0, 240, 240, 64, 64);
    /* the stage: blended, z-tested texel-lerp surfaces (round 16f's largest 3D group on Dream Land) */
    state_reset(); sUseAlpha = true;
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = gSh.a_tex = gSh.a_in = true;
    gfx_scegu_set_tex_lerp(true, 40, 70, 30);
    for (int i = 0; i < 6; i++) { /* the top in strips, as a model's triangles */
        const float x0 = 10 + 37 * i, x1 = x0 + 37, zf = 0.8f, zn = 0.3f;
        quad(vx(x0 + 10, 120, zf, pw(zf), (float)i, 0, 255, 255, 255, 255), vx(x1 + 10, 120, zf, pw(zf), (float)(i + 1), 0, 240, 250, 255, 255),
             vx(x1 - 4, 165, zn, pw(zn), (float)(i + 1), 3, 220, 230, 200, 255), vx(x0 - 4, 165, zn, pw(zn), (float)i, 3, 200, 210, 220, 255));
    }
    draw3d(T_STAGE);
    gfx_scegu_set_tex_lerp(true, 60, 30, 20);
    for (int i = 0; i < 6; i++) {
        const float x0 = 6 + 38 * i, x1 = x0 + 38;
        quad(vx(x0, 165, 0.3f, pw(0.3f), 0, 0, 255, 230, 200, 255), vx(x1, 165, 0.3f, pw(0.3f), 2, 0, 255, 230, 200, 255),
             vx(x1, 235, 0.35f, pw(0.35f), 2, 2, 180, 160, 140, 255), vx(x0, 235, 0.35f, pw(0.35f), 0, 2, 180, 160, 140, 255));
    }
    draw3d(T_FACE);
    /* platforms */
    gfx_scegu_set_tex_lerp(true, 30, 60, 90);
    for (int i = 0; i < 3; i++) {
        const float x = 40 + 60 * i, y = 80 + 10 * (i & 1), z = 0.6f;
        quad(vx(x, y, z, pw(z), 0, 0, 255, 255, 255, 255), vx(x + 45, y, z, pw(z), 2, 0, 255, 255, 255, 255),
             vx(x + 45, y + 8, z, pw(z), 2, 1, 200, 200, 200, 255), vx(x, y + 8, z, pw(z), 0, 1, 200, 200, 200, 255));
    }
    draw3d(T_STAGE);
    /* two fighters: texture x shade, opaque, z written */
    state_reset();
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = true;
    for (int f = 0; f < 2; f++) {
        const float cx = f ? 170 - 8 * sinf(t) : 80 + 6 * sinf(t), cy = 150, z = f ? 0.45f : 0.5f;
        quad(vx(cx - 12, cy - 30, z, pw(z), 0, 0, 255, 255, 255, 255), vx(cx + 12, cy - 30, z, pw(z), 1, 0, 255, 255, 255, 255),
             vx(cx + 12, cy + 10, z, pw(z), 1, 2, 200, 200, 200, 255), vx(cx - 12, cy + 10, z, pw(z), 0, 2, 200, 200, 200, 255));
        quad(vx(cx - 8, cy - 46, z, pw(z), 0, 0, 255, 255, 255, 255), vx(cx + 8, cy - 46, z, pw(z), 1, 0, 255, 255, 255, 255),
             vx(cx + 8, cy - 31, z, pw(z), 1, 1, 255, 255, 255, 255), vx(cx - 8, cy - 31, z, pw(z), 0, 1, 255, 255, 255, 255));
        draw3d(T_FIGHTER);
    }
    /* effects: blended, z-tested, no z write, texel lerp (the particle tint) */
    state_reset(); sUseAlpha = true; sDepthMask = false;
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = gSh.a_tex = gSh.a_in = true;
    gfx_scegu_set_tex_lerp(true, 255, 120, 0);
    for (int i = 0; i < 8; i++) {
        const float a = t * 1.3f + (float)i * 0.8f, cx = 120 + 60 * cosf(a), cy = 110 + 30 * sinf(a), s = 14, z = i & 1 ? 0.4f : 0.6f;
        quad(vx(cx - s, cy - s, z, pw(z), 0, 0, 255, 255, 200, 200), vx(cx + s, cy - s, z, pw(z), 1, 0, 255, 255, 200, 200),
             vx(cx + s, cy + s, z, pw(z), 1, 1, 255, 255, 200, 200), vx(cx - s, cy + s, z, pw(z), 0, 1, 255, 255, 200, 200));
    }
    draw3d(T_FX);
    gfx_soft_end_frame();
}

static void setup(void) {
    gfx_soft_init();
    make_textures();
}
static uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "rand";
    setup();
    if (!strcmp(mode, "rand")) {
        const int frames = argc > 2 ? atoi(argv[2]) : 200;
        static uint32_t fb0[W * H], fb1[W * H]; static uint16_t zb0[W * H], zb1[W * H];
        uint32_t h = 2166136261u; long bad = 0, badz = 0, scal = 0;
        for (int fr = 0; fr < frames; fr++) {
            const uint32_t seed = 0x9E3779B9u * (uint32_t)(fr + 1);
#if defined(HH_CM_NEON)
            sCmNeon = 0; gfx_soft_lerp_rows = 0; rand_frame(seed); /* the scalar loops */
            memcpy(fb1, sNatFB, sizeof fb1); memcpy(zb1, sNatZB, sizeof zb1);
            sCmNeon = 1;
#endif
            gfx_soft_lerp_rows = 0; rand_frame(seed);
            memcpy(fb0, sNatFB, sizeof fb0); memcpy(zb0, sNatZB, sizeof zb0);
            gfx_soft_lerp_rows = 1; rand_frame(seed);
            for (int i = 0; i < W * H; i++) {
                bad += fb0[i] != sNatFB[i]; badz += zb0[i] != sNatZB[i];
#if defined(HH_CM_NEON)
                scal += fb1[i] != sNatFB[i] || zb1[i] != sNatZB[i];
#endif
            }
            h = fnv(h, sNatFB, sizeof fb0); h = fnv(h, sNatZB, sizeof zb0);
            if (getenv("LR_DEBUG")) printf("frame %d %08x\n", fr, (unsigned)fnv(fnv(2166136261u, sNatFB, sizeof fb0), sNatZB, sizeof zb0));
        }
        printf("rand: %d frames, hash %08x, rows on vs off: %ld colour / %ld depth differ, vs the scalar loops: %ld\n",
               frames, (unsigned)h, bad, badz, scal);
        return bad || badz || scal;
    }
    if (!strcmp(mode, "scene")) {
        const int frames = argc > 2 ? atoi(argv[2]) : 4;
        const char *e = getenv("SSB64_LERPROWS");
        gfx_soft_lerp_rows = !(e && e[0] == '0');
        uint32_t h = 2166136261u;
        for (int fr = 0; fr < frames; fr++) { draw_scene(fr); h = fnv(h, sNatFB, (size_t)W * H * 4); }
        printf("scene: %d frames, rows %d, hash %08x\n", frames, gfx_soft_lerp_rows, (unsigned)h);
        return 0;
    }
    if (!strcmp(mode, "ppm")) {
        draw_scene(argc > 2 ? atoi(argv[2]) : 0);
        FILE *f = fopen(argc > 3 ? argv[3] : "scene.ppm", "wb");
        if (!f) return 1;
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { const uint32_t c = sNatFB[i]; fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f); }
        fclose(f);
        return 0;
    }
    fprintf(stderr, "modes: rand scene ppm\n");
    return 2;
}
