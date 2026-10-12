/* fx_scene.c -- synthetic effect scenes through the real renderer (no ROM, no capture): the Bench's "fx_*" views
 * (bench.c, the half-resolution effects test of 2026-10-02) and tools/halffx_test.c (the layer's proof and sheets).
 * Public renderer API only (gfx_soft_api, gfx_soft_pm_generic, gfx_scegu_set_phase, gfx_soft_clear_depth), so the
 * same frames draw in the Bench on the device and in the host harness, and against an older gfx_soft.c.
 *
 * The 320x240 frame Paper Mario renders (the square mode squeezes it). Kinds:
 *   FX_SMOKE  the worst case for the layer: a far backdrop, a back wall, a perspective floor, a slanted side wall, 36
 *             smoke puffs (texture x shade, blended, no depth write) at several depths, some crossing the side wall
 *             and the floor; a pillar drawn after them; fire-breath particles (the generic combiner's fast path 8,
 *             FC322BFF 5F0E923F, with PM64_FIRECHEAP / PM64_FIRENEAREST as set) in front of and behind it; 40 sparkles
 *             of 1.5-4 px; a solid box after them; a near puff; a blended decal on the back wall (full res); a 2D-phase
 *             HUD bar (blended, no depth: not an effect).
 *   FX_FIRE   the prologue's fire breath, as the fire round measured it: ~4 screens (318,000 px at full res) of fast
 *             path 8 particles over a battle stage (floor, back wall, two solid actors), Bowser's side to Mario's.
 *   FX_DUST   the castle lift's dust: ~110,000 px of fast path 4 puffs (FC20A204 FF13FFFF, (T1 x T0) x SHADE) over
 *             a sky backdrop and the castle's wall. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "gfx_rendering_api.h"

enum { FX_SMOKE, FX_FIRE, FX_DUST, FX_FIELD, FX_GEN, FX_KINDS };
extern struct GfxRenderingAPI gfx_soft_api;
extern void gfx_soft_pm_generic(bool on, uint32_t w0, uint32_t w1, bool two_cycle, const uint8_t prim[4], const uint8_t env[4],
                                uint8_t prim_lod, int ds, int dt, const int32_t t1map[6]);
extern void gfx_scegu_set_phase(int is3d);
extern void gfx_soft_clear_depth(void);
extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

#define FXW 320
#define FXH 240
#define FX_SX(x) ((float)(x) * (4.0f / 3.0f)) /* the GoldenEye scene's 240-wide x to 320 */

typedef struct { float u, v; uint8_t r, g, b, a; float x, y, z; float invw; } FxVtx; /* gfx_soft.c's SoftVtx */

static uint32_t fxs_rs = 1;
static uint32_t fxs_rnd(void) { fxs_rs ^= fxs_rs << 13; fxs_rs ^= fxs_rs >> 17; fxs_rs ^= fxs_rs << 5; return fxs_rs; }
static float fxs_frnd(float a, float b) { return a + (b - a) * (float)(fxs_rnd() & 0xFFFFFF) / 16777216.0f; }
/* sin/cos and sqrt without libm differences between the builds (glibc's and musl's sinf differ in the last bit) */
static float fxs_sin(float x) {
    const float tp = 6.2831853f;
    x -= tp * (float)(int)(x / tp);
    if (x < 0) x += tp;
    if (x > 3.14159265f) x -= tp;
    const float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}
static float fxs_cos(float x) { return fxs_sin(x + 1.5707963f); }
static float fxs_sqrt(float a) { /* Newton from a fixed start: the same float steps on both builds */
    if (a <= 0.0f) return 0.0f;
    float r = a > 1.0f ? a : 1.0f;
    for (int i = 0; i < 24; i++) r = 0.5f * (r + a / r);
    return r;
}

enum { T_BACK, T_WALL, T_FLOOR, T_PILLAR, T_SMOKE, T_SPARK, T_DECAL, T_SOLID, T_FIRE0, T_FIRE1, T_DUST0, T_DUST1, T_FLAT, T_SPRITE, T_NPOT, T_TILE, T_N };
static uint32_t fxs_tex[T_N];
static struct ShaderProgram *fxs_opaque, *fxs_blend, *fxs_edge, *fxs_shade;
static int fxs_ready;

