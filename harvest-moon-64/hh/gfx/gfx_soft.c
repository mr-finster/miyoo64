/**
 * Software rasterizer backend for the handheld / desktop port.
 *
 * Consumes the screen-space vertices beckerd's gfx_pc.c now emits (after the CPU
 * projection restored in gfx_emit_vertex): a 24-byte psp_fast_t {float u,v; RGBA
 * color; float x,y,z}, x/y in pixels, z in 0..1, u/v in texels, color packed 8888.
 *
 * Implements struct GfxRenderingAPI gfx_soft_api plus the texman / gfx_scegu
 * helpers gfx_pc.c calls directly (a thin compat layer over this cache/state).
 * Correctness-first: bounding-box edge-function rasterizer, nearest sampling,
 * affine u/v (the pipeline is already post-perspective-divide, like the PSP GU).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "savestate.h"
#include "port.h"
extern u32 port_time_us(void);
extern void port_log(const char *fmt, ...);
#define PROF_BEGIN() const u32 _pt0 = port_time_us()
#define PROF_END(slot) PORT_PROF_ADD(slot, port_time_us() - _pt0)
#include <math.h>

#include <PR/gbi.h>
#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"

/* GU pixel formats (values match gfx_pc.c's compat defines / pspgu.h). */
#define GU_PSM_5551 1
#define GU_PSM_8888 3

/* Vertex as emitted by gfx_pc.c (must match psp_fast_t byte layout, incl. the
 * non-PSP invw field used for perspective-correct texturing). */
typedef struct { float u, v; uint8_t r, g, b, a; float x, y, z; float invw; } SoftVtx;

/* -------------------------------------------------------------- framebuffer --- */
/* Current target (aliases of the native or internal buffers, see the frame
 * section). All excluded from savestates: they describe live allocations. */
static uint32_t *sFB SAVESTATE_EXCLUDE;  /* ARGB8888 (malloc'd -- keep live over load) */
static uint16_t *sZB SAVESTATE_EXCLUDE;  /* 0..65535, larger = farther (malloc'd) */
static int sW SAVESTATE_EXCLUDE, sH SAVESTATE_EXCLUDE;
static uint32_t *sNatFB SAVESTATE_EXCLUDE; /* native target (defined with the frame section) */
#ifdef PM64
static bool sEdgeOpaque SAVESTATE_EXCLUDE; /* gfx_soft_set_edge_opaque (X12) */
static bool sOwnTri SAVESTATE_EXCLUDE;     /* gfx_soft_set_own_edges (PM64_FILLRULE) */
static bool sHudExact SAVESTATE_EXCLUDE, sHudExactRun SAVESTATE_EXCLUDE; /* raster_tri's pixel-exact 2D (X27) */
static bool sRtActive SAVESTATE_EXCLUDE; /* render-to-texture target, see gfx_soft_rt_begin */
int gfx_soft_fill_raw_alpha = -1; /* a FILL-mode rectangle's own alpha (0 or 255; gfx_pc.c sets it around the draw,
                                  * as the combiner has no alpha then: G_RM_NOOP does not blend) or -1 */
static bool sRtZDirty SAVESTATE_EXCLUDE; /* its depth is not cleared yet (gfx_soft_rt_begin) */
static void rt_touch(void);
static void rt_zclear(void);
#endif


/* -------------------------------------------------------------- textures --- */
enum { WRAP_REPEAT = 0, WRAP_CLAMP = 1, WRAP_MIRROR = 2 };
#define MAX_TEXTURES 4096
#ifdef PM64
#define TEXARENA_SZ  (16 * 1024 * 1024) /* 5551 is stored unpacked (4 bytes a texel); m1 scenes peak at 10.4 MB */
#else
#define TEXARENA_SZ  (12 * 1024 * 1024) /* generous on 64 MB; tighten later */
#endif

typedef struct {
    const void *px;     /* is16 ? uint16 RGBA5551 : uint32 ARGB8888, w*h */
    int w, h;
    int wrap_s, wrap_t;
    bool valid;
    bool is16;          /* device round 10: 5551 stored 16-bit, not expanded */
} SoftTex;

/* Texture arena + table excluded from savestates: 12 MB would bloat every
 * state, and the cache is flushed on load so textures re-import from the
 * restored course data anyway. Byte-addressed: the N64's RGBA16/CI textures
 * (the road/kart/scenery fill that dominates the memory-bound raster) are
 * stored 16-bit rather than expanded to 32-bit, halving their cache
 * footprint; IA/I textures stay 32-bit ARGB. */
static SoftTex sTex[MAX_TEXTURES] SAVESTATE_EXCLUDE;
/* Heap, not a static array: a named section is PROGBITS even when zero-
 * initialised, so the arena was 12 MB of zeros in the ELF (M6: the shipped
 * game is code only and should be ~1 MB). Allocated once in gfx_soft_init. */
static uint8_t *sTexArena SAVESTATE_EXCLUDE;
static size_t sArenaUsed SAVESTATE_EXCLUDE; /* bytes */

/* RGBA5551 (a<<15|b<<10|g<<5|r, gfx_pc's layout) -> ARGB8888. 5->8 bits by
 * replication (x<<3|x>>2): cheap, and we are trading a cache miss for a few
 * ALU ops on a memory-bound path, so it is a clear win. */
static inline uint32_t unpack5551(uint16_t c) {
    const uint32_t r5 = c & 0x1f, g5 = (c >> 5) & 0x1f, b5 = (c >> 10) & 0x1f;
    /* Place x<<3 in each byte, then OR in all three x>>2 replication nibbles
     * with one shift+mask (bits 19-23 >> 5 land in 16-18, etc.; the lanes can't
     * cross because each byte's low 3 bits are still zero). Alpha is branchless:
     * -(c>>15) is 0 or all-ones. Bit-identical to the per-channel form. */
    uint32_t out = (r5 << 19) | (g5 << 11) | (b5 << 3);
    out |= (out >> 5) & 0x00070707u;
    return out | ((uint32_t)-(int32_t)(c >> 15) & 0xFF000000u);
}
/* One texel as ARGB8888 from either storage format. */
static inline uint32_t texel_at(const void *px, bool is16, int idx) {
    return is16 ? unpack5551(((const uint16_t *)px)[idx]) : ((const uint32_t *)px)[idx];
}
/* Ids are handed out from 1 and recycled on every cache reset (texman_clear):
 * gfx_pc drops its whole node pool on a reset and asks for fresh ids, so a
 * monotonic counter ran out (4096) after ~25 arena resets -- every later
 * texture got id 0, never uploaded, and drew as a white block. Long sessions
 * hit it around the 4-minute mark; it looked like save states "slowly
 * corrupting textures" because those were the long sessions. */
static uint32_t sNextTexId SAVESTATE_EXCLUDE = 1; /* 0 = none */
static uint32_t sBoundTex[2] SAVESTATE_EXCLUDE;   /* per tile */
static int sTexUploadTile SAVESTATE_EXCLUDE;      /* tile targeted by the next upload (select_texture) */

/* A textured draw whose bound texture is not valid comes out as a flat block of the combiner's colour (OoT's
 * red-sun bar after a long session: a cache reset under the draw's own imports, gfx_cache_epoch in gfx_pc.c). Said
 * in the log four times a session, so a device run catches it; the total at exit (the stress sweeps' count). */
uint32_t gfx_soft_untex_draws SAVESTATE_EXCLUDE;
static void untex_total(void) { port_log("gfx: %u untextured draws this session\n", (unsigned) gfx_soft_untex_draws); }
static void untex_note(const char *what, uint32_t id) {
    if (gfx_soft_untex_draws++ == 0) atexit(untex_total);
    if (gfx_soft_untex_draws <= 4)
        port_log("gfx: untextured draw (%s): tex %u not valid (bound %u/%u, arena %d%%)\n", what, (unsigned) id,
                 (unsigned) sBoundTex[0], (unsigned) sBoundTex[1], (int) (sArenaUsed * 100 / TEXARENA_SZ));
}

/* the size of the texture bound to a tile (gfx_pc.c's UV drop for two-tile draws); 0 when none */
void gfx_soft_tex_size(int tile, int *w, int *h) { const SoftTex *t = &sTex[sBoundTex[tile & 1]]; *w = t->valid ? t->w : 0; *h = t->valid ? t->h : 0; }

static inline uint32_t argb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t gfx_soft_new_texture(void) {
    if (sNextTexId >= MAX_TEXTURES) return 0;
    return sNextTexId++;
}

static void gfx_soft_select_texture(int tile, uint32_t texture_id) {
    if (tile < 0 || tile > 1) return;
    sBoundTex[tile] = texture_id;
    sTexUploadTile = tile;
}

/* type: GU_PSM_8888 (RGBA bytes) or GU_PSM_5551 (u16). 5551 is stored 16-bit
 * as-is; 8888 is normalised to ARGB8888. */
size_t gfx_soft_arena_peak;      /* bytes, highest arena use (bench log) */
uint32_t gfx_soft_phase_switches; /* 2D<->3D target switches (bench log; diddy-kong-racing a0ee8b5) */
size_t gfx_soft_arena_size(void) { return TEXARENA_SZ; }
static void gfx_soft_upload_texture(const uint8_t *buf, int width, int height, unsigned int type) {
    PROF_BEGIN();
    uint32_t id = sBoundTex[sTexUploadTile];
    if (id == 0 || id >= MAX_TEXTURES || width <= 0 || height <= 0) return;
    const size_t n = (size_t)width * height;
#ifdef PM64
    /* 5551 is unpacked to ARGB8888 here, once, not at every texel fetch (diddy-kong-racing
     * a0ee8b5: the per-fetch unpack was ~11% of the rasterizer's instructions).
     * Twice the arena per 16-bit texture: the bench logs the arena peak per scene. */
    const bool src16 = (type == GU_PSM_5551);
    const bool is16 = false;
#else
    const bool src16 = false;
    const bool is16 = (type == GU_PSM_5551);
#endif
    const size_t bytes = ((n * (is16 ? 2 : 4)) + 15u) & ~(size_t)15; /* keep 16-byte alignment */
    if (sArenaUsed + bytes > TEXARENA_SZ) {
        /* Never wrap in place: older SoftTex entries point into the arena and
         * would alias. gfx_pc checks texman_can_hold() before importing and
         * does a coordinated cache flush (gfx_texture_cache_reset ->
         * texman_clear) under pressure, so landing here means a direct-call
         * upload raced past that check — drop the upload and say so. */
        static int sFullLogs;
        if (sFullLogs < 8) { sFullLogs++; port_log("gfx_soft: tex arena full, dropping %dx%d upload\n", width, height); }
        return;
    }
    void *dst = &sTexArena[sArenaUsed];
    sArenaUsed += bytes;
    if (sArenaUsed > gfx_soft_arena_peak) gfx_soft_arena_peak = sArenaUsed;
    if (src16) {
        uint32_t *d = (uint32_t *)dst; /* unpack now: fetches read plain ARGB8888 */
        for (size_t i = 0; i < n; i++) d[i] = unpack5551(((const uint16_t *)buf)[i]);
    } else if (is16) {
        memcpy(dst, buf, n * 2); /* store 5551 verbatim, unpacked lazily at fetch */
    } else { /* GU_PSM_8888: RGBA bytes -> ARGB8888 */
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = argb(buf[i*4+0], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    }
    sTex[id].px = dst; sTex[id].is16 = is16; sTex[id].w = width; sTex[id].h = height; sTex[id].valid = true;
    { /* MK64_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("MK64_TEXDUMP"); }
        if (sTd) {
            static int sTdN; char path[512];
            snprintf(path, sizeof(path), "%s/tex_%04d_id%u_%dx%d.ppm", sTd, sTdN, id, width, height);
            FILE *f = fopen(path, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", width, height); for (size_t i = 0; i < n; i++) { uint32_t p = texel_at(dst, is16, (int)i); uint8_t c[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p }; fwrite(c, 1, 3, f); } fclose(f); }
            snprintf(path, sizeof(path), "%s/tex_%04d_id%u_%dx%d_a.pgm", sTd, sTdN, id, width, height);
            f = fopen(path, "wb");
            if (f) { fprintf(f, "P5\n%d %d\n255\n", width, height); for (size_t i = 0; i < n; i++) { uint8_t a = (uint8_t)(texel_at(dst, is16, (int)i) >> 24); fwrite(&a, 1, 1, f); } fclose(f); }
            sTdN++;
        }
    }
    PROF_END(PROF_TEX);
}

static void gfx_soft_set_sampler_parameters(int tile, bool linear, uint32_t cms, uint32_t cmt) {
    (void)linear;
    if (tile < 0 || tile > 1) return;
    uint32_t id = sBoundTex[tile];
    if (id && id < MAX_TEXTURES) {
        /* Same mapping as gfx_scegu's gfx_cm_to_opengl: CLAMP -> clamp, everything
         * else (incl. G_TX_MIRROR — the mirror copy is baked in doubled) -> repeat. */
        sTex[id].wrap_s = (cms & G_TX_CLAMP) ? WRAP_CLAMP : WRAP_REPEAT;
        sTex[id].wrap_t = (cmt & G_TX_CLAMP) ? WRAP_CLAMP : WRAP_REPEAT;
    }
}


/* Per-pixel wrap with the power-of-two mask hoisted (mask = size-1, else -1).
 * REPEAT on a power-of-two size is one AND -- the common case for N64
 * textures; CLAMP two compares. Only odd sizes/MIRROR take the slow path. */
static inline int wrapf(int c, int size, int mode, int mask) {
    if (mode == WRAP_REPEAT) {
        if (mask >= 0) return c & mask;
        c %= size; if (c < 0) c += size; return c;
    }
    return c < 0 ? 0 : (c >= size ? size - 1 : c); /* WRAP_CLAMP (MIRROR unreachable) */
}
static inline int pow2mask(int size) { return (size > 0 && (size & (size - 1)) == 0) ? size - 1 : -1; }
/* float -> int32 as the device's VFP converts (vcvt saturates, NaN gives 0), so the x86 proxy agrees with it where
 * a plain cast is undefined: a near-degenerate triangle's colour step reaches ~1e10 in 16.16, and x86's cvttss2si
 * gives INT_MIN there (mario-kart-64 b2dff40, R31; `make UBSAN=1` found three on m1). */
static inline int32_t sat_i32(float f) {
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
}

/* PM64_NEARW (default on; 0 = the W_MIN 2 clip and the span depth before it, 2026-10-04; goldeneye-007 c20b9ee,
 * ocarina-of-time's OOT_NEARW): with no near plane (PM64_NEARCLIP=0: Paper Mario's F3DZEX2 NoN draws everything in front of
 * the eye, w > 0) gfx_pc.c's clipToNearW now keeps everything from w = 1/64 on, not from w = 2. Faces that near reach
 * raster_tri with depths far below 0 -- z/w * 0.5 + 0.5 is about 1 - 16 / w with the camera's near plane at 16
 * (CAM_NEAR_CLIP): -1023 at w = 1/64 -- and the span setup lost them: each 16-pixel span started from its depth
 * clamped to [-8, 2] (the NoN clamp) and stepped by a slope clamped to +-1 a
 * pixel, so a span starting at z = -40 and rising got the nearest depth (0) many pixels too soon, in front of things
 * it lies behind. Now a span that starts below -1 and rises (or above 2 and falls) starts instead at the first pixel
 * inside [-1, 2], from that pixel's exact depth, and steps back to the start; the slope clamp is +-6. Every pixel then
 * stores what its exact depth clamps to (the RDP's 0..max), give or take 1: below -1 and above 2 both directions
 * agree, and past a step of 6 the depth crosses the whole range in one pixel either way. The values stay within 98
 * units (16 steps of 6 from 2), inside 24.8's int32 range. A span starting inside [-1, 2] is set up exactly as
 * before. tools/port/nearw_test.c (nearz) checks it against the exact depth. */
static int sNearW SAVESTATE_EXCLUDE = -1;
int gfx_soft_nearw_on(void) {
    if (sNearW < 0) { const char *e = getenv("HM64_NEARW"); sNearW = !(e && e[0] == '0'); }
    return sNearW;
}
#define ZQ_SCALE (65535.0f * 256.0f)
/* the depth slope as 24.8 a pixel: clamped to +-1 (old) or +-6 (PM64_NEARW) */
static inline int32_t span_zdq(float zdx) {
    const float m = gfx_soft_nearw_on() ? 6.0f : 1.0f;
    return (int32_t)((zdx < -m ? -m : zdx > m ? m : zdx) * ZQ_SCALE);
}
/* A span's first depth, zf outside [-1, 2] with PM64_NEARW on (above). span: its pixels (<= 16). */
static int32_t __attribute__((noinline)) span_zq_far(float zf, float zdx, int span, int32_t zdq) {
    float k;
    if (zf < -1.0f && zdx > 0.0f) k = (-1.0f - zf) / zdx;
    else if (zf > 2.0f && zdx < 0.0f) k = (zf - 2.0f) / -zdx;
    else return (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * ZQ_SCALE); /* never enters: every pixel clamps alike */
    int kc = span - 1;
    if (k < (float)kc) { kc = (int)k; if ((float)kc < k) kc++; } /* the first pixel inside (ceil; k >= 0) */
    /* in double: zf and k * zdx cancel there, and float kept only ~1e-4 of the sum */
    double zr = (double)zf + (double)kc * (double)zdx;
    zr = zr < -1.0 ? -1.0 : zr > 2.0 ? 2.0 : zr;
    return (int32_t)(zr * (double)ZQ_SCALE) - kc * zdq;
}
/* true: span_zq_far sets this span up (zf outside [-1, 2], not NaN, PM64_NEARW on) */
static inline bool span_z_out(float zf) {
    return (zf < -1.0f || zf > 2.0f) && gfx_soft_nearw_on();
}
/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}

/* 3D bilinear filtering (menu "3D FILTER: SMOOTH"; off by default). uf/vf are
 * 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* 1 = bilinear only when magnified (round 3's default). 0 = every textured pixel bilinear (the cross-port
 * P17, from the user's device report on DKR: the magnified-only gate chose per triangle, so floors flipped between
 * smooth and sharp along triangle edges as the camera moved; PM64_MAGBILIN=0). Round 16 made P17 the default; on the
 * device it cost plain 3D 7-9% (field 31.82 -> 34.58 ms); round 3 turned it off for that speed. Since 30 Sep the
 * user's rule is correct visuals first ("i'd rather slower but correct visuals"): the gate is a lossy speed trick,
 * so every textured pixel is bilinear again, as on the N64. PM64_MAGBILIN=1: the magnified-only gate. */
int gfx_soft_magbilin SAVESTATE_EXCLUDE = 0;
/* (internal area) / (native area): the magnified-only test measures screen
 * area in the 3D target's pixels, but what the eye sees is the native upscale,
 * so at 80% 3D RES a texel that covers 0.8 internal px covers a whole native
 * px and should still be bilinear. 1.0 when unified. Set with the targets. */
static float sGateScale SAVESTATE_EXCLUDE = 1.0f;
uint64_t gfx_soft_span_px SAVESTATE_EXCLUDE; /* 3D span pixels this window (overdraw proxy) */
uint64_t gfx_soft_tris SAVESTATE_EXCLUDE;   /* 3D triangles rasterized this window (setup proxy) */
/* Lerp two 8-bit channels packed in bits 0-7 and 16-23 of x/y by f (0..255).
 * Bit-identical to the per-channel  a + (((b-a)*f) >> 8)  used before: since
 * 256*a is a multiple of 256, floor((256a + (b-a)f)/256) == a + floor((b-a)f/256)
 * -- and here every term is non-negative and each lane's sum <= 255*256, so
 * nothing carries between lanes. */
static inline uint32_t lerp2(uint32_t x, uint32_t y, int f) {
    return ((x * (uint32_t)(256 - f) + y * (uint32_t)f) >> 8) & 0x00FF00FFu;
}
/* GCC declined to inline the old struct-pointer version (it was a 1.3KB
 * separate function with a full register-save prologue per bilinear pixel);
 * scalar params + the packed lerps make it small, always_inline pins it. */
static inline __attribute__((always_inline)) uint32_t
bilerp(const void *px, bool i16, int w, int h, int ws, int wt, int32_t uf, int32_t vf, int mu, int mv) {
    const int32_t su = uf - 0x8000, sv = vf - 0x8000;
    const int iu0 = wrapf(su >> 16, w, ws, mu), iu1 = wrapf((su >> 16) + 1, w, ws, mu);
    const int iv0 = wrapf(sv >> 16, h, wt, mv), iv1 = wrapf((sv >> 16) + 1, h, wt, mv);
    const int fu = (su >> 8) & 0xff, fv = (sv >> 8) & 0xff;
    const int b0 = iv0 * w, b1 = iv1 * w;
    const uint32_t a = texel_at(px, i16, b0 + iu0), b = texel_at(px, i16, b0 + iu1);
    const uint32_t c = texel_at(px, i16, b1 + iu0), d = texel_at(px, i16, b1 + iu1);
    /* even lanes = B,R ; odd lanes = G,A (shifted down 8) */
    const uint32_t te = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, fu), to = lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, fu);
    const uint32_t ue = lerp2(c & 0x00FF00FFu, d & 0x00FF00FFu, fu), uo = lerp2((c >> 8) & 0x00FF00FFu, (d >> 8) & 0x00FF00FFu, fu);
    return lerp2(te, ue, fv) | (lerp2(to, uo, fv) << 8);
}

/* -------------------------------------------------------------- shaders --- */
/* Per-channel texel use, derived from the combiner (device round 6b): the
 * cloud sprites are I4 textures used ONLY for alpha -- RGB = SHADE (white).
 * Multiplying the texel's grey intensity into RGB painted the soft edge grey. */
struct ShaderProgram { uint32_t shader_id; struct CCFeatures cc; bool rgb_tex, rgb_in, a_tex, a_in, rgb_texa, rgb_decal; };
#define MAX_SHADERS 64
static struct ShaderProgram sShaders[MAX_SHADERS];
static int sNumShaders;
static struct ShaderProgram *sCurShader;

static bool gfx_soft_z_is_from_0_to_1(void) { return true; }
static void gfx_soft_unload_shader(struct ShaderProgram *old) { if (sCurShader == old || !old) sCurShader = NULL; }
static void gfx_soft_load_shader(struct ShaderProgram *p) { sCurShader = p; }
static struct ShaderProgram *gfx_soft_create_and_load_new_shader(uint32_t shader_id) {
    struct ShaderProgram *p = &sShaders[sNumShaders < MAX_SHADERS ? sNumShaders++ : MAX_SHADERS - 1];
    p->shader_id = shader_id;
    gfx_cc_get_features(shader_id, &p->cc);
    p->rgb_tex = p->rgb_in = p->a_tex = p->a_in = p->rgb_texa = p->rgb_decal = false;
    for (int i = 0; i < 4; i++) {
        const int c0 = p->cc.c[0][i], c1 = p->cc.c[1][i];
        if (c0 == SHADER_TEXEL0 || c0 == SHADER_TEXEL1) p->rgb_tex = true;
        if (c0 == SHADER_TEXEL0A) { p->rgb_tex = true; p->rgb_texa = true; }
        if (c0 >= SHADER_INPUT_1 && c0 <= SHADER_INPUT_4) p->rgb_in = true;
        if (c1 == SHADER_TEXEL0 || c1 == SHADER_TEXEL0A || c1 == SHADER_TEXEL1) p->a_tex = true;
        if (c1 >= SHADER_INPUT_1 && c1 <= SHADER_INPUT_4) p->a_in = true;
    }
    /* (TEXEL0 - X) * TEXEL0_ALPHA + X = the texture decaled over input X by its
     * own alpha (G_CC_BLENDRGBA, the Sherbet Land penguin eyes; gfx_pc hands
     * us X as the vertex colour). Replacing the texel RGB with its alpha drew
     * every opaque eye texel white -- no pupils. */
    {
        const uint8_t *c = p->cc.c[0];
        if (c[0] == SHADER_TEXEL0 && c[2] == SHADER_TEXEL0A && c[1] == c[3] && c[1] >= SHADER_INPUT_1 && c[1] <= SHADER_INPUT_4) {
            p->rgb_decal = true;
            p->rgb_texa = false;
        }
    }
    sCurShader = p;
    return p;
}
static struct ShaderProgram *gfx_soft_lookup_shader(uint32_t shader_id) {
    for (int i = 0; i < sNumShaders; i++) if (sShaders[i].shader_id == shader_id) return &sShaders[i];
    return NULL;
}
static void gfx_soft_shader_get_info(struct ShaderProgram *p, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = p->cc.num_inputs;
    used_textures[0] = p->cc.used_textures[0];
    used_textures[1] = p->cc.used_textures[1];
}

/* -------------------------------------------------------------- state --- */
static bool sDepthTest = true, sDepthMask = true, sZDecal, sUseAlpha;
static int sVpX, sVpY, sVpW, sVpH;
static int sScX0, sScY0, sScX1, sScY1;
static bool sTexAdd;       /* GE ADD: texel + shade */

static void gfx_soft_set_depth_test(bool d) { sDepthTest = d; }
static void gfx_soft_set_depth_mask(bool m) { sDepthMask = m; }
static void gfx_soft_set_zmode_decal(bool d) { sZDecal = d; }
static void gfx_soft_set_use_alpha(bool a) { sUseAlpha = a; }
extern int gfx_trace_frames;
static void gfx_soft_set_viewport(int x, int y, int w, int h) {
    sVpX=x; sVpY=y; sVpW=w; sVpH=h;
    if (gfx_trace_frames > 0) port_log("  soft: viewport %d,%d %dx%d (target %dx%d %s)\n", x, y, w, h, sW, sH, sFB == sNatFB ? "native" : "internal");
}
static void gfx_soft_set_scissor(int x, int y, int w, int h) {
    sScX0 = x < 0 ? 0 : x; sScY0 = y < 0 ? 0 : y;
    sScX1 = (x + w > sW) ? sW : x + w; sScY1 = (y + h > sH) ? sH : y + h;
    if (gfx_trace_frames > 0) port_log("  soft: scissor %d,%d %dx%d -> [%d,%d)x[%d,%d) (target %dx%d)\n", x, y, w, h, sScX0, sScX1, sScY0, sScY1, sW, sH);
}

/* -------------------------------------------------------------- raster --- */
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
/* port_ifloorf/port_iceilf (port.h) replace floorf/ceilf (libm calls on
 * ARMv7). fmin3/fmax3 avoid fminf/fmaxf, whose NaN semantics force a call; the
 * coords here are finite (bounded by the W_MIN near clip in gfx_pc.c). */