static int fxs_pk8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void fxs_upload(int slot, int w, int h, const uint8_t *rgba) {
    fxs_tex[slot] = gfx_soft_api.new_texture();
    gfx_soft_api.select_texture(0, fxs_tex[slot]);
    gfx_soft_api.upload_texture(rgba, w, h, 3 /* GU_PSM_8888 */);
    gfx_soft_api.set_sampler_parameters(0, true, 0, 0); /* repeat */
}
static void fxs_px(uint8_t *p, int r, int g, int b, int a) { p[0] = (uint8_t)fxs_pk8(r); p[1] = (uint8_t)fxs_pk8(g); p[2] = (uint8_t)fxs_pk8(b); p[3] = (uint8_t)fxs_pk8(a); }

void fx_scene_init(void) {
    static uint8_t px[64 * 64 * 4];
    if (fxs_ready) return;
    fxs_ready = 1;
    /* TEXEL0 x SHADE (rgb and alpha), as gfx_cc's shader id: c = A, B, C, D in 3 bits each (TEXEL0 5, input 1 1) */
    const uint32_t tm = 5u | 1u << 6, id = tm | tm << 12;
    fxs_opaque = gfx_soft_api.create_and_load_new_shader(id);
    fxs_blend = gfx_soft_api.create_and_load_new_shader(id | (1u << 24) /* SHADER_OPT_ALPHA */);
    fxs_edge = gfx_soft_api.create_and_load_new_shader(id | (1u << 24) | (1u << 26) /* ALPHA | TEXTURE_EDGE: sprites */);
    fxs_shade = gfx_soft_api.create_and_load_new_shader(1u | 1u << 12 | (1u << 24) /* SHADE alone, blended: shadows */);
    fxs_rs = 12345;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) /* backdrop: sky to hills */
        fxs_px(&px[(y * 64 + x) * 4], 90 + y, 120 + y / 2, 170 - y + (int)(fxs_rnd() % 8), 255);
    fxs_upload(T_BACK, 64, 64, px);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* blocks with mortar lines */
        const int m = (y % 16 == 0) || ((x + (y / 16) * 8) % 16 == 0), n = (int)(fxs_rnd() % 24);
        if (m) fxs_px(&px[(y * 32 + x) * 4], 60, 60, 64, 255); else fxs_px(&px[(y * 32 + x) * 4], 150 + n, 146 + n, 136 + n, 255);
    }
    fxs_upload(T_WALL, 32, 32, px);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { /* floor: checker with grain */
        const int c = ((x / 8) ^ (y / 8)) & 1, n = (int)(fxs_rnd() % 30);
        if (c) fxs_px(&px[(y * 64 + x) * 4], 70 + n, 90 + n, 60 + n, 255); else fxs_px(&px[(y * 64 + x) * 4], 120 + n, 110 + n, 80 + n, 255);
    }
    fxs_upload(T_FLOOR, 64, 64, px);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* pillar: stripes */
        const int s = (x / 4) & 1, n = (int)(fxs_rnd() % 12);
        if (s) fxs_px(&px[(y * 32 + x) * 4], 40 + n, 70 + n, 110 + n, 255); else fxs_px(&px[(y * 32 + x) * 4], 200, 180 - n, 40, 255);
    }
    fxs_upload(T_PILLAR, 32, 32, px);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* smoke puff: a soft disc with grain */
        const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f, r = fxs_sqrt(dx * dx + dy * dy);
        const int a = r >= 1.0f ? 0 : (int)(220.0f * (1.0f - r * r) + (float)(fxs_rnd() % 30) - 15.0f), g = 120 + (int)(fxs_rnd() % 50);
        fxs_px(&px[(y * 32 + x) * 4], g, g, g + 8, a);
    }
    fxs_upload(T_SMOKE, 32, 32, px);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { /* sparkle */
        const float dx = (x - 3.5f) / 4.0f, dy = (y - 3.5f) / 4.0f, r = fxs_sqrt(dx * dx + dy * dy);
        fxs_px(&px[(y * 8 + x) * 4], 255, 250, 200, r >= 1.0f ? 0 : (int)(255 * (1.0f - r)));
    }
    fxs_upload(T_SPARK, 8, 8, px);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { /* a scorch decal */
        const float dx = (x - 7.5f) / 8.0f, dy = (y - 7.5f) / 8.0f, r = fxs_sqrt(dx * dx + dy * dy);
        fxs_px(&px[(y * 16 + x) * 4], 20, 15, 10, r >= 1.0f ? 0 : (int)(200 * (1.0f - r)));
    }
    fxs_upload(T_DECAL, 16, 16, px);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* a solid actor */
        const int n = (int)(fxs_rnd() % 20);
        fxs_px(&px[(y * 32 + x) * 4], 50 + n, 90 + n, 40 + n, 255);
    }
    fxs_upload(T_SOLID, 32, 32, px);
    for (int k = 0; k < 2; k++) { /* the fire's two alpha layers (I/IA textures: only their alpha is used) */
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
            const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f;
            const float r = fxs_sqrt(dx * dx + dy * dy) + fxs_frnd(-0.15f, 0.15f) * (k ? 1.6f : 1.0f);
            const int a = r >= 1.0f ? 0 : (int)(255.0f * (1.0f - r) * (k ? 1.3f : 1.8f));
            fxs_px(&px[(y * 32 + x) * 4], 255, 255, 255, a);
        }
        fxs_upload(T_FIRE0 + k, 32, 32, px);
    }
    for (int k = 0; k < 2; k++) { /* the dust's two layers: a cloud and a noise mask */
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
            const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f, r = fxs_sqrt(dx * dx + dy * dy);
            const int n = (int)(fxs_rnd() % 60);
            if (k) fxs_px(&px[(y * 32 + x) * 4], 200 + n / 2, 190 + n / 2, 170 + n / 2, 160 + n);
            else fxs_px(&px[(y * 32 + x) * 4], 240, 230, 210, r >= 1.0f ? 0 : (int)(255.0f * (1.0f - r * r)));
        }
        fxs_upload(T_DUST0 + k, 32, 32, px);
    }
    for (int i = 0; i < 4 * 4 * 4; i += 4) fxs_px(&px[i], 255, 255, 255, 255); /* flat white (the harness's flat check) */
    fxs_upload(T_FLAT, 4, 4, px);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 32; x++) { /* a sprite: an opaque body, transparent around it */
        const float dx = (x - 15.5f) / 14.0f, dy = (y - 31.5f) / 30.0f; const int in = dx * dx + dy * dy < 1.0f, n = (int)(fxs_rnd() % 40);
        fxs_px(&px[(y * 32 + x) * 4], 180 + n, 60 + n, 50 + (y & 8) * 8, in ? 255 : 0);
    }
    fxs_upload(T_SPRITE, 32, 64, px);
    gfx_soft_api.set_sampler_parameters(0, true, 2, 2); /* G_TX_CLAMP both ways */
    for (int y = 0; y < 24; y++) for (int x = 0; x < 48; x++) { /* not a power of two wide: wrapf's modulo */
        const int n = (int)(fxs_rnd() % 50);
        fxs_px(&px[(y * 48 + x) * 4], 100 + n + x, 160 - n, 90 + y * 3, 255);
    }
    fxs_upload(T_NPOT, 48, 24, px);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { /* a clamped tile (a sign, a panel) */
        const int n = (int)(fxs_rnd() % 70);
        fxs_px(&px[(y * 32 + x) * 4], 200 - n, 180 - (x ^ y) * 2, 120 + n, 255);
    }
    fxs_upload(T_TILE, 32, 32, px);
    gfx_soft_api.set_sampler_parameters(0, true, 2, 0); /* clamp in S, repeat in T */
}