#define ifloorf port_ifloorf
#define iceilf  port_iceilf
static inline float fmin3(float a, float b, float c) { float m = a < b ? a : b; return m < c ? m : c; }
static inline float fmax3(float a, float b, float c) { float m = a > b ? a : b; return m > c ? m : c; }
/* Per-pixel saturating clamps of a fixed-point value: clamp(q >> 16, 0..255)
 * for the 16.16 colours and clamp(q >> 8, 0..65535) for 24.8 z. On ARMv6+ each
 * is ONE branchless USAT with the shift folded in; the C form (what the desktop
 * proxy compiles) is bit-identical by USAT's definition. GCC only pattern-
 * matches the signed-compare shape -- the old ((unsigned)v > 255u) macro
 * compiled to a compare + conditional branch per channel, x4 per pixel. */
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 6) && !defined(__thumb__)
static inline int clamp8_sh16(int32_t q) { int r; __asm__("usat %0, #8, %1, asr #16" : "=r"(r) : "r"(q)); return r; }
static inline int clamp16_sh8(int32_t q) { int r; __asm__("usat %0, #16, %1, asr #8" : "=r"(r) : "r"(q)); return r; }
#else
static inline int clamp8_sh16(int32_t q) { int v = q >> 16; return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline int clamp16_sh8(int32_t q) { int v = q >> 8;  return v < 0 ? 0 : (v > 65535 ? 65535 : v); }
#endif
/* x/255, exact for 0 <= x <= 65534 (verified exhaustively; the first failure
 * is 65535, which no caller can produce: every input is an 8-bit product or a
 * blend taken under fa < 255). Three barrel-shifted ALU ops instead of a
 * umull by a magic constant that raster_tri had spilled to the stack. */
static inline int div255(int x) { return (x + 1 + (x >> 8)) >> 8; }
static inline float edge(const SoftVtx *a, const SoftVtx *b, float px, float py) {
    return (px - a->x) * (b->y - a->y) - (py - a->y) * (b->x - a->x);
}
/* PM64_FILLRULE (2026-10-03; ocarina-of-time a7c4909, the Deku Tree's vision: a one-pixel bright diagonal across
 * Ganondorf's flames): the row solve in raster_tri is biased toward inclusion, and a pixel centre exactly on
 * an edge counts as inside, so a pixel on the edge two triangles share is drawn by both. Opaque, that is
 * invisible; blended, the pixel is blended twice. The RDP without AA_EN writes a pixel only when one fixed
 * sample of it is covered (angrylion blender.c: cvbit, the mask's bit 7), and the edges split the samples
 * exactly, so exactly one triangle draws each pixel. Here, for such draws (gfx_pc: blended or alpha-tested,
 * no AA_EN), each row's ends are decided per pixel by edge functions taken in a canonical vertex order (the
 * lower y, then the lower x, first), so the two triangles of a shared edge compute the same float there and
 * only its sign differs; a centre exactly on the edge goes to the triangle the edge is a left (or top) edge
 * of. Other draws keep the inclusive solve, bit-identical. */
typedef struct { float px, py, ex, ey, sg; bool tie; } OwnEdge;
typedef struct { OwnEdge e[3]; } OwnTri;
static inline void own_edge(OwnEdge *o, const SoftVtx *a, const SoftVtx *b, float s) {
    const bool fwd = a->y < b->y || (a->y == b->y && a->x < b->x);
    const SoftVtx *p = fwd ? a : b, *q = fwd ? b : a;
    o->px = p->x; o->py = p->y; o->ex = q->x - p->x; o->ey = q->y - p->y;
    o->sg = fwd ? s : -s; /* the triangle's inside is where sg * e > 0 */
    o->tie = o->sg * o->ey > 0.0f || (o->ey == 0.0f && o->sg * o->ex < 0.0f); /* left edge, or top (y down) */
}
static inline bool own_px(const OwnTri *t, int x, float py) {
    const float fx = (float)x + 0.5f;
    for (int i = 0; i < 3; i++) {
        const OwnEdge *o = &t->e[i];
        const float e = (fx - o->px) * o->ey - (py - o->py) * o->ex;
        if (!(o->sg * e > 0.0f || (e == 0.0f && o->tie))) return false;
    }
    return true;
}

/* Span rasterizer. Per row: solve the three half-plane constraints for an
 * x-interval (no per-pixel inside tests), then walk the span with
 * incrementally stepped z / shade / (u/w, v/w, 1/w). Perspective division
 * happens every PERSP_SPAN pixels with linear u,v between (the N64-plugin
 * standard; MK64's ground polygons are huge, so the per-pixel fdiv the old
 * loop did dominated the race profile). Row bases are recomputed from the
 * vertex data each row, so precision stays within one mul-add of exact and
 * shared triangle edges stay crack-free. */
#ifdef PM64
/* Generic RDP colour combiner (gfx_pc pm_cc_generic decides when): both cycles of
 * (A - B) * C + D per channel with the RDP's own rounding, TEXEL1 sampled from the
 * next tile at a constant texel offset (both tiles share S/T), SHADE from the vertex
 * colour. Used for Paper Mario's lit sprites (TEXEL0 x TEXEL1 through two palettes),
 * windows, candles and boxes; everything else stays on the fast paths.
 * 2-cycle texel swap (round 15): the RDP fetches a pixel's two texels in its two texture
 * cycles, so the combiner's second cycle sees TEXEL0 = the tile+1 texel and TEXEL1 = the
 * next pixel's TEXEL0 (angrylion's combiner_2cycle; GLideN64 swaps them in stage 2). We
 * take this pixel's TEXEL0 for the latter. Without the swap, the light rays over Peach's
 * castle (3_letter, FC309861 550EFF4F: cycle 2 alpha (COMBINED - TEXEL0) * PRIM) came out
 * fully transparent. */
enum { S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_ZERO, S_COMBA, S_T0A, S_T1A, S_PRIMA, S_SHADEA, S_ENVA, S_PLOD, S_NOISE, S_N };
static struct {
    bool on, two, t0, t1;
    int8_t rgb[2][4], al[2][4];
    int prim[4], env[4], plod, ds, dt;
    int32_t t1a[2], t1c[2]; int t1k[2]; /* TEXEL1 from TEXEL0's S/T: (x + a) * 2^k + c, 16.16 texels (gen_t1) */
    uint32_t w0, w1; /* the raw combine, for PM64_CENSUS */
    int fast;        /* gen_classify_fast: 0 = the operand table (pm_gen_eval), 1..10 = a fast path */
    bool fast_ta;    /* fast path 1: alpha T0a (else T0a x SHADEa) */
    int k1[4], k2[4], k3s[4]; /* fast paths: the second cycle's constants per channel, k3s = D << 8 */
    int shroud, sh[3]; /* the shroud (gfx_pc.c pm_shroud): 5-bit factor (0 = none) and its tint colour */
} sGen SAVESTATE_EXCLUDE;
/* PM64_CENSUS=1: pixels per generic combine, 3D and 2D, printed at exit (round 16: which combines get fast paths). */
static int sGenCensus SAVESTATE_EXCLUDE = -1;
static struct { uint32_t w0, w1; uint8_t two, fast; uint64_t px3, px2; uint32_t draws; } sGenCs[64] SAVESTATE_EXCLUDE;
static void gen_census_print(void) {
    uint64_t t3 = 0, t2 = 0;
    for (int i = 0; i < 64 && sGenCs[i].draws; i++) { t3 += sGenCs[i].px3; t2 += sGenCs[i].px2; }
    fprintf(stderr, "census: generic combiner pixels, 3D %llu, 2D %llu\n", (unsigned long long) t3, (unsigned long long) t2);
    for (;;) { /* largest first */
        int b = -1;
        for (int i = 0; i < 64 && sGenCs[i].draws; i++)
            if (sGenCs[i].draws != 0xFFFFFFFFu && (b < 0 || sGenCs[i].px3 + sGenCs[i].px2 > sGenCs[b].px3 + sGenCs[b].px2)) b = i;
        if (b < 0) break;
        fprintf(stderr, "census:   FC%06X %08X %s: 3D %llu px, 2D %llu px, %u draws, fast path %d\n", (unsigned) sGenCs[b].w0,
                (unsigned) sGenCs[b].w1, sGenCs[b].two ? "2cyc" : "1cyc", (unsigned long long) sGenCs[b].px3,
                (unsigned long long) sGenCs[b].px2, sGenCs[b].draws, sGenCs[b].fast);
        sGenCs[b].draws = 0xFFFFFFFFu;
    }
}
static void gen_census_add(uint64_t px, bool is3d) {
    if (sGenCensus < 0) { const char *e = getenv("HM64_CENSUS"); sGenCensus = e && e[0] == '1'; if (sGenCensus) atexit(gen_census_print); }
    if (!sGenCensus) return;
    int i = 0;
    while (i < 64 && sGenCs[i].draws && !(sGenCs[i].w0 == sGen.w0 && sGenCs[i].w1 == sGen.w1 && sGenCs[i].two == sGen.two)) i++;
    if (i == 64) return;
    if (!sGenCs[i].draws) { sGenCs[i].w0 = sGen.w0; sGenCs[i].w1 = sGen.w1; sGenCs[i].two = sGen.two; sGenCs[i].fast = (uint8_t) sGen.fast; }
    sGenCs[i].draws++;
    if (is3d) sGenCs[i].px3 += px; else sGenCs[i].px2 += px;
}
/* TEXEL1's coordinate from TEXEL0's (round 16: the cross-port R29 for the generic combiner). Tile 1 has its own shift
 * and corner: x1 = (x0 + a) * 2^k + c in 16.16 texels, k tile 1's shift less tile 0's (as powers of two), a tile 0's
 * corner less the filter's half texel, c that half texel less tile 1's corner. With k = 0 it is a plain offset. The
 * fountain outside Peach's castle scrolls a 16x16 mask at shift t 2 (four times as tall) from a corner at 9.5 texels
 * over its 32x32 water. gfx_pc.c sets k = 0 and round 15's whole-texel offset under PM64_T1SCALE=0. */
static inline int32_t gen_t1(int32_t x, int i) {
    const int k = sGen.t1k[i];
    return k == 0 ? x + sGen.t1a[i] + sGen.t1c[i] : k > 0 ? (int32_t) ((uint32_t) (x + sGen.t1a[i]) << k) + sGen.t1c[i]
                                                          : ((x + sGen.t1a[i]) >> -k) + sGen.t1c[i];
}
/* gen_t1 from locals: the rows hold the draw's offsets in registers (the global's fields were reloaded every pixel,
 * as a framebuffer store may alias them) */
static inline int32_t gen_t1_l(int32_t x, int32_t a, int32_t c, int k) {
    return k == 0 ? x + a + c : k > 0 ? (int32_t) ((uint32_t) (x + a) << k) + c : ((x + a) >> -k) + c;
}
static int8_t gm_rgb_a(int v) { return v <= 5 ? v : v == 6 ? S_ONE : v == 7 ? S_NOISE : S_ZERO; }
static int8_t gm_rgb_b(int v) { return v <= 5 ? v : S_ZERO; } /* CENTER / K4: 0 */
static int8_t gm_rgb_c(int v) {
    static const int8_t t[16] = { S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_ZERO, S_COMBA, S_T0A, S_T1A, S_PRIMA, S_SHADEA, S_ENVA, S_ZERO, S_PLOD, S_ZERO };
    return v < 16 ? t[v] : S_ZERO; /* SCALE / LOD_FRACTION / K5: 0 */
}
static int8_t gm_rgb_d(int v) { return v <= 5 ? v : v == 6 ? S_ONE : S_ZERO; }
static int8_t gm_a_abd(int v) { return v <= 5 ? v : v == 6 ? S_ONE : S_ZERO; }
static int8_t gm_a_c(int v) { static const int8_t t[8] = { S_ZERO, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_PLOD, S_ZERO }; return t[v & 7]; }
static void gen_build_ops(void);
static void gen_classify_fast(void);
void gfx_soft_pm_shroud(int f5, uint8_t r, uint8_t g, uint8_t b) {
    sGen.shroud = f5; sGen.sh[0] = r; sGen.sh[1] = g; sGen.sh[2] = b;
}
bool gfx_soft_1cyc_t1 = true; /* gfx_pc sets it from PM64_1CYCT1 */
void gfx_soft_pm_generic(bool on, uint32_t w0, uint32_t w1, bool two_cycle, const uint8_t prim[4], const uint8_t env[4], uint8_t prim_lod, int ds, int dt,
                         const int32_t t1map[6]) {
    sGen.on = on;
    if (!on) return;
    sGen.two = two_cycle; sGen.w0 = w0 & 0xFFFFFF; sGen.w1 = w1;
    sGen.rgb[0][0] = gm_rgb_a((w0 >> 20) & 15); sGen.rgb[0][1] = gm_rgb_b((w1 >> 28) & 15);
    sGen.rgb[0][2] = gm_rgb_c((w0 >> 15) & 31); sGen.rgb[0][3] = gm_rgb_d((w1 >> 15) & 7);
    sGen.al[0][0] = gm_a_abd((w0 >> 12) & 7); sGen.al[0][1] = gm_a_abd((w1 >> 12) & 7);
    sGen.al[0][2] = gm_a_c((w0 >> 9) & 7);    sGen.al[0][3] = gm_a_abd((w1 >> 9) & 7);
    sGen.rgb[1][0] = gm_rgb_a((w0 >> 5) & 15); sGen.rgb[1][1] = gm_rgb_b((w1 >> 24) & 15);
    sGen.rgb[1][2] = gm_rgb_c(w0 & 31);        sGen.rgb[1][3] = gm_rgb_d((w1 >> 6) & 7);
    sGen.al[1][0] = gm_a_abd((w1 >> 21) & 7); sGen.al[1][1] = gm_a_abd((w1 >> 3) & 7);
    sGen.al[1][2] = gm_a_c((w1 >> 18) & 7);   sGen.al[1][3] = gm_a_abd(w1 & 7);
    if (!two_cycle && gfx_soft_1cyc_t1) { /* 1-cycle: TEXEL1 is TEXEL0 (gfx_pc.c pm_1cyc_t1_on) */
        for (int i = 0; i < 4; i++) {
            if (sGen.rgb[0][i] == S_T1) sGen.rgb[0][i] = S_T0;
            if (sGen.rgb[0][i] == S_T1A) sGen.rgb[0][i] = S_T0A;
            if (sGen.al[0][i] == S_T1) sGen.al[0][i] = S_T0;
        }
    }
    sGen.t0 = sGen.t1 = false;
    for (int k = 0; k < (two_cycle ? 2 : 1); k++)
        for (int i = 0; i < 4; i++) {
            const int r = sGen.rgb[k][i], a = sGen.al[k][i];
            const bool u0 = r == S_T0 || r == S_T0A || a == S_T0, u1 = r == S_T1 || r == S_T1A || a == S_T1;
            if (k == 0 ? u0 : u1) sGen.t0 = true; /* cycle 2 reads the texels swapped (above) */
            if (k == 0 ? u1 : u0) sGen.t1 = true;
        }
    for (int i = 0; i < 4; i++) { sGen.prim[i] = prim[i]; sGen.env[i] = env[i]; }
    sGen.plod = prim_lod; sGen.ds = ds; sGen.dt = dt;
    for (int i = 0; i < 2; i++) { sGen.t1a[i] = t1map[3 * i]; sGen.t1c[i] = t1map[3 * i + 1]; sGen.t1k[i] = t1map[3 * i + 2]; }
    gen_build_ops();
    gen_classify_fast();
}
static inline int gm_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* The RDP's combiner works in 9 bits (angrylion's color/alpha_combiner_equation): A, B and D are read through
 * special_9bit_exttable (0x180..0x1FF are negative), C is a signed 9-bit value, each cycle keeps
 * ((A - B) x C + D x 256 + 0x80) >> 8 in 9 bits with no clamp, so the second cycle sees COMBINED past 255 or below 0,
 * and only the pixel is clamped (special_9bit_clamptable: 0x100..0x17F -> 255, 0x180..0x1FF -> 0). A sum past 383
 * wraps to "negative" and comes out 0, not 255. The intro's Star Spirit glow (FC10E5E0 230B157F: cycle 1 (T0 - T1) x
 * T0 + 1, alpha (1 - T0) x T1 + T1, then ENV - PRIM x COMBINED, alpha COMBINED x T1) is drawn by that wrap: saturating
 * at 255 instead made a yellow haze over the N64's white cloud. Only the generic combiner's table path (pm_gen_eval);
 * the fast paths (gen_classify_fast) keep their clamp. PM64_CC9BIT=0: the saturating clamp of each cycle, as before. */
static int sCc9 SAVESTATE_EXCLUDE = 1;
static int sT1Rect SAVESTATE_EXCLUDE = 1; /* PM64_T1RECT (the rectangles' TEXEL1, raster_rect) */
static inline int cc_ext9(int v) { v &= 0x1ff; return v >= 0x180 ? v - 0x200 : v; }
static inline int cc_sext9(int v) { v &= 0x1ff; return v >= 0x100 ? v - 0x200 : v; }
static inline int cc_clamp9(int v) { v &= 0x1ff; return v < 0x100 ? v : v < 0x180 ? 255 : 0; }
/* Per-draw operand table: every (cycle, channel, operand) points at an int that is
 * either a per-draw constant or one of the per-pixel values below, so a pixel costs
 * four loads and one multiply-add per channel and cycle. */
static int sGenPix[4][4] SAVESTATE_EXCLUDE;   /* [T0, T1, SHADE, COMB][r g b a] */
static int sGenConst[S_N][4] SAVESTATE_EXCLUDE;
static const int *sGenOp[2][4][4] SAVESTATE_EXCLUDE; /* [cycle][r g b a][A B C D] */
static void gen_build_ops(void) {
    for (int i = 0; i < 4; i++) {
        sGenConst[S_PRIM][i] = sGen.prim[i]; sGenConst[S_ENV][i] = sGen.env[i];
        sGenConst[S_ONE][i] = 255; sGenConst[S_ZERO][i] = 0; sGenConst[S_PLOD][i] = sGen.plod; sGenConst[S_NOISE][i] = 128;
    }
    for (int k = 0; k < 2; k++)
        for (int ch = 0; ch < 4; ch++)
            for (int i = 0; i < 4; i++) {
                int sel = ch < 3 ? sGen.rgb[k][i] : sGen.al[k][i], c = ch;
                if (sel >= S_COMBA && sel <= S_ENVA) { sel -= S_COMBA; c = 3; } /* an alpha used as an RGB input */
                const int *p;
                switch (sel) {
                    case S_T0: p = &sGenPix[k][c]; break;     /* cycle 2: the tile+1 texel */
                    case S_T1: p = &sGenPix[1 - k][c]; break; /* cycle 2: (the next pixel's) TEXEL0 */
                    case S_SHADE: p = &sGenPix[2][c]; break;
                    case S_COMB: p = &sGenPix[3][c]; break;
                    default: p = &sGenConst[sel][c]; break;
                }
                sGenOp[k][ch][i] = p;
            }
}
/* Fast paths for the census's top combines (round 16). Each computes pm_gen_eval's exact result with the same integer
 * steps, just without the operand table; PM64_GENFAST=0 sends every draw through pm_gen_eval (the A/B).
 *   1: cycle 1 T0 x SHADE, alpha T0a x SHADEa or T0a; cycle 2 (COMBINED - K1) x K2 + K3, alpha passed. FC121803 FFFFFF78
 *      and FFFFFFF8, FC127E03 FFFFF3F8 and FFFFF378: the battle stages, the world behind the letter; 2/3 of the pixels.
 *   2: cycle 1 T0 x T1, alpha T1a; cycle 2 (K1 - K2) x COMBINED + K3, alpha passed. FC117E60 F5FFF578 (2D): the letter.
 *   3: cycle 2 (K1 - K2) x TEXEL0 + K3, which reads the tile+1 texel (the swap), alpha (COMBINED - TEXEL0) x Ka with
 *      cycle 1's alpha T0a x SHADEa: FC309861 550EFF4F, the light rays.
 *   4: cycle 1 T1 x T0 for colour and alpha, cycle 2 COMBINED x SHADE: FC20A204 FF13FFFF, the prologue's dust clouds
 *      when the castle is lifted (99.5% of the generic pixels there, 65% of the scene's instructions; the user: "very
 *      laggy"). Nearest too: with P17 off the magnified dust samples nearest.
 *   5: cycle 1 (K1 - K2) x T1 + K3, alpha T1a; cycle 2 passes COMBINED: FC317FFF 5FFEF438, the menus' text.
 *   6: cycle 1 T1, alpha T1a; cycle 2 passes COMBINED: FCFFFFFF FFFD7438, the menus' pictures.
 *      5 and 6 are the file select's 2D (the user: its 3D flips "quite slow"): ~300 instructions a pixel through
 *      the operand table, 2/3 of the 2D generic pixels of the flip after a file is chosen. Every step of both is
 *      exact: a pass-through cycle gives (C << 8 + 0x80) >> 8 = C.
 *   8: cycle 1 (K1 - K2) x SHADE + K3 for colour, alpha (T1a - T0a) x Ka + T0a; cycle 2 passes the colour, alpha
 *      COMBINEDa x Kb: FC322BFF 5F0E923F, Bowser's fire breath in the prologue (4 screens a frame, 85% of the scene's
 *      instructions through the operand table; the user: "huge fps lag").
 *  10: cycle 1 T0 x T1, alpha T1a; cycle 2 (SHADE - K) x COMBINED + COMBINED, alpha passed: FC117E80 F5FFF438, Bowser
 *      in the castle room (~19,000 pixels a frame there).
 *  11: 1-cycle (K - T0) x Ka + T0, alpha a constant: FC357E6A 12FCFCBE, the pause menu's darkened backdrop (a screen a
 *      frame while paused: the user's pause ran at 16.6 fps).
 *  12: cycle 1 (K1 - K2) x T0 + K3, alpha T1a; cycle 2 COMBINED x TEXEL0 (the tile+1 texel, t1), alpha passed:
 *      FC30FE01 5FFEF5F8, the pause menu's panels.
 * A constant is PRIM, ENV, their alphas, 1, 0, PRIM_LOD or the noise stand-in: gen_build_ops points those operands
 * at sGenConst, so their values are read once here. */
/* The fire breath made cheap (the user: "make it as cheap as possible dont care how it looks"): PM64_FIRECHEAP=0 off.
 * Fast path 8's draws (FC322BFF 5F0E923F) keep one particle quad of two (raster_tri skips the other pair of
 * triangles; the count restarts each task) and take their alpha from TEXEL0 alone (the second layer's fetch
 * dropped). With PM64_FIRENEAREST (nearest texels, default on) the fire costs a fraction of its N64 form. */
static int sFireCheap SAVESTATE_EXCLUDE = -1;
static uint32_t sFireTri SAVESTATE_EXCLUDE;
static inline bool fire_cheap(void) {
    if (sFireCheap < 0) { const char *e = getenv("HM64_FIRECHEAP"); sFireCheap = !(e && e[0] == '0'); }
    return sFireCheap;
}
static bool gen_const(int sel) {
    return sel == S_PRIM || sel == S_ENV || sel == S_ONE || sel == S_ZERO || sel == S_PLOD || sel == S_NOISE ||
           sel == S_PRIMA || sel == S_ENVA;
}
static bool gen_zero_product(int a, int b, int c) { return a == b || c == S_ZERO; }
static void gen_classify_fast(void) {
    static int sOn = -1;
    if (sOn < 0) { const char *e = getenv("HM64_GENFAST"); sOn = !(e && e[0] == '0'); }
    sGen.fast = 0;
    if (!sOn) return; /* the shroud: pm_gen_fast and the NEON rows apply it after the combine, as pm_gen_eval */
    const int8_t *r0 = sGen.rgb[0], *a0 = sGen.al[0], *r1 = sGen.rgb[1], *a1 = sGen.al[1];
    if (!sGen.two) { /* 7 (1-cycle): (K - SHADE) x T0 + SHADE, alpha T0a -- FC60FEC1 44FE7339, the Bowser fight's glow */
        if (sGen.t0 && !sGen.t1 && gen_const(r0[0]) && r0[1] == S_SHADE && r0[2] == S_T0 && r0[3] == S_SHADE &&
            gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T0) {
            sGen.fast = 7;
            for (int ch = 0; ch < 3; ch++) sGen.k1[ch] = *((const int *const *) sGenOp[0][ch])[0];
        } else if (sGen.t0 && !sGen.t1 && gen_const(r0[0]) && r0[1] == S_T0 && gen_const(r0[2]) && r0[3] == S_T0 &&
                   gen_zero_product(a0[0], a0[1], a0[2]) && gen_const(a0[3])) {
            sGen.fast = 11;
            for (int ch = 0; ch < 3; ch++) {
                const int *const *o = (const int *const *) sGenOp[0][ch];
                sGen.k1[ch] = *o[0]; sGen.k2[ch] = *o[2];
            }
            sGen.k1[3] = *((const int *const *) sGenOp[0][3])[3]; /* the alpha: d, passed ((d << 8) + 0x80) >> 8 = d */
        }
        return;
    }
    const bool pass_a = gen_zero_product(a1[0], a1[1], a1[2]) && a1[3] == S_COMB;
    /* texture x shade in either cycle, the other passing (1 with K1 0, K2 256, K3 0, exact: a pass is (C << 8 + 0x80)
     * >> 8 = C): FC1219FF FFFFFE38 (T0 x SHADE, then COMBINED) and FC26A004 1F1093FF (LOD_FRACTION reads 0, so
     * TEXEL0, then COMBINED x SHADE), the Star Rod's power-up in the prologue fight (the user: "this sequence is slow
     * too"). Its TEXEL1 is only ever multiplied by 0: not fetched. */
    const bool pass_rgb = gen_zero_product(r1[0], r1[1], r1[2]) && r1[3] == S_COMB;
    const bool ts0a = a0[0] == S_T0 && a0[1] == S_ZERO && a0[2] == S_SHADE && a0[3] == S_ZERO;
    const bool ts0t = gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T0; /* alpha T0a: FC127FFF FFFFF238 */
    const bool ts0 = r0[0] == S_T0 && r0[1] == S_ZERO && r0[2] == S_SHADE && r0[3] == S_ZERO && (ts0a || ts0t) &&
                     pass_rgb && pass_a;
    const bool ts1 = gen_zero_product(r0[0], r0[1], r0[2]) && r0[3] == S_T0 && gen_zero_product(a0[0], a0[1], a0[2]) &&
                     a0[3] == S_T0 && r1[0] == S_COMB && r1[1] == S_ZERO && r1[2] == S_SHADE && r1[3] == S_ZERO &&
                     a1[0] == S_COMB && a1[1] == S_ZERO && a1[2] == S_SHADE && a1[3] == S_ZERO;
    if (sGen.t0 && (ts0 || ts1)) {
        sGen.t1 = false;
        sGen.fast = 1; sGen.fast_ta = ts0 && !ts0a;
        for (int ch = 0; ch < 4; ch++) { sGen.k1[ch] = 0; sGen.k2[ch] = 256; sGen.k3s[ch] = 0; }
        return;
    }
    if (sGen.t0 && !sGen.t1 && r0[0] == S_T0 && r0[1] == S_ZERO && r0[2] == S_SHADE && r0[3] == S_ZERO &&
        r1[0] == S_COMB && gen_const(r1[1]) && gen_const(r1[2]) && gen_const(r1[3]) && pass_a) {
        if (a0[0] == S_T0 && a0[1] == S_ZERO && a0[2] == S_SHADE && a0[3] == S_ZERO) sGen.fast_ta = false;
        else if (gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T0) sGen.fast_ta = true;
        else return;
        sGen.fast = 1;
    } else if (sGen.t0 && sGen.t1 && r0[0] == S_T0 && r0[1] == S_ZERO && r0[2] == S_T1 && r0[3] == S_ZERO &&
               gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T1 &&
               gen_const(r1[0]) && gen_const(r1[1]) && r1[2] == S_COMB && gen_const(r1[3]) && pass_a) {
        sGen.fast = 2;
    } else if (sGen.t0 && sGen.t1 && gen_const(r1[0]) && gen_const(r1[1]) && r1[2] == S_T0 && gen_const(r1[3]) &&
               a0[0] == S_T0 && a0[1] == S_ZERO && a0[2] == S_SHADE && a0[3] == S_ZERO &&
               a1[0] == S_COMB && a1[1] == S_T0 && gen_const(a1[2]) && a1[3] == S_ZERO) {
        sGen.fast = 3;
    } else if (sGen.t0 && sGen.t1 && r0[1] == S_ZERO && r0[3] == S_ZERO && a0[1] == S_ZERO && a0[3] == S_ZERO &&
               ((r0[0] == S_T1 && r0[2] == S_T0) || (r0[0] == S_T0 && r0[2] == S_T1)) &&
               ((a0[0] == S_T1 && a0[2] == S_T0) || (a0[0] == S_T0 && a0[2] == S_T1)) &&
               r1[0] == S_COMB && r1[1] == S_ZERO && r1[2] == S_SHADE && r1[3] == S_ZERO &&
               a1[0] == S_COMB && a1[1] == S_ZERO && a1[2] == S_SHADE && a1[3] == S_ZERO) {
        sGen.fast = 4; /* no constants */
        return;
    } else if (sGen.t0 && sGen.t1 && gen_const(r0[0]) && gen_const(r0[1]) && r0[2] == S_SHADE && gen_const(r0[3]) &&
               a0[0] == S_T1 && a0[1] == S_T0 && gen_const(a0[2]) && a0[3] == S_T0 &&
               gen_zero_product(r1[0], r1[1], r1[2]) && r1[3] == S_COMB &&
               a1[0] == S_COMB && a1[1] == S_ZERO && gen_const(a1[2]) && a1[3] == S_ZERO) {
        sGen.fast = 8;
        for (int ch = 0; ch < 3; ch++) { /* the FIRST cycle's colour constants */
            const int *const *o = (const int *const *) sGenOp[0][ch];
            sGen.k1[ch] = *o[0]; sGen.k2[ch] = *o[1]; sGen.k3s[ch] = *o[3] << 8;
        }
        sGen.k1[3] = *((const int *const *) sGenOp[0][3])[2]; /* Ka */
        sGen.k2[3] = *((const int *const *) sGenOp[1][3])[2]; /* Kb */
        return;
    } else if (sGen.t0 && sGen.t1 && r0[0] == S_T0 && r0[1] == S_ZERO && r0[2] == S_T1 && r0[3] == S_ZERO &&
               gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T1 &&
               r1[0] == S_SHADE && gen_const(r1[1]) && r1[2] == S_COMB && r1[3] == S_COMB && pass_a) {
        sGen.fast = 10;
        for (int ch = 0; ch < 3; ch++) sGen.k1[ch] = *((const int *const *) sGenOp[1][ch])[1];
        return;
    } else if (sGen.t0 && sGen.t1 && gen_const(r0[0]) && gen_const(r0[1]) && r0[2] == S_T0 && gen_const(r0[3]) &&
               gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T1 &&
               r1[0] == S_COMB && r1[1] == S_ZERO && r1[2] == S_T0 && r1[3] == S_ZERO && pass_a) {
        sGen.fast = 12;
        for (int ch = 0; ch < 3; ch++) {
            const int *const *o = (const int *const *) sGenOp[0][ch];
            sGen.k1[ch] = *o[0]; sGen.k2[ch] = *o[1]; sGen.k3s[ch] = *o[3] << 8;
        }
        return;
    } else if (!sGen.t0 && sGen.t1 && gen_zero_product(a0[0], a0[1], a0[2]) && a0[3] == S_T1 &&
               gen_zero_product(r1[0], r1[1], r1[2]) && r1[3] == S_COMB && pass_a &&
               ((gen_zero_product(r0[0], r0[1], r0[2]) && r0[3] == S_T1) ||
                (gen_const(r0[0]) && gen_const(r0[1]) && r0[2] == S_T1 && gen_const(r0[3])))) {
        sGen.fast = r0[3] == S_T1 ? 6 : 5;
        for (int ch = 0; ch < 3; ch++) { /* 5: the FIRST cycle's constants */
            const int *const *o = (const int *const *) sGenOp[0][ch];
            sGen.k1[ch] = *o[0]; sGen.k2[ch] = *o[1]; sGen.k3s[ch] = *o[3] << 8;
        }
        return;
    } else return;
    for (int ch = 0; ch < 4; ch++) { /* the second cycle's constants, as pm_gen_eval would read them */
        const int *const *o = (const int *const *) sGenOp[1][ch];
        const int8_t *sel = ch < 3 ? r1 : a1;
        sGen.k1[ch] = gen_const(sel[0]) ? *o[0] : 0;
        sGen.k2[ch] = gen_const(sel[1]) ? *o[1] : 0;
        sGen.k3s[ch] = gen_const(sel[3]) ? *o[3] << 8 : 0;
        if (sGen.fast == 3 && ch == 3) sGen.k1[3] = *o[2]; /* the alpha's C, a constant */
    }
    if (sGen.fast == 1) for (int ch = 0; ch < 3; ch++) { /* 1's second cycle is (COMBINED - K1) x K2 + K3 */
        const int *const *o = (const int *const *) sGenOp[1][ch];
        sGen.k1[ch] = *o[1]; sGen.k2[ch] = *o[2];
    }
}
/* The fast paths' pixel, from the texels (ARGB) and the shade; returns ARGB like pm_gen_eval. */
static inline __attribute__((always_inline)) uint32_t pm_gen_fast(int fast, uint32_t t0, uint32_t t1, int sr, int sg, int sb, int sa) {
    int r, g, b, a;
    if (fast == 1) {
        const int c0r = ((int) ((t0 >> 16) & 255) * sr + 0x80) >> 8, c0g = ((int) ((t0 >> 8) & 255) * sg + 0x80) >> 8;
        const int c0b = ((int) (t0 & 255) * sb + 0x80) >> 8;
        a = sGen.fast_ta ? (int) (t0 >> 24) : ((int) (t0 >> 24) * sa + 0x80) >> 8;
        r = gm_clamp(((c0r - sGen.k1[0]) * sGen.k2[0] + sGen.k3s[0] + 0x80) >> 8);
        g = gm_clamp(((c0g - sGen.k1[1]) * sGen.k2[1] + sGen.k3s[1] + 0x80) >> 8);
        b = gm_clamp(((c0b - sGen.k1[2]) * sGen.k2[2] + sGen.k3s[2] + 0x80) >> 8);
    } else if (fast == 2) {
        const int c0r = ((int) ((t0 >> 16) & 255) * (int) ((t1 >> 16) & 255) + 0x80) >> 8;
        const int c0g = ((int) ((t0 >> 8) & 255) * (int) ((t1 >> 8) & 255) + 0x80) >> 8;
        const int c0b = ((int) (t0 & 255) * (int) (t1 & 255) + 0x80) >> 8;
        a = (int) (t1 >> 24);
        r = gm_clamp(((sGen.k1[0] - sGen.k2[0]) * c0r + sGen.k3s[0] + 0x80) >> 8);
        g = gm_clamp(((sGen.k1[1] - sGen.k2[1]) * c0g + sGen.k3s[1] + 0x80) >> 8);
        b = gm_clamp(((sGen.k1[2] - sGen.k2[2]) * c0b + sGen.k3s[2] + 0x80) >> 8);
    } else if (fast == 7) { /* 1-cycle (K - SHADE) x T0 + SHADE, alpha T0a */
        a = (int) (t0 >> 24);
        r = gm_clamp(((sGen.k1[0] - sr) * (int) ((t0 >> 16) & 255) + (sr << 8) + 0x80) >> 8);
        g = gm_clamp(((sGen.k1[1] - sg) * (int) ((t0 >> 8) & 255) + (sg << 8) + 0x80) >> 8);
        b = gm_clamp(((sGen.k1[2] - sb) * (int) (t0 & 255) + (sb << 8) + 0x80) >> 8);
    } else if (fast == 8) { /* (K1 - K2) x SHADE + K3, alpha ((T1a - T0a) x Ka + T0a) x Kb */
        const int t0a = (int) (t0 >> 24), t1a = fire_cheap() ? t0a : (int) (t1 >> 24);
        const int a0 = gm_clamp(((t1a - t0a) * sGen.k1[3] + (t0a << 8) + 0x80) >> 8);
        a = gm_clamp((a0 * sGen.k2[3] + 0x80) >> 8);
        r = gm_clamp(((sGen.k1[0] - sGen.k2[0]) * sr + sGen.k3s[0] + 0x80) >> 8);
        g = gm_clamp(((sGen.k1[1] - sGen.k2[1]) * sg + sGen.k3s[1] + 0x80) >> 8);
        b = gm_clamp(((sGen.k1[2] - sGen.k2[2]) * sb + sGen.k3s[2] + 0x80) >> 8);
    } else if (fast == 11) { /* 1-cycle (K - T0) x Ka + T0, alpha a constant */
        a = sGen.k1[3];
        r = gm_clamp(((sGen.k1[0] - (int) ((t0 >> 16) & 255)) * sGen.k2[0] + ((int) ((t0 >> 16) & 255) << 8) + 0x80) >> 8);
        g = gm_clamp(((sGen.k1[1] - (int) ((t0 >> 8) & 255)) * sGen.k2[1] + ((int) ((t0 >> 8) & 255) << 8) + 0x80) >> 8);
        b = gm_clamp(((sGen.k1[2] - (int) (t0 & 255)) * sGen.k2[2] + ((int) (t0 & 255) << 8) + 0x80) >> 8);
    } else if (fast == 12) { /* C0 = (K1 - K2) x T0 + K3, then C0 x t1 (cycle 2's TEXEL0); alpha T1a */
        a = (int) (t1 >> 24);
        const int c0r = gm_clamp(((sGen.k1[0] - sGen.k2[0]) * (int) ((t0 >> 16) & 255) + sGen.k3s[0] + 0x80) >> 8);
        const int c0g = gm_clamp(((sGen.k1[1] - sGen.k2[1]) * (int) ((t0 >> 8) & 255) + sGen.k3s[1] + 0x80) >> 8);
        const int c0b = gm_clamp(((sGen.k1[2] - sGen.k2[2]) * (int) (t0 & 255) + sGen.k3s[2] + 0x80) >> 8);
        r = (c0r * (int) ((t1 >> 16) & 255) + 0x80) >> 8;
        g = (c0g * (int) ((t1 >> 8) & 255) + 0x80) >> 8;
        b = (c0b * (int) (t1 & 255) + 0x80) >> 8;
    } else if (fast == 10) { /* C0 = T0 x T1, then (SHADE - K) x C0 + C0; alpha T1a */
        const int c0r = ((int) ((t0 >> 16) & 255) * (int) ((t1 >> 16) & 255) + 0x80) >> 8;
        const int c0g = ((int) ((t0 >> 8) & 255) * (int) ((t1 >> 8) & 255) + 0x80) >> 8;
        const int c0b = ((int) (t0 & 255) * (int) (t1 & 255) + 0x80) >> 8;
        a = (int) (t1 >> 24);
        r = gm_clamp(((sr - sGen.k1[0]) * c0r + (c0r << 8) + 0x80) >> 8);
        g = gm_clamp(((sg - sGen.k1[1]) * c0g + (c0g << 8) + 0x80) >> 8);
        b = gm_clamp(((sb - sGen.k1[2]) * c0b + (c0b << 8) + 0x80) >> 8);
    } else if (fast == 6) { /* TEXEL1 as it is */
        a = (int) (t1 >> 24); r = (int) ((t1 >> 16) & 255); g = (int) ((t1 >> 8) & 255); b = (int) (t1 & 255);
    } else if (fast == 5) { /* (K1 - K2) x T1 + K3, alpha T1a */
        a = (int) (t1 >> 24);
        r = gm_clamp(((sGen.k1[0] - sGen.k2[0]) * (int) ((t1 >> 16) & 255) + sGen.k3s[0] + 0x80) >> 8);
        g = gm_clamp(((sGen.k1[1] - sGen.k2[1]) * (int) ((t1 >> 8) & 255) + sGen.k3s[1] + 0x80) >> 8);
        b = gm_clamp(((sGen.k1[2] - sGen.k2[2]) * (int) (t1 & 255) + sGen.k3s[2] + 0x80) >> 8);
    } else if (fast == 4) { /* (T1 x T0) x SHADE, each step as pm_gen_eval rounds it; never past 254, so no clamp */
        const int c0r = ((int) ((t1 >> 16) & 255) * (int) ((t0 >> 16) & 255) + 0x80) >> 8;
        const int c0g = ((int) ((t1 >> 8) & 255) * (int) ((t0 >> 8) & 255) + 0x80) >> 8;
        const int c0b = ((int) (t1 & 255) * (int) (t0 & 255) + 0x80) >> 8;
        const int c0a = ((int) (t1 >> 24) * (int) (t0 >> 24) + 0x80) >> 8;
        r = (c0r * sr + 0x80) >> 8; g = (c0g * sg + 0x80) >> 8; b = (c0b * sb + 0x80) >> 8; a = (c0a * sa + 0x80) >> 8;
    } else { /* 3: cycle 2's TEXEL0 is the tile+1 texel, t1 */
        const int c0a = ((int) (t0 >> 24) * sa + 0x80) >> 8;
        a = gm_clamp(((c0a - (int) (t1 >> 24)) * sGen.k1[3] + 0x80) >> 8);
        r = gm_clamp(((sGen.k1[0] - sGen.k2[0]) * (int) ((t1 >> 16) & 255) + sGen.k3s[0] + 0x80) >> 8);
        g = gm_clamp(((sGen.k1[1] - sGen.k2[1]) * (int) ((t1 >> 8) & 255) + sGen.k3s[1] + 0x80) >> 8);
        b = gm_clamp(((sGen.k1[2] - sGen.k2[2]) * (int) (t1 & 255) + sGen.k3s[2] + 0x80) >> 8);
    }
    if (sGen.shroud) { /* as pm_gen_eval: the blender's first cycle, on the colour */
        const int f = sGen.shroud, gg = 32 - f;
        r = (sGen.sh[0] * f + r * gg) >> 5; g = (sGen.sh[1] * f + g * gg) >> 5; b = (sGen.sh[2] * f + b * gg) >> 5;
    }
    return (uint32_t) a << 24 | (uint32_t) r << 16 | (uint32_t) g << 8 | (uint32_t) b;
}
/* t0/t1 ARGB texels, s* the vertex colour; returns ARGB. */
static inline uint32_t pm_gen_eval(uint32_t t0, uint32_t t1, int sr, int sg, int sb, int sa) {
    sGenPix[0][0] = (t0 >> 16) & 255; sGenPix[0][1] = (t0 >> 8) & 255; sGenPix[0][2] = t0 & 255; sGenPix[0][3] = t0 >> 24;
    sGenPix[1][0] = (t1 >> 16) & 255; sGenPix[1][1] = (t1 >> 8) & 255; sGenPix[1][2] = t1 & 255; sGenPix[1][3] = t1 >> 24;
    sGenPix[2][0] = sr; sGenPix[2][1] = sg; sGenPix[2][2] = sb; sGenPix[2][3] = sa;
    sGenPix[3][0] = sGenPix[3][1] = sGenPix[3][2] = sGenPix[3][3] = 0;
    const int nk = sGen.two ? 2 : 1;
    if (sCc9) { /* PM64_CC9BIT: the RDP's 9-bit combiner (see cc_ext9) */
        for (int k = 0; k < nk; k++) {
            int out[4];
            for (int ch = 0; ch < 4; ch++) {
                const int *const *o = sGenOp[k][ch];
                out[ch] = (((cc_ext9(*o[0]) - cc_ext9(*o[1])) * cc_sext9(*o[2]) + (cc_ext9(*o[3]) << 8) + 0x80) >> 8) & 0x1ff;
            }
            sGenPix[3][0] = out[0]; sGenPix[3][1] = out[1]; sGenPix[3][2] = out[2]; sGenPix[3][3] = out[3];
        }
        for (int ch = 0; ch < 4; ch++) sGenPix[3][ch] = cc_clamp9(sGenPix[3][ch]);
    } else
    for (int k = 0; k < nk; k++) {
        int out[4];
        for (int ch = 0; ch < 4; ch++) {
            const int *const *o = sGenOp[k][ch];
            out[ch] = gm_clamp(((*o[0] - *o[1]) * *o[2] + (*o[3] << 8) + 0x80) >> 8);
        }
        sGenPix[3][0] = out[0]; sGenPix[3][1] = out[1]; sGenPix[3][2] = out[2]; sGenPix[3][3] = out[3];
    }
    if (sGen.shroud) { /* the RDP's first blender cycle: (P * a + M * (b + 1)) >> 5, b = ~a in 5 bits (angrylion) */
        const int f = sGen.shroud, g = 32 - f;
        for (int ch = 0; ch < 3; ch++) sGenPix[3][ch] = (sGen.sh[ch] * f + sGenPix[3][ch] * g) >> 5;
    }
    return (uint32_t)sGenPix[3][3] << 24 | (uint32_t)sGenPix[3][0] << 16 | (uint32_t)sGenPix[3][1] << 8 | (uint32_t)sGenPix[3][2];
}
#endif
#define PERSP_SPAN 16
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
/* The world's hot rows on NEON: z-tested texture x shade, bilinear or nearest,
 * opaque or alpha (test + blend), flat or smooth -- SPAN_LOOP_CM(1, 1, *, *,
 * *, 1) below, with the per-span setup of raster_tri's span loop. One call a
 * row: per span, the same float steps as raster_tri (scalar VFP, same order)
 * and one perspective divide; the fixed-point conversions run in NEON lanes
 * (to_fix16's clamp = vmax/vmin, then vcvt #16 = (int32_t)(f * 65536); z's
 * clamp and scale likewise; NaN and denormal inputs come out 0 either way).
 * Per pixel, texel addressing and the z test stay scalar and identical to
 * bilerp / FETCH_NEAREST; the two lerps, the shade clamp, the modulate and the
 * blend run in NEON lanes (B G R A) with the same integer arithmetic:
 *   lerp    (x*(256-f) + y*f) >> 8   in u16 (<= 255*256)
 *   shade   usat(q >> 16, 8)         vshrq 16, vqmovun_s32, vmin 255
 *   modulate div255(t*s) = (x + 1 + (x >> 8)) >> 8, x <= 255*255
 * so it is bit-identical (ocarina-of-time c935b73; checked here by the m1 ARM/qemu sweep, PM64_CMNEON=0 vs 1). In raster_tri
 * the span setup spilled to the stack, paid an FPSCR transfer per clamp and
 * re-converted the texture size every span, and each span paid a 76-instruction
 * call; flat triangles come in with zero colour steps. PM64_CMNEON=0 keeps the
 * scalar loops. */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16; /* texture */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx;  /* triangle x-steps */
    float ftw, fth;                                       /* (float)tw, (float)th */
    int32x4_t dacc;                                       /* B G R A 16.16 x-steps */
    int32_t zdq; int zbias, amode, amin, ablend; /* ablend 0: a texture edge, written unblended (X12) */
    bool persp, dmask, alpha, bilin, ztest; /* ztest 0: no depth test or write (X23) */
    int mode; /* 0: texture x shade; 1, 4: the generic combiner's fast paths */
    /* the generic combiner's fast paths 1 and 4 (the speed round): 1's cycle-2 constants (B G R A; A passes), 4's
     * TEXEL1 texture and its (x + a) * 2^k + c from TEXEL0's S/T (gen_t1) */
    int32x4_t gk1, gk2, gk3; bool gta;
    bool shr; uint16x4_t shk, shg; /* the shroud after a fast path's combine: (K x f + C x (32 - f)) >> 5, A kept */
    const void *px1; int w1, h1, ws1, wt1, mu1, mv1; bool t16_1;
    int zsk; /* PM64_ZSKIP: > 0 while the triangle's spans are tested (CM_ROW); the rows keep it between them */
} CmTri;
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* The pixel loop of one span: Tx (the texel, B G R A in u16 lanes) from the fetch (...). */
#define CM_PIXELS(ALPHA, ZT, MODE, ...)                                                                  \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = ZT ? clamp16_sh8(zq) : 0;                                                        \
        if (ZT && uz > *pz + zbias) continue;                                                           \
        uint16x4_t Tx, Tx1 = vdup_n_u16(0); __VA_ARGS__                                                 \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M;                                                                                   \
        if (MODE == 0) M = cm_div255(vmul_u16(Tx, S));                                                  \
        else if (MODE == 1) { /* pm_gen_fast 1: C0 = (T0 x S + 0x80) >> 8 (alpha T0a if gta), then clamp(((C0 - K1) x K2 + K3 + 0x80) >> 8) */ \
            uint16x4_t C0 = vshr_n_u16(vadd_u16(vmul_u16(Tx, S), vdup_n_u16(0x80)), 8);                 \
            if (gta) C0 = vset_lane_u16(vget_lane_u16(Tx, 3), C0, 3);                                   \
            const int32x4_t v = vshrq_n_s32(vaddq_s32(vmulq_s32(vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(C0)), gk1), gk2), gk3), 8); \
            M = vmin_u16(vqmovun_s32(v), vdup_n_u16(255));                                              \
        } else if (MODE == 7) { /* pm_gen_fast 7: clamp(((K - S) x T + (S << 8) + 0x80) >> 8), alpha T0a */ \
            const int32x4_t s32 = vreinterpretq_s32_u32(vmovl_u16(S));                                  \
            const int32x4_t v = vshrq_n_s32(vmlaq_s32(vaddq_s32(vshlq_n_s32(s32, 8), vdupq_n_s32(0x80)), \
                                                      vsubq_s32(gk1, s32), vreinterpretq_s32_u32(vmovl_u16(Tx))), 8); \
            M = vset_lane_u16(vget_lane_u16(Tx, 3), vmin_u16(vqmovun_s32(v), vdup_n_u16(255)), 3);      \
        } else if (MODE == 10) { /* pm_gen_fast 10: C0 = (T0 x T1 + 0x80) >> 8, then clamp(((S - K) x C0 + (C0 << 8) + 0x80) >> 8), alpha T1a */ \
            const int32x4_t c0 = vreinterpretq_s32_u32(vmovl_u16(vshr_n_u16(vadd_u16(vmul_u16(Tx, Tx1), vdup_n_u16(0x80)), 8))); \
            const int32x4_t v = vshrq_n_s32(vmlaq_s32(vaddq_s32(vshlq_n_s32(c0, 8), vdupq_n_s32(0x80)),  \
                                                      vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(S)), gk1), c0), 8); \
            M = vset_lane_u16(vget_lane_u16(Tx1, 3), vmin_u16(vqmovun_s32(v), vdup_n_u16(255)), 3);     \
        } else if (MODE == 8) { /* pm_gen_fast 8: clamp((K12 x S + K3 + 0x80) >> 8), alpha ((T1a - T0a) x Ka + T0a) x Kb */ \
            const int32x4_t v = vshrq_n_s32(vmlaq_s32(gk3, gk1, vreinterpretq_s32_u32(vmovl_u16(S))), 8);    \
            const int t0a = vget_lane_u16(Tx, 3);                                                        \
            const int a0 = ((vget_lane_u16(Tx1, 3) - t0a) * vgetq_lane_s32(gk2, 0) + (t0a << 8) + 0x80) >> 8; \
            M = vset_lane_u16((uint16_t) ((a0 * vgetq_lane_s32(gk2, 1) + 0x80) >> 8), vmin_u16(vqmovun_s32(v), vdup_n_u16(255)), 3); \
        } else { /* pm_gen_fast 4: ((T1 x T0 + 0x80) >> 8 x S + 0x80) >> 8 */                          \
            const uint16x4_t C0 = vshr_n_u16(vadd_u16(vmul_u16(Tx1, Tx), vdup_n_u16(0x80)), 8);         \
            M = vshr_n_u16(vadd_u16(vmul_u16(C0, S), vdup_n_u16(0x80)), 8);                             \
        }                                                                                               \
        if (MODE != 0 && shr) M = vshr_n_u16(vmla_u16(shk, M, shg), 5);                                 \
        if (ALPHA) { /* fa: modulated (0), texel (1) or shade (2) alpha -- the scalar fa */               \
            const int fa = amode == 0 ? vget_lane_u16(M, 3) : amode == 1 ? vget_lane_u16(Tx, 3)          \
                                                                           : vget_lane_u16(S, 3);           \
            if (fa < amin) continue;                                                                    \
            if (fa < 255 && ablend) { /* div255(f*fa + d*(255-fa)), every term <= 255*255 */              \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));         \
            }                                                                                           \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (ZT && dmask) *pz = (uint16_t)uz;                                                            \
    }