/* one batch of quads: two triangles each */
#define FXS_MAXV (3 * 1024)
static FxVtx fxs_v[FXS_MAXV];
static int fxs_nv;
static FxVtx fxs_vx(float x, float y, float z, float iw, float u, float v, int r, int g, int b, int a) {
    FxVtx s; memset(&s, 0, sizeof s);
    s.x = x; s.y = y; s.z = z; s.invw = iw; s.u = u; s.v = v;
    s.r = (uint8_t)fxs_pk8(r); s.g = (uint8_t)fxs_pk8(g); s.b = (uint8_t)fxs_pk8(b); s.a = (uint8_t)fxs_pk8(a);
    return s;
}
static void fxs_quad(FxVtx a, FxVtx b, FxVtx c, FxVtx d) {
    if (fxs_nv + 6 > FXS_MAXV) return;
    fxs_v[fxs_nv++] = a; fxs_v[fxs_nv++] = b; fxs_v[fxs_nv++] = c;
    fxs_v[fxs_nv++] = a; fxs_v[fxs_nv++] = c; fxs_v[fxs_nv++] = d;
}
static float fxs_iw(float z) { return 1.0f / (0.5f + 8.0f * z); }
enum { M_OPAQUE_Z, M_OPAQUE_NOZ, M_FX, M_DECAL, M_HUD, M_FIRE, M_DUST, M_EDGE, M_SHADOW, M_GEN, M_GENZ };
/* M_GEN / M_GENZ (blended, no depth write / opaque, depth written): the generic combiner with these */
static uint32_t fxs_gw0, fxs_gw1; static bool fxs_gtwo; static uint8_t fxs_gprim[4], fxs_genv[4]; static int fxs_gt0, fxs_gt1;
static int fxs_skip_fx; /* the harness: the scene without its effects */
/* the render state of a batch (as gfx_pc.c sets it from the render mode), then the draw */
static void fxs_flush(int mode, int tex) {
    if (!fxs_nv) return;
    const bool fx = mode == M_FX || mode == M_FIRE || mode == M_DUST || mode == M_GEN;
    if (fxs_skip_fx && fx) { fxs_nv = 0; return; }
    gfx_soft_api.set_use_alpha(fx || mode == M_DECAL || mode == M_HUD || mode == M_EDGE || mode == M_SHADOW);
    gfx_soft_api.set_depth_test(mode != M_OPAQUE_NOZ && mode != M_HUD);
    gfx_soft_api.set_depth_mask(mode == M_OPAQUE_Z || mode == M_EDGE || mode == M_GENZ);
    gfx_soft_api.set_zmode_decal(mode == M_DECAL || mode == M_SHADOW);
    gfx_soft_api.load_shader(mode == M_OPAQUE_Z || mode == M_OPAQUE_NOZ ? fxs_opaque : mode == M_EDGE ? fxs_edge : mode == M_SHADOW ? fxs_shade : fxs_blend);
    if (mode == M_GEN || mode == M_GENZ) {
        const int32_t t1map[6] = { 0, 3 << 16, 0, 0, 7 << 16, 0 };
        gfx_soft_pm_generic(true, fxs_gw0, fxs_gw1, fxs_gtwo, fxs_gprim, fxs_genv, 0, 0, 0, t1map);
#ifdef FXS_FASTLOG
        if (getenv("FXS_FASTLOG")) printf("gen %08x %08x: fast %d shroud %d\n", (unsigned)fxs_gw0, (unsigned)fxs_gw1, sGen.fast, sGen.shroud);
#endif
        gfx_soft_api.select_texture(0, fxs_tex[fxs_gt0]);
        gfx_soft_api.select_texture(1, fxs_tex[fxs_gt1]);
    } else if (mode == M_FIRE || mode == M_DUST) {
        /* fire: colour (PRIM - ENV) x SHADE + ENV, alpha ((T1a - T0a) x ENVa + T0a) x PRIMa; dust: (T1 x T0) x SHADE */
        static const uint8_t fprim[4] = { 255, 236, 120, 255 }, fenv[4] = { 230, 40, 0, 150 }, none[4] = { 0, 0, 0, 0 };
        const int32_t t1map[6] = { 0, 9 << 16, 0, 0, 5 << 16, 0 }; /* the second layer 9 x 5 texels on */
        const bool fire = mode == M_FIRE;
        gfx_soft_pm_generic(true, fire ? 0xFC322BFFu : 0xFC20A204u, fire ? 0x5F0E923Fu : 0xFF13FFFFu, true, fire ? fprim : none,
                            fire ? fenv : none, 0, 0, 0, t1map);
        gfx_soft_api.select_texture(0, fxs_tex[fire ? T_FIRE0 : T_DUST0]);
        gfx_soft_api.select_texture(1, fxs_tex[fire ? T_FIRE1 : T_DUST1]);
    } else {
        gfx_soft_pm_generic(false, 0, 0, false, NULL, NULL, 0, 0, 0, NULL);
        gfx_soft_api.select_texture(0, fxs_tex[tex]);
    }
    gfx_soft_api.draw_triangles((float *)fxs_v, (size_t)fxs_nv * sizeof(FxVtx) / 4, (size_t)fxs_nv / 3);
    fxs_nv = 0;
    if (mode == M_FIRE || mode == M_DUST || mode == M_GEN || mode == M_GENZ) gfx_soft_pm_generic(false, 0, 0, false, NULL, NULL, 0, 0, 0, NULL);
}
/* a camera-facing billboard: centre, half size, depth (z 0..1, larger = farther), a rotation, a colour */
static void fxs_billboard(float cx, float cy, float s, float z, float rot, int r, int g, int b, int a) {
    const float iw = fxs_iw(z), c = fxs_cos(rot) * s, sn = fxs_sin(rot) * s;
    fxs_quad(fxs_vx(cx - c + sn, cy - sn - c, z, iw, 0, 0, r, g, b, a), fxs_vx(cx + c + sn, cy + sn - c, z, iw, 1, 0, r, g, b, a),
             fxs_vx(cx + c - sn, cy + sn + c, z, iw, 1, 1, r, g, b, a), fxs_vx(cx - c - sn, cy - sn + c, z, iw, 0, 1, r, g, b, a));
}
/* a sprite tilted in depth (crossing a wall or the floor): z from za (left) to zb (right) */
static void fxs_tilted(float cx, float cy, float s, float za, float zb, int r, int g, int b, int a) {
    const float ia = fxs_iw(za), ib = fxs_iw(zb);
    fxs_quad(fxs_vx(cx - s, cy - s, za, ia, 0, 0, r, g, b, a), fxs_vx(cx + s, cy - s, zb, ib, 1, 0, r, g, b, a),
             fxs_vx(cx + s, cy + s, zb, ib, 1, 1, r, g, b, a), fxs_vx(cx - s, cy + s, za, ia, 0, 1, r, g, b, a));
}
static void fxs_wall(float x0, float y0, float x1, float y1, float z0, float z1, float tu, float tv) {
    const float i0 = fxs_iw(z0), i1 = fxs_iw(z1);
    fxs_quad(fxs_vx(x0, y0, z0, i0, 0, 0, 255, 255, 255, 255), fxs_vx(x1, y0, z1, i1, tu, 0, 255, 255, 255, 255),
             fxs_vx(x1, y1, z1, i1, tu, tv, 255, 255, 255, 255), fxs_vx(x0, y1, z0, i0, 0, tv, 200, 200, 200, 255));
}
static void fxs_backdrop(void) { /* opaque, no depth: the frame's clear */
    fxs_quad(fxs_vx(0, 0, 1, 1, 0, 0, 255, 255, 255, 255), fxs_vx(FXW, 0, 1, 1, 2, 0, 255, 255, 255, 255),
             fxs_vx(FXW, FXH, 1, 1, 2, 2, 255, 255, 255, 255), fxs_vx(0, FXH, 1, 1, 0, 2, 255, 255, 255, 255));
    fxs_flush(M_OPAQUE_NOZ, T_BACK);
}
static void fxs_floor(float zfar) {
    const float f = fxs_iw(zfar), n = fxs_iw(0.1f);
    fxs_quad(fxs_vx(-30, 120, zfar, f, 0, 0, 255, 255, 255, 255), fxs_vx(FXW + 30, 120, zfar, f, 8, 0, 255, 255, 255, 255),
             fxs_vx(800, 300, 0.1f, n, 8, 8, 255, 255, 255, 255), fxs_vx(-480, 300, 0.1f, n, 0, 8, 160, 160, 160, 255));
    fxs_flush(M_OPAQUE_Z, T_FLOOR);
}
/* the fire breath's particles: n quads from the mouth (x0, y0) to (x1, y1), growing, at depths z0 .. z1 */
static void fxs_fire(int fr, int n, float x0, float y0, float x1, float y1, float s0, float s1, float z0, float z1) {
    const float t = (float)fr * 0.15f;
    for (int i = 0; i < n; i++) {
        const float k = (float)i / (float)(n > 1 ? n - 1 : 1), w = fxs_sin(t * 2.1f + (float)i * 1.7f);
        const float cx = x0 + (x1 - x0) * k + 10.0f * w, cy = y0 + (y1 - y0) * k + 14.0f * fxs_cos(t * 1.3f + (float)i);
        const int sh = 255 - (int)(120.0f * k);
        fxs_billboard(cx, cy, s0 + (s1 - s0) * k, z0 + (z1 - z0) * k, t + (float)i * 0.9f, 255, sh, sh, 255);
        if ((i & 63) == 63) fxs_flush(M_FIRE, 0); /* the game's batches */
    }
    fxs_flush(M_FIRE, 0);
}


/* FX_FIELD (2026-10-03, the final exact speed check): the field and castle scenes' plain 3D, texture x shade through
 * cm_row_neon mode 0 -- a backdrop without depth, a perspective floor, walls repeating at a power of two, one not a
 * power of two wide (wrapf's modulo), clamped panels, a tessellated wall of small triangles (the per-triangle setup),
 * Gouraud-shaded and flat triangles, texture-edge sprites (clamped, alpha-tested, depth-written), blended shadows
 * (shade alone, ZMODE_DEC) and a see-through water quad. */
static void fxs_field(int fr) {
    const float t = (float)fr * 0.15f;
    fxs_wall(0, 0, FXW, 110, 0.97f, 0.97f, 3, 1); fxs_flush(M_OPAQUE_NOZ, T_BACK);
    fxs_floor(0.95f);
    fxs_wall(20, 20, 300, 125, 0.93f, 0.93f, 6, 2); fxs_flush(M_OPAQUE_Z, T_WALL);
    fxs_wall(-40, 0, 90, 230, 0.20f, 0.90f, 4, 5); fxs_flush(M_OPAQUE_Z, T_NPOT);         /* slanted, NPOT, repeat */
    fxs_wall(250, 40, 330, 220, 0.85f, 0.30f, 1, 1); fxs_flush(M_OPAQUE_Z, T_TILE);       /* clamped S */
    for (int gy = 0; gy < 12; gy++) for (int gx = 0; gx < 16; gx++) {                     /* a tessellated house front */
        const float x0 = 110 + gx * 7.0f, y0 = 40 + gy * 6.5f + 2 * fxs_sin(t + gx * 0.3f), z = 0.6f + 0.002f * gx, iw = fxs_iw(z);
        const int s0 = 150 + ((gx * 13 + gy * 7) & 63), s1 = 150 + ((gx * 5 + gy * 11) & 63);
        fxs_quad(fxs_vx(x0, y0, z, iw, gx / 16.0f, gy / 12.0f, s0, s0, s1, 255), fxs_vx(x0 + 7, y0, z, iw, (gx + 1) / 16.0f, gy / 12.0f, s1, s0, s0, 255),
                 fxs_vx(x0 + 7, y0 + 6.5f, z, iw, (gx + 1) / 16.0f, (gy + 1) / 12.0f, s0, s1, s0, 255), fxs_vx(x0, y0 + 6.5f, z, iw, gx / 16.0f, (gy + 1) / 12.0f, s1, s1, s1, 255));
    }
    fxs_flush(M_OPAQUE_Z, T_WALL);
    for (int i = 0; i < 6; i++) {                                                         /* shadows under the actors */
        const float cx = 60 + i * 42 + 6 * fxs_sin(t + i), cy = 200 - (i & 1) * 20, z = 0.25f + 0.03f * i, iw = fxs_iw(z);
        fxs_quad(fxs_vx(cx - 18, cy - 5, z, iw, 0, 0, 0, 0, 0, 110), fxs_vx(cx + 18, cy - 5, z, iw, 0, 0, 0, 0, 0, 110),
                 fxs_vx(cx + 18, cy + 5, z, iw, 0, 0, 0, 0, 0, 110), fxs_vx(cx - 18, cy + 5, z, iw, 0, 0, 0, 0, 0, 110));
    }
    fxs_flush(M_SHADOW, T_FLAT);
    for (int i = 0; i < 6; i++) {                                                         /* the actors: sprites */
        const float cx = 60 + i * 42 + 6 * fxs_sin(t + i), cy = 200 - (i & 1) * 20, z = 0.24f + 0.03f * i, iw = fxs_iw(z), h = 50 + 6 * (i % 3);
        const float u0 = (fr + i) & 1 ? 1.0f : 0.0f;                                     /* flipped every other frame */
        fxs_quad(fxs_vx(cx - h * 0.35f, cy - h, z, iw, u0, 0, 255, 255, 255, 255), fxs_vx(cx + h * 0.35f, cy - h, z, iw, 1 - u0, 0, 255, 255, 255, 255),
                 fxs_vx(cx + h * 0.35f, cy, z, iw, 1 - u0, 1, 255, 255, 255, 255), fxs_vx(cx - h * 0.35f, cy, z, iw, u0, 1, 255, 255, 255, 255));
    }
    fxs_flush(M_EDGE, T_SPRITE);
    { const float z = 0.5f, iw = fxs_iw(z);                                               /* water: see-through */
      fxs_quad(fxs_vx(100, 170, z, iw, t * 0.1f, 0, 120, 160, 255, 150), fxs_vx(240, 170, z, iw, 2 + t * 0.1f, 0, 120, 160, 255, 150),
               fxs_vx(260, 235, z, iw, 2 + t * 0.1f, 1, 120, 160, 255, 150), fxs_vx(80, 235, z, iw, t * 0.1f, 1, 120, 160, 255, 150));
      fxs_flush(M_FX, T_FLOOR); }
}