/* bilerp: four taps around (u - 0.5, v - 0.5), lerped along u then v */
#define CM_BILIN(TAP, WU, WV) CM_BILIN_AT(TAP, WU, WV, uf, vf, tw, Tx)
#define CM_BILIN_AT(TAP, WU, WV, UF, VF, TW, OUT)                                                       \
    {   const int32_t su = (UF) - 0x8000, sv = (VF) - 0x8000;                                           \
        const int iu0 = WU(su >> 16), iu1 = WU((su >> 16) + 1);                                         \
        const int b0 = WV(sv >> 16) * (TW), b1 = WV((sv >> 16) + 1) * (TW);                             \
        const uint32_t a = TAP(b0 + iu0), b = TAP(b0 + iu1), c = TAP(b1 + iu0), d = TAP(b1 + iu1);      \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));       \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));       \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        OUT = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2), vget_high_u16(H), wts, 3), 8); }
/* FETCH_NEAREST */
#define CM_NEAR(TAP, WU, WV) CM_NEAR_AT(TAP, WU, WV, uf, vf, tw, Tx)
#define CM_NEAR_AT(TAP, WU, WV, UF, VF, TW, OUT)                                                        \
    {   const uint32_t tt = TAP(WV((VF) >> 16) * (TW) + WU((UF) >> 16));                               \
        OUT = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(tt)))); }
#define CM_TAP32(i) px32[i]
#define CM_TAP16(i) texel_at(px, true, (i))
#define CM_WRAP_RRU(x) ((x) & mu)
#define CM_WRAP_RRV(x) ((x) & mv)
#define CM_WRAP_GU(x) wrapf((x), tw, ws, mu)
#define CM_WRAP_GV(x) wrapf((x), th, wt, mv)
/* fast path 4's two taps: TEXEL0 (any storage) and TEXEL1 at gen_t1's coordinates */
#define CM_TAPA(i) texel_at(px, t16, (i))
#define CM_TAPB(i) texel_at(px1, t16_1, (i))
#define CM_WRAP_1U(x) wrapf((x), tw1, ws1, mu1)
#define CM_WRAP_1V(x) wrapf((x), th1, wt1, mv1)
#define CM_G4_BILIN CM_BILIN_AT(CM_TAPA, CM_WRAP_GU, CM_WRAP_GV, uf, vf, tw, Tx) \
                    CM_BILIN_AT(CM_TAPB, CM_WRAP_1U, CM_WRAP_1V, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), tw1, Tx1)
#define CM_G4_NEAR CM_NEAR_AT(CM_TAPA, CM_WRAP_GU, CM_WRAP_GV, uf, vf, tw, Tx) \
                   CM_NEAR_AT(CM_TAPB, CM_WRAP_1U, CM_WRAP_1V, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), tw1, Tx1)
/* both textures repeating at a power of two and stored 32-bit: masks and plain loads, as the rows' rr variants */
#define CM_TAPA32(i) px32[i]
#define CM_TAPB32(i) ((const uint32_t *) px1)[i]
#define CM_WRAP_1RU(x) ((x) & mu1)
#define CM_WRAP_1RV(x) ((x) & mv1)
#define CM_G4_BILIN_RR CM_BILIN_AT(CM_TAPA32, CM_WRAP_RRU, CM_WRAP_RRV, uf, vf, tw, Tx) \
                       CM_BILIN_AT(CM_TAPB32, CM_WRAP_1RU, CM_WRAP_1RV, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), tw1, Tx1)
#define CM_G4_NEAR_RR CM_NEAR_AT(CM_TAPA32, CM_WRAP_RRU, CM_WRAP_RRV, uf, vf, tw, Tx) \
                      CM_NEAR_AT(CM_TAPB32, CM_WRAP_1RU, CM_WRAP_1RV, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), tw1, Tx1)
/* fast path 8's tap with the bilinear on the texels' alphas only (the fire breath: its colour takes no texel), a
 * texture repeating at a power of two, 32-bit: the lane arithmetic of CM_BILIN_AT on one lane, h = (x (256 - f) +
 * y f) >> 8 across, then down (cm_row_f8) */
#define CM_F8A_TAP(PX, UF, VF, TW, MU, MV, OUT)                                                         \
    {   const int32_t su = (UF) - 0x8000, sv = (VF) - 0x8000;                                           \
        const int iu0 = (su >> 16) & (MU), iu1 = ((su >> 16) + 1) & (MU);                               \
        const int r0 = ((sv >> 16) & (MV)) * (TW), r1 = (((sv >> 16) + 1) & (MV)) * (TW);               \
        const int fu = (su >> 8) & 0xff, fv = (sv >> 8) & 0xff;                                         \
        const int h0 = ((int) (PX[r0 + iu0] >> 24) * (256 - fu) + (int) (PX[r0 + iu1] >> 24) * fu) >> 8;  \
        const int h1 = ((int) (PX[r1 + iu0] >> 24) * (256 - fu) + (int) (PX[r1 + iu1] >> 24) * fu) >> 8;  \
        OUT = (h0 * (256 - fv) + h1 * fv) >> 8; }
/* One row: raster_tri's span loop for this path, float for float (fetch = ...).
 * PM64_ZSKIP (2026-10-07, branch speed-2026-10-07, from super-mario-64's SM64_ZSKIP; default 1): a depth-tested span whose
 * nearest depth (the per-pixel depth is linear along the span, so at one of its ends; checked to stay in int32, so it
 * never wraps) is further than the farthest depth stored under it plus the decal window fails CM_PIXELS' depth test at
 * every pixel, which then writes nothing: the span is skipped whole (no texel setup, no pixels), its float state and
 * 1/w stepped as after it. A sufficient test, never a guess. A triangle stops testing after four spans in front running
 * (T->zsk, 0 with PM64_ZSKIP=0; speed only). Same pixels. */
#define CM_ROW(ALPHA, ZT, MODE, ...)                                                                     \
    {   int zsk = ZT ? T->zsk : 0;                                                                      \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
        const float32x2_t zc = vmin_f32(vmax_f32(vdup_n_f32(zf), vdup_n_f32(-8.0f)), vdup_n_f32(2.0f));   \
        int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);                  \
        if (span_z_out(zf)) zq = span_zq_far(zf, zdx, span, zdq); /* PM64_NEARW (span_zq_far) */         \
        if (ZT && zsk > 0 && span >= 4) { /* PM64_ZSKIP */                                              \
            const int64_t zl = (int64_t)zq + (int64_t)(span - 1) * zdq; /* the last pixel's */          \
            uint16x4_t m4;                                                                              \
            if (span >= 8) { const uint16x8_t m8 = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8));  \
                             m4 = vmax_u16(vget_low_u16(m8), vget_high_u16(m8)); }                      \
            else m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));                                  \
            m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);                                             \
            const int u0 = clamp16_sh8(zq), u1 = clamp16_sh8((int32_t)zl);                              \
            if (zl >= INT32_MIN && zl <= INT32_MAX && (u0 < u1 ? u0 : u1) > (int)vget_lane_u16(m4, 0) + zbias) { \
                zsk = 4;                                                                                \
                zf += zdx * fspan;                                                                      \
                cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;         \
                uw += uwdx * fspan;                                                                     \
                vw += vwdx * fspan;                                                                     \
                sw = sw_b; rw = persp ? ((sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f) : 0.0f;                  \
                fb += span; zp += span; n -= span;                                                      \
                continue;                                                                               \
            }                                                                                           \
            zsk--;                                                                                      \
        }                                                                                               \
        float rw_b = 0.0f, u_a, v_a, du, dv;                                                            \
        if (persp) {                                                                                    \
            rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;                                                \
            u_a = uw * rw; v_a = vw * rw;                                                               \
            const float nu = (uw + uwdx * fspan) * rw_b - u_a;                                          \
            const float nv = (vw + vwdx * fspan) * rw_b - v_a;                                          \
            if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); }    \
            else                    { du = nu / fspan;               dv = nv / fspan; }                 \
        } else {                                                                                        \
            u_a = uw; v_a = vw; du = uwdx; dv = vwdx;                                                   \
        }                                                                                               \
        const float32x4_t fq = { u_a, du, v_a, dv };                                                    \
        const int32x4_t tq = vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vmulq_f32(fq, tsz), vdupq_n_f32(-16000.0f)), \
                                                       vdupq_n_f32(16000.0f)), 16);                     \
        int32_t uf = vgetq_lane_s32(tq, 0), duf = vgetq_lane_s32(tq, 1);                                \
        int32_t vf = vgetq_lane_s32(tq, 2), dvf = vgetq_lane_s32(tq, 3);                                \
        const float32x4_t cq = { cb, cg, cr, ca };                                                      \
        int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                        \
        uint32_t *const end = fb + span;                                                                \
        uint16_t *pz = zp;                                                                              \
        CM_PIXELS(ALPHA, ZT, MODE, __VA_ARGS__)                                                         \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }                                                                                                   \
    if (ZT) ((CmTri *)T)->zsk = zsk; }
static int sCmNeon SAVESTATE_EXCLUDE = 1; /* PM64_CMNEON=0: the scalar loops, for A/B */
static int sZSkip SAVESTATE_EXCLUDE = 1;   /* PM64_ZSKIP (CM_ROW) */
static int sFastRows SAVESTATE_EXCLUDE = 1; /* PM64_FASTROWS (tri_rows) */
static int sF8A SAVESTATE_EXCLUDE = 1;    /* PM64_F8A=0: fast path 8's four-channel row instead of cm_row_f8 */
/* Fast path 8's row on its own (the fire breath, 4 screens a frame): inside cm_row_neon's many variants the
 * compiler kept this loop's values on the stack (~180 instructions a pixel); alone, with the bilinear on the two
 * alphas only (the colour takes no texel), the same integer steps as CM_PIXELS mode 8: the alpha
 * ((T1a - T0a) x Ka + T0a) x Kb, the colour clamp((K12 x S + K3) >> 8), the shroud, the blend. Both textures
 * repeating at a power of two and 32-bit, bilinear. */
/* the nearest tap (CM_NEAR_AT on one lane) */
#define CM_F8N_TAP(PX, UF, VF, TW, MU, MV, OUT) { OUT = (int) (PX[(((VF) >> 16) & (MV)) * (TW) + (((UF) >> 16) & (MU))] >> 24); }
#define CM_F8_PIXELS(ALPHA, ZT, TAP)                                                                    \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = ZT ? clamp16_sh8(zq) : 0;                                                        \
        if (ZT && uz > *pz + zbias) continue;                                                           \
        int ta0, ta1;                                                                                   \
        TAP(px32, uf, vf, tw, mu, mv, ta0)                                                              \
        if (f8one) ta1 = ta0;                                                                           \
        else TAP(px1_32, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), tw1, mu1, mv1, ta1) \
        const int a0 = ((ta1 - ta0) * f8ka + (ta0 << 8) + 0x80) >> 8;                                   \
        const int fa = (a0 * f8kb + 0x80) >> 8;                                                         \
        if (ALPHA && fa < amin) continue;                                                               \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M = vmin_u16(vqmovun_s32(vshrq_n_s32(vmlaq_s32(gk3, gk1, vreinterpretq_s32_u32(vmovl_u16(S))), 8)), \
                                vdup_n_u16(255));                                                       \
        if (shr) M = vshr_n_u16(vmla_u16(shk, M, shg), 5);                                              \
        if (ALPHA && fa < 255 && ablend) {                                                              \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));         \
            M = cm_div255(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t) fa)), D, vdup_n_u16((uint16_t) (255 - fa)))); \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (ZT && dmask) *pz = (uint16_t)uz;                                                            \
    }
#pragma push_macro("CM_PIXELS")
#undef CM_PIXELS
#define CM_PIXELS(ALPHA, ZT, MODE, ...) CM_F8_PIXELS(ALPHA, ZT, MODE)
#define CM_F8_FN(NAME, ALPHA, ZT, TAP)                                                                   \
static void __attribute__((noinline)) NAME(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float zf, float cr, \
                                           float cg, float cb, float ca, float uw, float vw, float sw, float rw) { \
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);                                         \
    const uint32_t *const px32 = (const uint32_t *) T->px, *const px1_32 = (const uint32_t *) T->px1;   \
    const int tw = T->w, mu = T->mu, mv = T->mv, tw1 = T->w1, mu1 = T->mu1, mv1 = T->mv1;               \
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;                           \
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;                         \
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };                                         \
    const int32x4_t dacc = T->dacc, gk1 = T->gk1, gk3 = T->gk3;                                         \
    const int32_t zdq = T->zdq;                                                                         \
    const int zbias = T->zbias, amin = T->amin, ablend = T->ablend;                                     \
    const bool persp = T->persp, dmask = T->dmask, shr = T->shr;                                        \
    const uint16x4_t shk = T->shk, shg = T->shg;                                                        \
    const int32_t g1a0 = sGen.t1a[0], g1c0 = sGen.t1c[0], g1a1 = sGen.t1a[1], g1c1 = sGen.t1c[1];       \
    const int g1k0 = sGen.t1k[0], g1k1 = sGen.t1k[1];                                                   \
    const int f8ka = vgetq_lane_s32(T->gk2, 0), f8kb = vgetq_lane_s32(T->gk2, 1);                       \
    const bool f8one = fire_cheap();                                                                    \
    (void)zbias; (void)dmask; (void)amin; (void)ablend;                                                 \
    CM_ROW(ALPHA, ZT, TAP, )                                                                            \
}
CM_F8_FN(cm_row_f8_az, 1, 1, CM_F8A_TAP)
CM_F8_FN(cm_row_f8_a, 1, 0, CM_F8A_TAP)
CM_F8_FN(cm_row_f8_z, 0, 1, CM_F8A_TAP)
CM_F8_FN(cm_row_f8_, 0, 0, CM_F8A_TAP)
CM_F8_FN(cm_row_f8n_az, 1, 1, CM_F8N_TAP) /* PM64_FIRENEAREST */
CM_F8_FN(cm_row_f8n_a, 1, 0, CM_F8N_TAP)
CM_F8_FN(cm_row_f8n_z, 0, 1, CM_F8N_TAP)
CM_F8_FN(cm_row_f8n_, 0, 0, CM_F8N_TAP)
#undef CM_F8_FN
#undef CM_PIXELS
#pragma pop_macro("CM_PIXELS")
/* The rows in functions of their own (the final exact speed check, 2026-10-03). Inside cm_row_neon, with every mode's
 * variants in one function, the compiler kept the loop's constants (the masks, the texture's width and pointer, the
 * steps, dmask, the row's end) on the stack and built the bilinear's lane vectors through memory: plain texture x
 * shade took ~94 instructions a pixel without a depth test and ~126 with one. Here each variant is compiled alone
 * (as cm_row_f8 for the fire), with the depth write (dmask) a constant and the 32-bit taps loaded straight into the
 * lanes: the same macros and integer steps, so the same pixels. Mode 0 (texture x shade) and the generic combiner's
 * fast paths 1 and 7 (one texture), 32-bit, any wrap; fast paths 4 and 10 (two textures) when both repeat at a power
 * of two (rr4). The rest (16-bit textures, which Paper Mario never makes; 4 and 10 with other wraps; fast path 8, which
 * has cm_row_f8) stay in cm_row_neon. PM64_CMSPLIT=0: every row in cm_row_neon as before. */
static int sCmSplit SAVESTATE_EXCLUDE = 1;
/* CM_BILIN_AT on a 32-bit texture, the taps loaded straight into the lanes ((a, c) and (b, d), as its vcreate puts
 * them) instead of through core registers and the stack */
#define CM_BILIN32_AT(PX, TW, WU, WV, UF, VF, OUT)                                                      \
    {   const int32_t su = (UF) - 0x8000, sv = (VF) - 0x8000;                                           \
        const int iu0 = WU(su >> 16), iu1 = WU((su >> 16) + 1);                                         \
        const uint32_t *const r0 = (PX) + WV(sv >> 16) * (TW), *const r1 = (PX) + WV((sv >> 16) + 1) * (TW); \
        const uint32x2_t ac = vld1_lane_u32(r1 + iu0, vld1_dup_u32(r0 + iu0), 1);                       \
        const uint32x2_t bd = vld1_lane_u32(r1 + iu1, vld1_dup_u32(r0 + iu1), 1);                       \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(ac));                                         \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(bd));                                         \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        OUT = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2), vget_high_u16(H), wts, 3), 8); }
#define CM_NEAR32_AT(PX, TW, WU, WV, UF, VF, OUT) \
    { OUT = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32((PX) + WV((VF) >> 16) * (TW) + WU((UF) >> 16))))); }
#define CM_S_B(WU, WV) CM_BILIN32_AT(px32, tw, WU, WV, uf, vf, Tx)
#define CM_S_N(WU, WV) CM_NEAR32_AT(px32, tw, WU, WV, uf, vf, Tx)
/* fast paths 4 and 10: TEXEL1 at gen_t1's coordinates, both repeating at a power of two */
#define CM_S_B2(WU, WV) CM_BILIN32_AT(px32, tw, WU, WV, uf, vf, Tx) \
    CM_BILIN32_AT(px1_32, tw1, CM_WRAP_1RU, CM_WRAP_1RV, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), Tx1)
#define CM_S_N2(WU, WV) CM_NEAR32_AT(px32, tw, WU, WV, uf, vf, Tx) \
    CM_NEAR32_AT(px1_32, tw1, CM_WRAP_1RU, CM_WRAP_1RV, gen_t1_l(uf, g1a0, g1c0, g1k0), gen_t1_l(vf, g1a1, g1c1, g1k1), Tx1)
#define CM_S_FN(NAME, MODE, KIND, WU, WV, ALPHA, ZT, DM)                                                 \
static void __attribute__((noinline)) NAME(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float zf, float cr, \
                                           float cg, float cb, float ca, float uw, float vw, float sw, float rw) { \
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);                                         \
    const uint32_t *const px32 = (const uint32_t *) T->px, *const px1_32 = (const uint32_t *) T->px1;   \
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;                     \
    const int tw1 = T->w1, mu1 = T->mu1, mv1 = T->mv1;                                                  \
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;                           \
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;                         \
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };                                         \
    const int32x4_t dacc = T->dacc, gk1 = T->gk1, gk2 = T->gk2, gk3 = T->gk3;                           \
    const int32_t zdq = T->zdq;                                                                         \
    const int zbias = T->zbias, amode = T->amode, amin = T->amin, ablend = T->ablend;                   \
    const bool persp = T->persp, dmask = DM, gta = T->gta, shr = T->shr;                                \
    const uint16x4_t shk = T->shk, shg = T->shg;                                                        \
    const int32_t g1a0 = sGen.t1a[0], g1c0 = sGen.t1c[0], g1a1 = sGen.t1a[1], g1c1 = sGen.t1c[1];       \
    const int g1k0 = sGen.t1k[0], g1k1 = sGen.t1k[1];                                                   \
    (void)th; (void)ws; (void)wt; (void)mu; (void)mv; (void)zbias; (void)dmask; (void)amode; (void)amin; (void)ablend; \
    (void)gk1; (void)gk2; (void)gk3; (void)gta; (void)shr; (void)shk; (void)shg; (void)px1_32; (void)tw1; (void)mu1; (void)mv1; \
    (void)g1a0; (void)g1c0; (void)g1a1; (void)g1c1; (void)g1k0; (void)g1k1;                             \
    CM_ROW(ALPHA, ZT, MODE, KIND(WU, WV))                                                               \
}
/* the six depth/alpha variants: depth written, tested only, none; with and without alpha */
#define CM_S_SET(M, W, WU, WV, K, KIND)                                                                  \
    CM_S_FN(cm_row_m##M##_##W##_##K##_azd, M, KIND, WU, WV, 1, 1, 1)                                    \
    CM_S_FN(cm_row_m##M##_##W##_##K##_zd,  M, KIND, WU, WV, 0, 1, 1)                                    \
    CM_S_FN(cm_row_m##M##_##W##_##K##_az,  M, KIND, WU, WV, 1, 1, 0)                                    \
    CM_S_FN(cm_row_m##M##_##W##_##K##_z,   M, KIND, WU, WV, 0, 1, 0)                                    \
    CM_S_FN(cm_row_m##M##_##W##_##K##_a,   M, KIND, WU, WV, 1, 0, 0)                                    \
    CM_S_FN(cm_row_m##M##_##W##_##K##_,    M, KIND, WU, WV, 0, 0, 0)
#define CM_S_ROWS(M, W, K) cm_row_m##M##_##W##_##K##_azd, cm_row_m##M##_##W##_##K##_zd, cm_row_m##M##_##W##_##K##_az, \
                           cm_row_m##M##_##W##_##K##_z, cm_row_m##M##_##W##_##K##_a, cm_row_m##M##_##W##_##K##_,      \
                           cm_row_m##M##_##W##_##K##_a, cm_row_m##M##_##W##_##K##_
#define CM_S_ONE(M) CM_S_SET(M, rr, CM_WRAP_RRU, CM_WRAP_RRV, b, CM_S_B) CM_S_SET(M, rr, CM_WRAP_RRU, CM_WRAP_RRV, n, CM_S_N) \
                    CM_S_SET(M, g,  CM_WRAP_GU,  CM_WRAP_GV,  b, CM_S_B) CM_S_SET(M, g,  CM_WRAP_GU,  CM_WRAP_GV,  n, CM_S_N)
#define CM_S_TWO(M) CM_S_SET(M, rr, CM_WRAP_RRU, CM_WRAP_RRV, b, CM_S_B2) CM_S_SET(M, rr, CM_WRAP_RRU, CM_WRAP_RRV, n, CM_S_N2)
CM_S_ONE(0)
CM_S_ONE(1)
CM_S_ONE(7)
CM_S_TWO(4)
CM_S_TWO(10)
typedef void (*CmRowFn)(const CmTri *, uint32_t *, uint16_t *, int, float, float, float, float, float, float, float, float, float);
/* [mode][general wrap][nearest][depth: written, tested only, none][no alpha]; 4 and 10: repeat (rr4) only */
static const CmRowFn sCmRowS[5][32] = {
    { CM_S_ROWS(0, rr, b), CM_S_ROWS(0, rr, n), CM_S_ROWS(0, g, b), CM_S_ROWS(0, g, n) },
    { CM_S_ROWS(1, rr, b), CM_S_ROWS(1, rr, n), CM_S_ROWS(1, g, b), CM_S_ROWS(1, g, n) },
    { CM_S_ROWS(7, rr, b), CM_S_ROWS(7, rr, n), CM_S_ROWS(7, g, b), CM_S_ROWS(7, g, n) },
    { CM_S_ROWS(4, rr, b), CM_S_ROWS(4, rr, n) },
    { CM_S_ROWS(10, rr, b), CM_S_ROWS(10, rr, n) },
};
/* fast path 8's rows (cm_row_f8): [nearest][depth tested][alpha] */
static const CmRowFn sCmRowF8[2][2][2] = {
    { { cm_row_f8_, cm_row_f8_a }, { cm_row_f8_z, cm_row_f8_az } }, { { cm_row_f8n_, cm_row_f8n_a }, { cm_row_f8n_z, cm_row_f8n_az } },
};
/* The row function of a triangle's rows, as cm_row_neon picked it on every row: a row of its own (sCmRowS, the split
 * rows; cm_row_f8, fast path 8); NULL: cm_row_neon's own loops. */
static CmRowFn cm_row_own(const CmTri *T) {
    const bool rr = T->ws == WRAP_REPEAT && T->wt == WRAP_REPEAT && T->mu >= 0 && T->mv >= 0; /* wrapf = mask */
    const bool rr4 = rr && !T->t16 && !T->t16_1 && T->ws1 == WRAP_REPEAT && T->wt1 == WRAP_REPEAT && T->mu1 >= 0 && T->mv1 >= 0;
    if (sCmSplit && !T->t16) { /* the rows of their own (sCmRowS) */
        const int m = T->mode == 0 ? 0 : T->mode == 1 ? 1 : T->mode == 7 ? 2 : T->mode == 4 ? 3 : T->mode == 10 ? 4 : -1;
        if (m >= 0 && (m < 3 || rr4)) {
            const int depth = T->ztest ? (T->dmask ? 0 : 1) : 2;
            return sCmRowS[m][(!rr) << 4 | (!T->bilin) << 3 | depth << 1 | (!T->alpha)];
        }
    }
    if (T->mode == 8 && rr4 && sF8A) return sCmRowF8[!T->bilin][T->ztest][T->alpha];
    return NULL;
}
#undef CM_S_ONE
#undef CM_S_TWO
#undef CM_S_ROWS
#undef CM_S_SET
#undef CM_S_FN
#undef CM_S_B
#undef CM_S_N
#undef CM_S_B2
#undef CM_S_N2
#undef CM_BILIN32_AT
#undef CM_NEAR32_AT
static void __attribute__((noinline))
cm_row_neon(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin, ablend = T->ablend;
    const bool persp = T->persp, dmask = T->dmask;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    const int32x4_t gk1 = T->gk1, gk2 = T->gk2, gk3 = T->gk3;
    const bool gta = T->gta, t16 = T->t16;
    const bool shr = T->shr; const uint16x4_t shk = T->shk, shg = T->shg;
    (void)shr; (void)shk; (void)shg;
    const int32_t g1a0 = sGen.t1a[0], g1c0 = sGen.t1c[0], g1a1 = sGen.t1a[1], g1c1 = sGen.t1c[1];
    const int g1k0 = sGen.t1k[0], g1k1 = sGen.t1k[1];
    const int f8ka = vgetq_lane_s32(T->gk2, 0), f8kb = vgetq_lane_s32(T->gk2, 1);
    const uint32_t *const px1_32 = (const uint32_t *) T->px1;
    (void)g1a0; (void)g1c0; (void)g1a1; (void)g1c1; (void)g1k0; (void)g1k1; (void)f8ka; (void)f8kb; (void)px1_32;
    const void *const px1 = T->px1;
    const int tw1 = T->w1, th1 = T->h1, ws1 = T->ws1, wt1 = T->wt1, mu1 = T->mu1, mv1 = T->mv1;
    const bool t16_1 = T->t16_1;
    (void)th; (void)ws; (void)wt; (void)px32; (void)gk1; (void)gk2; (void)gk3; (void)gta; (void)t16; (void)px1;
    (void)tw1; (void)th1; (void)ws1; (void)wt1; (void)mu1; (void)mv1; (void)t16_1;
    { /* the rows of their own (sCmRowS, cm_row_f8) */
        const CmRowFn f = cm_row_own(T);
        if (f) { f(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw); return; }
    }
    if (T->mode == 4 || T->mode == 8 || T->mode == 10) { /* the generic combiner's fast paths 4, 8, 10 (two textures) */
        const bool rr4 = rr && !t16 && !t16_1 && ws1 == WRAP_REPEAT && wt1 == WRAP_REPEAT && mu1 >= 0 && mv1 >= 0;
#define CM_G4M(ALPHA, ZT, M) if (rr4) { if (T->bilin) { CM_ROW(ALPHA, ZT, M, CM_G4_BILIN_RR) } else { CM_ROW(ALPHA, ZT, M, CM_G4_NEAR_RR) } } \
                             else     { if (T->bilin) { CM_ROW(ALPHA, ZT, M, CM_G4_BILIN) } else { CM_ROW(ALPHA, ZT, M, CM_G4_NEAR) } }
        /* (fast path 8 with both repeating: cm_row_f8, above) */
#define CM_G4(ALPHA, ZT) if (T->mode == 8) { CM_G4M(ALPHA, ZT, 8) } else if (T->mode == 10) { CM_G4M(ALPHA, ZT, 10) } \
                         else { CM_G4M(ALPHA, ZT, 4) }
        if (T->ztest) { if (T->alpha) { CM_G4(1, 1) } else { CM_G4(0, 1) } }
        else          { if (T->alpha) { CM_G4(1, 0) } else { CM_G4(0, 0) } }
#undef CM_G4
#undef CM_G4M
        return;
    }
#define CM_VARIANTS(KIND, ALPHA, ZT) if (T->mode == 1) { CM_VARIANTS_M(KIND, ALPHA, ZT, 1) }             \
                                     else if (T->mode == 7) { CM_VARIANTS_M(KIND, ALPHA, ZT, 7) }        \
                                     else { CM_VARIANTS_M(KIND, ALPHA, ZT, 0) }
#define CM_VARIANTS_M(KIND, ALPHA, ZT, MODE)                                                            \
    if (rr) { if (T->t16) { CM_ROW(ALPHA, ZT, MODE, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }        \
              else        { CM_ROW(ALPHA, ZT, MODE, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }      \
    else    { if (T->t16) { CM_ROW(ALPHA, ZT, MODE, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }          \
              else        { CM_ROW(ALPHA, ZT, MODE, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
    if (T->ztest) {
        if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 1, 1) } else { CM_VARIANTS(CM_BILIN, 0, 1) } }
        else          { if (T->alpha) { CM_VARIANTS(CM_NEAR, 1, 1) }  else { CM_VARIANTS(CM_NEAR, 0, 1) } }
    } else { /* X23: no depth test (backgrounds, the sky) */
        if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 1, 0) } else { CM_VARIANTS(CM_BILIN, 0, 0) } }
        else          { if (T->alpha) { CM_VARIANTS(CM_NEAR, 1, 0) }  else { CM_VARIANTS(CM_NEAR, 0, 0) } }
    }
#undef CM_VARIANTS
#undef CM_VARIANTS_M
}
#undef CM_PIXELS
#undef CM_BILIN
#undef CM_NEAR
#undef CM_TAP32
#undef CM_TAP16
#undef CM_WRAP_RRU
#undef CM_WRAP_RRV
#undef CM_WRAP_GU
#undef CM_WRAP_GV
#undef CM_BILIN_AT
#undef CM_NEAR_AT
#undef CM_TAPA
#undef CM_TAPB
#undef CM_WRAP_1U
#undef CM_WRAP_1V
#undef CM_G4_BILIN
#undef CM_G4_NEAR
#undef CM_TAPA32
#undef CM_TAPB32
#undef CM_WRAP_1RU
#undef CM_WRAP_1RV
#undef CM_G4_BILIN_RR
#undef CM_G4_NEAR_RR
#undef CM_ROW
#define HH_CM_NEON 1
#endif
/* The bench's in-process A/B: -1 when there is no NEON kernel to switch. */
int gfx_soft_set_cmneon(int on) {
#if defined(HH_CM_NEON)
    sCmNeon = on != 0; return 0;
#else
    (void)on; return -1;
#endif
}
/* ...and PM64_CMSPLIT's (the rows of their own, 2026-10-03): -1 without NEON */
int gfx_soft_set_cmsplit(int on) {
#if defined(HH_CM_NEON)
    sCmSplit = on != 0; return 0;
#else
    (void)on; return -1;
#endif
}
/* ...and the speed round's (2026-10-07): bit 1 PM64_FASTROWS, 2 PM64_ZSKIP. Returns the mask before, -1 without NEON */
int gfx_soft_set_speedsw(int mask) {
#if defined(HH_CM_NEON)
    const int old = (sFastRows ? 1 : 0) | (sZSkip ? 2 : 0);
    sFastRows = (mask & 1) != 0; sZSkip = (mask & 2) != 0;
    return old;
#else
    (void)mask; return -1;
#endif
}
#if defined(HH_CM_NEON)
/* PM64_FASTROWS (2026-10-07, branch speed-2026-10-07, from super-mario-64's SM64_FASTROWS; default 1): raster_tri's row loop,
 * for the triangles whose rows all go to a NEON row (cm_rows / gen_rows, without PM64_FILLRULE's owned edges), in a
 * small function of its own (tri_rows): raster_tri kept ~60 values live across the row loop and reloaded or spilled
 * most of them every row. The same bases, edge solve, snap and start values, float for float: the edge kinds
 * (w_i_dx > 0, < 0, == 0) once a triangle instead of three compares a row; b < 0 from the float's bits (exact for
 * every value, NaN included: no VFP compare and its wait on vmrs); the snap's three sums in NEON lanes; the row's start
 * values (depth, shade, u/w, v/w, 1/w: 24 multiplies, 16 adds) as NEON lanes, multiply then add, each rounded as the
 * VFP rounds them (vmla is not fused, -ffp-contract=off keeps the VFP's unfused too; NEON flushes denormals to zero
 * where the VFP keeps them, and no value here comes near one: screen coordinates, barycentrics, 1/w, colours 0..255,
 * depth and their steps are each 0 or far above 1e-30); the row function picked once a
 * triangle (cm_row_own) where cm_row_neon picked it every row. 0: raster_tri's own loop. Same pixels. */
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's { z, r, g, b } and { a, u/w, v/w, 1/w } */
    float32x4_t fa, fb;                       /* flat: { -, r0, g0, b0 }, { a0, -, -, - } in place of the sums */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    int minx, maxx, sw, persp;
    int ek[3];                                /* each edge's w_i_dx: 0 > 0, 1 < 0, 2 == 0 */
    uint32_t *fbuf; uint16_t *zb;
    CmRowFn fn; const CmTri *k;
} TriRows;
/* f < 0.0f, from its bits: -0.0 and NaN false, as the compare */
static inline int f_neg(float f) { uint32_t u; memcpy(&u, &f, 4); return u - 0x80000001u < 0x7F800000u; }
#if defined(MIYOO)
/* the Miyoo's split (raster_tri): every ystep-th row from y, the other core drawing the rows between */
#define TR_STEP_PARAM , const int ystep
#define TR_STEP_ARG(n) , n
#define TR_YNEXT y += ystep
#else
#define TR_STEP_PARAM
#define TR_STEP_ARG(n)
#define TR_YNEXT y++
#endif
/* Rows y..maxy - 1 of the triangle, each exactly as raster_tri's loop draws it. Returns their span pixels. */
static inline __attribute__((always_inline)) uint32_t tri_rows_t(const TriRows *T, int y, const int maxy, const int FLAT TR_STEP_PARAM) {
    const float e0c = T->e0c, e0x = T->e0x, e0y = T->e0y, e1c = T->e1c, e1x = T->e1x, e1y = T->e1y;
    const float e2c = T->e2c, e2x = T->e2x, e2y = T->e2y, inv_area = T->inv_area;
    const float w0dx = T->w0dx, w1dx = T->w1dx, w2dx = T->w2dx;
    const int minx = T->minx, maxx = T->maxx, ek0 = T->ek[0], ek1 = T->ek[1], ek2 = T->ek[2];
    const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };
    uint32_t px = 0;
    for (; y < maxy; TR_YNEXT) {
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
        /* raster_tri's solve as it is there, its ds tests made once a triangle */
#define TRI_EDGE(EK, BS, DS)                                                                                     \
        if (EK == 0) {                                                                                          \
            if (f_neg(BS)) {                                                                                     \
                float q = -(BS) / (DS) - 1e-5f; if (q > 1e6f) q = 1e6f;                                          \
                const int a = minx + iceilf(q);                                                                  \
                if (a > xs) xs = a;                                                                              \
            }                                                                                                    \
        } else if (EK == 1) {                                                                                   \
            float q = -(BS) / (DS) + 1e-5f; if (q > 1e6f) q = 1e6f;                                              \
            const int a = minx + 1 + ifloorf(q);                                                                 \
            if (a < xe) xe = a;                                                                                  \
        } else if (f_neg(BS)) continue; /* the row outside this edge (xs = xe there) */
        TRI_EDGE(ek0, b0, w0dx)
        TRI_EDGE(ek1, b1, w1dx)
        TRI_EDGE(ek2, b2, w2dx)
#undef TRI_EDGE
        if (xs >= xe) continue;
        /* the snap: b_i + (xs - minx) * w_i_dx >= 0 for all three, in three lanes (the fourth 0 + 0 * o) */
        const float32x4_t B3 = vcombine_f32(vset_lane_f32(b1, vdup_n_f32(b0), 1), vset_lane_f32(0.0f, vdup_n_f32(b2), 1));
        float32x4_t W;
        for (;;) {
            W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
            const uint32x4_t ge = vcgeq_f32(W, vdupq_n_f32(0.0f));
            const uint32x2_t g2 = vpmin_u32(vget_low_u32(ge), vget_high_u32(ge));
            if (vget_lane_u32(vpmin_u32(g2, g2), 0)) break;
            if (++xs >= xe) break;
        }
        if (xs >= xe) continue;
        const int n = xe - xs;
        px += (uint32_t)n;
        /* w0*z0 + w1*z1 + w2*z2 for all eight (W: w0, w1, w2 as raster_tri makes them) */
        float32x4_t A = vmulq_lane_f32(T->va0, vget_low_f32(W), 0), B = vmulq_lane_f32(T->vb0, vget_low_f32(W), 0);
        A = vmlaq_lane_f32(A, T->va1, vget_low_f32(W), 1); B = vmlaq_lane_f32(B, T->vb1, vget_low_f32(W), 1);
        A = vmlaq_lane_f32(A, T->va2, vget_high_f32(W), 0); B = vmlaq_lane_f32(B, T->vb2, vget_high_f32(W), 0);
        if (FLAT) { /* the shade is the vertex colour */
            static const uint32_t ma[4] = { 0, ~0u, ~0u, ~0u }, mb[4] = { ~0u, 0, 0, 0 };
            A = vbslq_f32(vld1q_u32(ma), T->fa, A); B = vbslq_f32(vld1q_u32(mb), T->fb, B);
        }
        const float sw = vgetq_lane_f32(B, 3);
        const float rw = T->persp ? ((sw > 1e-12f) ? 1.0f / sw : 0.0f) : 0.0f;
        const int idx = y * T->sw + xs;
        T->fn(T->k, T->fbuf + idx, T->zb + idx, n, vgetq_lane_f32(A, 0), vgetq_lane_f32(A, 1), vgetq_lane_f32(A, 2),
              vgetq_lane_f32(A, 3), vgetq_lane_f32(B, 0), vgetq_lane_f32(B, 1), vgetq_lane_f32(B, 2), sw, rw);
    }
    return px;
}
static uint32_t __attribute__((noinline)) tri_rows_f(const TriRows *T, int y, int maxy TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, 1 TR_STEP_ARG(ystep)); }
static uint32_t __attribute__((noinline)) tri_rows_s(const TriRows *T, int y, int maxy TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, 0 TR_STEP_ARG(ystep)); }
#if defined(MIYOO)
/* The Mini's second core (kit miyoo_worker.c, the shared Miyoo kit KM2): a 3D triangle whose rows all go to a
 * NEON row (PM64_FASTROWS' tri_rows) and that is big enough is drawn by both cores at once, this one its even rows from
 * miny, the worker the odd ones (DKR's and MK64's split on the Mini). Exact: tri_rows computes each row from the
 * triangle's constants and its own y alone (the bases, the edge solve, the snap and the row's start values are
 * recomputed per row; nothing steps from one row to the next), so a row comes out the same whichever core draws it; the
 * shares write disjoint rows of colour and depth; and the triangle is finished (miyoo_worker_wait) before anything else
 * draws. All of the setup is done first, by this core, and the worker only reads it (the TriRows, the texture decoded at
 * upload, the generic combiner's per-draw state sGen, which its NEON rows only read). What a row writes besides pixels is
 * per share: the worker has its own copy of the CmTri (PM64_ZSKIP's run of misses, carried row to row, only decides which
 * spans are tested, and a tested span is skipped only when it draws nothing), and its own pixel count, added after the
 * wait. pm_gen_eval's scratch (sGenPix) is used only by raster_tri's own scalar loop, never split. Not split: the
 * triangles that take raster_tri's own loop (PM64_FILLRULE's owned edges, the scalar paths), 2D and texrects.
 * PM64_THREADS=1 (debug.cfg too): one core, as before. PM64_SPLIT=<px>: the smallest triangle split, in pixels of area
 * (default TR_SPLIT_PX; 1 with PM64_SPLITROWS=2 splits nearly every triangle, the stress check). */
#include "miyoo_worker.h"
#define TR_SPLIT_PX   64 /* below this, the handoff costs about what it saves */
#define TR_SPLIT_ROWS 4
typedef struct { TriRows trr; CmTri cmt; int flat, y, maxy; uint32_t px; } TrJob;
static void tr_job(void *arg) {
    TrJob *const j = (TrJob *)arg;
    j->px = j->flat ? tri_rows_f(&j->trr, j->y, j->maxy, 2) : tri_rows_s(&j->trr, j->y, j->maxy, 2);
}
static int sTrMinPx SAVESTATE_EXCLUDE = -1, sTrMinRows SAVESTATE_EXCLUDE = TR_SPLIT_ROWS, sTrStarted SAVESTATE_EXCLUDE;
static uint32_t sTrTris SAVESTATE_EXCLUDE, sTrSplits SAVESTATE_EXCLUDE;  /* the PERF line's split count */
static uint64_t sTrPx SAVESTATE_EXCLUDE, sTrSplitPx SAVESTATE_EXCLUDE;   /* the PERF line's split share of the 3D pixels */
/* port_host.c's PERF line: this window's split triangles, the split share of the 3D pixels, the worker's handoffs */
int gfx_soft_mt_line(char *buf, int n) {
    unsigned jobs = 0, sleeps = 0;
    unsigned long long spins = 0;
    miyoo_worker_stats(&jobs, &sleeps, &spins);
    const int r = snprintf(buf, (size_t)n, " | mt %s split %u/%u tris %.0f%% px, jobs %u, sleeps %u", miyoo_worker_ready() ? "on" : "off",
                           (unsigned)sTrSplits, (unsigned)sTrTris, sTrPx ? 100.0 * (double)sTrSplitPx / (double)sTrPx : 0.0, jobs, sleeps);
    sTrSplits = sTrTris = 0;
    sTrPx = sTrSplitPx = 0;
    return r;
}
#endif
#endif


/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
    float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) return -1; /* (winding cull happens in gfx_sp_tri1; the GE cull mode is never set here) */
#ifdef PM64
    if (sGen.on && sGen.fast == 8 && fire_cheap() && ((sFireTri++ >> 1) & 1)) return -1; /* the cheap fire: one quad of two */
#endif

    /* Clip 3D geometry to scissor AND viewport. On N64 the RSP confines
     * geometry to the viewport (guard-band clip) and the RDP scissors to the
     * scissor rect; MK64's results / driver's-points / split-screen scenes
     * render into quadrant viewports and rely on that confinement. We do no
     * RSP clipping, and CONVERT's aspect widening pushes geometry past the
     * quadrant edge, so without the viewport bound the scenes (and their
     * depth-test-free sky pass) bled into neighbouring quadrants -- the
     * clouds/track fragments on the win screen. viewport==full-screen for 1P
     * racing, so this is a no-op there. Texrects (2D path) stay scissor-only,
     * matching the N64 (they bypass the viewport). */
    const bool vp_ok = sVpW > 0 && sVpH > 0;
    const int vpx0 = !vp_ok ? 0    : (sVpX < 0 ? 0 : sVpX);
    const int vpy0 = !vp_ok ? 0    : (sVpY < 0 ? 0 : sVpY);
    const int vpx1 = !vp_ok ? sW   : (sVpX + sVpW > sW ? sW : sVpX + sVpW);
    const int vpy1 = !vp_ok ? sH   : (sVpY + sVpH > sH ? sH : sVpY + sVpH);
    /* mins: a bare (int) truncation equals ifloorf under the >= 0 clamp that
     * follows (they differ only for negative x, which both clamp to 0). */
    int minx = imax(imax(imax((int)fmin3(v0->x, v1->x, v2->x), sScX0), vpx0), 0);
    int maxx = imin(imin(imin(iceilf (fmax3(v0->x, v1->x, v2->x)), sScX1), vpx1), sW);
    int miny = imax(imax(imax((int)fmin3(v0->y, v1->y, v2->y), sScY0), vpy0), 0);
    int maxy = imin(imin(imin(iceilf (fmax3(v0->y, v1->y, v2->y)), sScY1), vpy1), sH);
    if (minx >= maxx || miny >= maxy) return -1;
    /* Barycentrics = edge/area with SIGNED area: inside is all-w >= 0 for
     * either winding, no separate sign handling needed. (Divide sits after
     * the bbox reject so off-screen triangles don't pay for it.) */
    const float inv_area = 1.0f / area;

    const SoftTex *tex = (sCurShader && sCurShader->cc.used_textures[0]) ? &sTex[sBoundTex[0]] : NULL;
#ifdef PM64
    const bool gen = sGen.on;
    if (gen) tex = (sGen.t0 || sGen.t1) ? &sTex[sBoundTex[sGen.t0 ? 0 : 1]] : NULL;
    const SoftTex *gtex1 = gen && sGen.t1 ? &sTex[sBoundTex[1]] : NULL;
    if (gtex1 && !gtex1->valid) { untex_note("tri t1", sBoundTex[1]); gtex1 = NULL; }
    const void *g1px = gtex1 ? gtex1->px : NULL; const bool g1_16 = gtex1 && gtex1->is16;
    const int g1w = gtex1 ? gtex1->w : 1, g1h = gtex1 ? gtex1->h : 1, g1ws = gtex1 ? gtex1->wrap_s : 0, g1wt = gtex1 ? gtex1->wrap_t : 0;
    const int g1mu = gtex1 ? pow2mask(g1w) : -1, g1mv = gtex1 ? pow2mask(g1h) : -1;
    const bool gt0 = gen && sGen.t0;
#else
    const bool gen = false;
#endif
    if (tex && !tex->valid) { untex_note("tri", (uint32_t) (tex - sTex)); tex = NULL; }
    const int tmask_u = tex ? pow2mask(tex->w) : -1, tmask_v = tex ? pow2mask(tex->h) : -1;
    const bool tex_edge = sUseAlpha && sCurShader && sCurShader->cc.opt_texture_edge;
    const int ablend = !(tex_edge && sEdgeOpaque); /* 0: the texel's own colour past amin (X12) */
    const bool ztest = use_z && sDepthTest;
#ifdef PM64
    if (ztest && sRtZDirty && sRtActive) rt_zclear(); /* the target's deferred depth clear (gfx_soft_rt_begin) */
#endif
    /* Magnified-only bilinear (device round 12, fixed round 13): the 4-tap
     * blend only helps when a texel covers >~1 screen pixel (close/magnified
     * surfaces). On minified surfaces (distant road/jungle) the 4 taps scatter
     * across cache lines for no visible gain, so sample nearest there.
     * Magnified when the screen-space triangle area is >= its TEXEL-space
     * area. u/v here are normalised (texture widths -- the fetch multiplies by
     * tw/th), so the uv cross product must be scaled by tw*th; round 12 forgot
     * that and compared px^2 to normalised^2, i.e. was always bilinear. Both
     * sides are 2x the signed area; the 2.0f bias keeps bilinear until ~2x
     * minified. (Screen area is in internal-target pixels: fine while the 3D
     * target is locked at native; with the res ladder it would want
     * hysteresis.) The PM64 generic combiner follows the same rule (round 15:
     * it sampled nearest always, so the battle stages drew blocky under SMOOTH). */
    bool use_bilin = gfx_soft_bilinear && tex;
    if (use_bilin && gfx_soft_magbilin) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
    }
#ifdef PM64
    /* Fast path 8's draws (FC322BFF 5F0E923F, Bowser's fire breath) sample nearest: a look change for speed, the
     * user's pick (the fire round's sheets). PM64_FIRENEAREST=0: bilinear as the N64 */
    static int sFireNearest = -1;
    if (sFireNearest < 0) { const char *e = getenv("HM64_FIRENEAREST"); sFireNearest = !(e && e[0] == '0'); }
    if (use_bilin && sFireNearest && gen && sGen.fast == 8) use_bilin = false;
#endif
    /* Pixel-exact 2D (X27, from SM64 eb0a164): a screen-aligned ortho quad drawn at one texel per pixel (a message
     * window's corner) samples nearest from whole-pixel corners, so every texel lands on one pixel; bilinear only
     * blurred it by its sub-pixel offset. PM's texrects (the HUD, text, icons, the letter) already draw this way.
     * PM64_HUDEXACT=0: off (the A/B). */
    if (use_bilin && !sHudExactRun && sHudExact && v0->invw == v1->invw && v1->invw == v2->invw) {
        const float ex1 = v1->x - v0->x, ey1 = v1->y - v0->y, ex2 = v2->x - v0->x, ey2 = v2->y - v0->y;
        const float du1 = (v1->u - v0->u) * tex->w, du2 = (v2->u - v0->u) * tex->w;
        const float dv1 = (v1->v - v0->v) * tex->h, dv2 = (v2->v - v0->v) * tex->h;
        const float su = (du1 * ey2 - du2 * ey1) / area, sv = (dv2 * ex1 - dv1 * ex2) / area; /* texels per pixel */
        const float cu = (du2 * ex1 - du1 * ex2) / area, cv = (dv1 * ey2 - dv2 * ey1) / area; /* cross terms */
        if (fabsf(fabsf(su) - 1.0f) <= 0.02f && fabsf(fabsf(sv) - 1.0f) <= 0.02f && fabsf(cu) < 1.0f / 64 && fabsf(cv) < 1.0f / 64) {
            SoftVtx q[3] = { *v0, *v1, *v2 };
            for (int i = 0; i < 3; i++) { q[i].x = floorf(q[i].x + 0.5f); q[i].y = floorf(q[i].y + 0.5f); }
            sHudExactRun = true;
            const int r = raster_tri(&q[0], &q[1], &q[2], use_z);
            sHudExactRun = false;
            return r;
        }
    }
    if (sHudExactRun) use_bilin = false;
    const int amin = tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
    const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in;
    const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in;
    const bool rgb_texa = sCurShader && sCurShader->rgb_texa, rgb_decal = sCurShader && sCurShader->rgb_decal;
    const void *texpx = tex ? tex->px : NULL; const bool t16 = tex && tex->is16;
    /* Flat shade (one vertex colour) skips the per-pixel colour stepping. */
    const bool flat = v0->r == v1->r && v1->r == v2->r && v0->g == v1->g && v1->g == v2->g &&
                      v0->b == v1->b && v1->b == v2->b && v0->a == v1->a && v1->a == v2->a;

    /* Vertex fields as locals (device round 13): the previous row's framebuffer
     * stores are an aliasing barrier under -fno-strict-aliasing, so GCC was
     * reloading -- and ARM->VFP transferring, ~10 stall cycles each -- all 18 of
     * these on EVERY scanline (12 ldrb + 12 vmov + 12 vcvt in the y-loop).
     * uint8->float and float copies are exact, so every use is bit-identical. */
    const float z0 = v0->z, z1 = v1->z, z2 = v2->z;
    const float r0 = v0->r, r1 = v1->r, r2 = v2->r, g0 = v0->g, g1 = v1->g, g2 = v2->g;
    const float bl0 = v0->b, bl1 = v1->b, bl2 = v2->b, a0 = v0->a, a1 = v1->a, a2 = v2->a;
    const float iw0 = v0->invw, iw1 = v1->invw, iw2 = v2->invw;

    const bool persp = tex && iw0 > 0.0f && iw1 > 0.0f && iw2 > 0.0f;
    const float uw0 = persp ? v0->u * iw0 : v0->u, uw1 = persp ? v1->u * iw1 : v1->u, uw2 = persp ? v2->u * iw2 : v2->u;
    const float vw0 = persp ? v0->v * iw0 : v0->v, vw1 = persp ? v1->v * iw1 : v1->v, vw2 = persp ? v2->v * iw2 : v2->v;

    /* Barycentric x-gradients (normalized by the signed area, so inside is
     * simply all-w >= 0 whichever way the triangle winds). */
    const float w0dx = (v2->y - v1->y) * inv_area;
    const float w1dx = (v0->y - v2->y) * inv_area;
    const float w2dx = (v1->y - v0->y) * inv_area;

    /* Per-pixel x-gradients: triangle-invariant, so build them once here rather
     * than rebuilding the identical values on every scanline (was ~24 mul + 16
     * add/row). Values are bit-identical -- same inputs, same order. */
    const float zdx  = w0dx * z0 + w1dx * z1 + w2dx * z2;
    float crdx = w0dx*r0 + w1dx*r1 + w2dx*r2;
    float cgdx = w0dx*g0 + w1dx*g1 + w2dx*g2;
    float cbdx = w0dx*bl0 + w1dx*bl1 + w2dx*bl2;
    float cadx = w0dx*a0 + w1dx*a1 + w2dx*a2;
    const float uwdx = w0dx*uw0 + w1dx*uw1 + w2dx*uw2;
    const float vwdx = w0dx*vw0 + w1dx*vw1 + w2dx*vw2;
    const float swdx = w0dx*iw0 + w1dx*iw1 + w2dx*iw2;
    if (flat) { crdx = cgdx = cbdx = cadx = 0.0f; } /* flat: no per-pixel colour step */
    /* Fixed-point deltas are invariant too (were rebuilt every 16-px span). */
    const int32_t zdq = span_zdq(zdx); /* +-1 a pixel, +-6 with PM64_NEARW */
    const int32_t rdq = sat_i32(crdx * 65536.0f), gdq = sat_i32(cgdx * 65536.0f), bdq = sat_i32(cbdx * 65536.0f), adq = sat_i32(cadx * 65536.0f);
    /* Flat triangles: the shade colour every pixel would clamp (rq = (int32_t)(cr * 65536.0f)
     * with cr == r0 on every span, the steps all 0), clamped once. (MK64 perf round 24, via
     * ocarina-of-time d267ca3.) */
    const int fsr = clamp8_sh16((int32_t)(r0 * 65536.0f)), fsg = clamp8_sh16((int32_t)(g0 * 65536.0f));
    const int fsb = clamp8_sh16((int32_t)(bl0 * 65536.0f)), fsa = clamp8_sh16((int32_t)(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const int zbias = sZDecal ? 4 : 0; const bool dmask = sDepthMask, texmix = sTexAdd || rgb_decal; /* the rare combines share one test */
    const bool cm_mod = rgb_tex && rgb_in && !texmix && !rgb_texa; /* plain texture x shade: see SPAN_LOOP_CM */
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
#if defined(HH_CM_NEON)
    /* The NEON rows (cm_row_neon, ocarina-of-time c935b73): z-tested texture x shade, not the PM64
     * generic combiner (its own loop). */
    /* ...and without a depth test too (X23, goldeneye-007 287aa70: backgrounds, the sky; the bench's 3.9M
     * pixels of texture x shade without a depth test, a quarter of its 3D, went to the scalar loops).
     * PM64_CMNOZ=0: those stay scalar. */
    static int sCmNoZ = -1;
    if (sCmNoZ < 0) { const char *e = getenv("HM64_CMNOZ"); sCmNoZ = !(e && e[0] == '0'); }
    const bool cm_rows = sCmNeon && !gen && tex && (ztest || sCmNoZ) && cm_mod;
#ifdef PM64
    /* ...and the generic combiner's fast paths 1 (T0 x SHADE, then (C - K1) x K2 + K3: the forest's Star Spirits
     * scene, two screens a frame) and 4 ((T1 x T0) x SHADE: the castle lift's dust), same integer steps as
     * pm_gen_fast (the speed round). PM64_GENNEON=0: their scalar loops. */
    static int sGenNeon = -1;
    if (sGenNeon < 0) { const char *e = getenv("HM64_GENNEON"); sGenNeon = !(e && e[0] == '0'); }
    const bool gen_rows = sCmNeon && sGenNeon && gen && tex && (ztest || sCmNoZ) &&
                          (sGen.fast == 1 || sGen.fast == 7 || ((sGen.fast == 4 || sGen.fast == 8 || sGen.fast == 10) && gtex1 && gt0));
#else
    const bool gen_rows = false;
#endif
    CmTri cmt;
    if (cm_rows || gen_rows) {
        const int32_t d[4] = { bdq, gdq, rdq, adq };
        cmt.px = texpx; cmt.w = tw; cmt.h = th; cmt.ws = ws; cmt.wt = wt; cmt.mu = tmask_u; cmt.mv = tmask_v; cmt.t16 = t16;
        cmt.swdx = swdx; cmt.uwdx = uwdx; cmt.vwdx = vwdx; cmt.zdx = zdx;
        cmt.crdx = crdx; cmt.cgdx = cgdx; cmt.cbdx = cbdx; cmt.cadx = cadx;
        cmt.ftw = (float)tw; cmt.fth = (float)th;
        cmt.dacc = vld1q_s32(d); cmt.zdq = zdq; cmt.zbias = zbias;
        cmt.amode = !a_tex ? 2 : !a_in ? 1 : 0; /* fa = !a_tex ? sa : !a_in ? ta : div255(ta*sa) */
        cmt.ztest = ztest; cmt.amin = amin; cmt.ablend = ablend; cmt.persp = persp; cmt.dmask = dmask; cmt.alpha = sUseAlpha; cmt.bilin = use_bilin;
        cmt.mode = 0;
        cmt.zsk = sZSkip ? 4 : 0; /* PM64_ZSKIP (CM_ROW) */
#ifdef PM64
        cmt.shr = false; cmt.shk = cmt.shg = vdup_n_u16(0);
        if (gen_rows) {
            cmt.amode = 0; /* the combined alpha */
            cmt.mode = sGen.fast;
            if (sGen.shroud) { /* lanes B G R A; A: x 32 >> 5, unchanged */
                const int f = sGen.shroud;
                const uint16_t k[4] = { (uint16_t) (sGen.sh[2] * f), (uint16_t) (sGen.sh[1] * f), (uint16_t) (sGen.sh[0] * f), 0 };
                const uint16_t gv[4] = { (uint16_t) (32 - f), (uint16_t) (32 - f), (uint16_t) (32 - f), 32 };
                cmt.shr = true; cmt.shk = vld1_u16(k); cmt.shg = vld1_u16(gv);
            }
            if (sGen.fast == 7) {
                const int32_t k1[4] = { sGen.k1[2], sGen.k1[1], sGen.k1[0], 0 };
                cmt.gk1 = vld1q_s32(k1);
            } else if (sGen.fast == 1) {
                const int32_t k1[4] = { sGen.k1[2], sGen.k1[1], sGen.k1[0], 0 }, k2[4] = { sGen.k2[2], sGen.k2[1], sGen.k2[0], 256 };
                const int32_t k3[4] = { sGen.k3s[2] + 0x80, sGen.k3s[1] + 0x80, sGen.k3s[0] + 0x80, 0x80 };
                cmt.gk1 = vld1q_s32(k1); cmt.gk2 = vld1q_s32(k2); cmt.gk3 = vld1q_s32(k3); cmt.gta = sGen.fast_ta;
            } else {
                if (sGen.fast == 10) {
                    const int32_t k1[4] = { sGen.k1[2], sGen.k1[1], sGen.k1[0], 0 };
                    cmt.gk1 = vld1q_s32(k1);
                }
                if (sGen.fast == 8) {
                    const int32_t k12[4] = { sGen.k1[2] - sGen.k2[2], sGen.k1[1] - sGen.k2[1], sGen.k1[0] - sGen.k2[0], 0 };
                    const int32_t k3[4] = { sGen.k3s[2] + 0x80, sGen.k3s[1] + 0x80, sGen.k3s[0] + 0x80, 0 };
                    const int32_t ka[4] = { sGen.k1[3], sGen.k2[3], 0, 0 };
                    cmt.gk1 = vld1q_s32(k12); cmt.gk3 = vld1q_s32(k3); cmt.gk2 = vld1q_s32(ka);
                }
                cmt.px1 = g1px; cmt.w1 = g1w; cmt.h1 = g1h; cmt.ws1 = g1ws; cmt.wt1 = g1wt; cmt.mu1 = g1mu; cmt.mv1 = g1mv; cmt.t16_1 = g1_16;
            }
        }
#endif
    }
#endif

    /* Row edge bases (device round 13): edge(a,b,fx,py) is
     *   (fx - a.x)*(b.y - a.y) - (py - a.y)*(b.x - a.x)
     * and fx never changes with y, so the first product is a whole-triangle
     * constant and (b.x - a.x) is triangle-invariant. Per row only the second
     * product and the final subtract/scale remain -- the SAME operations in the
     * same order as edge() (bit-identical, still no row-to-row accumulation, so
     * shared edges stay crack-free), minus 12 vertex loads + 9 subs + 3 muls. */
    const float fx = minx + 0.5f;
    const float e0c = (fx - v1->x) * (v2->y - v1->y), e0x = v2->x - v1->x, e0y = v1->y; /* edge(v1,v2) */
    const float e1c = (fx - v2->x) * (v0->y - v2->y), e1x = v0->x - v2->x, e1y = v2->y; /* edge(v2,v0) */
    const float e2c = (fx - v0->x) * (v1->y - v0->y), e2x = v1->x - v0->x, e2y = v0->y; /* edge(v0,v1) */
    uint32_t tri_px = 0; /* span pixels of this triangle (<= 57,600); folded into the global once below */
    OwnTri oe; /* PM64_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    if (own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&oe.e[0], v1, v2, s); own_edge(&oe.e[1], v2, v0, s); own_edge(&oe.e[2], v0, v1, s);
    }

#if defined(HH_CM_NEON)
    if ((cm_rows || gen_rows) && !own && sFastRows) { /* PM64_FASTROWS: the rows in tri_rows */
        TriRows trr;
        trr.va0 = (float32x4_t){ z0, r0, g0, bl0 }; trr.vb0 = (float32x4_t){ a0, uw0, vw0, iw0 };
        trr.va1 = (float32x4_t){ z1, r1, g1, bl1 }; trr.vb1 = (float32x4_t){ a1, uw1, vw1, iw1 };
        trr.va2 = (float32x4_t){ z2, r2, g2, bl2 }; trr.vb2 = (float32x4_t){ a2, uw2, vw2, iw2 };
        trr.fa = (float32x4_t){ 0.0f, r0, g0, bl0 }; trr.fb = (float32x4_t){ a0, 0.0f, 0.0f, 0.0f };
        trr.e0c = e0c; trr.e0x = e0x; trr.e0y = e0y; trr.e1c = e1c; trr.e1x = e1x; trr.e1y = e1y;
        trr.e2c = e2c; trr.e2x = e2x; trr.e2y = e2y; trr.inv_area = inv_area;
        trr.w0dx = w0dx; trr.w1dx = w1dx; trr.w2dx = w2dx;
        trr.minx = minx; trr.maxx = maxx; trr.sw = sW; trr.persp = persp;
        trr.ek[0] = w0dx > 0.0f ? 0 : w0dx < 0.0f ? 1 : 2;
        trr.ek[1] = w1dx > 0.0f ? 0 : w1dx < 0.0f ? 1 : 2;
        trr.ek[2] = w2dx > 0.0f ? 0 : w2dx < 0.0f ? 1 : 2;
        trr.fbuf = fbuf; trr.zb = zb;
        trr.fn = cm_row_own(&cmt); if (!trr.fn) trr.fn = cm_row_neon; trr.k = &cmt;
#if defined(MIYOO)
        if (__builtin_expect(sTrMinPx < 0, 0)) {
            const char *e = getenv("HM64_SPLIT"), *r = getenv("HM64_SPLITROWS");
            sTrMinPx = e && e[0] ? atoi(e) : TR_SPLIT_PX;
            if (r && r[0] && atoi(r) >= 2) sTrMinRows = atoi(r);
        }
        if (!sTrStarted) { sTrStarted = 1; miyoo_worker_start(); } /* here, on the game thread, after the sound's thread exists */
        sTrTris++;
        if (miyoo_worker_ready() && maxy - miny >= sTrMinRows && (area < 0.0f ? -area : area) >= 2.0f * (float)sTrMinPx) {
            TrJob job; /* the worker's share: the odd rows, with its own CmTri (see TrJob) */
            job.trr = trr; job.cmt = cmt; job.trr.k = &job.cmt;
            job.flat = flat; job.y = miny + 1; job.maxy = maxy; job.px = 0;
            miyoo_worker_run(tr_job, &job);
            tri_px = flat ? tri_rows_f(&trr, miny, maxy, 2) : tri_rows_s(&trr, miny, maxy, 2);
            miyoo_worker_wait();
            tri_px += job.px;
            sTrSplits++; sTrSplitPx += tri_px;
        } else {
            tri_px = flat ? tri_rows_f(&trr, miny, maxy, 1) : tri_rows_s(&trr, miny, maxy, 1);
        }
        sTrPx += tri_px;
#else
        tri_px = flat ? tri_rows_f(&trr, miny, maxy) : tri_rows_s(&trr, miny, maxy);
#endif
        return (int)tri_px;
    }
#endif
    for (int y = miny; y < maxy; y++) {
        const float py = y + 0.5f;
        /* Row bases recomputed exactly each row (no row-to-row drift). */
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;

        /* Solve b_i + (x - minx) * w_i_dx >= 0 for the pixel interval. */
        int xs = minx, xe = maxx;
        {
            const float bs[3] = { b0, b1, b2 }, ds[3] = { w0dx, w1dx, w2dx };
            for (int i = 0; i < 3; i++) {
                /* Bias both bounds toward inclusion: a 1-px overlap between
                 * neighbouring triangles is invisible, a 1-px crack is not.
                 * The snap loop below restores exactness on the left edge. */
                /* A near-horizontal edge (|ds| ~ 1 ulp) on a large off-screen
                 * polygon makes the quotient ~1e11: the int conversion saturates
                 * to INT_MAX and `minx + 1 + INT_MAX` wrapped negative, so xe
                 * went to INT_MIN and the WHOLE triangle vanished for a frame
                 * (pre-existing; round 13). Clamping the float first is exact
                 * for every quotient that fits and just means "way past the
                 * screen" for the ones that do not. */
                if (ds[i] > 0.0f) {
                    if (bs[i] < 0.0f) {
                        float q = -bs[i] / ds[i] - 1e-5f; if (q > 1e6f) q = 1e6f;
                        int a = minx + iceilf(q);
                        if (a > xs) xs = a;
                    }
                } else if (ds[i] < 0.0f) {
                    float q = -bs[i] / ds[i] + 1e-5f; if (q > 1e6f) q = 1e6f;
                    int a = minx + 1 + ifloorf(q);
                    if (a < xe) xe = a;
                } else if (bs[i] < 0.0f) {
                    xs = xe; /* row entirely outside this edge */
                    break;
                }
            }
        }
        if (xs >= xe) continue;
        if (own) { /* PM64_FILLRULE: the row's ends by the owned-edge test, one pixel either side */
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;
            while (xs < xe && !own_px(&oe, xs, py)) xs++;
            while (xe > xs && !own_px(&oe, xe - 1, py)) xe--;
        } else
        /* Snap against rounding from the divisions above so the first pixel
         * really is inside per the linear edge functions. */
        while (xs < xe) {
            const float o = (float)(xs - minx);
            if (b0 + o * w0dx >= 0 && b1 + o * w1dx >= 0 && b2 + o * w2dx >= 0) break;
            xs++;
        }
        if (xs >= xe) continue;
        tri_px += (uint32_t)(xe - xs); /* fill/overdraw proxy (before z-reject) */

        /* Barycentrics at the span start. */
        const float o = (float)(xs - minx);
        float w0 = b0 + o * w0dx;
        float w1 = b1 + o * w1dx;
        float w2 = b2 + o * w2dx;

        float zf = w0*z0 + w1*z1 + w2*z2;
        float cr, cg, cb, ca;
        if (flat) { cr = r0; cg = g0; cb = bl0; ca = a0; } /* constant: no barycentric build */
        else { cr = w0*r0 + w1*r1 + w2*r2; cg = w0*g0 + w1*g1 + w2*g2; cb = w0*bl0 + w1*bl1 + w2*bl2; ca = w0*a0 + w1*a1 + w2*a2; }
        float uw = w0*uw0 + w1*uw1 + w2*uw2;
        float vw = w0*vw0 + w1*vw1 + w2*vw2;
        float sw = w0*iw0 + w1*iw1 + w2*iw2;

        int idx = y * sW + xs;

        /* 1/w at the span start. The old loop recomputed it every span, but
         * the next span's start w IS this span's end w (same float op), so
         * carry the reciprocal forward: one divide per span instead of two.
         * Only perspective spans need it -- round 12 had hoisted the divides
         * out of that test, so untextured/affine spans paid a vdiv for nothing. */
        float rw = 0.0f;
        if (persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
#if defined(HH_CM_NEON)
        if (cm_rows || gen_rows) { /* the whole row; flat triangles: zero colour steps, so rq.. clamp to the fs* */
            cm_row_neon(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            continue;
        }
#endif
        int x = xs;
        while (x < xe) {
            int span = xe - x;
            if (span > PERSP_SPAN) span = PERSP_SPAN;
            const float fspan = (float)span;
            const float sw_b = sw + swdx * fspan;           /* == the old end-of-span  sw += swdx*span */
            float rw_b = 0.0f;
            /* Perspective: exact u,v at span start and end, lerp between. */
            float u_a, v_a, du, dv;
            if (tex) {
                if (persp) {
                    rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
                    u_a = uw * rw; v_a = vw * rw;
                    const float nu = (uw + uwdx * fspan) * rw_b - u_a;
                    const float nv = (vw + vwdx * fspan) * rw_b - v_a;
                    /* /16 is an exact power of two: multiplying by 0.0625f is
                     * bit-identical and skips two divides on every full span.
                     * Only the last, partial span still divides. */
                    if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); }
                    else                    { du = nu / fspan;               dv = nv / fspan; }
                } else {
                    u_a = uw; v_a = vw; du = uwdx; dv = vwdx;
                }
            } else { u_a = v_a = du = dv = 0.0f; }

            /* The pixel loop is integer-only (device round 6): every per-pixel
             * interpolant -- texel u/v, z, r/g/b/a -- steps in fixed point,
             * converted once per 16-pixel span. The float->int conversions
             * and compares that were in the loop stall the Cortex-A7's VFP;
             * z is 24.8 of the 16-bit depth, colours 16.16. */
            int32_t uf = 0, vf = 0, duf = 0, dvf = 0;
            if (tex) {
                uf = to_fix16(u_a * (float)tw); duf = to_fix16(du * (float)tw);
                vf = to_fix16(v_a * (float)th); dvf = to_fix16(dv * (float)th);
            }
            /* the span's start depth, clamped wide enough for a NoN vertex in front of the near plane (its depth
             * below 0; each pixel clamps to 0..1 in clamp16_sh8): -1 cut its slope short (ocarina-of-time OOT_NON) */
            int32_t zq = span_z_out(zf) ? span_zq_far(zf, zdx, span, zdq) /* PM64_NEARW */
                                        : (int32_t)((zf < -8.0f ? -8.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            int32_t rq = (int32_t)(cr * 65536.0f), gq = (int32_t)(cg * 65536.0f), bq = (int32_t)(cb * 65536.0f), aq = (int32_t)(ca * 65536.0f);

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
#ifdef PM64
#define GEN_FETCH_NEAREST                                                                              \
                        if (texpx) { const int iu = wrapf((gt0 ? uf : gen_t1(uf, 0)) >> 16, tw, ws, tmask_u), iv = wrapf((gt0 ? vf : gen_t1(vf, 1)) >> 16, th, wt, tmask_v); \
                                     const uint32_t tt = texel_at(texpx, t16, iv * tw + iu); if (gt0) t0 = tt; else t1 = tt; } \
                        if (g1px && gt0) { const int iu = wrapf(gen_t1(uf, 0) >> 16, g1w, g1ws, g1mu), iv = wrapf(gen_t1(vf, 1) >> 16, g1h, g1wt, g1mv); \
                                     t1 = texel_at(g1px, g1_16, iv * g1w + iu); }
/* SMOOTH: both texels bilinear, TEXEL1 at its own tile's scale and corner (gen_t1) */
#define GEN_FETCH_BILINEAR                                                                             \
                        if (texpx) { const uint32_t tt = bilerp(texpx, t16, tw, th, ws, wt, gt0 ? uf : gen_t1(uf, 0), gt0 ? vf : gen_t1(vf, 1), tmask_u, tmask_v); \
                                     if (gt0) t0 = tt; else t1 = tt; }                                  \
                        if (g1px && gt0) t1 = bilerp(g1px, g1_16, g1w, g1h, g1ws, g1wt, gen_t1(uf, 0), gen_t1(vf, 1), g1mu, g1mv);
#define GEN_COLOR_NEAREST                                                                              \
                    {   uint32_t t0 = 0, t1 = 0; GEN_FETCH_NEAREST                                      \
                        const uint32_t c = pm_gen_eval(t0, t1, sr, sg, sb, sa);                         \
                        fr = (c >> 16) & 255; fg = (c >> 8) & 255; fb = c & 255; fa = c >> 24; }
#define GEN_COLOR_BILINEAR                                                                             \
                    {   uint32_t t0 = 0, t1 = 0; GEN_FETCH_BILINEAR                                     \
                        const uint32_t c = pm_gen_eval(t0, t1, sr, sg, sb, sa);                         \
                        fr = (c >> 16) & 255; fg = (c >> 8) & 255; fb = c & 255; fa = c >> 24; }
/* The fast paths (gen_classify_fast), SMOOTH only: F is a constant in each span loop. */
#define GEN_FAST(BILIN, F)                                                                             \
                    {   uint32_t t0 = 0, t1 = 0; GEN_FETCH_##BILIN                                      \
                        const uint32_t c = pm_gen_fast(F, t0, t1, sr, sg, sb, sa);                      \
                        fr = (c >> 16) & 255; fg = (c >> 8) & 255; fb = c & 255; fa = c >> 24; }
#else
#define GEN_COLOR_NEAREST
#define GEN_COLOR_BILINEAR
#endif
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once above): the colour
 * accumulators and their steps drop out of the pixel loop (flat => every step is 0).
 * CM=1: the combine is plain texture x shade (cm_mod), so the walk of the combine flags drops
 * out. Both bit-exact (MK64 perf round 24, via ocarina-of-time d267ca3). */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
#define SPAN_LOOP(TEX, ZTEST, ALPHA, BILIN) SPAN_LOOP_X(TEX, ZTEST, ALPHA, BILIN, 0, 0, 0)
#define SPAN_LOOP_G(TEX, ZTEST, ALPHA, BILIN, GEN) SPAN_LOOP_X(TEX, ZTEST, ALPHA, BILIN, GEN, 0, 0)
#define SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, CM) SPAN_LOOP_X(TEX, ZTEST, ALPHA, BILIN, 0, FLAT, CM)
#define SPAN_LOOP_X(TEX, ZTEST, ALPHA, BILIN, GEN, FLAT, CM)                                           \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq COLSTEP_##FLAT) {     \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias) continue; }                                      \
                const int sr = FLAT ? fsr : clamp8_sh16(rq), sg = FLAT ? fsg : clamp8_sh16(gq);         \
                const int sb = FLAT ? fsb : clamp8_sh16(bq), sa = FLAT ? fsa : clamp8_sh16(aq);         \
                int fr, fg, fb, fa;                                                                     \
                if (GEN == 1) { GEN_COLOR_##BILIN }                                                     \
                else if (GEN > 10) { GEN_FAST(BILIN, GEN - 10) }                                        \
                else if (TEX) {                                                                         \
                    FETCH_##BILIN                                                                       \
                    const int ta = (t>>24)&0xff;                                                        \
                    const int tr = (!CM && rgb_texa) ? ta : (t>>16)&0xff, tg = (!CM && rgb_texa) ? ta : (t>>8)&0xff, tb = (!CM && rgb_texa) ? ta : t&0xff; \
                    if (CM)             { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (texmix) {                                                          \
                        if (rgb_decal)  { fr=div255(tr*ta + sr*(255-ta)); fg=div255(tg*ta + sg*(255-ta)); \
                                          fb=div255(tb*ta + sb*(255-ta)); }                             \
                        else            { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    }                                                                                   \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = !a_tex ? sa : (!a_in ? ta : div255(ta*sa));                                    \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (ALPHA) {                                                                            \
                    if (fa < amin) continue;                                                            \
                    if (fa < 255 && ablend) {                                                           \
                        const uint32_t d = fbuf[idx];                                                   \
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;                           \
                        fr = div255(fr*fa + dr*(255-fa));                                               \
                        fg = div255(fg*fa + dg*(255-fa));                                               \
                        fb = div255(fb*fa + db*(255-fa));                                               \
                    }                                                                                   \
                }                                                                                       \
                fbuf[idx] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);                              \
                if (ZTEST && dmask) zb[idx] = (uint16_t)uz;                                             \
            }
            if (gen) {
                /* the fast paths with nearest texels too: with P17 off a magnified texture samples nearest, and the
                 * file select's turning windows (fast path 1) went through the operand table (the user: "quite slow") */
                if (!use_bilin && sGen.fast >= 1 && sGen.fast <= 3) {
                    const int F = sGen.fast;
                    if (F == 1) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 11) else SPAN_LOOP_G(1, 1, 0, NEAREST, 11) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 11) else SPAN_LOOP_G(1, 0, 0, NEAREST, 11) }
                    } else if (F == 2) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 12) else SPAN_LOOP_G(1, 1, 0, NEAREST, 12) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 12) else SPAN_LOOP_G(1, 0, 0, NEAREST, 12) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 13) else SPAN_LOOP_G(1, 1, 0, NEAREST, 13) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 13) else SPAN_LOOP_G(1, 0, 0, NEAREST, 13) }
                    }
                } else if (use_bilin && sGen.fast == 1) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 11) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 11) }
                    else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 11) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 11) }
                } else if (use_bilin && sGen.fast == 3) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 13) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 13) }
                    else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 13) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 13) }
                } else if (use_bilin && sGen.fast == 2) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 12) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 12) }
                    else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 12) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 12) }
                } else if (sGen.fast == 7) {
                    if (use_bilin) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 17) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 17) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 17) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 17) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 17) else SPAN_LOOP_G(1, 1, 0, NEAREST, 17) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 17) else SPAN_LOOP_G(1, 0, 0, NEAREST, 17) }
                    }
                } else if (sGen.fast == 10) {
                    if (use_bilin) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 20) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 20) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 20) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 20) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 20) else SPAN_LOOP_G(1, 1, 0, NEAREST, 20) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 20) else SPAN_LOOP_G(1, 0, 0, NEAREST, 20) }
                    }
                } else if (sGen.fast == 8) {
                    if (use_bilin) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 18) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 18) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 18) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 18) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 18) else SPAN_LOOP_G(1, 1, 0, NEAREST, 18) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 18) else SPAN_LOOP_G(1, 0, 0, NEAREST, 18) }
                    }
                } else if (sGen.fast == 4) {
                    if (use_bilin) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 14) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 14) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 14) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 14) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 14) else SPAN_LOOP_G(1, 1, 0, NEAREST, 14) }
                        else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 14) else SPAN_LOOP_G(1, 0, 0, NEAREST, 14) }
                    }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, BILINEAR, 1) else SPAN_LOOP_G(1, 1, 0, BILINEAR, 1) }
                    else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, BILINEAR, 1) else SPAN_LOOP_G(1, 0, 0, BILINEAR, 1) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_G(1, 1, 1, NEAREST, 1) else SPAN_LOOP_G(1, 1, 0, NEAREST, 1) }
                    else       { if (sUseAlpha) SPAN_LOOP_G(1, 0, 1, NEAREST, 1) else SPAN_LOOP_G(1, 0, 0, NEAREST, 1) }
                }
            } else if (tex && ztest && cm_mod) {
                /* the hot path: z-tested texture x shade */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                }
            } else if (tex) {
                if (flat && ztest) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 0) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 0) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 0) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 0) }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR) else SPAN_LOOP(1, 1, 0, BILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BILINEAR) else SPAN_LOOP(1, 0, 0, BILINEAR) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEAREST) else SPAN_LOOP(1, 1, 0, NEAREST) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEAREST) else SPAN_LOOP(1, 0, 0, NEAREST) }
                }
            } else if (!ztest && !sUseAlpha && rdq == 0 && gdq == 0 && bdq == 0) {
                /* Constant-colour run: no z, no alpha and zero steps, so every pixel of the span is
                 * argb(clamp(rq), clamp(gq), clamp(bq), 255). */
                const uint32_t c = argb((uint8_t)clamp8_sh16(rq), (uint8_t)clamp8_sh16(gq), (uint8_t)clamp8_sh16(bq), 255);
                uint32_t *const row = fbuf + idx;
                for (int i = 0; i < span; i++) row[i] = c;
                idx += span;
            } else {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(0, 1, 1, NEAREST) else SPAN_LOOP(0, 1, 0, NEAREST) }
                else       { if (sUseAlpha) SPAN_LOOP(0, 0, 1, NEAREST) else SPAN_LOOP(0, 0, 0, NEAREST) }
            }