/* FX_GEN: the generic combiner's fast paths through the NEON rows, each over textures of every wrap: 1 (T0 x SHADE,
 * then (C - K1) x K2 + K3, the Star Spirits' FC121803 FFFFFF78 and FC127E03 FFFFF378), 7 (the fight's glow, 1-cycle
 * FC60FEC1 44FE7339), 10 (Bowser's room, FC117E80 F5FFF438), 4 (the dust) over a texture that does not repeat at a
 * power of two; then again under the Star Power shroud. */
extern void gfx_soft_pm_shroud(int f5, uint8_t r, uint8_t g, uint8_t b);
static void fxs_genset(uint32_t w0, uint32_t w1, bool two, int t0, int t1) {
    static const uint8_t pr[4] = { 250, 220, 120, 200 }, en[4] = { 40, 90, 160, 120 };
    fxs_gw0 = w0; fxs_gw1 = w1; fxs_gtwo = two; fxs_gt0 = t0; fxs_gt1 = t1;
    memcpy(fxs_gprim, pr, 4); memcpy(fxs_genv, en, 4);
}
static void fxs_genscene(int fr) {
    const float t = (float)fr * 0.15f;
    static const int texs[4] = { T_SMOKE, T_NPOT, T_SPRITE, T_TILE };
    fxs_floor(0.95f);
    fxs_wall(20, 20, 300, 125, 0.93f, 0.93f, 6, 2); fxs_flush(M_OPAQUE_Z, T_WALL);
    for (int sh = 0; sh < 2; sh++) {
        gfx_soft_pm_shroud(sh ? 12 : 0, 20, 10, 60);
        for (int k = 0; k < 4; k++) {
            const float x = 20 + k * 75, y = 20 + sh * 110;
            fxs_genset(0xFC121803u, 0xFFFFFF78u, true, texs[k], texs[k]);       /* fast path 1, blended */
            fxs_billboard(x + 20, y + 20, 22, 0.5f, t + k, 255, 200, 160, 230); fxs_flush(M_GEN, 0);
            fxs_genset(0xFC127E03u, 0xFFFFF378u, true, texs[k], texs[k]);       /* fast path 1, alpha T0a */
            fxs_billboard(x + 45, y + 30, 20, 0.45f, -t + k, 180, 255, 200, 255); fxs_flush(M_GEN, 0);
            fxs_genset(0xFC60FEC1u, 0x44FE7339u, false, texs[k], texs[k]);      /* fast path 7, opaque, depth written */
            fxs_tilted(x + 30, y + 70, 18, 0.3f, 0.6f, 90, 200, 255, 255); fxs_flush(M_GENZ, 0);
            fxs_genset(0xFC117E80u, 0xF5FFF438u, true, texs[k], T_WALL);        /* fast path 10 */
            fxs_billboard(x + 15, y + 75, 16, 0.4f, t * 0.5f, 255, 255, 255, 255); fxs_flush(M_GEN, 0);
            fxs_genset(0xFC20A204u, 0xFF13FFFFu, true, texs[k], T_NPOT);        /* fast path 4, TEXEL1 not a power of two */
            fxs_billboard(x + 50, y + 80, 14, 0.35f, t * 0.7f, 230, 220, 200, 200); fxs_flush(M_GEN, 0);
            fxs_genset(0xFC20A204u, 0xFF13FFFFu, true, texs[k], T_DUST1);       /* fast path 4 */
            fxs_billboard(x + 35, y + 45, 15, 0.38f, t * 0.9f, 230, 220, 200, 200); fxs_flush(M_GENZ, 0);
        }
    }
    gfx_soft_pm_shroud(0, 0, 0, 0);
}