#undef SPAN_LOOP
#undef SPAN_LOOP_G
#undef SPAN_LOOP_CM
#undef SPAN_LOOP_X
#undef COLSTEP_0
#undef COLSTEP_1
#undef GEN_COLOR_NEAREST
#undef GEN_COLOR_BILINEAR
#undef FETCH_NEAREST
#undef FETCH_BILINEAR
            zf += zdx * fspan;
            cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;
            uw += uwdx * fspan;
            vw += vwdx * fspan;
            sw = sw_b; rw = rw_b; /* identical to sw += swdx*span; reciprocal carried */
            x += span;
        }
    }
    return (int)tri_px;
}

static bool sPhase3D SAVESTATE_EXCLUDE, sIntDirty SAVESTATE_EXCLUDE, sNatDirty SAVESTATE_EXCLUDE; /* see the frame section */
static bool sHad3D SAVESTATE_EXCLUDE;         /* any 3D batch this frame */
static bool sIntClean SAVESTATE_EXCLUDE;      /* internal colour is all-zero (cleared by the last composite) */
static bool sUnified SAVESTATE_EXCLUDE;       /* internal res == native: 3D draws straight to native (see frame section) */
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */

/* PM64_OVERDRAW=<frame> (the speed round's census, after OoT's basement one): from that frame on (1: all), the pixels
 * each kind of draw writes, per frame, and the screen's overdraw (all of them over the 320x240 frame); render-to-
 * texture draws apart. */
enum { OD_GEN3D, OD_BLEND3D, OD_OPAQUE3D, OD_UNTEX3D, OD_GEN2D, OD_2D, OD_RT, OD_2DCOPY, OD_2DPLAIN, OD_2DGENERAL, OD_N };
static uint64_t sOd[OD_N] SAVESTATE_EXCLUDE;
static uint32_t sOdFrames SAVESTATE_EXCLUDE;
static int sOdOn SAVESTATE_EXCLUDE = -1, sOdFrom SAVESTATE_EXCLUDE, sOdAll SAVESTATE_EXCLUDE;
static void od_print(void) {
    static const char *k[OD_N] = { "3D generic combiner", "3D blended", "3D opaque", "3D untextured", "2D generic",
                                   "2D", "render-to-texture", "  2D 1:1 copy", "  2D plain texel", "  2D general" };
    uint64_t t = 0;
    for (int i = 0; i < OD_RT; i++) t += sOd[i]; /* (the 2D split's rows are part of OD_2D) */
    fprintf(stderr, "overdraw: %u frames, %.2f screen writes a pixel\n", sOdFrames, sOdFrames ? (double) t / sOdFrames / 76800.0 : 0.0);
    for (int i = 0; i < OD_N; i++)
        fprintf(stderr, "overdraw:   %-20s %8.0f px a frame (%.2f screens)\n", k[i], sOdFrames ? (double) sOd[i] / sOdFrames : 0.0,
                sOdFrames ? (double) sOd[i] / sOdFrames / 76800.0 : 0.0);
}
static inline int od_on(void) {
    if (sOdOn < 0) { const char *e = getenv("HM64_OVERDRAW"); sOdFrom = e ? atoi(e) : 0; sOdOn = sOdFrom > 0 ? 1 : 0; if (sOdOn) atexit(od_print); }
    return sOdOn && sOdAll >= sOdFrom;
}

/* PM64_PIXPROBE=x,y[,from] (debug): every draw that changes that pixel of the frame (320x240 native), with its state,
 * from game frame `from` on -- what drew a wrong pixel (the hit flash's white box). */
static int sPpX SAVESTATE_EXCLUDE = -2, sPpY SAVESTATE_EXCLUDE, sPpFrom SAVESTATE_EXCLUDE;
static int pp_on(void) {
    if (sPpX == -2) { const char *e = getenv("HM64_PIXPROBE"); sPpX = -1; if (e) sscanf(e, "%d,%d,%d", &sPpX, &sPpY, &sPpFrom); }
    if (sPpX < 0 || sRtActive || sFB == NULL || sPpX >= sW || sPpY >= sH) return 0;
    extern uint32_t port_frame __attribute__((weak));
    return !&port_frame || (int) port_frame >= sPpFrom;
}
static void pp_log(const char *what, uint32_t before) {
    const uint32_t now = sFB[sPpY * sW + sPpX];
    if (now == before) return;
    extern uint32_t port_frame __attribute__((weak));
    const struct ShaderProgram *p = sCurShader;
    const SoftTex *t = p && p->cc.used_textures[0] ? &sTex[sBoundTex[0]] : NULL;
    fprintf(stderr, "PIXPROBE f%u %s %08X -> %08X shader %08X rgb_tex %d rgb_in %d a_tex %d a_in %d texa %d decal %d alpha %d edge %d gen %d fast %d gw %08X %08X tex %dx%d t16 %d\n",
            &port_frame ? port_frame : 0, what, before, now, p ? p->shader_id : 0, p ? p->rgb_tex : -1, p ? p->rgb_in : -1,
            p ? p->a_tex : -1, p ? p->a_in : -1, p ? p->rgb_texa : -1, p ? p->rgb_decal : -1, sUseAlpha,
            p ? p->cc.opt_texture_edge : -1, sGen.on, sGen.fast, sGen.w0, sGen.w1, t ? t->w : 0, t ? t->h : 0, t ? t->is16 : -1);
}

#ifdef PM64
/* G_ZS_PRIM (gfx_pc.c pm_primz_sync): 1, triangles write only the prim depth (a blender that keeps the colour);
 * 2, rectangles draw only where the prim depth is nearer than the depth there. Both only when one depth buffer
 * serves the 3D and the 2D (Paper Mario's 320x240). */
static int sPrimZMode SAVESTATE_EXCLUDE, sPrimZ SAVESTATE_EXCLUDE;
void gfx_soft_set_primz(int mode, int z) { sPrimZMode = mode; sPrimZ = z; }
static void primz_tri(const SoftVtx *a, const SoftVtx *b, const SoftVtx *c) {
    const float area = edge(a, b, c->x, c->y);
    if (area == 0.0f) return;
    const float inv = 1.0f / area;
    const int minx = imax(imax((int) fmin3(a->x, b->x, c->x), sScX0), 0), maxx = imin(imin(iceilf(fmax3(a->x, b->x, c->x)), sScX1), sW);
    const int miny = imax(imax((int) fmin3(a->y, b->y, c->y), sScY0), 0), maxy = imin(imin(iceilf(fmax3(a->y, b->y, c->y)), sScY1), sH);
    for (int y = miny; y < maxy; y++) {
        const float py = (float) y + 0.5f;
        uint16_t *zr = sZB + (size_t) y * sW;
        for (int x = minx; x < maxx; x++) {
            const float px = (float) x + 0.5f;
            if (edge(b, c, px, py) * inv >= 0.0f && edge(c, a, px, py) * inv >= 0.0f && edge(a, b, px, py) * inv >= 0.0f)
                zr[x] = (uint16_t) sPrimZ;
        }
    }
}
#endif
static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
#ifdef PM64
    if (sRtActive) rt_touch();
#endif
    (void)buf_vbo_len;
    PROF_BEGIN();
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
#ifdef PM64
    if (sPrimZMode == 1 && sUnified && !sRtActive) {
        for (size_t i = 0; i < num_tris; i++) primz_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2]);
        PROF_END(sPhase3D ? PROF_R3D : PROF_R2D);
        return;
    }
#endif
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
    for (size_t i = 0; i < num_tris; i++) {
        const int ppon = sPpX != -1 && pp_on(); const uint32_t ppb = ppon ? sFB[sPpY * sW + sPpX] : 0;
        const int px = raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
        if (ppon) pp_log("tri", ppb);
#ifdef PM64
        if (sGen.on && sGenCensus != 0 && px > 0) gen_census_add((uint64_t) px, true);
        if (px > 0 && od_on()) {
            const bool tx = sCurShader && sCurShader->cc.used_textures[0];
            sOd[sRtActive ? OD_RT : sGen.on ? OD_GEN3D : !tx ? OD_UNTEX3D : sUseAlpha ? OD_BLEND3D : OD_OPAQUE3D] += (uint64_t) px;
        }
#endif
        /* Profiling counters are 3D-only: this path also serves the ortho
         * (2D-phase) batches, whose time lands in PROF_R2D, and counting their
         * pixels here understated the GFX3D line's ns/px (round 13). */
        if (sPhase3D && px >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)px; }
    }
    PROF_END(sPhase3D ? PROF_R3D : PROF_R2D);
}
/* gfx_pc tells us when a batch is real perspective geometry (the sky quads
 * also render on the internal target but must not count as "3D present"). */
void gfx_scegu_note_persp(void) { sHad3D = true; }

/* 2D path (HUD, menus, logo): gfx_pc.c emits PSP GU_SPRITES — TWO corners per
 * rect in a VertexColor {s16 u,v; RGBA; u16 x,y,z}: screen pixels + TEXEL UVs
 * (tile-origin already subtracted), combiner colour pre-folded by
 * gfx_eval_vertex_color. GU takes the sprite colour from the second vertex. */
typedef struct __attribute__((aligned(16))) {
    int16_t u, v; /* signed, as gfx_pc.c writes them: a texrect may start left of / above its tile origin (ssb64 4843c2c + the DKR session) */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

static void draw_2d_impl(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites);
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    const int ppon = sPpX != -1 && pp_on(); const uint32_t ppb = ppon ? sFB[sPpY * sW + sPpX] : 0;
#ifdef PM64
    if (sPrimZMode == 2 && sUnified && !sRtActive && sZB) {
        /* the compare: the pixels whose depth is at or nearer than the prim depth keep their colour */
        static uint32_t *keepIdx, *keepPx;
        static int keepCap;
        const int n = sW * sH;
        if (keepCap < n) {
            free(keepIdx); free(keepPx);
            keepIdx = (uint32_t *) malloc((size_t) n * 4); keepPx = (uint32_t *) malloc((size_t) n * 4);
            keepCap = keepIdx && keepPx ? n : 0;
        }
        if (keepCap >= n) {
            int k = 0;
            for (int i = 0; i < n; i++)
                if (sZB[i] <= sPrimZ) { keepIdx[k] = (uint32_t) i; keepPx[k++] = sFB[i]; }
            draw_2d_impl(buf_vbo, buf_vbo_len, num_sprites);
            for (int j = 0; j < k; j++) sFB[keepIdx[j]] = keepPx[j];
            if (ppon) pp_log("2d", ppb);
            return;
        }
    }
#endif
    draw_2d_impl(buf_vbo, buf_vbo_len, num_sprites);
    if (ppon) pp_log("2d", ppb);
}
static void draw_2d_impl(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
#ifdef PM64
    if (sRtActive) rt_touch();
#endif
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && sCurShader->cc.used_textures[0]) ? &sTex[sBoundTex[0]] : NULL;
#ifdef PM64
    const bool gen = sGen.on;
    if (gen) tex = (sGen.t0 || sGen.t1) ? &sTex[sBoundTex[sGen.t0 ? 0 : 1]] : NULL;
    const SoftTex *gtex1 = gen && sGen.t0 && sGen.t1 ? &sTex[sBoundTex[1]] : NULL;
    if (gtex1 && !gtex1->valid) { untex_note("rect t1", sBoundTex[1]); gtex1 = NULL; }
#else
    const bool gen = false;
#endif
    if (tex && !tex->valid) { untex_note("rect", (uint32_t) (tex - sTex)); tex = NULL; }
    const bool modulate = !sCurShader || sCurShader->cc.num_inputs > 0;
    sNatDirty = true; /* gfx_pc switches to the 2D phase before calling us */
    PROF_BEGIN();

    for (size_t s = 0; s < num_sprites; s++) {
        const Vtx2D *a = &q[s*2+0], *b = &q[s*2+1];
        {
            extern int gfx_trace_frames;
            extern void port_log(const char *fmt, ...);
            if (gfx_trace_frames > 0)
                port_log("  2d.s: (%u,%u)-(%u,%u) uv(%u,%u)-(%u,%u) col %02X%02X%02X%02X tex %u(%dx%d) ua=%d te=%d mod=%d add=%d\n",
                         a->x, a->y, b->x, b->y, a->u, a->v, b->u, b->v,
                         b->r, b->g, b->b, b->a,
                         tex ? sBoundTex[0] : 0, tex ? tex->w : 0, tex ? tex->h : 0,
                         (int)sUseAlpha,
                         (sCurShader && sCurShader->cc.opt_texture_edge) ? 1 : 0,
                         (int)modulate, (int)sTexAdd);
        }
        int x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
        float u0 = a->u, u1 = b->u, v0 = a->v, v1 = b->v;
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; float tu = u0; u0 = u1; u1 = tu; }
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; float tv = v0; v0 = v1; v1 = tv; }
        if (x1 <= x0 || y1 <= y0) continue;
        const float dudx = (u1 - u0) / (float)(x1 - x0);
        const float dvdy = (v1 - v0) / (float)(y1 - y0);
        int cx0 = imax(imax(x0, sScX0), 0), cx1 = imin(imin(x1, sScX1), sW);
        int cy0 = imax(imax(y0, sScY0), 0), cy1 = imin(imin(y1, sScY1), sH);
#ifdef PM64
        if (gen && sGenCensus != 0 && cx1 > cx0 && cy1 > cy0) gen_census_add((uint64_t) (cx1 - cx0) * (uint64_t) (cy1 - cy0), false);
        if (cx1 > cx0 && cy1 > cy0 && od_on()) sOd[sRtActive ? OD_RT : gen ? OD_GEN2D : OD_2D] += (uint64_t) (cx1 - cx0) * (uint64_t) (cy1 - cy0);
#endif
        const int cr = b->r, cg = b->g, cb2 = b->b, ca = b->a; /* GU: colour of 2nd vertex */
        /* Per-sprite constants hoisted out of the pixel loop. amin folds the
         * texture-edge (0x55) and near-zero (8) alpha rejects. "plain" = the
         * texel is the final colour (no combiner input, or a white opaque
         * prim, which is what nearly every HUD sprite and menu tile uses). */
        const bool use_alpha = sUseAlpha;
        const int amin = use_alpha ? ((sCurShader && sCurShader->cc.opt_texture_edge) ? 0x55 : 8) : 0;
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa, rgb_decal = sCurShader && sCurShader->rgb_decal;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        bool plain = !gen && tex && !t16 && rgb_tex && a_tex && !rgb_texa &&
                     ((!rgb_in && !a_in) || (!sTexAdd && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
        /* ...and opaque, whatever the alpha: without blending the pixel's alpha is never read (argb(..., 255)), so a
         * texel colour as it is copies too. The backgrounds behind every field map (296x6 strips, colour TEXEL0,
         * alpha from the primitive) went through the general loop: 59,200 px, ~3 M ARM instructions a frame
         * (the speed round). PM64_BGCOPY=0: the general loop. */
        static int sBgCopy = -1;
        if (sBgCopy < 0) { const char *e = getenv("HM64_BGCOPY"); sBgCopy = !(e && e[0] == '0'); }
        if (!plain && sBgCopy && !gen && tex && !t16 && rgb_tex && !rgb_texa && !use_alpha &&
            (!rgb_in || (!rgb_decal && !sTexAdd && cr == 255 && cg == 255 && cb2 == 255)))
            plain = true;
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
#ifdef PM64
            if (gen) { /* generic combiner, per pixel */
                const float gv = v0 + (y - y0 + 0.5f) * dvdy;
                const int gso = sGen.t0 ? 0 : sGen.ds, gto = sGen.t0 ? 0 : sGen.dt; /* TEXEL1-only: its own tile's offset */
                const int gfast = sGen.fast; /* gen_classify_fast */
                const int giv = tex ? wrapf(ifloorf(gv) + gto, th, wt2, mv) : 0;
                int32_t guf = to_fix16(u0 + (cx0 - x0 + 0.5f) * dudx);
                /* The pause menu's two combines (fast paths 11 and 12: the backdrop, the panels; the user's pause ran at
                 * 16.6 fps) with their own loops: the operands in registers, the same steps as pm_gen_fast and the
                 * blend below. PM64_GEN2DLOOP=0: the loop below. */
                static int sGen2dLoop = -1;
                if (sGen2dLoop < 0) { const char *e = getenv("HM64_GEN2DLOOP"); sGen2dLoop = !(e && e[0] == '0'); }
                if (sGen2dLoop && !sGen.shroud && tex && sGen.t0 && ((gfast == 11 && !gtex1) || (gfast == 12 && gtex1))) {
                    const int k10 = sGen.k1[0], k11 = sGen.k1[1], k12 = sGen.k1[2];
                    const int k20 = sGen.k2[0], k21 = sGen.k2[1], k22 = sGen.k2[2];
                    const int k30 = sGen.k3s[0] + 0x80, k31 = sGen.k3s[1] + 0x80, k32 = sGen.k3s[2] + 0x80;
                    const void *const p1 = gtex1 ? gtex1->px : NULL;
                    const bool p1_16 = gtex1 && gtex1->is16;
                    const int w1 = gtex1 ? gtex1->w : 0, h1 = gtex1 ? gtex1->h : 0;
                    const int ws1 = gtex1 ? gtex1->wrap_s : 0, wt1 = gtex1 ? gtex1->wrap_t : 0;
                    const int mu1 = gtex1 ? pow2mask(w1) : -1, mv1 = gtex1 ? pow2mask(h1) : -1;
                    const int row1 = gtex1 ? wrapf(ifloorf(gv) + sGen.dt, h1, wt1, mv1) * w1 : 0, ds1 = sGen.ds;
                    const int ka = sGen.k1[3];
                    if (gfast == 11 && use_alpha && ka < amin) continue;
                    for (int x = cx0; x < cx1; x++, guf += duf) {
                        const uint32_t t0 = texel_at(texpx, t16, giv * tw + wrapf((guf >> 16) + gso, tw, ws2, mu));
                        const int tr = (int) ((t0 >> 16) & 255), tg = (int) ((t0 >> 8) & 255), tb = (int) (t0 & 255);
                        int fr, fg, fb, fa;
                        if (gfast == 11) {
                            fa = ka;
                            fr = gm_clamp(((k10 - tr) * k20 + (tr << 8) + 0x80) >> 8);
                            fg = gm_clamp(((k11 - tg) * k21 + (tg << 8) + 0x80) >> 8);
                            fb = gm_clamp(((k12 - tb) * k22 + (tb << 8) + 0x80) >> 8);
                        } else {
                            const uint32_t t1 = texel_at(p1, p1_16, row1 + wrapf((guf >> 16) + ds1, w1, ws1, mu1));
                            fa = (int) (t1 >> 24);
                            if (use_alpha && fa < amin) continue;
                            fr = (gm_clamp(((k10 - k20) * tr + k30) >> 8) * (int) ((t1 >> 16) & 255) + 0x80) >> 8;
                            fg = (gm_clamp(((k11 - k21) * tg + k31) >> 8) * (int) ((t1 >> 8) & 255) + 0x80) >> 8;
                            fb = (gm_clamp(((k12 - k22) * tb + k32) >> 8) * (int) (t1 & 255) + 0x80) >> 8;
                        }
                        if (use_alpha && fa < 255) {
                            const uint32_t d = row[x];
                            const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                            fr = (fr*fa + dr*(255-fa))/255; fg = (fg*fa + dg*(255-fa))/255; fb = (fb*fa + db*(255-fa))/255;
                        }
                        row[x] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);
                    }
                    continue;
                }
                for (int x = cx0; x < cx1; x++, guf += duf) {
                    uint32_t t0 = 0, t1 = 0;
                    if (tex) { const uint32_t tt = texel_at(texpx, t16, giv * tw + wrapf((guf >> 16) + gso, tw, ws2, mu)); if (sGen.t0) t0 = tt; else t1 = tt; }
                    if (gtex1) {
                        /* TEXEL1 at its own tile's shift and corner (gen_t1, as the triangles take it), not at tile 0's
                         * scale plus the corners' whole-texel difference: the bulb glow (effects/bulb_glow.c: the
                         * star rod's bloom, Watt, Kammy's orb) reads the frame through tile 1 at shift 0 while its
                         * glow texture's tile 0 has shift 1, so the frame inside the glow came out at half scale and
                         * moved left by the corners' difference. PM64_T1RECT=0: the old offset. */
                        const int t1u = sT1Rect ? gen_t1(guf, 0) >> 16 : (guf >> 16) + sGen.ds;
                        const int t1v = sT1Rect ? gen_t1(to_fix16(gv), 1) >> 16 : ifloorf(gv) + sGen.dt;
                        t1 = texel_at(gtex1->px, gtex1->is16, wrapf(t1v, gtex1->h, gtex1->wrap_t, pow2mask(gtex1->h)) * gtex1->w +
                                                              wrapf(t1u, gtex1->w, gtex1->wrap_s, pow2mask(gtex1->w)));
                    }
                    const uint32_t c = gfast ? pm_gen_fast(gfast, t0, t1, cr, cg, cb2, ca) : pm_gen_eval(t0, t1, cr, cg, cb2, ca);
                    int fr = (c >> 16) & 255, fg = (c >> 8) & 255, fb = c & 255, fa = (int)(c >> 24);
                    if (use_alpha) {
                        if (fa < amin) continue;
                        if (fa < 255) {
                            const uint32_t d = row[x];
                            const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                            fr = (fr*fa + dr*(255-fa))/255; fg = (fg*fa + dg*(255-fa))/255; fb = (fb*fa + db*(255-fa))/255;
                        }
                    }
                    row[x] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);
                }
                continue;
            }
#endif
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
#ifdef PM64
                    /* A FILL into a texture target writes the fill colour's own alpha bit, as the RDP does: the
                     * file select's windows clear their contents texture to alpha 0 before the text goes in, and
                     * flip onto the screen through it (draw_box, M2). Forcing 255 made the whole texture opaque:
                     * big flat panels in the window colour. PM64_FILLALPHA=0: the old store (A/B). */
                    const uint32_t c = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2,
                                            gfx_soft_fill_raw_alpha >= 0 && sRtActive ? (uint8_t)gfx_soft_fill_raw_alpha : 255);
#else
                    const uint32_t c = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255);
#endif
                    for (int x = cx0; x < cx1; x++) row[x] = c;
                } else if (ca >= amin) {
                    for (int x = cx0; x < cx1; x++) {
                        const uint32_t d = row[x];
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                        row[x] = argb((uint8_t)((cr*ca + dr*(255-ca))/255), (uint8_t)((cg*ca + dg*(255-ca))/255), (uint8_t)((cb2*ca + db*(255-ca))/255), 255);
                    }
                }
                continue;
            }
            const float v = v0 + (y - y0 + 0.5f) * dvdy;
            const int iv = wrapf(ifloorf(v), th, wt2, mv);
            const int trow_base = iv * tw; /* texel index of this scanline's row start */
            int32_t uf = to_fix16(u0 + (cx0 - x0 + 0.5f) * dudx);
            if (plain) {
                /* 1:1 copy inside the texture: straight run, no wrap per texel. */
                const int iu0 = uf >> 16;
                if (duf == 65536 && iu0 >= 0 && iu0 + (cx1 - cx0) <= tw) {
                    const uint32_t *src = (const uint32_t *)texpx + trow_base + iu0;
                    if (sOdOn > 0 && sOdAll >= sOdFrom) sOd[OD_2DCOPY] += (uint64_t) (cx1 - cx0);
                    if (!use_alpha) {
                        for (int x = cx0; x < cx1; x++) row[x] = src[x - cx0] | 0xFF000000u;
                    } else {
                        for (int x = cx0; x < cx1; x++) {
                            const uint32_t t = src[x - cx0]; const int ta = (int)(t >> 24);
                            if (ta < amin) continue;
                            if (ta >= 255) { row[x] = t | 0xFF000000u; continue; }
                            const uint32_t d = row[x];
                            const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                            row[x] = argb((uint8_t)((((t>>16)&0xff)*ta + dr*(255-ta))/255), (uint8_t)((((t>>8)&0xff)*ta + dg*(255-ta))/255), (uint8_t)(((t&0xff)*ta + db*(255-ta))/255), 255);
                        }
                    }
                    continue;
                }
                if (sOdOn > 0 && sOdAll >= sOdFrom) sOd[OD_2DPLAIN] += (uint64_t) (cx1 - cx0);
                for (int x = cx0; x < cx1; x++, uf += duf) {
                    const uint32_t t = texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                    if (!use_alpha) { row[x] = t | 0xFF000000u; continue; }
                    const int ta = (int)(t >> 24);
                    if (ta < amin) continue;
                    if (ta >= 255) { row[x] = t | 0xFF000000u; continue; }
                    const uint32_t d = row[x];
                    const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                    row[x] = argb((uint8_t)((((t>>16)&0xff)*ta + dr*(255-ta))/255), (uint8_t)((((t>>8)&0xff)*ta + dg*(255-ta))/255), (uint8_t)(((t&0xff)*ta + db*(255-ta))/255), 255);
                }
                continue;
            }
            /* General: per-channel combiner (RGB and alpha each from the
             * texel, the input colour, or their product). */
            if (sOdOn > 0 && sOdAll >= sOdFrom) sOd[OD_2DGENERAL] += (uint64_t) (cx1 - cx0);
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                const int ta = (t>>24)&0xff;
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff;
                int fr, fg, fb, fa;
                if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }
                else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }
                else if (rgb_decal) { fr=(tr*ta + cr*(255-ta))/255; fg=(tg*ta + cg*(255-ta))/255; fb=(tb*ta + cb2*(255-ta))/255; }
                else if (sTexAdd) { fr=tr+cr; fg=tg+cg; fb=tb+cb2; if (fr>255)fr=255; if (fg>255)fg=255; if (fb>255)fb=255; }
                else              { fr=(tr*cr)/255; fg=(tg*cg)/255; fb=(tb*cb2)/255; }
                fa = !a_tex ? ca : (!a_in ? ta : (ta*ca)/255);
                if (use_alpha) {
                    if (fa < amin) continue;
                    if (fa < 255) {
                        const uint32_t d = row[x];
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                        fr = (fr*fa + dr*(255-fa))/255;
                        fg = (fg*fa + dg*(255-fa))/255;
                        fb = (fb*fa + db*(255-fa))/255;
                    }
                }
                row[x] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);
            }
        }
    }
    PROF_END(PROF_R2D);
}