/* one frame of a kind */
void fx_scene_draw(int kind, int fr) {
    const float t = (float)fr * 0.15f;
    fx_scene_init();
    gfx_soft_api.start_frame();
    gfx_soft_clear_depth();
    gfx_scegu_set_phase(1);
    fxs_backdrop();
    if (kind == FX_FIELD) {
        fxs_field(fr);
    } else if (kind == FX_GEN) {
        fxs_genscene(fr);
    } else if (kind == FX_FIRE) {
        fxs_wall(0, 20, FXW, 140, 0.92f, 0.92f, 8, 3); fxs_flush(M_OPAQUE_Z, T_WALL);
        fxs_floor(0.92f);
        fxs_wall(30, 70, 90, 200, 0.45f, 0.45f, 2, 4); fxs_flush(M_OPAQUE_Z, T_SOLID);   /* Mario's side */
        fxs_wall(220, 30, 310, 210, 0.42f, 0.42f, 3, 6); fxs_flush(M_OPAQUE_Z, T_PILLAR); /* Bowser */
        /* ~4 screens at full res (the fire round's census: 318,000 px a frame): 120 particles, 24 -> 64 px across */
        fxs_fire(fr, 120, 230, 90, 50, 150, 14, 36, 0.40f, 0.47f);
    } else if (kind == FX_DUST) {
        fxs_wall(FX_SX(20), 60, FX_SX(220), 240, 0.6f, 0.6f, 6, 6); fxs_flush(M_OPAQUE_Z, T_WALL); /* the castle */
        for (int i = 0; i < 30; i++) { /* ~110,000 px at full res */
            const float a = t + (float)i * 0.8f;
            fxs_billboard(160 + 120 * fxs_sin(a * 0.4f + (float)i), 170 + 50 * fxs_cos(a * 0.7f + (float)i * 0.3f), 26 + 4 * (float)(i % 4),
                          i & 1 ? 0.5f : 0.7f, a * 0.2f, 230, 220, 200, 200);
        }
        fxs_flush(M_DUST, 0);
    } else {
        fxs_wall(FX_SX(40), 30, FXW, 150, 0.90f, 0.90f, 8, 4); fxs_flush(M_OPAQUE_Z, T_WALL); /* back wall */
        fxs_floor(0.9f);
        fxs_wall(0, 0, FX_SX(70), 200, 0.25f, 0.88f, 5, 6); fxs_flush(M_OPAQUE_Z, T_WALL);    /* side wall, slanted */
        for (int i = 0; i < 36; i++) { /* a smoke column: behind where the pillar will be, crossing the side wall */
            const float a = t + (float)i * 0.7f;
            fxs_billboard(FX_SX(110 + 60 * fxs_sin(a * 0.5f + (float)i)), 50 + 3.5f * (float)i + 6 * fxs_cos(a), 26 + 6 * (float)(i % 5),
                          0.5f + 0.02f * (float)(i % 6), a * 0.3f, 210, 205, 200, 140 + (i % 4) * 25);
        }
        fxs_tilted(FX_SX(60), 90, 40, 0.3f, 0.7f, 190, 190, 195, 230);  /* crossing the side wall */
        fxs_tilted(FX_SX(75), 160, 34, 0.5f, 0.5f, 200, 196, 190, 220); /* standing in the floor */
        fxs_flush(M_FX, T_SMOKE);
        fxs_wall(FX_SX(118), 10, FX_SX(146), 230, 0.40f, 0.40f, 1, 6); fxs_flush(M_OPAQUE_Z, T_PILLAR); /* the pillar, after */
        fxs_fire(fr, 16, FX_SX(170), 90, FX_SX(95), 135, 14, 22, 0.30f, 0.50f); /* in front of the pillar, then behind it */
        for (int i = 0; i < 40; i++) { /* sparkles: 1.5-4 px */
            fxs_rs = 777u + (uint32_t)i * 31u + (uint32_t)fr * 7u;
            const float s = fxs_frnd(0.75f, 2.0f), x = fxs_frnd(FX_SX(80), FX_SX(200)), y = fxs_frnd(60, 170), z = fxs_frnd(0.2f, 0.7f);
            fxs_billboard(x, y, s, z, 0, 255, 255, 255, 255);
        }
        fxs_flush(M_FX, T_SPARK);
        const float gx = 10 * fxs_sin(t);
        fxs_wall(FX_SX(150) + gx, 95, FX_SX(182) + gx, 200, 0.45f, 0.45f, 1, 3); fxs_flush(M_OPAQUE_Z, T_SOLID); /* an actor, after */
        fxs_billboard(FX_SX(185), 150, 44, 0.2f, t * 0.2f, 180, 178, 175, 170); fxs_flush(M_FX, T_SMOKE);    /* near puff */
        { /* a scorch decal on the back wall: blended, ZMODE_DEC, not an effect */
            const float z = 0.90f, iw = fxs_iw(z);
            fxs_quad(fxs_vx(FX_SX(200), 40, z, iw, 0, 0, 255, 255, 255, 255), fxs_vx(FX_SX(230), 40, z, iw, 1, 0, 255, 255, 255, 255),
                     fxs_vx(FX_SX(230), 70, z, iw, 1, 1, 255, 255, 255, 255), fxs_vx(FX_SX(200), 70, z, iw, 0, 1, 255, 255, 255, 255));
            fxs_flush(M_DECAL, T_DECAL);
        }
    }
    gfx_scegu_set_phase(0); /* the 2D phase: a HUD bar, blended, no depth (the layer goes under it) */
    fxs_quad(fxs_vx(8, 8, 0, 1, 0, 0, 30, 30, 60, 200), fxs_vx(120, 8, 0, 1, 0.01f, 0, 30, 30, 60, 200),
             fxs_vx(120, 30, 0, 1, 0.01f, 0.01f, 30, 30, 60, 200), fxs_vx(8, 30, 0, 1, 0, 0.01f, 30, 30, 60, 200));
    fxs_flush(M_HUD, T_FLAT);
    gfx_soft_api.end_frame();
}