/* -------------------------------------------------------------- frame --- */
/* Two render targets (device round 5: "only downscale the 3D, keep text and
 * icons crisp"):
 *   NATIVE   -- the mode's base size (the output size for CONVERT / 320x240 STRETCH,CROP).
 *               All 2D (texrects, ortho HUD quads, menus) draws here, always
 *               full-resolution. This is what gets presented.
 *   INTERNAL -- native scaled by the dynamic-res ladder. Perspective 3D draws
 *               here. When the frame switches to a 2D draw (gfx_pc calls
 *               gfx_scegu_set_phase(0)) the internal image is nearest-upscaled
 *               INTO native, coverage-masked by its alpha (cleared to 0, the
 *               rasterizer writes 255), then cleared so later 3D batches can
 *               composite on top again. Depth stays per-target.
 * sFB/sZB/sW/sH always alias the CURRENT target so the raster code is
 * unchanged. gfx_current_dimensions follows the current target so gfx_pc's
 * viewport/scissor/2D mapping use the right size. */
static uint16_t *sNatZB SAVESTATE_EXCLUDE;
static uint32_t *sIntFB SAVESTATE_EXCLUDE; static uint16_t *sIntZB SAVESTATE_EXCLUDE;
static int sNatW SAVESTATE_EXCLUDE, sNatH SAVESTATE_EXCLUDE, sIntW SAVESTATE_EXCLUDE, sIntH SAVESTATE_EXCLUDE;
static int sWantNatW SAVESTATE_EXCLUDE, sWantNatH SAVESTATE_EXCLUDE, sWantIntW SAVESTATE_EXCLUDE, sWantIntH SAVESTATE_EXCLUDE; /* requested by the WM */
/* Internal size == native (ladder at 100%): the two-target split is pure
 * overhead there, so 3D draws STRAIGHT to native and every seed/composite is
 * skipped. This makes the top rung the CHEAPEST one (no ~2 ms composite), and
 * it's also the most correct compositing order -- sky, track, karts and the
 * alpha-blended item boxes all blend against what's actually beneath them on
 * one buffer, exactly as the N64 does. (sUnified is declared with the phase
 * flags above so the raster path can see it.) */
/* Exact nearest-neighbour maps between the targets (x * w_to / w_from with
 * integer division, no 16.16 truncation): quadrant edges land on the same
 * pixel in both, so the results-screen scenes meet their black panels with
 * no seam (device round 5). Rebuilt whenever a size changes. */
#define MAP_MAX 512
static uint16_t sN2I_x[MAP_MAX] SAVESTATE_EXCLUDE, sN2I_y[MAP_MAX] SAVESTATE_EXCLUDE; /* native px -> internal px */
static uint16_t sI2N_x[MAP_MAX] SAVESTATE_EXCLUDE, sI2N_y[MAP_MAX] SAVESTATE_EXCLUDE; /* internal px -> native px */
static void build_maps(void) {
    if (sNatW <= 0 || sNatH <= 0 || sIntW <= 0 || sIntH <= 0) return;
    for (int x = 0; x < sNatW && x < MAP_MAX; x++) sN2I_x[x] = (uint16_t)(x * sIntW / sNatW);
    for (int y = 0; y < sNatH && y < MAP_MAX; y++) sN2I_y[y] = (uint16_t)(y * sIntH / sNatH);
    for (int x = 0; x < sIntW && x < MAP_MAX; x++) sI2N_x[x] = (uint16_t)(x * sNatW / sIntW);
    for (int y = 0; y < sIntH && y < MAP_MAX; y++) sI2N_y[y] = (uint16_t)(y * sNatH / sIntH);
}
/* Presented image: always the native target (composited at end_frame). */
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }
#ifdef PM64
/* The 3D depth at a native pixel (0..65535, larger = farther: z/w * 0.5 + 0.5 of the whole range), -1 off screen: Paper
 * Mario's depth queries (port_gfx.c port_depth_query) */
int gfx_soft_depth_at(int x, int y) {
    const bool intz = !sUnified && sIntZB;
    const int w = intz ? sIntW : sNatW, h = intz ? sIntH : sNatH;
    const uint16_t *zb = intz ? sIntZB : sNatZB;
    if (!zb || sNatW <= 0 || sNatH <= 0) return -1;
    x = x * w / sNatW; y = y * h / sNatH;
    if (x < 0 || y < 0 || x >= w || y >= h) return -1;
    return zb[y * w + x];
}
#endif

static void alloc_target(uint32_t **fb, uint16_t **zb, int *w, int *h, int nw, int nh) {
    if (*fb && *w == nw && *h == nh) return;
    free(*fb); free(*zb);
    *w = nw; *h = nh;
    *fb = (uint32_t *)malloc((size_t)nw * nh * 4);
    *zb = (uint16_t *)malloc((size_t)nw * nh * 2);
}

#ifdef PM64
/* Render to texture (Paper Mario draws into other colour images: the RDP-computed
 * sprite-shading palettes before every lit actor, 3D windows, the tattle camera ...).
 * While a SETCIMG points away from the framebuffers, drawing goes to this CPU target at
 * the N64's own resolution (width from SETCIMG, 240 rows so gfx_pc's 320x240 mapping is
 * 1:1). Its pixels live in RDRAM in N64 format: each draw first loads the scissored
 * rectangle it can touch (big-endian RGBA16 / I8 -> ARGB), and the end of the target
 * writes the loaded rectangle back, where the texture importer reads it (content-hashed,
 * so the change is seen). Rectangles, not whole buffers: most targets are 1..48 px. */
static uint32_t *sRtFB SAVESTATE_EXCLUDE;
static uint16_t *sRtZB SAVESTATE_EXCLUDE;
static int sRtW SAVESTATE_EXCLUDE, sRtH SAVESTATE_EXCLUDE, sRtRows SAVESTATE_EXCLUDE, sRtSiz SAVESTATE_EXCLUDE;
static uint8_t *sRtMem SAVESTATE_EXCLUDE;
static int sRtX0 SAVESTATE_EXCLUDE, sRtY0 SAVESTATE_EXCLUDE, sRtX1 SAVESTATE_EXCLUDE, sRtY1 SAVESTATE_EXCLUDE; /* loaded rect, empty if x0>=x1 */
static void rt_xfer(bool load) {
    for (int y = sRtY0; y < sRtY1; y++) {
        for (int x = sRtX0; x < sRtX1; x++) {
            uint32_t *px = &sRtFB[y * sRtW + x];
            if (sRtSiz == 2) { /* RGBA16, big-endian */
                uint8_t *m = sRtMem + (y * sRtW + x) * 2;
                if (load) {
                    const uint16_t c = (uint16_t)(m[0] << 8 | m[1]);
                    const uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
                    *px = (c & 1 ? 0xFF000000u : 0) | (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
                } else {
                    const uint32_t v = *px;
                    const uint16_t c = (uint16_t)(((v >> 19) & 31) << 11 | ((v >> 11) & 31) << 6 | ((v >> 3) & 31) << 1 | (v >> 31));
                    m[0] = (uint8_t)(c >> 8); m[1] = (uint8_t)c;
                }
            } else if (sRtSiz == 1) { /* I8 */
                uint8_t *m = sRtMem + y * sRtW + x;
                if (load) *px = 0xFF000000u | *m * 0x010101u;
                else *m = (uint8_t)(((*px >> 16 & 255) + (*px >> 8 & 255) + (*px & 255)) / 3);
            } else { /* RGBA32 */
                uint8_t *m = sRtMem + (y * sRtW + x) * 4;
                if (load) *px = (uint32_t)m[3] << 24 | (uint32_t)m[0] << 16 | (uint32_t)m[1] << 8 | m[2];
                else { const uint32_t v = *px; m[0] = v >> 16; m[1] = v >> 8; m[2] = (uint8_t)v; m[3] = v >> 24; }
            }
        }
    }
}
/* Make sure the current scissor rectangle is loaded (called before every draw). */
static void rt_touch(void) {
    int x0 = sScX0, y0 = sScY0, x1 = sScX1, y1 = sScY1 > sRtRows ? sRtRows : sScY1;
    if (x0 >= x1 || y0 >= y1) return;
    if (sRtX0 < sRtX1 && x0 >= sRtX0 && y0 >= sRtY0 && x1 <= sRtX1 && y1 <= sRtY1) return;
    if (sRtX0 < sRtX1) { /* grow: write back what we have, reload the union */
        rt_xfer(false);
        if (sRtX0 < x0) x0 = sRtX0; if (sRtY0 < y0) y0 = sRtY0; if (sRtX1 > x1) x1 = sRtX1; if (sRtY1 > y1) y1 = sRtY1;
    }
    sRtX0 = x0; sRtY0 = y0; sRtX1 = x1; sRtY1 = y1;
    rt_xfer(true);
}
static void select_target(bool is3d);
static void rt_zclear(void) { memset(sRtZB, 0xFF, (size_t)sRtW * 240 * 2); sRtZDirty = false; }
void gfx_soft_rt_begin(void *mem, int width, int siz, int rows) {
    extern void gfx_soft_rt_end(void);
    if (sRtActive) gfx_soft_rt_end();
    if (width < 1 || width > 1024) return;
    if (!sRtFB || sRtW != width) {
        free(sRtFB); free(sRtZB);
        sRtFB = (uint32_t *)malloc((size_t)width * 240 * 4);
        sRtZB = (uint16_t *)malloc((size_t)width * 240 * 2);
    }
    /* The depth clear waits for the first depth-tested triangle (raster_tri; round 16): 7_battle begins 60 targets a
     * pass, 320 wide, for 16x1 palette strips that never test depth, and cleared 153.6 KB each (~9 MB a pass). */
    sRtZDirty = true;
    sRtW = width; sRtH = 240; sRtRows = rows < 240 ? rows : 240; sRtSiz = siz; sRtMem = (uint8_t *)mem;
    sRtX0 = sRtY0 = sRtX1 = sRtY1 = 0;
    sRtActive = true;
    select_target(sPhase3D);
}
void gfx_soft_rt_end(void) {
    if (!sRtActive) return;
    if (sRtX0 < sRtX1) rt_xfer(false);
    static int sRtLog = -1;
    if (sRtLog < 0) sRtLog = getenv("HM64_RTLOG") != NULL;
    if (sRtLog)
        port_log("rt end: mem %p w %d siz %d rect [%d,%d)x[%d,%d) px0 %08X px1 %08X\n", sRtMem, sRtW, sRtSiz, sRtX0, sRtX1, sRtY0, sRtY1,
                 sRtX0 < sRtX1 ? sRtFB[sRtY0 * sRtW + sRtX0] : 0, sRtX0 + 1 < sRtX1 ? sRtFB[sRtY0 * sRtW + sRtX0 + 1] : 0);
    sRtActive = false;
    select_target(sPhase3D);
}
#endif
static void select_target(bool is3d) {
    extern struct GfxDimensions gfx_current_dimensions;
    sPhase3D = is3d;
#ifdef PM64
    if (sRtActive) { sFB = sRtFB; sZB = sRtZB; sW = sRtW; sH = sRtH; }
    else
#endif
    if (is3d && !sUnified) { sFB = sIntFB; sZB = sIntZB; sW = sIntW; sH = sIntH; }
    else                   { sFB = sNatFB; sZB = sNatZB; sW = sNatW; sH = sNatH; }
    sScX0 = 0; sScY0 = 0; sScX1 = sW; sScY1 = sH;
    sVpX = 0; sVpY = 0; sVpW = sW; sVpH = sH;
    gfx_current_dimensions.width = (uint32_t)sW;
    gfx_current_dimensions.height = (uint32_t)sH;
    gfx_current_dimensions.aspect_ratio = sH ? (float)sW / (float)sH : 1.0f;
#ifdef PM64
    if (sRtActive) { /* N64 pixels map 1:1 onto a texture target: present the 320x240 frame to gfx_pc, clip to the real width here */
        gfx_current_dimensions.width = 320; gfx_current_dimensions.height = 240; gfx_current_dimensions.aspect_ratio = 4.0f / 3.0f;
    }
#endif
}

/* Nearest-upscale the internal 3D image into native where the 3D wrote
 * (alpha == 255), then clear it for any later 3D batch this frame. */
static void composite_internal(void) {
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    PROF_BEGIN();
    if (sIntW == sNatW && sIntH == sNatH) {
        for (int i = 0; i < sNatW * sNatH; i++) if (sIntFB[i] >> 24) sNatFB[i] = sIntFB[i];
    } else {
        for (int y = 0; y < sNatH; y++) {
            const uint32_t *src = sIntFB + sN2I_y[y] * sIntW;
            uint32_t *dst = sNatFB + y * sNatW;
            for (int x = 0; x < sNatW; x++) {
                const uint32_t p = src[sN2I_x[x]];
                if (p >> 24) dst[x] = p;
            }
        }
    }
    memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
    sIntDirty = false;
    sNatDirty = true;
    sIntClean = true; /* colour is cleared; start_frame can skip its own memset */
    PROF_END(PROF_COMP);
}

/* WM -> backend: base (presented) size and the ladder's internal 3D size.
 * Buffers are (re)allocated lazily at the next start_frame. */
void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h) {
    sWantNatW = nat_w; sWantNatH = nat_h;
    sWantIntW = int_w > 0 ? int_w : nat_w;
    sWantIntH = int_h > 0 ? int_h : nat_h;
}
void gfx_soft_get_internal(int *w, int *h) { if (w) *w = sIntW; if (h) *h = sIntH; }

/* Entering 3D after 2D content exists (sky quads, menu backdrop): seed the
 * internal colour with a nearest-downsample of native so alpha-blended 3D
 * (item boxes, exhaust) blends against the real backdrop. Alpha is written 0
 * so those pixels still count as "not drawn" for the composite. */
static void seed_internal_from_native(void) {
    PROF_BEGIN();
    sIntClean = false;
    if (sIntW == sNatW && sIntH == sNatH) {
        for (int i = 0; i < sIntW * sIntH; i++) sIntFB[i] = sNatFB[i] & 0x00FFFFFFu;
        PROF_END(PROF_COMP);
        return;
    }
    for (int y = 0; y < sIntH; y++) {
        const uint32_t *src = sNatFB + sI2N_y[y] * sNatW;
        uint32_t *dst = sIntFB + y * sIntW;
        for (int x = 0; x < sIntW; x++) dst[x] = src[sI2N_x[x]] & 0x00FFFFFFu;
    }
    PROF_END(PROF_COMP);
}

/* gfx_pc direct-call: switch the draw target for the next batch. */
void gfx_scegu_set_phase(int is3d) {
    if ((bool)is3d == sPhase3D) return;
    gfx_soft_phase_switches++;
    if (!sUnified) { /* unified: everything is already on native, nothing to move */
        if (!is3d && sIntDirty) composite_internal();
        if (is3d && sNatDirty) seed_internal_from_native();
    }
    select_target((bool)is3d);
}

static void ensure_targets(void) {
    extern struct GfxDimensions gfx_current_dimensions;
    if (sWantNatW <= 0) { /* no WM request yet: native = whatever gfx_pc has */
        sWantNatW = (int)gfx_current_dimensions.width; sWantNatH = (int)gfx_current_dimensions.height;
        sWantIntW = sWantNatW; sWantIntH = sWantNatH;
    }
    const int ow = sNatW, oh = sNatH, iw = sIntW, ih = sIntH;
    alloc_target(&sNatFB, &sNatZB, &sNatW, &sNatH, sWantNatW, sWantNatH);
    alloc_target(&sIntFB, &sIntZB, &sIntW, &sIntH, sWantIntW, sWantIntH);
    sUnified = (sIntW == sNatW && sIntH == sNatH);
    sGateScale = (sNatW > 0 && sNatH > 0) ? (float)((double)sIntW * sIntH / ((double)sNatW * sNatH)) : 1.0f;
    if (ow != sNatW || oh != sNatH || iw != sIntW || ih != sIntH) { build_maps(); sIntClean = false; }
}

static void gfx_soft_init(void) {
#if defined(HH_CM_NEON)
    { const char *e = getenv("HM64_CMNEON"); if (e && e[0] == '0') sCmNeon = 0; }
#if defined(HH_CM_NEON)
    { const char *e = getenv("HM64_F8A"); if (e && e[0] == '0') sF8A = 0; }
    { const char *e = getenv("HM64_CMSPLIT"); if (e && e[0] == '0') sCmSplit = 0; }
    { const char *e = getenv("HM64_FASTROWS"); if (e && e[0] == '0') sFastRows = 0; } /* the speed round (2026-10-07) */
    { const char *e = getenv("HM64_ZSKIP"); if (e && e[0] == '0') sZSkip = 0; }
#endif
#endif
    { const char *e = getenv("HM64_MAGBILIN"); if (e && e[0]) gfx_soft_magbilin = e[0] != '0'; }
    { const char *e = getenv("HM64_HUDEXACT"); sHudExact = !(e && e[0] == '0'); }
    { const char *e = getenv("HM64_CC9BIT"); sCc9 = !(e && e[0] == '0'); }
    { const char *e = getenv("HM64_T1RECT"); sT1Rect = !(e && e[0] == '0'); }
    if (!sTexArena) {
        sTexArena = aligned_alloc(16, TEXARENA_SZ);
        if (!sTexArena) { fprintf(stderr, "gfx_soft: cannot allocate the %u MB texture arena\n", (unsigned) (TEXARENA_SZ >> 20)); abort(); }
    }
    ensure_targets(); select_target(false);
}
static void gfx_soft_on_resize(void) { ensure_targets(); select_target(sPhase3D); }
u32 gfx_soft_frames_rendered SAVESTATE_EXCLUDE; /* frames that reached end_frame (frameskip excluded) */
#ifdef PM64
/* Paper Mario clears its own buffers every frame (a fill rectangle on the colour
 * image, and one on the depth image: gfx_pc routes that here), and draws one frame
 * as several gfx tasks (background, then scene) into the same framebuffer, so the
 * backend must not clear at the start of each task. */
void gfx_soft_clear_depth(void) {
    PROF_BEGIN();
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    PROF_END(PROF_CLR);
}
#endif
static void gfx_soft_start_frame(void) {
#ifdef PM64
    sFireTri = 0;
#endif
    if (sOdOn > 0) { /* by game frame (port_frame) where there is one: a game frame can hold several renderer frames */
        extern uint32_t port_frame __attribute__((weak));
        static uint32_t last = 0xFFFFFFFFu;
        const uint32_t f = &port_frame ? port_frame : ++sOdAll;
        if (&port_frame) sOdAll = (int) f;
        if (f != last) { last = f; if (sOdAll >= sOdFrom) sOdFrames++; }
    }
    ensure_targets();
#ifndef PM64
    PROF_BEGIN();
    memset(sNatFB, 0, (size_t)sNatW * sNatH * 4);
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    PROF_END(PROF_CLR);
#endif
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
static void gfx_soft_end_frame(void) {
    if (!sUnified && sIntDirty) composite_internal();
    gfx_soft_last_frame_had_3d = sHad3D;
    gfx_soft_frames_rendered++;
    select_target(false); /* leave native current for present/menu/dumps */
}
static void gfx_soft_finish_render(void) { }

/* -------------------------------------------------------------- shims --- */
/* gfx_pc.c calls these sceGu/texman helpers directly; software equivalents. */
void gfx_scegu_set_cull_mode(uint32_t cull) { (void)cull; /* GE-only API; winding cull lives in gfx_sp_tri1 */ }
void gfx_scegu_set_texfunc_add(bool on) { sTexAdd = on; }
/* a texture edge without FORCE_BL (gfx_pc, X12): the RDP writes such a pixel as the texel's own colour or not at all */
void gfx_soft_set_edge_opaque(bool on) { sEdgeOpaque = on; }
/* PM64_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; }
void gfx_scegu_sync_pending(void) { }
void gfx_scegu_upload_texture(const uint8_t *buf, int w, int h, unsigned int type) { gfx_soft_upload_texture(buf, w, h, type); }
int  gfx_vram_space_available(void) { return 1; }

int  texman_usage_percent(void) { return (int)(sArenaUsed * 100 / TEXARENA_SZ); }
#ifdef PM64
/* bytes is sized by the source format (2 a texel for 5551, which is stored unpacked at 4):
 * count it twice, so the coordinated flush happens before an upload could be dropped. */
int  texman_can_hold(unsigned int num, unsigned int bytes) { (void)num; return sArenaUsed + (size_t)bytes * 2u <= TEXARENA_SZ; }
#else
int  texman_can_hold(unsigned int num, unsigned int bytes) { (void)num; return sArenaUsed + bytes <= TEXARENA_SZ; }
#endif
void texman_bind_tex(unsigned int num) { sBoundTex[sTexUploadTile] = num; }
void texman_clear(void) { sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; }

/* Framebuffer readback (copy_framebuffer): the race-start wipe and the
 * stadium TV screens ask for small patches of the PREVIOUS frame at N64
 * 320x240 coordinates, stored as big-endian N64 RGBA16 tiles that re-enter
 * through the normal texture import next frame. The soft renderer owns its
 * framebuffer, so unlike the PSP's deferred GPU readback this is a direct
 * nearest-neighbour copy from sFB (which still holds the last rendered frame
 * while the game builds the next display list). The stub here was why the
 * transition wipe drew see-through and the Jumbotrons were blank. */
void port_fb_copy_request(int x, int y, int w, int h, uint16_t *target) {
    /* Always read the NATIVE (presented, composited) target -- the internal
     * 3D buffer is cleared once composited and may be a different size. */
    const uint32_t *fb = sNatFB; const int fw = sNatW, fh = sNatH;
    if (!fb || !target || w <= 0 || h <= 0 || w > 320 || h > 240 || fw <= 0 || fh <= 0) return;
    { extern void port_fb_tile_note(void *target, unsigned int bytes); port_fb_tile_note(target, (unsigned int)(w * h * 2)); } /* rewritten every frame: keeps its content hash */
    for (int ty = 0; ty < h; ty++) {
        int fy = (y + ty) * fh / 240;
        if (fy < 0) fy = 0;
        if (fy >= fh) fy = fh - 1;
        const uint32_t *row = &fb[fy * fw];
        for (int tx = 0; tx < w; tx++) {
            int fx = (x + tx) * fw / 320;
            if (fx < 0) fx = 0;
            if (fx >= fw) fx = fw - 1;
            const uint32_t p = row[fx];
            /* ARGB8888 -> N64 RGBA16 (r[15:11] g[10:6] b[5:1] a[0]=1), BE. */
            const uint16_t v = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07C0) | ((p >> 2) & 0x003E) | 1);
            target[ty * w + tx] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

#ifdef PM64
/* The frame drawn so far, N64 rows y0..y1 (320 pixels each), into fb, the framebuffer's memory, as RGBA16 in the N64's
 * byte order (gfx_pc.c pm_frame_texture_sync: a texture load from the framebuffer being drawn). The caller has drawn
 * what was queued. Reads only: the native target, with the internal 3D target's drawn pixels over it when the two
 * are apart (what composite_internal would make of them). Returns 1 when a byte of fb changed. */
int gfx_soft_frame_rows(uint8_t *fb, int y0, int y1) {
    if (!sNatFB || !fb || sNatW <= 0 || sNatH <= 0) return 0;
    if (y0 < 0) y0 = 0;
    if (y1 > 239) y1 = 239;
    const bool in3d = !sUnified && sIntDirty && sIntFB && sNatW <= MAP_MAX && sNatH <= MAP_MAX;
    int changed = 0;
    for (int y = y0; y <= y1; y++) {
        const int fy = y * sNatH / 240;
        const uint32_t *nat = sNatFB + (size_t) fy * sNatW;
        const uint32_t *in = in3d ? sIntFB + (size_t) sN2I_y[fy] * sIntW : NULL;
        uint8_t *m = fb + (size_t) y * 640;
        for (int x = 0; x < 320; x++) {
            const int fx = x * sNatW / 320;
            uint32_t p = nat[fx];
            if (in) { const uint32_t q = in[sN2I_x[fx]]; if (q >> 24) p = q; }
            /* ARGB8888 -> r[15:11] g[10:6] b[5:1] a[0] = 1, big-endian (port_fb_copy_request's packing) */
            const uint16_t c = (uint16_t) (((p >> 8) & 0xF800) | ((p >> 5) & 0x07C0) | ((p >> 2) & 0x003E) | 1);
            if (m[2 * x] != (uint8_t) (c >> 8) || m[2 * x + 1] != (uint8_t) c) {
                m[2 * x] = (uint8_t) (c >> 8); m[2 * x + 1] = (uint8_t) c;
                changed = 1;
            }
        }
    }
    return changed;
}
#endif

/* Debug: dump the framebuffer as a binary PPM (for headless verification). */
void gfx_soft_dump_ppm(const char *path) {
    if (!sNatFB) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sNatW, sNatH);
    for (int i = 0; i < sNatW * sNatH; i++) {
        uint32_t p = sNatFB[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

struct GfxRenderingAPI gfx_soft_api = {
    gfx_soft_z_is_from_0_to_1,
    gfx_soft_unload_shader,
    gfx_soft_load_shader,
    gfx_soft_create_and_load_new_shader,
    gfx_soft_lookup_shader,
    gfx_soft_shader_get_info,
    gfx_soft_new_texture,
    gfx_soft_select_texture,
    gfx_soft_upload_texture,
    gfx_soft_set_sampler_parameters,
    gfx_soft_set_depth_test,
    gfx_soft_set_depth_mask,
    gfx_soft_set_zmode_decal,
    gfx_soft_set_viewport,
    gfx_soft_set_scissor,
    gfx_soft_set_use_alpha,
    gfx_soft_draw_triangles,
    gfx_soft_init,
    gfx_soft_on_resize,
    gfx_soft_start_frame,
    gfx_soft_end_frame,
    gfx_soft_finish_render,
};
