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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include "savestate.h"
#if defined(MIYOO) && defined(__GLIBC__)
void *aligned_alloc(size_t alignment, size_t size); /* C11: glibc's stdlib.h declares it only for C11 (we build gnu99) */
#endif
#include "../port.h"
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
/* Lazy native clear + deferred full-screen fill (MK64 perf round 24, ocarina-of-time
 * d267ca3 / 84d6940): the frame's clear -- or the full-screen opaque solid rect
 * most frames open with -- is written only when native is first touched, and a
 * later full-screen fill replaces it unwritten. SF64_LAZYCLEAR=0: off (A/B). */
static bool sNatStale SAVESTATE_EXCLUDE;       /* native holds nothing of this frame yet */
static uint32_t sNatFill SAVESTATE_EXCLUDE;    /* what it will hold when touched */
static int sLazyClear SAVESTATE_EXCLUDE = -1;
static void nat_ready(void);
static void target_ready(void);


/* -------------------------------------------------------------- textures --- */
enum { WRAP_REPEAT = 0, WRAP_CLAMP = 1, WRAP_MIRROR = 2 };
#define MAX_TEXTURES 4096
/* 20 MB (was 12): 16-bit textures are now stored unpacked (4 bytes a texel),
 * and they are ~65-87% of the arena in Corneria. Pages are touched only as
 * the arena fills (the 85% reset in gfx_pc bounds it at ~17 MB). */
#define TEXARENA_SZ  (20 * 1024 * 1024)

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
 * zip is code only and should be ~1 MB). Allocated once in gfx_soft_init. */
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
static int sEdgeOpaque SAVESTATE_EXCLUDE;         /* gfx_soft_set_edge_opaque (X12): 0 blend, 1 never, 2 not over an empty pixel */
static bool sOwnTri SAVESTATE_EXCLUDE;     /* gfx_soft_set_own_edges (SF64_FILLRULE) */
static int sTexUploadTile SAVESTATE_EXCLUDE;      /* tile targeted by the next upload (select_texture) */

/* SF64_FASTSPAN=0: the generic span loops only (A/B for tools/handheld/sweep.sh). */
static bool sFastSpans SAVESTATE_EXCLUDE = true;
static inline uint32_t argb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* PERF counters (main.c's TEXMEM line): arena bytes held by 16-bit
 * textures, the arena's peak since the last report, and 3D/2D target switches
 * per frame (each non-unified switch costs a seed or a composite). */
size_t gfx_soft_arena16 SAVESTATE_EXCLUDE, gfx_soft_arena_peak SAVESTATE_EXCLUDE;
uint32_t gfx_soft_phase_sw SAVESTATE_EXCLUDE, gfx_soft_phase_sw_max SAVESTATE_EXCLUDE;
static uint32_t sPhaseSwFrame SAVESTATE_EXCLUDE;
size_t gfx_soft_arena_size(void) { return TEXARENA_SZ; }
size_t gfx_soft_arena_used(void);

static uint32_t gfx_soft_new_texture(void) {
    if (sNextTexId >= MAX_TEXTURES) return 0;
    return sNextTexId++;
}

static void gfx_soft_select_texture(int tile, uint32_t texture_id) {
    if (tile < 0 || tile > 1) return;
    sBoundTex[tile] = texture_id;
    sTexUploadTile = tile;
}

/* type: GU_PSM_8888 (RGBA bytes) or GU_PSM_5551 (u16). Both are stored as
 * ARGB8888: 5551 is unpacked here once instead of on every texel fetch
 * (diddy-kong-racing a0ee8b5: the per-fetch unpack was ~11% of raster instructions;
 * unpack5551 is the same function, so the output is bit-identical). */
const void *gfx_soft_tex_src; /* SF64_FRAMESTAT / drawprof: the source of the texture being uploaded (gfx_pc) */
static const void *sTexSrc[MAX_TEXTURES] SAVESTATE_EXCLUDE;
static uint8_t sTexLow[MAX_TEXTURES][32] SAVESTATE_EXCLUDE; /* [k]: texels with alpha < 8 (k + 1), x 255 / all */
static void gfx_soft_upload_texture(const uint8_t *buf, int width, int height, unsigned int type) {
    PROF_BEGIN();
    uint32_t id = sBoundTex[sTexUploadTile];
    if (id == 0 || id >= MAX_TEXTURES || width <= 0 || height <= 0) return;
    sTexSrc[id] = gfx_soft_tex_src;
    const size_t n = (size_t)width * height;
    const bool is16 = (type == GU_PSM_5551);   /* the source format; storage is always 32-bit */
    const size_t bytes = ((n * 4) + 15u) & ~(size_t)15; /* keep 16-byte alignment */
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
    if (is16) gfx_soft_arena16 += bytes;
    if (sArenaUsed > gfx_soft_arena_peak) gfx_soft_arena_peak = sArenaUsed;
    if (is16) {
        const uint16_t *src16 = (const uint16_t *)buf;
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = unpack5551(src16[i]);
    } else { /* GU_PSM_8888: RGBA bytes -> ARGB8888 */
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = argb(buf[i*4+0], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    }
    sTex[id].px = dst; sTex[id].is16 = false; sTex[id].w = width; sTex[id].h = height; sTex[id].valid = true;
    { /* SF64_CELLSKIP's pick (raster_tri): the share of texels below each alpha step of 8, cumulative, in 255ths */
        uint32_t h[32] = { 0 };
        const uint32_t *d = (const uint32_t *)dst;
        for (size_t i = 0; i < n; i++) h[d[i] >> 27]++;
        uint32_t c = 0;
        for (int k = 0; k < 32; k++) { c += h[k]; sTexLow[id][k] = (uint8_t)(c * 255u / n); }
    }
    { /* SF64_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("SF64_TEXDUMP"); }
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
/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}
/* float -> int32 as the device's VFP converts (vcvt saturates, NaN gives 0), so the x86 proxy
 * agrees with it where a plain cast is undefined: a near-degenerate triangle's colour step can
 * reach ~1e10 in 16.16, and x86's cvttss2si gives INT_MIN there (mario-kart-64 b2dff40, the
 * lead's R31; make PLAT=pc UBSAN=1 finds it). */
static inline int32_t sat_i32(float f) {
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
}

/* 3D bilinear filtering (menu "3D FILTER: SMOOTH"; off by default). uf/vf are
 * 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* 1 (default again, round 12): bilinear only when magnified. 0 (P17, kit k1.2; SF64_MAGBILIN=0):
 * every textured pixel bilinear, as the N64 filters -- the gate chose bilinear or nearest per
 * triangle, so a floor could switch between smooth and sharp along triangle edges (DKR's floor
 * "flashing"). Star Fox never showed that, and P17 cost +3.3% ARM instructions while changing
 * nothing visible on a square output: the user's rule keeps it off (hh_game.h HH_MAGBILIN_DEFAULT). */
int gfx_soft_magbilin SAVESTATE_EXCLUDE = 1;
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
static bool sTexLerp;      /* out = K + (vertex - K) * texel: (A - K) * TEXEL0 + K tints (from ocarina-of-time) */
static int sLerpR, sLerpG, sLerpB;
static bool sForceIn;      /* 2-cycle: the vertex colour carries cycle 2, always modulate (from ocarina-of-time) */
/* Per-pixel fog (gfx_pc gfx_tri_fog, the fog reference mario-kart-64 7d23507): the vertex alpha is the
 * fog factor; the pixel goes to lerp(colour, sFogR/G/B, factor) after the combine, as the RDP's
 * cycle-1 fog blend does. sFogA >= 0: the combiner's constant alpha (PRIM/ENV, 0/1), which the
 * factor displaced. */
static bool sFogPx;
static int sFogR, sFogG, sFogB, sFogA = -1;

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
/* SF64_FILLRULE (2026-10-03; ocarina-of-time a7c4909, the Deku Tree's vision: a one-pixel bright diagonal across
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
#ifndef PERSP_SPAN
#define PERSP_SPAN 16 /* -DPERSP_SPAN=1 (EXTRA_CFLAGS): per-pixel perspective, for the lossy-trick audit */
#endif
/* A replay's span (defer_replay): bit i set = pixel i is one to write -- inside the replay's box
 * (x0..x1) and nothing final on it yet (alpha 0, or this replay's own tag). 0: the caller steps over
 * the span, its floats advanced and the next span's 1/w taken afresh (the expression the skipped span
 * would have carried), so the pixels it does draw come out bit for bit as a direct draw's. */
#if defined(MIYOO)
/* Miyoo: one count per thread (the second core's share of a split triangle counts into its own, added to the
 * game thread's after the wait: raster_tri). TLS lies outside the save-state region by construction. */
static __thread uint32_t sDfLive;
#else
static uint32_t sDfLive SAVESTATE_EXCLUDE; /* replay spans with a pixel to write, this frame (the gate, defer_batch) */
#endif
/* Round 28: a reverse replay (defer_replay under sMaskRev) keeps the first write, so a pixel it has tagged is done */
static bool sMaskRev SAVESTATE_EXCLUDE;
static inline uint32_t span_live(const uint32_t *p, int n, int x, int rx0, int rx1, uint32_t tag) {
    if (x + n <= rx0 || x >= rx1) return 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (n == 16) { /* the 16 alpha bytes in one de-interleaving load */
        static const uint8_t kLane[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        static const uint8_t kBit[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
        const int lo = rx0 - x < 0 ? 0 : rx0 - x, hi = rx1 - x > 16 ? 16 : rx1 - x;
        const uint8x16_t a = vld4q_u8((const uint8_t *)p).val[3], ln = vld1q_u8(kLane);
        uint8x16_t ok = sMaskRev ? vceqq_u8(a, vdupq_n_u8(0)) : vceqq_u8(vminq_u8(a, veorq_u8(a, vdupq_n_u8((uint8_t)tag))), vdupq_n_u8(0));
        ok = vandq_u8(ok, vandq_u8(vcgeq_u8(ln, vdupq_n_u8((uint8_t)lo)), vcltq_u8(ln, vdupq_n_u8((uint8_t)hi))));
        const uint8x16_t b = vandq_u8(ok, vld1q_u8(kBit));
        uint8x8_t r = vpadd_u8(vget_low_u8(b), vget_high_u8(b));
        r = vpadd_u8(r, r); r = vpadd_u8(r, r);
        return (uint32_t)vget_lane_u8(r, 0) | ((uint32_t)vget_lane_u8(r, 1) << 8);
    }
#endif
    uint32_t m = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t a = p[i] >> 24;
        if ((a == 0 || (!sMaskRev && a == tag)) && x + i >= rx0 && x + i < rx1) m |= 1u << i;
    }
    return m;
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
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
 * so it is bit-identical (proved on ARM by tools/handheld/qsweep.sh). In raster_tri
 * the span setup spilled to the stack, paid an FPSCR transfer per clamp and
 * re-converted the texture size every span, and each span paid a 76-instruction
 * call; flat triangles come in with zero colour steps. SF64_CMNEON=0 keeps the
 * scalar loops (ocarina-of-time c935b73). */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16; /* texture */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx;  /* triangle x-steps */
    float ftw, fth;                                       /* (float)tw, (float)th */
    int32x4_t dacc;                                       /* B G R A 16.16 x-steps */
    int32_t zdq; int zbias, amode, amin, fga, fgatex, ablend; /* ablend 0: TEX_EDGE, written unblended (X12) */
    int cthr; /* SF64_CELLSKIP: a cell whose largest tap alpha is below this fails the alpha compare (0: no skip) */
    bool persp, dmask, alpha, bilin, fog, ztest, texonly, mask, texlerp; /* mask: the no-z replay (defer_replay), tag its alpha */
    uint16_t tag; int rx0, rx1; /* the replay's tag and columns */
    int16x4_t lerpk;                                      /* texel lerp: K (B G R 0, sLerpR/G/B) */
    uint16x4_t fogc;                                      /* fog: B G R 0 (sFogR/G/B) */
} CmTri;
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* The pixel loop of one span: Tx (the texel, B G R A in u16 lanes) from the fetch (...).
 *   FOG: the fogged world, SPAN_LOOP_CM(1, 1, *, *, *, 3) -- lerp(M, fog, shade alpha) per lane with
 *        the scalar's arithmetic, div255(m*(255-sa) + f*sa), every term <= 255*255; the pixel's alpha
 *        is taken before it (and a constant fga after it), as the scalar loop does.
 *   ZT=0: no depth test or write, the scalar SPAN_LOOP(1, 0, ...) (round 12: Corneria's sky, a
 *        full-screen quad drawn without depth, was ~60k of the ~143k 3D pixels a play frame).
 *   TO:  1 the texel alone (opaque only), the scalar `!rgb_in` combine: decals and the sky.
 *        2 the texel lerp K + (shade - K) x texel / 255 (sTexLerp; round 22, the smart bomb's blast: ~2/3 of
 *        its cost was that sphere in the scalar loop), with the scalar's C division: it truncates toward 0, so
 *        per lane |d| x t goes through div255 (exact to 65534) and takes d's sign back; alpha modulated.
 *   ZT=2: no depth, a deferred backdrop's replay (defer_replay): only the span's span_live pixels (lm)
 *        are written, with the replay's tag as alpha. ZT=3: the same with every pixel live. */
#define CM_PIXELS(ALPHA, FOG, ZT, TO, ...)                                                               \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        int uz = 0;                                                                                     \
        if (ZT == 1) { uz = clamp16_sh8(zq); if (uz > *pz + zbias) continue; }                          \
        if (ZT == 2) { const uint32_t ok = lm & 1u; lm >>= 1; if (!ok) continue; }                       \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        uint16x4_t S = vdup_n_u16(0), M;                                                                \
        if (TO == 1) { M = Tx; }                                                                        \
        else { S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255)); M = cm_div255(vmul_u16(Tx, S)); } \
        if (TO == 2) { /* K + trunc((S - K) x T / 255) in B G R, the modulated alpha kept in lane 3 */     \
            const int16x4_t d = vsub_s16(vreinterpret_s16_u16(S), lerpk);                                 \
            const uint16x4_t q = cm_div255(vmul_u16(vreinterpret_u16_s16(vabs_s16(d)), Tx));              \
            const int16x4_t sq = vbsl_s16(vclt_s16(d, vdup_n_s16(0)), vneg_s16(vreinterpret_s16_u16(q)), vreinterpret_s16_u16(q)); \
            M = vbsl_u16(vcreate_u16(0x0000FFFFFFFFFFFFull), vreinterpret_u16_s16(vadd_s16(lerpk, sq)), M); \
        }                                                                                               \
        if (ALPHA && !FOG && ZT == 1 && !dmask && nblend) {                                             \
            /* X34 (the smart bomb): the blend decided in NEON. Moving the pixel's alpha to an ARM register \
             * to branch on it stalls the A7 for every blended pixel; here both outcomes are made and one \
             * is chosen by mask. A rejected pixel (fa < amin) stores back what was there, the same bytes as \
             * not writing it; with no depth write (dmask 0) nothing else depended on the branch. Blending \
             * at fa 255 gives M exactly (div255(m x 255) = m), so ablend 1 needs no fa < 255 test. */     \
            const uint16x4_t FA = vdup_lane_u16(amode == 0 ? M : amode == 1 ? Tx : S, 3);                \
            const uint8x8_t old8 = vreinterpret_u8_u32(vld1_dup_u32(pf));                                 \
            const uint16x4_t D = vget_low_u16(vmovl_u8(old8));                                            \
            uint16x4_t R = M;                                                                           \
            if (ablend) {                                                                               \
                const uint16x4_t B = cm_div255(vmla_u16(vmul_u16(M, FA), D, vsub_u16(v255, FA)));         \
                R = ablend == 1 ? B : vbsl_u16(vtst_u16(vdup_lane_u16(D, 3), v255), B, M);              \
            }                                                                                           \
            const uint8x8_t new8 = vmovn_u16(vcombine_u16(vorr_u16(R, a255), vorr_u16(R, a255)));         \
            const uint8x8_t rej = vmovn_u16(vcombine_u16(vclt_u16(FA, vdup_n_u16((uint16_t)amin)), vclt_u16(FA, vdup_n_u16((uint16_t)amin)))); \
            vst1_lane_u32(pf, vreinterpret_u32_u8(vbsl_u8(rej, old8, new8)), 0);                          \
            continue;                                                                                   \
        }                                                                                               \
        int fa = 255;                                                                                   \
        if (ALPHA) /* fa: modulated (0), texel (1) or shade (2) alpha -- the scalar fa */                 \
            fa = amode == 0 ? vget_lane_u16(M, 3) : amode == 1 ? vget_lane_u16(Tx, 3) : vget_lane_u16(S, 3); \
        if (FOG) {                                                                                      \
            const uint16x4_t SA = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255(vmla_u16(vmul_u16(M, vsub_u16(v255, SA)), fogc, SA));                         \
            if (ALPHA && fga >= 0) fa = fgatex ? div255(vget_lane_u16(Tx, 3) * fga) : fga; /* the constant alpha the fog factor displaced (x the texel's, FOGTEXA) */ \
        }                                                                                               \
        if (ALPHA) {                                                                                    \
            if (fa < amin) continue;                                                                    \
            if (fa < 255 && (ablend == 1 || (ablend == 2 && (*pf >> 24) != 0))) { /* div255(f*fa + d*(255-fa)) */ \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));         \
            }                                                                                           \
        }                                                                                               \
        M = ZT >= 2 ? vset_lane_u16(tag, M, 3) : vorr_u16(M, a255);                                     \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (ZT == 1 && dmask) *pz = (uint16_t)uz;                                                       \
    }
/* bilerp: four taps around (u - 0.5, v - 0.5), lerped along u then v. X33 (the smart bomb, "unplayably
 * laggy"): a magnified texture keeps the same four texels for many pixels in a row (the blast's 16x16 over
 * the whole screen), so the taps, their wrap and their unpacking are kept for the row's current texel cell
 * (bk_u, bk_v) and redone only when it changes. The same values, so the same pixels. */
#define CM_BILIN(TAP, WU, WV)                                                                           \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        if (!bkon || (su >> 16) != bk_u || (sv >> 16) != bk_v) {                                        \
            bk_u = su >> 16; bk_v = sv >> 16;                                                           \
            const int iu0 = WU(bk_u), iu1 = WU(bk_u + 1);                                               \
            const int b0 = WV(bk_v) * tw, b1 = WV(bk_v + 1) * tw;                                       \
            const uint32_t a = TAP(b0 + iu0), b = TAP(b0 + iu1), c = TAP(b1 + iu0), d = TAP(b1 + iu1);  \
            bkX = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));                  \
            bkY = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));                  \
            if (csk) { /* SF64_CELLSKIP: the cell's largest tap alpha against the triangle's threshold */ \
                const uint32_t m0 = (a > b ? a : b) >> 24, m1 = (c > d ? c : d) >> 24;                  \
                bkrej = (int)(m0 > m1 ? m0 : m1) < cthr;                                                \
            }                                                                                           \
        }                                                                                               \
        if (csk && bkrej) continue; /* every pixel of the cell fails the alpha compare */             \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = bkX, Y = bkY;                                                              \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        Tx = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2), vget_high_u16(H), wts, 3), 8);
/* FETCH_NEAREST */
#define CM_NEAR(TAP, WU, WV)                                                                            \
        const uint32_t t1 = TAP(WV(vf >> 16) * tw + WU(uf >> 16));                                      \
        Tx = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(t1))));
#define CM_TAP32(i) px32[i]
#define CM_TAP16(i) texel_at(px, true, (i))
#define CM_WRAP_RRU(x) ((x) & mu)
#define CM_WRAP_RRV(x) ((x) & mv)
#define CM_WRAP_GU(x) wrapf((x), tw, ws, mu)
#define CM_WRAP_GV(x) wrapf((x), th, wt, mv)
/* One row: raster_tri's span loop for this path, float for float (fetch = ...). */
#define CM_ROW(ALPHA, FOG, ZT, TO, ...)                                                                  \
    int32_t bk_u = INT32_MIN, bk_v = 0; uint16x8_t bkX = vdupq_n_u16(0), bkY = bkX;                    \
    (void)bk_u; (void)bk_v; (void)bkX; (void)bkY;                                                       \
    const bool csk = (ALPHA) && !(FOG) && cthr > 0; bool bkrej = false;                                \
    (void)csk; (void)bkrej;                                                                             \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
        uint32_t lm = ZT == 2 ? span_live(fb, span, xc, rx0, rx1, tag) : 0; (void)lm;                  \
        if (ZT == 2 && lm) sDfLive++;                                                                   \
        if (ZT == 2 && !lm) {                                                                           \
            zf += zdx * fspan;                                                                          \
            cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;             \
            uw += uwdx * fspan; vw += vwdx * fspan;                                                     \
            sw = sw_b; stale = true; fb += span; zp += span; n -= span; xc += span;                     \
            continue;                                                                                   \
        }                                                                                               \
        if (ZT == 2 && stale) { if (persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f; stale = false; }      \
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
        const float32x2_t zc = vmin_f32(vmax_f32(vdup_n_f32(zf), vdup_n_f32(-1.0f)), vdup_n_f32(2.0f));   \
        int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);                  \
        const float32x4_t cq = { cb, cg, cr, ca };                                                      \
        int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                        \
        uint32_t *const end = fb + span;                                                                \
        uint16_t *pz = zp;                                                                              \
        if (ZT == 2 && lm == (1u << span) - 1u) { CM_PIXELS(ALPHA, FOG, 3, TO, __VA_ARGS__) }           \
        else { CM_PIXELS(ALPHA, FOG, ZT, TO, __VA_ARGS__) }                                             \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span; xc += span;                                                    \
    }
static int sNeonBlend SAVESTATE_EXCLUDE = 1; /* X34: SF64_NEONBLEND=0 branches on the alpha in ARM, as before (A/B) */
static int sBilCache SAVESTATE_EXCLUDE = 1; /* X33: SF64_BILCACHE=0 refetches the four taps every pixel (A/B) */
static int sCmNeon SAVESTATE_EXCLUDE = 1; /* SF64_CMNEON=0: the scalar loops, for A/B (ARM qsweep: 49 frames, 0 differ) */
int gfx_soft_neon_on(void) { return sCmNeon; } /* the present's squeeze follows the same switch (gfx_sdl12.c) */
/* neon=alt in the display cfg (round 12; ocarina-of-time's cmneon=alt): NEON on and off in turn, one PERF
 * window each, so one play session times both on the device. Called at each PERF line: returns the
 * mode the window just ended used (-1: no NEON build), then flips it when alternating. */
static int sNeonAlt SAVESTATE_EXCLUDE;
int gfx_soft_neon_alt_active(void) { return sNeonAlt; } /* neon=alt: every PERF window prints */
int gfx_soft_neon_alt_step(void) {
    const int used = sCmNeon;
    if (sNeonAlt) sCmNeon = !sCmNeon;
    return used;
}
/* SF64_CELLSKIP (2026-10-11, the speed audit's bomb item 2): in a blended bilinear row (the blast, its light, the
 * glows, the water's splash sprays: magnified textures with transparent rims, so whole runs of pixels sit in one
 * texel cell), a cell whose four taps' largest alpha already fails the alpha compare is stepped over: a bilinear
 * alpha is never above its largest tap (two lerps with weights summing to 256, then >> 8), and the pixel's alpha is
 * that texel alpha (amode 1) or div255 of it times the shade alpha (amode 0, the modulate, on a flat triangle: prim
 * alpha comes in as a flat shade), both non-decreasing in it. A rejected pixel stores nothing (or, X34, the bytes it
 * read), so stepping over it gives the same pixels; the depth test still comes first. The threshold is worked out once
 * a triangle (cm_cell_thr, CmTri.cthr); the fog rows (their alpha is the fog's constant), smooth-alpha triangles and
 * shade-alpha rows (amode 2) are left alone. SF64_CELLSKIP=0: off. */
static int sCellSkip SAVESTATE_EXCLUDE = 1;
#define CELLSKIP_SHARE 64 /* of 255: the least share of a texture's texels below a triangle's threshold for its rows to skip */
/* The least largest-tap alpha that can pass, for a triangle (0: no skip). amode 1 (the texel's alpha): amin. amode 0
 * (texel x shade alpha) on a flat triangle (prim alpha, the blast's): its rows hold a0 exactly (no step; vcvt #16
 * truncates as the C cast does, then >> 16 and the clamp), so the least t with div255(t x sa) >= amin. A smooth
 * triangle: no skip. */
static int cm_cell_thr(int amode, int amin, bool flat, float a0) {
    if (amode == 1) return amin;
    if (amode != 0 || !flat || !(a0 == a0)) return 0;
    const int sa = a0 <= 0.0f ? 0 : a0 >= 255.0f ? 255 : (int)(a0 * 65536.0f) >> 16;
    int lo = 0, top = 256; /* the least t in [0, 256] with div255(t x sa) >= amin (256: none passes) */
    while (lo < top) { const int mid = (lo + top) >> 1; if (mid < 256 && div255(mid * sa) >= amin) top = mid; else lo = mid + 1; }
    return lo;
}
static void __attribute__((noinline))
cm_row_neon(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, int xc,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const int rx0 = T->rx0, rx1 = T->rx1; bool stale = false; (void)rx0; (void)rx1; (void)stale; (void)xc;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const uint16x4_t v255 = vdup_n_u16(255), fogc = T->fogc;
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin, fga = T->fga, fgatex = T->fgatex, ablend = T->ablend;
    const bool persp = T->persp, dmask = T->dmask; const uint32_t tag = T->tag; const int16x4_t lerpk = T->lerpk;
    const bool bkon = sBilCache != 0; (void)bkon;
    const bool nblend = sNeonBlend != 0; (void)nblend;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    const int cthr = 0; (void)cthr; /* SF64_CELLSKIP: its rows are cm_row_neon_cs */
    (void)th; (void)ws; (void)wt; (void)px32; (void)v255; (void)fogc; (void)fga; (void)fgatex; (void)ablend; (void)zbias; (void)dmask; (void)tag; (void)lerpk;
#define CM_VARIANTS(KIND, ALPHA, FOG, ZT, TO)                                                           \
    if (rr) { if (T->t16) { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }     \
              else        { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }   \
    else    { if (T->t16) { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }       \
              else        { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
#define CM_KIND(ALPHA, FOG, ZT, TO) \
    if (T->bilin) { CM_VARIANTS(CM_BILIN, ALPHA, FOG, ZT, TO) } else { CM_VARIANTS(CM_NEAR, ALPHA, FOG, ZT, TO) }
    if (T->fog)           { if (T->alpha) { CM_KIND(1, 1, 1, 0) } else { CM_KIND(0, 1, 1, 0) } } /* z-tested */
    else if (!T->ztest && T->mask) { if (T->texonly) { CM_KIND(0, 0, 2, 1) } else { CM_KIND(0, 0, 2, 0) } } /* replay */
    else if (!T->ztest)   { if (T->texonly) { CM_KIND(0, 0, 0, 1) } else { CM_KIND(0, 0, 0, 0) } } /* opaque */
    else if (T->texonly)  { CM_KIND(0, 0, 1, 1) }                                                    /* opaque */
    else if (T->texlerp)  { if (T->alpha) { CM_KIND(1, 0, 1, 2) } else { CM_KIND(0, 0, 1, 2) } }      /* z-tested */
    else if (T->alpha)    { CM_KIND(1, 0, 1, 0) }
    else                  { CM_KIND(0, 0, 1, 0) }
#undef CM_KIND
#undef CM_VARIANTS
}
/* SF64_CELLSKIP's rows (raster_tri sends a triangle here when CmTri.cthr > 0: blended, unfogged, bilinear, the cell
 * cache on): cm_row_neon's alpha rows with the cell skip compiled in, so cm_row_neon itself keeps its code. */
static void __attribute__((noinline))
cm_row_neon_cs(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, int xc,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const int rx0 = T->rx0, rx1 = T->rx1; bool stale = false; (void)rx0; (void)rx1; (void)stale; (void)xc;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const uint16x4_t v255 = vdup_n_u16(255), fogc = T->fogc;
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin, fga = T->fga, fgatex = T->fgatex, ablend = T->ablend;
    const bool persp = T->persp, dmask = T->dmask; const uint32_t tag = T->tag; const int16x4_t lerpk = T->lerpk;
    const bool bkon = sBilCache != 0; (void)bkon;
    const bool nblend = sNeonBlend != 0; (void)nblend;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    const int cthr = bkon ? T->cthr : 0; (void)cthr; /* SF64_CELLSKIP */
    (void)th; (void)ws; (void)wt; (void)px32; (void)v255; (void)fogc; (void)fga; (void)fgatex; (void)ablend; (void)zbias; (void)dmask; (void)tag; (void)lerpk;
#define CM_VARIANTS(KIND, ALPHA, FOG, ZT, TO)                                                           \
    if (rr) { if (T->t16) { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }     \
              else        { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }   \
    else    { if (T->t16) { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }       \
              else        { CM_ROW(ALPHA, FOG, ZT, TO, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
#define CM_KIND(ALPHA, FOG, ZT, TO) \
    if (T->bilin) { CM_VARIANTS(CM_BILIN, ALPHA, FOG, ZT, TO) } else { CM_VARIANTS(CM_NEAR, ALPHA, FOG, ZT, TO) }
    if (T->texlerp)       { CM_VARIANTS(CM_BILIN, 1, 0, 1, 2) } /* z-tested, blended, bilinear (raster_tri: cthr > 0) */
    else                  { CM_VARIANTS(CM_BILIN, 1, 0, 1, 0) }
#undef CM_KIND
#undef CM_VARIANTS
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
#undef CM_ROW
#define HH_CM_NEON 1
#endif


#if !defined(HH_CM_NEON)
int gfx_soft_neon_alt_active(void) { return 0; }
int gfx_soft_neon_alt_step(void) { return -1; }
#endif

#if !defined(HANDHELD)
/* SF64_SPANSTAT=1 (desktop only, test): 3D span pixels (before the z test) and triangles by the
 * path raster_tri takes, and why a textured triangle misses the NEON rows -- the round 11 speed
 * plan's census (the device's qemu profile had raster_tri's scalar loops at 45% of Corneria's
 * instructions and the NEON rows at 17%). Printed at exit. */
enum { SS_ROWS, SS_FOG3, SS_FOG4, SS_FLAT, SS_NOZ, SS_LERP, SS_DECALA, SS_ADD, SS_TEXA, SS_TEXONLY, SS_NOTEX_RGB,
       SS_CONST, SS_UFOG, SS_UNTEX, SS_N };
static const char *const sSpanStatName[SS_N] = {
    "NEON rows (texture x shade, z-tested; opaque when fogged)", "fogged texture x shade, blended (CM=3 scalar)",
    "fogged general (CM=4)", "textured flat, z-tested (FLAT loop)", "textured, no z test", "texel lerp (K..shade)",
    "decal by texel alpha", "texel + shade (ADD)", "rgb from texel alpha", "texel only (decal: no vertex colour)",
    "texture not in rgb", "untextured, constant colour run", "untextured, fogged", "untextured" };
static int sSpanStat = -1;
static uint64_t sSpanStatPx[SS_N], sSpanStatTri[SS_N], sSpanStatBlend[SS_N];
/* round 12 counts before taking two other ports' paths: blended textured 16-px spans whose every pixel
 * fails the alpha test (the most goldeneye-007 ac941f3's skip could save), and 2D sprite pixels
 * whose rgb is the input and alpha the texel (mario-kart-64 28cc007's own loop) */
static uint64_t sSsBlendSpans, sSsBlendPx, sSsDeadSpans, sSsDeadPx, sSs2dPx, sSs2dInTexA;
/* the no-z-test textured draws by state: shader id, blended, flat, texture x shade, bilinear, texture size */
static struct { uint32_t shader; uint8_t blend, flat, mod, bilin; uint16_t tw, th; uint64_t px, tris; } sSsNoz[32];
static int sSsNozN;
/* SF64_FRAMESTAT=from-to (desktop only, test): per rendered frame, the 3D span pixels (before the z test) by
 * texture, the six largest; with SF64_FRAMESTAT_DUMP=dir the textures of those draws as PPMs (local only). */
static int sFrameStat SAVESTATE_EXCLUDE; static long sFsFrom SAVESTATE_EXCLUDE, sFsTo SAVESTATE_EXCLUDE, sFsFrame SAVESTATE_EXCLUDE;
static struct { const void *key; uint8_t blend, path; uint16_t tw, th; uint64_t px, tris; } sFs[64];
static int sFsN; static uint64_t sFsTot;
static const void *fs_src(const void *key) {
    for (int t = 1; t < MAX_TEXTURES; t++) if (sTex[t].valid && sTex[t].px == key) return sTexSrc[t];
    return NULL;
}
static void framestat_end(void) {
    static int init = 0;
    if (!init) { init = 1; const char *e = getenv("SF64_FRAMESTAT"); if (e) { sFsFrom = atol(e); const char *d = strchr(e, '-'); sFsTo = d ? atol(d + 1) : sFsFrom; } }
    if (sFsFrom || sFsTo) {
        if (sFrameStat && sFsN) {
            int order[64]; for (int i = 0; i < sFsN; i++) order[i] = i;
            for (int i = 0; i < sFsN; i++) for (int j = i + 1; j < sFsN; j++) if (sFs[order[j]].px > sFs[order[i]].px) { int t = order[i]; order[i] = order[j]; order[j] = t; }
            char buf[1024]; int n = snprintf(buf, sizeof(buf), "framestat %ld: %llu px |", sFsFrame, (unsigned long long)sFsTot);
            for (int k = 0; k < sFsN && k < 6 && n < (int)sizeof(buf) - 80; k++) {
                const int i = order[k];
                n += snprintf(buf + n, sizeof(buf) - n, " %ux%u%s p%u %llupx/%llut@%p", sFs[i].tw, sFs[i].th, sFs[i].blend ? "b" : "", sFs[i].path,
                              (unsigned long long)sFs[i].px, (unsigned long long)sFs[i].tris, fs_src(sFs[i].key));
                const char *dd = getenv("SF64_FRAMESTAT_DUMP");
                if (dd && sFs[i].key && sFs[i].tw * sFs[i].th <= 128 * 128) {
                    char path[512]; snprintf(path, sizeof(path), "%s/tex_%ux%u_%p.ppm", dd, sFs[i].tw, sFs[i].th, sFs[i].key);
                    FILE *f = fopen(path, "rb");
                    if (!f) {
                        f = fopen(path, "wb");
                        if (f) {
                            fprintf(f, "P6\n%u %u\n255\n", sFs[i].tw, sFs[i].th);
                            for (int t = 0; t < sFs[i].tw * sFs[i].th; t++) {
                                const uint32_t c = texel_at(sFs[i].key, false, t); /* only 32-bit on the desktop */
                                const uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
                                fwrite(rgb, 1, 3, f);
                            }
                        }
                    }
                    if (f) fclose(f);
                }
            }
            port_log("%s\n", buf);
        }
        sFsFrame++;
        sFrameStat = sFsFrame >= sFsFrom && sFsFrame <= sFsTo;
        sFsN = 0; sFsTot = 0;
    }
}
static void spanstat_report(void) {
    uint64_t tot = 0;
    port_log("spanstat: blended textured spans %llu (%llu px), all pixels alpha-rejected %llu (%llu px)\n",
             (unsigned long long)sSsBlendSpans, (unsigned long long)sSsBlendPx, (unsigned long long)sSsDeadSpans,
             (unsigned long long)sSsDeadPx);
    port_log("spanstat: 2D sprite px %llu, rgb from the input with the texel's alpha (blended) %llu\n",
             (unsigned long long)sSs2dPx, (unsigned long long)sSs2dInTexA);
    { extern uint64_t gfx_soft_ss2d_mag(void); port_log("spanstat: 2D magnified bilerp sprite px (X14) %llu\n", (unsigned long long)gfx_soft_ss2d_mag()); }
    for (int i = 0; i < sSsNozN; i++)
        port_log("spanstat: no-z %08X blend %u flat %u mod %u bilin %u tex %ux%u: %llu px %llu tris\n", sSsNoz[i].shader,
                 sSsNoz[i].blend, sSsNoz[i].flat, sSsNoz[i].mod, sSsNoz[i].bilin, sSsNoz[i].tw, sSsNoz[i].th,
                 (unsigned long long)sSsNoz[i].px, (unsigned long long)sSsNoz[i].tris);
    for (int i = 0; i < SS_N; i++) tot += sSpanStatPx[i];
    port_log("spanstat: %llu 3D span pixels\n", (unsigned long long)tot);
    for (int i = 0; i < SS_N; i++) {
        if (!sSpanStatTri[i]) continue;
        port_log("spanstat: %5.1f%% %12llu px %9llu tris (%4.1f%% of its px blended)  %s\n",
                 tot ? 100.0 * sSpanStatPx[i] / tot : 0.0, (unsigned long long)sSpanStatPx[i],
                 (unsigned long long)sSpanStatTri[i], sSpanStatPx[i] ? 100.0 * sSpanStatBlend[i] / sSpanStatPx[i] : 0.0,
                 sSpanStatName[i]);
    }
}
#endif

/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
static void int_rows_touch(int y0, int y1); /* internal target: seed/dirty rows (see the frame section) */
static uint32_t *sIntFB SAVESTATE_EXCLUDE;  /* defined with the targets below */
static uint16_t *sIntZB SAVESTATE_EXCLUDE; static int sIntW SAVESTATE_EXCLUDE, sIntH SAVESTATE_EXCLUDE; /* (defer_flush) */
/* the HUD unsquish's 3D target (see the 2D path's HUD notes): 320x240, the frame's own layout */
#define HUD3_W 320
#define HUD3_H 240
static uint32_t *sHud3FB SAVESTATE_EXCLUDE; static uint16_t *sHud3ZB SAVESTATE_EXCLUDE;
static int sHud3Y0 SAVESTATE_EXCLUDE = HUD3_H, sHud3Y1 SAVESTATE_EXCLUDE;
static uint8_t sHud3Seeded[HUD3_H] SAVESTATE_EXCLUDE; /* rows of this pass already holding the layer's pixels */
static void hud3_seed(int y0, int y1);
static inline void hud3_touch(int y0, int y1) {
    if (y0 < sHud3Y0) sHud3Y0 = y0;
    if (y1 > sHud3Y1) sHud3Y1 = y1;
    hud3_seed(y0, y1);
}
/* A textured draw without a texture comes out as a flat block of the combiner's colour (ocarina-of-time
 * 2b69f7b: the red sunset bar after a long session). Said on the console the first 4 times and every
 * 1000th, so a device run catches it and a sweep can count it. */
static uint32_t sUntex SAVESTATE_EXCLUDE;
static void untex_note(void) {
    if (++sUntex <= 4 || sUntex % 1000 == 0)
        port_log_always("gfx: untextured draw #%u: tex %u not valid (bound %u/%u, arena %d%%)\n", (unsigned) sUntex,
                        (unsigned) sBoundTex[0], (unsigned) sBoundTex[0], (unsigned) sBoundTex[1],
                        (int) (sArenaUsed * 100 / TEXARENA_SZ));
}
static const SoftTex *sReplayTex SAVESTATE_EXCLUDE; /* defer_flush: the texture the draw had bound */
static uint8_t sMaskTag SAVESTATE_EXCLUDE;          /* defer_replay: its tag (0: not replaying) */
static int sRepX0 SAVESTATE_EXCLUDE, sRepX1 SAVESTATE_EXCLUDE; /* defer_replay: its columns */
static uint32_t sDpBatchPx SAVESTATE_EXCLUDE; /* round 27 drawprof: span pixels of the current batch */
#if defined(MIYOO)
/* The Mini's second core (miyoo_worker.c, the shared Miyoo kit KM2). A big 3D triangle's rows are shared:
 * the game thread draws rows miny, miny+2, ... and the worker miny+1, miny+3, ..., each running this same
 * function on the same vertices (raster_tri_rows with a row offset and a step of 2), then the game thread
 * waits. Every pixel comes out the same as one core's:
 *  - each row is derived from y alone (the row bases, the span solve, the span start's interpolants: no
 *    row-to-row accumulation), and the setup both run is the same float operations on the same inputs in
 *    the same FPSCR mode (the port never changes it), so a row computes exactly what the whole loop does
 *    there; the NEON rows' cell cache starts afresh every row;
 *  - the shares write disjoint rows of colour and depth;
 *  - everything both read is fixed while they run: the vertices, the texture (decoded at upload), the
 *    combiner and blend state, the scissor/viewport, the replay's tag and columns (sRepX0/1, sMaskTag,
 *    sMaskRev); the one write outside the rows, the replay's span count sDfLive, is per thread (TLS) and
 *    the worker's part is added after the wait; the rows the target seeds (int_rows_touch, hud3_touch)
 *    are touched by the game thread before the handoff, and the worker skips that, the untextured-draw note
 *    and the batch's pixel count (the game thread adds the worker's pixels).
 * Split: at least SF64_SPLITROWS rows (8) and a bounding box of SF64_SPLITPX pixels or more (1600, about
 * 800 drawn); the choice never decides a pixel, only who draws a row. SF64_THREADS=1: never split. */
#include "miyoo_worker.h"
static int raster_tri_rows(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z, int ry0, int rstep);
typedef struct { const SoftVtx *v0, *v1, *v2; bool use_z; int px; uint32_t live; } SplitJob;
static void split_job(void *arg) {
    SplitJob *j = (SplitJob *)arg;
    const uint32_t l0 = sDfLive;
    const int px = raster_tri_rows(j->v0, j->v1, j->v2, j->use_z, 1, 2);
    j->px = px > 0 ? px : 0;
    j->live = sDfLive - l0;
}
static int sSplitRows SAVESTATE_EXCLUDE = -1, sSplitPx SAVESTATE_EXCLUDE;
static int sSplitOn SAVESTATE_EXCLUDE = 1; /* the Miyoo Bench's one-core windows (main.c bench_hook) */
void gfx_soft_split_set(int on) { sSplitOn = on; }
uint32_t gfx_soft_split_tris SAVESTATE_EXCLUDE, gfx_soft_split_px SAVESTATE_EXCLUDE; /* main.c's PERF line: shared this window */
static inline bool split_ok(int minx, int maxx, int miny, int maxy) {
    if (sSplitRows < 0) {
        const char *r = getenv("SF64_SPLITROWS"), *p = getenv("SF64_SPLITPX");
        sSplitRows = r && atoi(r) >= 2 ? atoi(r) : 8;
        sSplitPx = p && atoi(p) >= 0 ? atoi(p) : 1600;
    }
    return sSplitOn && miyoo_worker_ready() && maxy - miny >= sSplitRows && (maxx - minx) * (maxy - miny) >= sSplitPx;
}
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
    return raster_tri_rows(v0, v1, v2, use_z, 0, 1);
}
/* ry0 0: the game thread (rstep 1: all rows, or shared after the setup); 1: the worker's share (rstep 2). */
static int raster_tri_rows(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z, int ry0, int rstep) {
    const bool lead = ry0 == 0;
#else
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
#endif
    float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) return -1; /* (winding cull happens in gfx_sp_tri1; the GE cull mode is never set here) */

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
#if defined(MIYOO)
    if (lead)
#endif
    {
    if (sFB == sIntFB) int_rows_touch(miny, maxy);
    else if (sFB == sHud3FB) hud3_touch(miny, maxy);
    }
    /* Barycentrics = edge/area with SIGNED area: inside is all-w >= 0 for
     * either winding, no separate sign handling needed. (Divide sits after
     * the bbox reject so off-screen triangles don't pay for it.) */
    const float inv_area = 1.0f / area;

    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? (sReplayTex ? sReplayTex : &sTex[sBoundTex[0]]) : NULL;
#if defined(MIYOO)
    if (tex && !tex->valid) { if (lead) untex_note(); tex = NULL; }
#else
    if (tex && !tex->valid) { untex_note(); tex = NULL; }
#endif
    const int tmask_u = tex ? pow2mask(tex->w) : -1, tmask_v = tex ? pow2mask(tex->h) : -1;
    const bool tex_edge = sUseAlpha && sCurShader && sCurShader->cc.opt_texture_edge;
    const bool ztest = use_z && sDepthTest;
    const bool mskip = sMaskTag && !ztest; const uint8_t aout = mskip ? sMaskTag : 255; /* the replay (defer_replay) */
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
     * hysteresis.) */
    bool use_bilin = gfx_soft_bilinear && tex;
    if (use_bilin && gfx_soft_magbilin) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
    }
    const int amin = tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
    const int ablend = (tex_edge && sEdgeOpaque) ? (sEdgeOpaque == 2 ? 2 : 0) : 1; /* X12: 1 blend, 0 never (the texel's own colour past amin), 2 not over a pixel nothing has been drawn to */
    const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
    const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
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
    const int32_t zdq = (int32_t)((zdx < -1.0f ? -1.0f : zdx > 1.0f ? 1.0f : zdx) * (65535.0f * 256.0f));
    const int32_t rdq = sat_i32(crdx * 65536.0f), gdq = sat_i32(cgdx * 65536.0f), bdq = sat_i32(cbdx * 65536.0f), adq = sat_i32(cadx * 65536.0f);

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const int zbias = sZDecal ? 4 : 0; const bool dmask = sDepthMask, texmix = sTexAdd || rgb_decal || sTexLerp;
    const bool texlerp = sTexLerp; const int lkr = sLerpR, lkg = sLerpG, lkb = sLerpB; /* the rare combines share one test */
    /* Fogged per pixel (sFogPx): only triangles with some fog take the fog loops -- fog sits near the far
     * plane, and lerp(c, fog, 0) = c exactly. Star Fox's guard on the reference: a constant alpha (sFogA)
     * takes them too, or an unfogged triangle's pixels would read the factor 0 as their alpha. */
    const bool fogtri = sFogPx && ((v0->a | v1->a | v2->a) != 0 || sFogA >= 0);
    const int fgr = sFogR, fgg = sFogG, fgb = sFogB, fga = sFogA;
    /* texture x shade, the common 3D combine: see SPAN_LOOP_CM */
    const bool cm_mod = rgb_tex && rgb_in && !texmix && !rgb_texa && !fogtri;
    /* texture x shade, fogged (CM=3: Star Fox's fogged world, and the NEON rows when opaque) */
    const bool cm_fog = fogtri && rgb_tex && rgb_in && !texmix && !rgb_texa;
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
#if defined(HH_CM_NEON)
    /* The NEON rows (cm_row_neon): z-tested texture x shade, the path the
     * scalar dispatch below sends to SPAN_LOOP_CM(1, 1, *, *, *, 1). */
    /* Round 12: the rows also take texture x shade and the texel alone (decals, the sky) without a depth
     * test, the texel alone with one (opaque, both), and fogged texture x shade blended. */
    const bool cm_texonly = rgb_tex && !rgb_in && !texmix && !rgb_texa && !fogtri && !sUseAlpha;
    /* texel lerp (TO=2): z-tested, the plain lerp only (no ADD, decal or texel-alpha rgb), unfogged */
    const bool cm_tl = rgb_tex && rgb_in && texlerp && !sTexAdd && !rgb_decal && !rgb_texa && !fogtri;
    const bool cm_rows = sCmNeon && sFastSpans && tex &&
                         ((ztest && (cm_mod || cm_fog || cm_tl)) || (!sUseAlpha && (cm_mod || cm_texonly)));
    CmTri cmt;
    if (cm_rows) {
        const int32_t d[4] = { bdq, gdq, rdq, adq };
        cmt.px = texpx; cmt.w = tw; cmt.h = th; cmt.ws = ws; cmt.wt = wt; cmt.mu = tmask_u; cmt.mv = tmask_v; cmt.t16 = t16;
        cmt.swdx = swdx; cmt.uwdx = uwdx; cmt.vwdx = vwdx; cmt.zdx = zdx;
        cmt.crdx = crdx; cmt.cgdx = cgdx; cmt.cbdx = cbdx; cmt.cadx = cadx;
        cmt.ftw = (float)tw; cmt.fth = (float)th;
        cmt.dacc = vld1q_s32(d); cmt.zdq = zdq; cmt.zbias = zbias;
        cmt.amode = !a_tex ? 2 : !a_in ? 1 : 0; /* fa = !a_tex ? sa : !a_in ? ta : div255(ta*sa) */
        cmt.cthr = sCellSkip && sBilCache && sUseAlpha && use_bilin && !cm_fog && ztest && !sReplayTex ? cm_cell_thr(cmt.amode, amin, flat, a0) : 0; /* SF64_CELLSKIP */
        if (cmt.cthr > 0) { /* worth it only where a quarter of the texels or more sit below the threshold (else its rows' checks cost more than they skip) */
            const int k = cmt.cthr / 8 - 1; const uint32_t id = sBoundTex[0];
            if (k < 0 || id >= MAX_TEXTURES || sTexLow[id][k > 31 ? 31 : k] < CELLSKIP_SHARE) cmt.cthr = 0;
        }
        cmt.amin = amin; cmt.ablend = ablend; cmt.persp = persp; cmt.dmask = dmask; cmt.alpha = sUseAlpha; cmt.bilin = use_bilin;
        cmt.fog = cm_fog; cmt.fga = fga; cmt.fgatex = a_tex; cmt.ztest = ztest; cmt.texonly = !cm_mod && !cm_fog && cm_texonly;
        cmt.mask = mskip; cmt.tag = aout; cmt.rx0 = sRepX0; cmt.rx1 = sRepX1;
        cmt.texlerp = !cm_mod && !cm_fog && cm_tl && ztest;
        { const int16_t k[4] = { (int16_t)lkb, (int16_t)lkg, (int16_t)lkr, 0 }; cmt.lerpk = vld1_s16(k); }
        { const uint16_t f[4] = { (uint16_t)fgb, (uint16_t)fgg, (uint16_t)fgr, 0 }; cmt.fogc = vld1_u16(f); }
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
    OwnTri oe; /* SF64_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    if (own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&oe.e[0], v1, v2, s); own_edge(&oe.e[1], v2, v0, s); own_edge(&oe.e[2], v0, v1, s);
    }

#if defined(MIYOO)
    int ystart = miny + ry0, ystep = rstep;
    SplitJob job;
    const bool split = rstep == 1 && split_ok(minx, maxx, miny, maxy);
    if (split) { /* the worker: rows miny+1, miny+3, ...; here: miny, miny+2, ... */
        job.v0 = v0; job.v1 = v1; job.v2 = v2; job.use_z = use_z; job.px = 0; job.live = 0;
        miyoo_worker_run(split_job, &job);
        ystep = 2;
    }
    for (int y = ystart; y < maxy; y += ystep) {
#else
    for (int y = miny; y < maxy; y++) {
#endif
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
        if (own) { /* SF64_FILLRULE: the row's ends by the owned-edge test, one pixel either side */
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
        if (cm_rows) { /* the whole row; flat triangles: zero colour steps, so rq.. clamp to the fs* */
            if (cmt.cthr) cm_row_neon_cs(&cmt, fbuf + idx, zb + idx, xe - xs, xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else cm_row_neon(&cmt, fbuf + idx, zb + idx, xe - xs, xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            continue;
        }
#endif
        int x = xs;
        bool stale = false; /* a replay skipped the span that carries 1/w (span_live) */
        while (x < xe) {
            int span = xe - x;
            if (span > PERSP_SPAN) span = PERSP_SPAN;
            const float fspan = (float)span;
            const float sw_b = sw + swdx * fspan;           /* == the old end-of-span  sw += swdx*span */
            uint32_t lm = mskip ? span_live(fbuf + idx, span, x, sRepX0, sRepX1, aout) : 0; /* the replay's pixels */
            if (lm) sDfLive++;
            if (mskip && !lm) {
                zf += zdx * fspan;
                cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;
                uw += uwdx * fspan; vw += vwdx * fspan;
                sw = sw_b; stale = true; idx += span; x += span;
                continue;
            }
            if (stale) { if (persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f; stale = false; }
            (void)lm;
            const bool mtest = mskip && lm != (1u << span) - 1u; /* all live: no per-pixel test */
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
            int32_t zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            int32_t rq = (int32_t)(cr * 65536.0f), gq = (int32_t)(cg * 65536.0f), bq = (int32_t)(cb * 65536.0f), aq = (int32_t)(ca * 65536.0f);
            /* Flat: every colour step is 0, so the clamped shade is the same on
             * every pixel of the span -- take it once (bit-exact by construction). */
            const int fsr = clamp8_sh16(rq), fsg = clamp8_sh16(gq), fsb = clamp8_sh16(bq), fsa = clamp8_sh16(aq);
#if !defined(HANDHELD)
            int ss_rej = 0; /* SF64_SPANSTAT: this span's alpha-rejected pixels */
#define SS_AREJ (ss_rej++)
#else
#define SS_AREJ ((void)0)
#endif

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
/* FLAT=1: one shade colour for the span (fsr..fsa), so the four colour
 * accumulators and their steps drop out of the pixel loop. CM=1: the combine is
 * plain texture x shade (cm_mod), so the walk of the combine flags drops out.
 * (MK64 perf round 24 via ocarina-of-time d267ca3; bit-exact, tools/handheld/sweep.sh.)
 * CM=3: texture x shade, then the fog (cm_fog); CM=4: the general combine, then
 * the fog (fogtri); both take a constant alpha the fog factor displaced (sFogA). */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
#define SPAN_LOOP(TEX, ZTEST, ALPHA, BILIN) SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, 0, 0)
#define SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, CM)                                               \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq COLSTEP_##FLAT) {     \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias) continue; }                                      \
                else if (mtest) { const uint32_t ok = lm & 1u; lm >>= 1; if (!ok) continue; }          \
                const int sr = FLAT ? fsr : clamp8_sh16(rq), sg = FLAT ? fsg : clamp8_sh16(gq);         \
                const int sb = FLAT ? fsb : clamp8_sh16(bq), sa = FLAT ? fsa : clamp8_sh16(aq);         \
                int fr, fg, fb, fa, fta = 255; /* fta: the texel alpha a fogged constant alpha multiplies */ \
                if (TEX) {                                                                              \
                    FETCH_##BILIN                                                                       \
                    const int ta = (t>>24)&0xff;                                                        \
                    if (a_tex) fta = ta;                                                                \
                    const int tr = (CM % 4 == 0 && rgb_texa) ? ta : (t>>16)&0xff, tg = (CM % 4 == 0 && rgb_texa) ? ta : (t>>8)&0xff, tb = (CM % 4 == 0 && rgb_texa) ? ta : t&0xff; \
                    if (CM == 1 || CM == 3) { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }   \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (texmix) {                                                          \
                        if (texlerp)    { fr=lkr+((sr-lkr)*tr)/255; fg=lkg+((sg-lkg)*tg)/255; fb=lkb+((sb-lkb)*tb)/255; } \
                        else if (rgb_decal)  { fr=div255(tr*ta + sr*(255-ta)); fg=div255(tg*ta + sg*(255-ta)); \
                                          fb=div255(tb*ta + sb*(255-ta)); }                             \
                        else            { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    }                                                                                   \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = !a_tex ? sa : (!a_in ? ta : div255(ta*sa));                                    \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (CM == 3 || CM == 4) { fr=div255(fr*(255-sa) + fgr*sa); fg=div255(fg*(255-sa) + fgg*sa); \
                                     fb=div255(fb*(255-sa) + fgb*sa);                                    \
                                     if (ALPHA && fga >= 0) fa = fta < 255 ? div255(fta*fga) : fga; } /* the constant alpha (x the texel's, FOGTEXA) */ \
                if (ALPHA) {                                                                            \
                    if (fa < amin) { SS_AREJ; continue; }                                               \
                    if (fa < 255 && (ablend == 1 || (ablend == 2 && (fbuf[idx] >> 24) != 0))) {       \
                        const uint32_t d = fbuf[idx];                                                   \
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;                           \
                        fr = div255(fr*fa + dr*(255-fa));                                               \
                        fg = div255(fg*fa + dg*(255-fa));                                               \
                        fb = div255(fb*fa + db*(255-fa));                                               \
                    }                                                                                   \
                }                                                                                       \
                fbuf[idx] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,ZTEST ? 255 : aout);               \
                if (ZTEST && dmask) zb[idx] = (uint16_t)uz;                                             \
            }
            if (tex && ztest && cm_mod && sFastSpans) {
                /* the hot path: z-tested texture x shade */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                }
            } else if (tex && ztest && cm_fog && sFastSpans) {
                /* the fogged world: z-tested texture x shade, fogged (scalar; NEON above when opaque) */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 3) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 3) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 3)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 3) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 3) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 3) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 3)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 3) }
                }
            } else if (tex && fogtri) {
                /* fogged, not texture x shade (decals, constant alphas): the general loops with the fog step */
                if (ztest) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 4) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 4) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 4)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 4) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, BILINEAR, 0, 4) else SPAN_LOOP_CM(1, 0, 0, BILINEAR, 0, 4) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEAREST, 0, 4)  else SPAN_LOOP_CM(1, 0, 0, NEAREST, 0, 4) }
                }
            } else if (tex) {
                if (flat && ztest && sFastSpans) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 0) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 0) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 0)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 0) }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR) else SPAN_LOOP(1, 1, 0, BILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BILINEAR) else SPAN_LOOP(1, 0, 0, BILINEAR) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEAREST) else SPAN_LOOP(1, 1, 0, NEAREST) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEAREST) else SPAN_LOOP(1, 0, 0, NEAREST) }
                }
            } else if (!ztest && !sUseAlpha && !fogtri && rdq == 0 && gdq == 0 && bdq == 0 && sFastSpans) {
                /* Constant-colour run: no z, no alpha and zero steps, so every
                 * pixel of the span is argb(clamp(rq), clamp(gq), clamp(bq), 255). */
                const uint32_t c = argb((uint8_t)fsr, (uint8_t)fsg, (uint8_t)fsb, aout);
                uint32_t *const row = fbuf + idx;
                if (mskip) { for (int i = 0; i < span; i++) if ((lm >> i) & 1u) row[i] = c; }
                else for (int i = 0; i < span; i++) row[i] = c;
                idx += span;
            } else if (fogtri) {
                if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(0, 1, 1, NEAREST, 0, 4) else SPAN_LOOP_CM(0, 1, 0, NEAREST, 0, 4) }
                else       { if (sUseAlpha) SPAN_LOOP_CM(0, 0, 1, NEAREST, 0, 4) else SPAN_LOOP_CM(0, 0, 0, NEAREST, 0, 4) }
            } else {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(0, 1, 1, NEAREST) else SPAN_LOOP(0, 1, 0, NEAREST) }
                else       { if (sUseAlpha) SPAN_LOOP(0, 0, 1, NEAREST) else SPAN_LOOP(0, 0, 0, NEAREST) }
            }
#undef SPAN_LOOP
#undef SPAN_LOOP_CM
#undef SS_AREJ
#if !defined(HANDHELD)
            if (sSpanStat == 1 && tex && sUseAlpha) {
                sSsBlendSpans++; sSsBlendPx += (uint64_t)span;
                if (ss_rej == span) { sSsDeadSpans++; sSsDeadPx += (uint64_t)span; }
            }
#endif
#undef COLSTEP_0
#undef COLSTEP_1
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
#if !defined(HANDHELD)
    if (sSpanStat < 0) {
        const char *e = getenv("SF64_SPANSTAT"); sSpanStat = e && e[0] == '1';
        if (sSpanStat) atexit(spanstat_report);
    }
    if (sSpanStat) {
        int c;
        if (tex && ztest && sFastSpans && (cm_mod || (cm_fog && !sUseAlpha))) c = SS_ROWS;
        else if (tex && ztest && cm_fog && sFastSpans) c = SS_FOG3;
        else if (tex && fogtri) c = SS_FOG4;
        else if (tex && flat && ztest && sFastSpans) c = SS_FLAT;
        else if (tex && !ztest) c = SS_NOZ;
        else if (tex && texlerp) c = SS_LERP;
        else if (tex && rgb_decal) c = SS_DECALA;
        else if (tex && sTexAdd) c = SS_ADD;
        else if (tex && rgb_texa) c = SS_TEXA;
        else if (tex && !rgb_in) c = SS_TEXONLY;
        else if (tex) c = SS_NOTEX_RGB;
        else if (!ztest && !sUseAlpha && !fogtri && rdq == 0 && gdq == 0 && bdq == 0) c = SS_CONST;
        else if (fogtri) c = SS_UFOG;
        else c = SS_UNTEX;
        sSpanStatPx[c] += tri_px; sSpanStatTri[c]++;
        if (sFrameStat) { /* SF64_FRAMESTAT: this frame's span pixels by texture (the smart bomb study) */
            const void *key = tex ? tex->px : NULL;
            int i;
            for (i = 0; i < sFsN; i++) if (sFs[i].key == key && sFs[i].blend == sUseAlpha) break;
            if (i == sFsN && i < 64) { sFs[i].key = key; sFs[i].blend = sUseAlpha; sFs[i].tw = (uint16_t)tw; sFs[i].th = (uint16_t)th; sFs[i].px = 0; sFs[i].tris = 0; sFs[i].path = (uint8_t)c; sFsN++; }
            if (i < 64) { sFs[i].px += tri_px; sFs[i].tris++; }
            sFsTot += tri_px;
        }
        if (sUseAlpha) sSpanStatBlend[c] += tri_px;
        if (c == SS_NOZ) {
            const uint32_t sh = sCurShader ? sCurShader->shader_id : 0;
            int i;
            for (i = 0; i < sSsNozN; i++)
                if (sSsNoz[i].shader == sh && sSsNoz[i].blend == sUseAlpha && sSsNoz[i].flat == flat &&
                    sSsNoz[i].mod == cm_mod && sSsNoz[i].bilin == use_bilin && sSsNoz[i].tw == tw && sSsNoz[i].th == th) break;
            if (i == sSsNozN && i < 32) {
                sSsNoz[i].shader = sh; sSsNoz[i].blend = sUseAlpha; sSsNoz[i].flat = flat; sSsNoz[i].mod = cm_mod;
                sSsNoz[i].bilin = use_bilin; sSsNoz[i].tw = (uint16_t)tw; sSsNoz[i].th = (uint16_t)th; sSsNozN++;
            }
            if (i < 32) { sSsNoz[i].px += tri_px; sSsNoz[i].tris++; }
        }
    }
#endif
#if defined(MIYOO)
    if (split) {
        miyoo_worker_wait();
        tri_px += (uint32_t)job.px;
        sDfLive += job.live;
        gfx_soft_split_tris++; gfx_soft_split_px += tri_px;
    }
    if (!lead) return (int)tri_px; /* the worker's share: the game thread counts it */
#endif
    sDpBatchPx += tri_px;
    return (int)tri_px;
}

static bool sPhase3D SAVESTATE_EXCLUDE, sIntDirty SAVESTATE_EXCLUDE, sNatDirty SAVESTATE_EXCLUDE; /* see the frame section */
static bool sHad3D SAVESTATE_EXCLUDE;         /* any 3D batch this frame */
static bool sIntClean SAVESTATE_EXCLUDE;      /* internal colour is all-zero (cleared by the last composite) */
static bool sUnified SAVESTATE_EXCLUDE;       /* internal res == native: 3D draws straight to native (see frame section) */
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */

/* Deferred backdrops (the speed round after r18: "feels slower than 30fps"). Late Corneria draws a
 * full-screen sky (64x32 bilinear) without a depth test before the world, and the world then paints over
 * most of it (SF64_SPANSTAT; skipping every such draw was -27% ARM instructions). An opaque draw without
 * a depth test neither reads nor writes depth and never reads the pixel under it, so while nothing else
 * is on the internal target it is queued here (vertices and every bit of state raster_tri reads) and
 * replayed later into only the pixels nothing drew since: direct draws write alpha 255; a replay writes
 * its own tag (1..254) as alpha and passes over pixels holding 255 or another replay's tag (the composite
 * and X12's "empty pixel" test only ask for non-zero). Same pixels, same order where they meet.
 *   A draw that blends with the pixels under it first has the queue replayed over its vertices' box only
 *   (the replay keeps every float the full draw computes: rows are clipped by the scissor, which the span
 *   setup never sees, and columns per pixel, see span_live). Everything else that could tell
 *   the difference replays the whole queue and empties it: the composite and any target switch, and a
 *   texture-cache reset or an in-place texture rewrite (the queued texture memory).
 * SF64_NOZDEFER=0: off; 2: every frame (see the gate below); on the device a nozdefer= line in the display cfg. */
typedef struct {
    struct ShaderProgram sh; SoftTex tex;
    bool has_sh, depth_test, depth_mask, zdecal, tex_add, tex_lerp, force_in, fog_px;
    int lerp_r, lerp_g, lerp_b, fog_r, fog_g, fog_b, fog_a, edge_opaque;
    bool own; /* SF64_FILLRULE's flag (sOwnTri): off for every batch queued (opaque), kept so a replay under a blended draw doesn't take it */
    int vp[4], sc[4];
    int first, n; /* triangles in sDfTri */
} DeferBatch;
#define DEFER_BATCHES 32
#define DEFER_TRIS 512
static DeferBatch sDfB[DEFER_BATCHES] SAVESTATE_EXCLUDE;
static SoftVtx sDfTri[DEFER_TRIS * 3] SAVESTATE_EXCLUDE;
static int sDfNB SAVESTATE_EXCLUDE, sDfNT SAVESTATE_EXCLUDE;
static bool sIntDrawn SAVESTATE_EXCLUDE;     /* a direct draw has written the internal target since its clear */
static uint8_t sTagNext SAVESTATE_EXCLUDE = 1; /* the next replay's tag; back to 1 when the target is cleared */
/* Partial replays in this queue's life: each re-runs every queued triangle over its box, so an effects-heavy
 * moment (dozens of blended sprites and smoke over a queued backdrop of many triangles) cost 80-150 ms
 * frames on the device (round 19's run, the end of Corneria). Past a screen's worth of boxes or 16
 * replays the queue is drawn whole instead: bounded at about one extra pass over the backdrop. */
static int sDfPartN SAVESTATE_EXCLUDE; static uint32_t sDfPartPx SAVESTATE_EXCLUDE;
/* main.c's NOZDEFER line each PERF window: frames that queued a backdrop, partial replays, caps hit */
uint32_t gfx_soft_defer_frames SAVESTATE_EXCLUDE, gfx_soft_defer_partials SAVESTATE_EXCLUDE, gfx_soft_defer_capped SAVESTATE_EXCLUDE;
#define DEFER_PARTIALS 16
static int sDeferCap SAVESTATE_EXCLUDE = 1; /* SF64_DEFERCAP=0 (tests): no cap, as round 19 shipped */
/* 2026-10-11 (the speed audit's bomb item 1): a smart bomb's frames ran up to 16 partial replays of the 34-sheet
 * backdrop (the device's bomb window: 12 a frame, 412 of 750 frames capped), each re-setting-up every queued
 * triangle over its box, and then the full flush anyway. SF64_EARLYFLUSH=n (default 3; 0: off, as before): after
 * n partials in a queue's life, or at a blended draw whose box is over a quarter of the target, the queue is drawn
 * whole instead -- the cap's own path, taken sooner. The flush point only picks the order (the sweeps prove any
 * order gives the same pixels), so the picture is the same. */
#define EARLY_PARTIALS 3
static int sEarlyFlush SAVESTATE_EXCLUDE = EARLY_PARTIALS;
uint32_t gfx_soft_defer_early SAVESTATE_EXCLUDE; /* main.c's NOZDEFER line: queues drawn whole early */
/* the replays' own clock (main.c's NOZDEFER line; drawprof's replay bucket): every replay, partial or full */
uint32_t gfx_soft_replay_us SAVESTATE_EXCLUDE, gfx_soft_replay_px SAVESTATE_EXCLUDE;
static int sNozDefer SAVESTATE_EXCLUDE = -1;   /* SF64_NOZDEFER: 0 off, 1 gated (default), 2 every frame */
/* Round 28 (the owner: the bomb is "all the triangles and transparencies they form"): the backdrop is opaque
 * sheets drawn with no depth test, and in heavy frames (a bomb's shake, a steep climb) all 34 of Corneria's
 * reach the screen, 150-200k shaded pixels where 45k survive. Opaque with no depth test, the last sheet at a
 * pixel is all that shows, so the queue is replayed last-to-first and a pixel written once is done (span_live
 * under sMaskRev): the same pixels, each shaded once. And when the gate would draw the backdrop directly,
 * it is queued and replayed at once instead, so that reverse order serves every frame. SF64_DEFERREV=0: as
 * before. */
static int sDeferRev SAVESTATE_EXCLUDE = -1;
static bool sDfEager SAVESTATE_EXCLUDE; /* round 28: the queue holds backdrops the shut gate queued, to replay before the next draw */
/* The gate: a replay costs ~50 instructions a 16-pixel span it visits on top of the pixels, so deferring
 * only pays where the world covers much of the backdrop (every frame deferred, ARM qprof: late Corneria
 * 3500-4000 -8.6%, early Corneria 1000-1500, open sea and sky, +6.4%). A frame with a queue measures it --
 * the backdrops' span pixels (sDfSpanPx, the full replay's) against the spans a replay drew (sDfLive x
 * 16) -- and the next frame defers only if at least 30% was covered; while it doesn't, every 16th frame
 * queues to look again. Gated: late -8.2%, early +0.9%. Pixels are the same either way (sweeps with
 * SF64_NOZDEFER=2 too): the gate only picks the cheaper order. */
static uint32_t sDfSpanPx SAVESTATE_EXCLUDE;
static bool sDeferPays SAVESTATE_EXCLUDE = true, sDeferGate SAVESTATE_EXCLUDE = true;
/* drawprof's replay bucket (2026-10-11): the replays' time and span pixels, taken out of the batch that set them
 * off (a blended draw over a queued backdrop paid for the backdrop's sheets, so the blast's or the water's ms held
 * them); partial and full replays counted. */
static uint32_t sDpRepUs SAVESTATE_EXCLUDE, sDpRepPx SAVESTATE_EXCLUDE, sDpRepPart SAVESTATE_EXCLUDE, sDpRepFull SAVESTATE_EXCLUDE;
/* Replay the queue over [x0, x1) x [y0, y1) of the internal target, exactly those pixels: a pixel past
 * the box written by one triangle's span and not the next's would pass for final; full: everything, then
 * empty the queue. */
static void defer_replay(int x0, int x1, int y0, int y1, bool full) {
    if (!sDfNB) return;
    if (sTagNext >= 254) full = true; /* out of tags (254 is the full replay's): replay everything */
    PROF_BEGIN();
    const uint64_t px0 = gfx_soft_span_px; const uint32_t bp0 = sDpBatchPx;
    uint32_t *const fb = sFB; uint16_t *const zb = sZB; const int w = sW, h = sH;
    struct ShaderProgram *const sh = sCurShader;
    const bool dt = sDepthTest, dm = sDepthMask, zd = sZDecal, ua = sUseAlpha, ta = sTexAdd, tl = sTexLerp, fi = sForceIn, fp = sFogPx;
    const int lr = sLerpR, lg = sLerpG, lb = sLerpB, fr = sFogR, fg = sFogG, fbl = sFogB, fa = sFogA, eo = sEdgeOpaque;
    const bool ow = sOwnTri;
    const int vx = sVpX, vy = sVpY, vw = sVpW, vh = sVpH, s0 = sScX0, s1 = sScY0, s2 = sScX1, s3 = sScY1;
    sFB = sIntFB; sZB = sIntZB; sW = sIntW; sH = sIntH;
    sMaskTag = full ? 254 : sTagNext++; sUseAlpha = false;
    if (full) { y0 = 0; y1 = sIntH; x0 = 0; x1 = sIntW; }
    sRepX0 = x0; sRepX1 = x1;
    sMaskRev = sDeferRev > 0;
    for (int bi = 0; bi < sDfNB; bi++) {
        const int b = sMaskRev ? sDfNB - 1 - bi : bi;
        const DeferBatch *d = &sDfB[b];
        sCurShader = d->has_sh ? (struct ShaderProgram *)&d->sh : NULL; sReplayTex = &d->tex;
        sDepthTest = d->depth_test; sDepthMask = d->depth_mask; sZDecal = d->zdecal; sTexAdd = d->tex_add;
        sTexLerp = d->tex_lerp; sForceIn = d->force_in; sFogPx = d->fog_px;
        sLerpR = d->lerp_r; sLerpG = d->lerp_g; sLerpB = d->lerp_b;
        sFogR = d->fog_r; sFogG = d->fog_g; sFogB = d->fog_b; sFogA = d->fog_a; sEdgeOpaque = d->edge_opaque;
        sOwnTri = d->own;
        sVpX = d->vp[0]; sVpY = d->vp[1]; sVpW = d->vp[2]; sVpH = d->vp[3];
        sScX0 = d->sc[0]; sScX1 = d->sc[2]; /* rows only: the span setup depends on the left edge, not the first row */
        sScY0 = d->sc[1] > y0 ? d->sc[1] : y0; sScY1 = d->sc[3] < y1 ? d->sc[3] : y1;
        if (sScY0 >= sScY1) continue;
        for (int ii = 0; ii < d->n; ii++) {
            const int i = sMaskRev ? d->first + d->n - 1 - ii : d->first + ii;
            const int px = raster_tri(&sDfTri[i*3+0], &sDfTri[i*3+1], &sDfTri[i*3+2], true);
            if (px >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)px; if (full) sDfSpanPx += (uint32_t)px; }
        }
    }
    sMaskTag = 0; sReplayTex = NULL; sMaskRev = false;
    sFB = fb; sZB = zb; sW = w; sH = h; sCurShader = sh;
    sDepthTest = dt; sDepthMask = dm; sZDecal = zd; sUseAlpha = ua; sTexAdd = ta; sTexLerp = tl; sForceIn = fi; sFogPx = fp;
    sLerpR = lr; sLerpG = lg; sLerpB = lb; sFogR = fr; sFogG = fg; sFogB = fbl; sFogA = fa; sEdgeOpaque = eo;
    sOwnTri = ow;
    sVpX = vx; sVpY = vy; sVpW = vw; sVpH = vh; sScX0 = s0; sScY0 = s1; sScX1 = s2; sScY1 = s3;
    if (full) { sDfNB = sDfNT = 0; sIntDrawn = true; sDfPartN = 0; sDfPartPx = 0; sDfEager = false; }
    { const u32 dt = port_time_us() - _pt0; const uint32_t dpx = (uint32_t)(gfx_soft_span_px - px0);
      gfx_soft_replay_us += dt; gfx_soft_replay_px += dpx;
      sDpRepUs += dt; sDpRepPx += sDpBatchPx - bp0; sDpBatchPx = bp0; if (full) sDpRepFull++; else sDpRepPart++;
      PORT_PROF_ADD(PROF_R3D, dt); }
}
static void defer_flush(void) { defer_replay(0, 0, 0, 0, true); }
/* true: the batch was queued */
static bool defer_batch(const SoftVtx *v, size_t num_tris) {
    if (sNozDefer < 0) sNozDefer = 1; /* (set in gfx_soft_init from SF64_NOZDEFER or the cfg's nozdefer= line) */
    if (sDeferRev < 0) { const char *e = getenv("SF64_DEFERREV"); sDeferRev = !(e && e[0] == '0'); }
    if (sNozDefer && !sDeferGate && sDeferRev && !sUnified && sPhase3D && sFB == sIntFB) {
        /* round 28: the gate is shut, but the backdrop's sheets are still queued, and replayed in reverse just
         * before the first other draw of the frame (nothing is under them yet, so every pixel is live once) */
        if (!sDepthTest && !sUseAlpha && !sIntDrawn && (num_tris >= 4 || sDfEager)) { /* sheets to save: a plain 2-triangle sky draws directly (qemu: queueing it cost +2.4%) */
            sDeferGate = true;
            const bool q = defer_batch(v, num_tris);
            sDeferGate = false;
            if (q) { sDfEager = true; return true; }
            return false;
        }
        if (sDfEager) { const uint32_t keep = sDfSpanPx; defer_flush(); sDfSpanPx = keep; sDfEager = false; }
        return false;
    }
    if (!sNozDefer || !sDeferGate || sUnified || !sPhase3D || sFB != sIntFB) return false;
    if (sDepthTest || sUseAlpha) { /* not a backdrop: replay first where it may blend with what is under it */
        const bool tex_edge = sUseAlpha && sCurShader && sCurShader->cc.opt_texture_edge;
        if (sDfNB && sUseAlpha && !(tex_edge && sEdgeOpaque == 1)) {
            float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
            for (size_t i = 1; i < num_tris * 3; i++) {
                x0 = v[i].x < x0 ? v[i].x : x0; x1 = v[i].x > x1 ? v[i].x : x1;
                y0 = v[i].y < y0 ? v[i].y : y0; y1 = v[i].y > y1 ? v[i].y : y1;
            }
            if (x0 == x0 && x1 == x1 && y0 == y0 && y1 == y1) { /* (a NaN: everything) */
                x0 = x0 < 1.0f ? 0.0f : x0 - 1.0f; x1 = x1 > (float)sIntW ? (float)sIntW : x1 + 2.0f;
                y0 = y0 < 1.0f ? 0.0f : y0 - 1.0f; y1 = y1 > (float)sIntH ? (float)sIntH : y1 + 2.0f;
                const int bx0 = (int)x0, bx1 = (int)x1 > sIntW ? sIntW : (int)x1, by0 = (int)y0, by1 = (int)y1 > sIntH ? sIntH : (int)y1;
                const uint32_t area = bx1 > bx0 && by1 > by0 ? (uint32_t)(bx1 - bx0) * (uint32_t)(by1 - by0) : 0;
                if (sDeferCap && sEarlyFlush && (sDfPartN >= sEarlyFlush || area * 4u > (uint32_t)(sIntW * sIntH))) {
                    gfx_soft_defer_early++; defer_flush(); /* SF64_EARLYFLUSH */
                } else if (!sDeferCap || (sDfPartN < DEFER_PARTIALS && sDfPartPx + area <= (uint32_t)(sIntW * sIntH))) {
                    sDfPartN++; sDfPartPx += area; gfx_soft_defer_partials++;
                    defer_replay(bx0, bx1, by0, by1, false);
                } else { gfx_soft_defer_capped++; defer_flush(); }
            } else defer_flush();
        }
        return false;
    }
    if (sIntDrawn) return false; /* something is already drawn: it has to stay on top of this one */
    if (sDfNB == DEFER_BATCHES || sDfNT + (int)num_tris > DEFER_TRIS) { defer_flush(); return false; }
    DeferBatch *d = &sDfB[sDfNB++];
    d->has_sh = sCurShader != NULL; if (sCurShader) d->sh = *sCurShader;
    { const uint32_t id = sBoundTex[0]; if (id < MAX_TEXTURES) d->tex = sTex[id]; else memset(&d->tex, 0, sizeof(d->tex)); }
    d->depth_test = sDepthTest; d->depth_mask = sDepthMask; d->zdecal = sZDecal; d->tex_add = sTexAdd;
    d->tex_lerp = sTexLerp; d->force_in = sForceIn; d->fog_px = sFogPx;
    d->lerp_r = sLerpR; d->lerp_g = sLerpG; d->lerp_b = sLerpB;
    d->fog_r = sFogR; d->fog_g = sFogG; d->fog_b = sFogB; d->fog_a = sFogA; d->edge_opaque = sEdgeOpaque;
    d->own = sOwnTri;
    d->vp[0] = sVpX; d->vp[1] = sVpY; d->vp[2] = sVpW; d->vp[3] = sVpH;
    d->sc[0] = sScX0; d->sc[1] = sScY0; d->sc[2] = sScX1; d->sc[3] = sScY1;
    d->first = sDfNT; d->n = (int)num_tris;
    memcpy(&sDfTri[sDfNT * 3], v, num_tris * 3 * sizeof(SoftVtx));
    sDfNT += (int)num_tris;
    return true;
}

/* Round 27, the device draw profiler (the owner: "bomb is better but still laggy ... go ham"): qemu counts
 * instructions, but the bomb's device frames cost 3-4x what those predict. drawprof=1 in sf64_display.cfg (or
 * SF64_DRAWPROF=1; off by default) times every 3D batch by its texture; a frame whose batches took over 40 ms logs its six
 * costliest: texture size, blended, the texture's source (an offset into gPortSceneRam when it lies there,
 * else the address), ms, triangles, span pixels. The clock adds ~1 us a batch; off, nothing. */
static int sDrawProf SAVESTATE_EXCLUDE = -1;
static uint32_t sDpMinUs SAVESTATE_EXCLUDE = 40000; /* SF64_DRAWPROF_MS=n: log frames over n ms (tests: 0 logs every frame) */
static struct { const void *src; uint16_t tw, th; uint8_t blend; uint32_t us, tris, px; } sDp[48] SAVESTATE_EXCLUDE;
static int sDpN SAVESTATE_EXCLUDE; static uint32_t sDpTot SAVESTATE_EXCLUDE, sDpFrame SAVESTATE_EXCLUDE, sDpPx SAVESTATE_EXCLUDE;
static void drawprof_init(void) {
    const char *e = getenv("SF64_DRAWPROF");
    sDrawProf = e && e[0] == '1'; /* off by default everywhere (it was on on the device for the round 27 bomb study) */
    { const char *m = getenv("SF64_DRAWPROF_MS"); if (m && m[0]) sDpMinUs = (uint32_t)atoi(m) * 1000u; }
    if (!e) {
        const char *dir = getenv("SF64_DATA");
        char path[512], line[64];
        snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) if (strncmp(line, "drawprof=", 9) == 0 && (line[9] == '0' || line[9] == '1')) sDrawProf = line[9] - '0';
            fclose(f);
        }
    }
    if (sDrawProf) port_log_always("drawprof: on (3D batches timed by texture; frames over 40 ms logged)\n");
}
static void drawprof_end(void) {
    if (sDrawProf <= 0) return;
    if (sDpTot > sDpMinUs && sDpN) {
        extern unsigned char gPortSceneRam[];
        int order[48]; for (int i = 0; i < sDpN; i++) order[i] = i;
        for (int i = 0; i < sDpN; i++) for (int j = i + 1; j < sDpN; j++) if (sDp[order[j]].us > sDp[order[i]].us) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        char buf[768]; int n = snprintf(buf, sizeof(buf), "DRAWPROF frame %u: 3D batches %.1f ms, %u px |", sDpFrame, sDpTot / 1000.0f, sDpPx);
        for (int k = 0; k < sDpN && k < 6 && n < (int)sizeof(buf) - 90; k++) {
            const int i = order[k];
            const uintptr_t a = (uintptr_t)sDp[i].src, b = (uintptr_t)gPortSceneRam;
            if (a >= b && a < b + 0x400000u)
                n += snprintf(buf + n, sizeof(buf) - n, " %ux%u%s scene+%lu %.1fms %ut %upx", sDp[i].tw, sDp[i].th, sDp[i].blend ? "b" : "",
                              (unsigned long)(a - b), sDp[i].us / 1000.0f, sDp[i].tris, sDp[i].px);
            else
                n += snprintf(buf + n, sizeof(buf) - n, " %ux%u%s %p %.1fms %ut %upx", sDp[i].tw, sDp[i].th, sDp[i].blend ? "b" : "",
                              sDp[i].src, sDp[i].us / 1000.0f, sDp[i].tris, sDp[i].px);
        }
        port_log_always("%s | replay %.1fms %upx %u partial %u full\n", buf, sDpRepUs / 1000.0f, sDpRepPx, sDpRepPart, sDpRepFull);
    }
    sDpN = 0; sDpTot = 0; sDpPx = 0; sDpFrame++; sDpRepUs = sDpRepPx = sDpRepPart = sDpRepFull = 0;
}
static void gfx_soft_draw_triangles_impl(float buf_vbo[], size_t buf_vbo_len, size_t num_tris);
static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    if (sDrawProf < 0) drawprof_init();
    if (!sDrawProf) { gfx_soft_draw_triangles_impl(buf_vbo, buf_vbo_len, num_tris); return; }
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    const void *src = tex ? sTexSrc[sBoundTex[0]] : NULL;
    const uint16_t tw = tex ? (uint16_t)tex->w : 0, th = tex ? (uint16_t)tex->h : 0; const uint8_t bl = sUseAlpha ? 1 : 0;
    sDpBatchPx = 0;
    const uint32_t r0 = sDpRepUs;
    const uint32_t t0 = port_time_us();
    gfx_soft_draw_triangles_impl(buf_vbo, buf_vbo_len, num_tris);
    const uint32_t dt = port_time_us() - t0 - (sDpRepUs - r0); /* the replays it set off: their own bucket */
    int i;
    for (i = 0; i < sDpN; i++) if (sDp[i].src == src && sDp[i].tw == tw && sDp[i].th == th && sDp[i].blend == bl) break;
    if (i == sDpN && i < 48) { sDp[i].src = src; sDp[i].tw = tw; sDp[i].th = th; sDp[i].blend = bl; sDp[i].us = 0; sDp[i].tris = 0; sDp[i].px = 0; sDpN++; }
    if (i < 48) { sDp[i].us += dt; sDp[i].tris += (uint32_t)num_tris; sDp[i].px += sDpBatchPx; }
    sDpTot += dt + (sDpRepUs - r0); sDpPx += sDpBatchPx; /* (the total as before: batches with their replays) */
}
static void gfx_soft_draw_triangles_impl(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    target_ready();
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
    if (defer_batch(v, num_tris)) return; /* (a replay it sets off keeps its own clock) */
    PROF_BEGIN();
    if (sFB == sIntFB && num_tris) sIntDrawn = true;
    for (size_t i = 0; i < num_tris; i++) {
        const int px = raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
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
    int16_t u, v; /* signed: a rect left of / above its tile origin gives -1, not 65535 */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* HUD unsquish (round 21, the owner's calls). CONVERT squeezes the 320x240 frame 4 -> 3 at present, text and
 * icons with it. The HUD pieces the game tags (fox_hud.c PORT_HUD_ANCHOR, gfx_pc gfx_hud_piece) draw instead into
 * a square layer of their own at 1:1, each moved to its place on the panel, and present lays the layer over
 * the squeezed frame. The layer is premultiplied (rgb = colour x coverage, alpha = coverage), so a translucent
 * piece -- the radio's text box -- still shows the frame under it. A piece's perspective parts (the gold rings,
 * the radar's marks) get a 3D pass of their own on a 320x240 target and are moved the same way.
 *   1 left: x kept   2 right: x - 80   3 centre: x - 40   4 radio: x - HUD_RADIO_DX
 *   5 radar: scaled HUD_RADAR_S about its bottom-right corner into the panel's (beside a radio message)
 *   7 / 8: the tally's left / right team column: centred, HUD_TEAM_DX nearer the middle one
 *   0/6: the squeezed frame, as before. gfx_soft_hud_unsquish: gfx_sdl12 (CONVERT; hud=0 / SF64_HUDUNSQUISH=0 off). */
int gfx_soft_hud_unsquish SAVESTATE_EXCLUDE;
static int sHudAnchor SAVESTATE_EXCLUDE;
#define HUD_W 240
#define HUD_H 240
#define HUD_RADIO_DX 22           /* the radio's team shield bar starts at x 22 */
#define HUD_RADAR_S 0.55f
#define HUD_TEAM_DX 12              /* the owner: the tally's portraits closer together */
#define HUD_RADAR_SX 304.0f       /* the radar frame's bottom-right corner (222 + 48 x 1.7, 148 + 44 x 1.7) */
#define HUD_RADAR_SY 223.0f
static uint32_t *sHudFB SAVESTATE_EXCLUDE;
static int sHudY0 SAVESTATE_EXCLUDE = HUD_H, sHudY1 SAVESTATE_EXCLUDE; /* rows holding anything */
/* and per row the columns (present overlays only those: the whole rows cost ~0.8M ARM instructions a frame) */
static uint8_t sHudRX0[HUD_H] SAVESTATE_EXCLUDE, sHudRX1[HUD_H] SAVESTATE_EXCLUDE;
static void hud_mark(int y0, int y1, int x0, int x1) {
    if (x0 < 0) x0 = 0;
    if (x1 > HUD_W) x1 = HUD_W;
    if (y0 < 0) y0 = 0;
    if (y1 > HUD_H) y1 = HUD_H;
    if (x0 >= x1 || y0 >= y1) return;
    if (y0 < sHudY0) sHudY0 = y0;
    if (y1 > sHudY1) sHudY1 = y1;
    for (int y = y0; y < y1; y++) {
        if (sHudRX1[y] == 0 || x0 < sHudRX0[y]) sHudRX0[y] = (uint8_t)x0;
        if (x1 > sHudRX1[y]) sHudRX1[y] = (uint8_t)x1;
    }
}
static bool sHudBlend SAVESTATE_EXCLUDE;                              /* the 2D path is drawing into sHudFB */
int gfx_soft_hud_anchor_now(void) { return sHudAnchor; }
static inline bool hud_on(void) { return gfx_soft_hud_unsquish && sHudAnchor >= 1 && sHudAnchor <= 8 && sHudAnchor != 6; }
static inline int hud_tx(int x) { /* 320-frame x -> panel x, by the current anchor */
    switch (sHudAnchor) {
        case 2: return x - 80;
        case 3: return x - 40;
        case 4: return x - HUD_RADIO_DX;
        case 5: return (int)port_ifloorf((float)HUD_W - (HUD_RADAR_SX - (float)x) * HUD_RADAR_S + 0.5f);
        case 7: return x - 40 + HUD_TEAM_DX;
        case 8: return x - 40 - HUD_TEAM_DX;
        default: return x;
    }
}
static inline int hud_ty(int y) {
    return sHudAnchor == 5 ? (int)port_ifloorf(HUD_RADAR_SY - (HUD_RADAR_SY - (float)y) * HUD_RADAR_S + 0.5f) : y;
}
/* alpha of a blended write: 255 on the frame; on the HUD layer the coverage of "over" (rgb is premultiplied,
 * so the frame's own blend already gives the premultiplied colour) */
#define HUD_OVER_A(d, a) (sHudBlend ? (uint8_t)((a) + ((int)((d) >> 24) * (255 - (a))) / 255) : 255)

/* X14, magnified 2D sprites (super-smash-bros 872636f): the N64 filters texture rectangles like triangles, so a
 * sprite drawn bigger than its texture comes out smooth; the port sampled it nearest (square blocks). gfx_pc
 * sets gfx_soft_2d_bilin from the draw's G_TF_BILERP per sprite; one magnified along either axis takes the
 * 3D path's bilerp at the pixel centre. 1:1 and shrunk sprites are unchanged. SF64_2DBILIN=0: as before. */
bool gfx_soft_2d_bilin SAVESTATE_EXCLUDE;
static int sBilin2DOn SAVESTATE_EXCLUDE = -1;
static uint64_t sSs2dMagPx SAVESTATE_EXCLUDE; /* SF64_SPANSTAT: pixels of magnified bilerp sprites */
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    (void)buf_vbo_len;
    if (!sHudBlend && hud_on() && sFB == sNatFB) { /* a tagged HUD piece: onto the 1:1 layer */
        if (!sHudFB) { sHudFB = (uint32_t *)calloc((size_t)HUD_W * HUD_H, 4); if (!sHudFB) return; }
        uint32_t *const fb = sFB; const int w = sW, h = sH, s0 = sScX0, s1 = sScY0, s2 = sScX1, s3 = sScY1;
        sFB = sHudFB; sW = HUD_W; sH = HUD_H; sScX0 = 0; sScY0 = 0; sScX1 = HUD_W; sScY1 = HUD_H;
        sHudBlend = true;
        gfx_scegu_draw_triangles_2d(buf_vbo, buf_vbo_len, num_sprites);
        sHudBlend = false;
        sFB = fb; sW = w; sH = h; sScX0 = s0; sScY0 = s1; sScX1 = s2; sScY1 = s3;
        return;
    }
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { untex_note(); tex = NULL; }
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
        if (sHudBlend) { x0 = hud_tx(x0); x1 = hud_tx(x1); y0 = hud_ty(y0); y1 = hud_ty(y1); }
        float u0 = a->u, u1 = b->u, v0 = a->v, v1 = b->v;
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; float tu = u0; u0 = u1; u1 = tu; }
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; float tv = v0; v0 = v1; v1 = tv; }
        if (x1 <= x0 || y1 <= y0) continue;
        const float dudx = (u1 - u0) / (float)(x1 - x0);
        const float dvdy = (v1 - v0) / (float)(y1 - y0);
        int cx0 = imax(imax(x0, sScX0), 0), cx1 = imin(imin(x1, sScX1), sW);
        int cy0 = imax(imax(y0, sScY0), 0), cy1 = imin(imin(y1, sScY1), sH);
        const int cr = b->r, cg = b->g, cb2 = b->b, ca = b->a; /* GU: colour of 2nd vertex */
        if (sHudBlend) hud_mark(cy0, cy1, cx0, cx1);
        if (!tex && (!sUseAlpha || ca >= 255) && cx0 == 0 && cy0 == 0 && cx1 == sW && cy1 == sH && sFB == sNatFB && sLazyClear > 0) {
            /* an opaque rect over the whole target: it becomes the frame's fill */
            sNatStale = true; sNatFill = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255);
            continue;
        }
        if (!tex && sUseAlpha && ca < 255 && sNatStale && sFB == sNatFB && sLazyClear > 0 &&
            cx0 <= imax(sScX0, 0) + 2 && cy0 <= imax(sScY0, 0) + 2 && cx1 >= imin(sScX1, sW) - 2 &&
            cy1 >= imin(sScY1, sH) - 2 && (cx1 - cx0) * (cy1 - cy0) * 5 >= sW * sH * 4) {
            /* Motion blur: a translucent rect over the whole scissored screen
             * opens the frame -- the game fades the old picture instead of
             * clearing. Keep the ring buffer's picture (frame - 3) under it.
             * (Star Fox's rect stops a pixel short of its scissor: 1-cycle
             * rects exclude the lower-right edge, hence the 2 px slack.) */
            static uint32_t sBlurFrames SAVESTATE_EXCLUDE;
            if ((sBlurFrames++ % 300) == 0) {
                port_log("gfx_soft: motion-blur frames %u (alpha %d)\n", (unsigned) sBlurFrames, ca);
            }
            sNatStale = false;
            /* Fade the whole scissored screen: the column the rect leaves out
             * sat in a CRT's overscan, but here it would keep every picture ever
             * drawn into it -- a bright line down the panel's right edge. */
            cx0 = imax(sScX0, 0); cy0 = imax(sScY0, 0); cx1 = imin(sScX1, sW); cy1 = imin(sScY1, sH);
        }
        target_ready();
        /* Per-sprite constants hoisted out of the pixel loop. amin folds the
         * texture-edge (0x55) and near-zero (8) alpha rejects. "plain" = the
         * texel is the final colour (no combiner input, or a white opaque
         * prim, which is what nearly every HUD sprite and menu tile uses). */
        const bool use_alpha = sUseAlpha;
        const int amin = use_alpha ? ((sCurShader && sCurShader->cc.opt_texture_edge) ? 0x55 : 8) : 0;
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa, rgb_decal = sCurShader && sCurShader->rgb_decal;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
#if !defined(HANDHELD)
        if (sSpanStat == 1 && tex) {
            const uint64_t npx = (uint64_t)(cx1 > cx0 ? cx1 - cx0 : 0) * (uint64_t)(cy1 > cy0 ? cy1 - cy0 : 0);
            sSs2dPx += npx;
            if (!rgb_tex && a_tex && use_alpha) sSs2dInTexA += npx;
        }
#endif
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (sBilin2DOn < 0) { const char *e = getenv("SF64_2DBILIN"); sBilin2DOn = !(e && e[0] == '0'); }
        const bool mag2 = tex && gfx_soft_2d_bilin && (dudx < 0.999f || dvdy < 0.999f);
#if !defined(HANDHELD)
        if (sSpanStat == 1 && mag2) sSs2dMagPx += (uint64_t)(cx1 > cx0 ? cx1 - cx0 : 0) * (uint64_t)(cy1 > cy0 ? cy1 - cy0 : 0);
#endif
        const bool bil2 = mag2 && sBilin2DOn;
        const bool plain = tex && !bil2 && !t16 && rgb_tex && a_tex && !rgb_texa &&
                           ((!rgb_in && !a_in) || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
                    const uint32_t c = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255);
                    for (int x = cx0; x < cx1; x++) row[x] = c;
                } else if (ca >= amin) {
                    for (int x = cx0; x < cx1; x++) {
                        const uint32_t d = row[x];
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                        row[x] = argb((uint8_t)((cr*ca + dr*(255-ca))/255), (uint8_t)((cg*ca + dg*(255-ca))/255), (uint8_t)((cb2*ca + db*(255-ca))/255), HUD_OVER_A(d, ca));
                    }
                }
                continue;
            }
            const float v = v0 + (y - y0 + 0.5f) * dvdy;
            const int32_t vf_row = bil2 ? to_fix16(v) : 0; /* bilerp: texel coordinate of the pixel centre */
            const int iv = wrapf(ifloorf(v), th, wt2, mv);
            const int trow_base = iv * tw; /* texel index of this scanline's row start */
            int32_t uf = to_fix16(u0 + (cx0 - x0 + 0.5f) * dudx);
            if (plain) {
                /* 1:1 copy inside the texture: straight run, no wrap per texel. */
                const int iu0 = uf >> 16;
                if (duf == 65536 && iu0 >= 0 && iu0 + (cx1 - cx0) <= tw) {
                    const uint32_t *src = (const uint32_t *)texpx + trow_base + iu0;
                    if (!use_alpha) {
                        for (int x = cx0; x < cx1; x++) row[x] = src[x - cx0] | 0xFF000000u;
                    } else {
                        for (int x = cx0; x < cx1; x++) {
                            const uint32_t t = src[x - cx0]; const int ta = (int)(t >> 24);
                            if (ta < amin) continue;
                            if (ta >= 255) { row[x] = t | 0xFF000000u; continue; }
                            const uint32_t d = row[x];
                            const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                            row[x] = argb((uint8_t)((((t>>16)&0xff)*ta + dr*(255-ta))/255), (uint8_t)((((t>>8)&0xff)*ta + dg*(255-ta))/255), (uint8_t)(((t&0xff)*ta + db*(255-ta))/255), HUD_OVER_A(d, ta));
                        }
                    }
                    continue;
                }
                for (int x = cx0; x < cx1; x++, uf += duf) {
                    const uint32_t t = texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                    if (!use_alpha) { row[x] = t | 0xFF000000u; continue; }
                    const int ta = (int)(t >> 24);
                    if (ta < amin) continue;
                    if (ta >= 255) { row[x] = t | 0xFF000000u; continue; }
                    const uint32_t d = row[x];
                    const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                    row[x] = argb((uint8_t)((((t>>16)&0xff)*ta + dr*(255-ta))/255), (uint8_t)((((t>>8)&0xff)*ta + dg*(255-ta))/255), (uint8_t)(((t&0xff)*ta + db*(255-ta))/255), HUD_OVER_A(d, ta));
                }
                continue;
            }
            /* General: per-channel combiner (RGB and alpha each from the
             * texel, the input colour, or their product). */
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = bil2 ? bilerp(texpx, t16, tw, th, ws2, wt2, uf, vf_row, mu, mv)
                                        : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                const int ta = (t>>24)&0xff;
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff;
                int fr, fg, fb, fa;
                if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }
                else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }
                else if (sTexLerp) { fr=sLerpR+((cr-sLerpR)*tr)/255; fg=sLerpG+((cg-sLerpG)*tg)/255; fb=sLerpB+((cb2-sLerpB)*tb)/255; }
                else if (rgb_decal) { fr=(tr*ta + cr*(255-ta))/255; fg=(tg*ta + cg*(255-ta))/255; fb=(tb*ta + cb2*(255-ta))/255; }
                else if (sTexAdd) { fr=tr+cr; fg=tg+cg; fb=tb+cb2; if (fr>255)fr=255; if (fg>255)fg=255; if (fb>255)fb=255; }
                else              { fr=(tr*cr)/255; fg=(tg*cg)/255; fb=(tb*cb2)/255; }
                fa = !a_tex ? ca : (!a_in ? ta : (ta*ca)/255);
                uint8_t oa = 255;
                if (use_alpha) {
                    if (fa < amin) continue;
                    if (fa < 255) {
                        const uint32_t d = row[x];
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                        fr = (fr*fa + dr*(255-fa))/255;
                        fg = (fg*fa + dg*(255-fa))/255;
                        fb = (fb*fa + db*(255-fa))/255;
                        oa = HUD_OVER_A(d, fa);
                    }
                }
                row[x] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,oa);
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
/* Motion blur (Star Fox, gBlurAlpha < 255: warp zones, the level-exit warp,
 * Aquas, Andross): instead of clearing, the game opens the frame with a
 * translucent background-colour rect over what the framebuffer already holds.
 * The N64 renders into three framebuffers in turn (gFrameBuffers[frame % 3]),
 * so that is the picture from three frames back, faded -- the long, stepped
 * trails of the real console. Native colour is therefore a ring of three
 * buffers, rotated per rendered frame (sNatFB is the current one), and the
 * lazy clear is skipped when such a rect opens the frame (see draw_rects).
 * Cost: two more native buffers, no copies. */
static uint32_t *sNatRing[3] SAVESTATE_EXCLUDE;
static int sNatRingIdx SAVESTATE_EXCLUDE;
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
static void nat_ready(void) {
    if (sNatStale && sNatFB) {
        const uint32_t c = sNatFill; uint32_t *p = sNatFB; const int n = sNatW * sNatH;
        if (c == 0) memset(p, 0, (size_t)n * 4); else for (int i = 0; i < n; i++) p[i] = c;
        sNatStale = false;
    }
}
/* Before writing the current target (only native is cleared lazily). */
static void target_ready(void) { if (sFB == sNatFB) nat_ready(); }
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { nat_ready(); if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }

static void alloc_target(uint32_t **fb, uint16_t **zb, int *w, int *h, int nw, int nh) {
    if (*fb && *w == nw && *h == nh) return;
    free(*fb); free(*zb);
    *w = nw; *h = nh;
    *fb = (uint32_t *)malloc((size_t)nw * nh * 4);
    *zb = (uint16_t *)malloc((size_t)nw * nh * 2);
}

static void select_target(bool is3d) {
    extern struct GfxDimensions gfx_current_dimensions;
    defer_flush();
    sPhase3D = is3d;
    if (is3d && !sUnified) { sFB = sIntFB; sZB = sIntZB; sW = sIntW; sH = sIntH; }
    else                   { sFB = sNatFB; sZB = sNatZB; sW = sNatW; sH = sNatH; }
    sScX0 = 0; sScY0 = 0; sScX1 = sW; sScY1 = sH;
    sVpX = 0; sVpY = 0; sVpW = sW; sVpH = sH;
    gfx_current_dimensions.width = (uint32_t)sW;
    gfx_current_dimensions.height = (uint32_t)sH;
    gfx_current_dimensions.aspect_ratio = sH ? (float)sW / (float)sH : 1.0f;
}

/* Nearest-upscale the internal 3D image into native where the 3D wrote
 * (alpha == 255), then clear it for any later 3D batch this frame. */
/* Row bookkeeping for the internal target (diddy-kong-racing a0ee8b5, the lead's P19).
 * At 3D 100% Star Fox switches 2D<->3D ~5 times a frame (the HUD, radio and
 * lock-on markers between 3D passes), and a full-screen seed + composite +
 * clear per switch cost 3-6 ms on the device (24 Sep log). The seed is lazy --
 * a row is copied from native the first time a triangle touches it -- and the
 * composite and clear cover only the rows seeded or drawn since the last
 * composite. The output is identical: untouched rows hold no drawn (alpha > 0)
 * pixel, and a row is seeded before any triangle blends against it. */
static bool sSeedPending SAVESTATE_EXCLUDE;
static uint16_t sSeedGen SAVESTATE_EXCLUDE, sRowGen[MAP_MAX] SAVESTATE_EXCLUDE;
static int sTouchY0 SAVESTATE_EXCLUDE = 1 << 30, sTouchY1 SAVESTATE_EXCLUDE = -1; /* [y0, y1) internal rows */
static void seed_row(int y) {
    const uint32_t *src = sNatFB + sI2N_y[y] * sNatW;
    uint32_t *dst = sIntFB + y * sIntW;
    for (int x = 0; x < sIntW; x++) dst[x] = src[sI2N_x[x]] & 0x00FFFFFFu;
}
static void int_rows_touch(int y0, int y1) {
    if (y0 < sTouchY0) sTouchY0 = y0;
    if (y1 > sTouchY1) sTouchY1 = y1;
    if (sSeedPending) { /* (no profiling clock here: this runs per triangle; seeds count as r3d) */
        for (int y = y0; y < y1 && y < MAP_MAX; y++) {
            if (sRowGen[y] != sSeedGen) { sRowGen[y] = sSeedGen; seed_row(y); }
        }
    }
}
static void int_rows_reset(void) {
    sTouchY0 = 1 << 30; sTouchY1 = -1;
    sSeedPending = false;
}
static void composite_internal(void) {
    defer_flush();
    sIntDrawn = false; sTagNext = 1;
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    nat_ready();
    PROF_BEGIN();
    const int ty0 = sTouchY0 < 0 ? 0 : sTouchY0, ty1 = sTouchY1 > sIntH ? sIntH : sTouchY1;
    if (sIntW == sNatW && sIntH == sNatH) {
        for (int i = 0; i < sNatW * sNatH; i++) if (sIntFB[i] >> 24) sNatFB[i] = sIntFB[i];
        memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
    } else if (ty0 < ty1) {
        for (int y = 0; y < sNatH; y++) {
            if (sN2I_y[y] < ty0 || sN2I_y[y] >= ty1) continue;
            const uint32_t *src = sIntFB + sN2I_y[y] * sIntW;
            uint32_t *dst = sNatFB + y * sNatW;
            for (int x = 0; x < sNatW; x++) {
                const uint32_t p = src[sN2I_x[x]];
                if (p >> 24) dst[x] = p; /* (a deferred backdrop's pixel keeps its replay tag as alpha: native alpha is only ever tested for non-zero) */
            }
        }
        memset(sIntFB + ty0 * sIntW, 0, (size_t)sIntW * (ty1 - ty0) * 4);
    }
    int_rows_reset();
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
    nat_ready(); /* rows are read from native, lazily */
    sIntClean = false;
    if (sIntW == sNatW && sIntH == sNatH) {
        PROF_BEGIN();
        for (int i = 0; i < sIntW * sIntH; i++) sIntFB[i] = sNatFB[i] & 0x00FFFFFFu;
        PROF_END(PROF_COMP);
        return;
    }
    /* lazy: rows are copied as triangles reach them (int_rows_touch) */
    sSeedPending = true;
    if (++sSeedGen == 0) { memset(sRowGen, 0, sizeof(sRowGen)); sSeedGen = 1; }
}

/* gfx_pc direct-call: switch the draw target for the next batch. */
static bool sHud3Pass SAVESTATE_EXCLUDE; static int sHud3Anchor SAVESTATE_EXCLUDE;
/* A tagged piece's perspective parts: its own 3D pass on the 320x240 HUD target (z cleared, nothing under
 * it), then moved onto the layer by the piece's anchor as opaque pixels. */
static void hud3_begin(void) {
    extern struct GfxDimensions gfx_current_dimensions;
    if (!sHud3FB) {
        sHud3FB = (uint32_t *)calloc((size_t)HUD3_W * HUD3_H, 4);
        sHud3ZB = (uint16_t *)malloc((size_t)HUD3_W * HUD3_H * 2);
        if (!sHud3FB || !sHud3ZB) { free(sHud3FB); free(sHud3ZB); sHud3FB = NULL; sHud3ZB = NULL; return; }
        memset(sHud3ZB, 0xFF, (size_t)HUD3_W * HUD3_H * 2);
    }
    if (!sHudFB) { sHudFB = (uint32_t *)calloc((size_t)HUD_W * HUD_H, 4); if (!sHudFB) return; }
    defer_flush();
    sHud3Pass = true; sHud3Anchor = sHudAnchor; sPhase3D = true;
    sFB = sHud3FB; sZB = sHud3ZB; sW = HUD3_W; sH = HUD3_H;
    sScX0 = 0; sScY0 = 0; sScX1 = sW; sScY1 = sH;
    sVpX = 0; sVpY = 0; sVpW = sW; sVpH = sH;
    gfx_current_dimensions.width = HUD3_W; gfx_current_dimensions.height = HUD3_H;
    gfx_current_dimensions.aspect_ratio = (float)HUD3_W / (float)HUD3_H;
}
static void hud3_end(void) {
    sHud3Pass = false;
    if (sHud3Y0 >= sHud3Y1) return;
    const int save = sHudAnchor; sHudAnchor = sHud3Anchor;
    const int y0 = sHud3Y0 < 0 ? 0 : sHud3Y0, y1 = sHud3Y1 > HUD3_H ? HUD3_H : sHud3Y1;
    if (sHudAnchor == 5) { /* scaled: each panel pixel takes the frame pixel it lands on */
        const int dx0 = hud_tx(0), dx1 = hud_tx(HUD3_W), dy0 = hud_ty(y0), dy1 = hud_ty(y1);
        for (int y = dy0 < 0 ? 0 : dy0; y < dy1 && y < HUD_H; y++) {
            const int sy = (int)(HUD_RADAR_SY - (HUD_RADAR_SY - ((float)y + 0.5f)) / HUD_RADAR_S);
            if (sy < y0 || sy >= y1) continue;
            for (int x = dx0 < 0 ? 0 : dx0; x < dx1 && x < HUD_W; x++) {
                const int sx = (int)(HUD_RADAR_SX - ((float)HUD_W - ((float)x + 0.5f)) / HUD_RADAR_S);
                if (sx < 0 || sx >= HUD3_W) continue;
                const uint32_t p = sHud3FB[sy * HUD3_W + sx];
                if (p >> 24) { sHudFB[y * HUD_W + x] = p | 0xFF000000u; hud_mark(y, y + 1, x, x + 1); }
            }
        }
    } else {
        const int dx = hud_tx(0);
        for (int y = y0; y < y1 && y < HUD_H; y++) {
            const uint32_t *src = sHud3FB + y * HUD3_W;
            uint32_t *dst = sHudFB + y * HUD_W;
            int any = 0;
            for (int x = 0; x < HUD3_W; x++) {
                const int xd = x + dx;
                if (xd < 0 || xd >= HUD_W || !(src[x] >> 24)) continue;
                dst[xd] = src[x] | 0xFF000000u;
                if (!any) { any = 1; hud_mark(y, y + 1, xd, xd + 1); }
                else if (xd + 1 > sHudRX1[y]) sHudRX1[y] = (uint8_t)(xd + 1);
            }
        }
    }
    sHudAnchor = save;
    memset(sHud3FB + y0 * HUD3_W, 0, (size_t)(y1 - y0) * HUD3_W * 4);
    memset(sHud3ZB + y0 * HUD3_W, 0xFF, (size_t)(y1 - y0) * HUD3_W * 2);
    memset(sHud3Seeded + y0, 0, (size_t)(y1 - y0));
    sHud3Y0 = HUD3_H; sHud3Y1 = 0;
}
void gfx_soft_set_hud_anchor(int a) { sHudAnchor = a; }
/* A 3D piece may blend over what the piece's 2D parts drew (the radio's damage flash, a translucent quad over
 * the portrait: round 21 drew it over nothing, a solid red square -- the owner's IMG_0192). So the rows a
 * pass touches first take the layer's pixels (moved back by the anchor, alpha 0 = nothing drawn, as the 3D
 * target's own seed), and only what the pass draws goes back. Not for the radar: its marks are opaque. */
static void hud3_seed(int y0, int y1) {
    if (!sHudFB || sHud3Anchor == 5) return;
    const int save = sHudAnchor; sHudAnchor = sHud3Anchor;
    const int d = hud_tx(0); /* panel x = frame x + d for the moved anchors */
    sHudAnchor = save;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < HUD3_H; y++) {
        if (sHud3Seeded[y]) continue;
        sHud3Seeded[y] = 1;
        if (y >= HUD_H || y < sHudY0 || y >= sHudY1) continue;
        const uint32_t *src = sHudFB + y * HUD_W;
        uint32_t *dst = sHud3FB + y * HUD3_W;
        for (int x = 0; x < HUD3_W; x++) {
            const int xs = x + d;
            if (xs >= 0 && xs < HUD_W) dst[x] = src[xs] & 0x00FFFFFFu;
        }
    }
}
/* present (gfx_sdl12.c): lay the layer's row y over the squeezed RGB565 row, before the VI gamma */
int gfx_soft_hud_rows(int *y0, int *y1) {
    if (!gfx_soft_hud_unsquish || !sHudFB || sHudY0 >= sHudY1) return 0;
    *y0 = sHudY0; *y1 = sHudY1; return 1;
}
/* Round 25, the crosshair (gfx_pc gfx_reticle_capture): the quad's box (current target pixels) and its combined
 * texels. The texture's line art goes on the 1:1 HUD layer in the box's place on the panel: shrunk, every
 * opaque texel is one pixel at X0 + tx x N / tw (so a 1-px line stays 1 px, as sharp as the HUD); grown past
 * the texture's size, each pixel takes its nearest texel. Composited over the layer once, in the draw's own
 * alpha (the quad was XLU). */
#if defined(HANDHELD)
void gfx_soft_reticle_lineart(const float bb[4], const uint32_t *px, int tw, int th) {
    extern void gfx_sdl12_native_to_panel(float x, float y, float *px, float *py);
    if (!sHudFB) { sHudFB = (uint32_t *)calloc((size_t)HUD_W * HUD_H, 4); if (!sHudFB) return; }
    if (sW <= 0 || sH <= 0 || sNatW <= 0 || sNatH <= 0 || tw < 1 || th < 1) return;
    const float sx = (float)sNatW / (float)sW, sy = (float)sNatH / (float)sH; /* target -> the 320x240 frame */
    float x0, y0, x1, y1;
    gfx_sdl12_native_to_panel(bb[0] * sx, bb[1] * sy, &x0, &y0);
    gfx_sdl12_native_to_panel(bb[2] * sx, bb[3] * sy, &x1, &y1);
    const int X0 = (int)port_ifloorf(x0 + 0.5f), Y0 = (int)port_ifloorf(y0 + 0.5f);
    int N = (int)port_ifloorf(x1 + 0.5f) - X0, M = (int)port_ifloorf(y1 + 0.5f) - Y0;
    if (N < 1) N = 1;
    if (M < 1) M = 1;
    if (N > 2 * HUD_W || M > 2 * HUD_H) return;
    static uint32_t art[2 * HUD_W * 2 * HUD_H / 4] SAVESTATE_EXCLUDE; /* N x M, up to HUD_W x HUD_H */
    if ((size_t)N * (size_t)M > sizeof(art) / sizeof(art[0])) return;
    memset(art, 0, (size_t)N * (size_t)M * 4);
    if (N <= tw && M <= th) {
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) {
                const uint32_t t = px[ty * tw + tx];
                if ((t >> 24) == 0) continue;
                uint32_t *d = &art[(ty * M / th) * N + tx * N / tw];
                if ((t >> 24) >= (*d >> 24)) *d = t; /* the stronger texel keeps a shared pixel */
            }
    } else {
        for (int j = 0; j < M; j++)
            for (int i = 0; i < N; i++) art[j * N + i] = px[(j * th / M) * tw + i * tw / N];
    }
    for (int j = 0; j < M; j++) {
        const int y = Y0 + j;
        if (y < 0 || y >= HUD_H) continue;
        int lo = HUD_W, hi = -1;
        for (int i = 0; i < N; i++) {
            const int x = X0 + i;
            if (x < 0 || x >= HUD_W) continue;
            const uint32_t t = art[j * N + i];
            const uint32_t a = t >> 24;
            if (a == 0) continue;
            uint32_t *pd = &sHudFB[y * HUD_W + x];
            const uint32_t d = *pd;
            const uint32_t r = (((t >> 16) & 0xff) * a) / 255 + (((d >> 16) & 0xff) * (255 - a)) / 255;
            const uint32_t g = (((t >> 8) & 0xff) * a) / 255 + (((d >> 8) & 0xff) * (255 - a)) / 255;
            const uint32_t b = ((t & 0xff) * a) / 255 + ((d & 0xff) * (255 - a)) / 255;
            const uint32_t na = a + ((d >> 24) * (255 - a)) / 255;
            *pd = na << 24 | r << 16 | g << 8 | b;
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (hi >= lo) hud_mark(y, y + 1, lo, hi + 1);
    }
}
#endif
void gfx_soft_hud_overlay_row(int y, uint16_t *dst) {
    const uint32_t *src = sHudFB + y * HUD_W;
    for (int x = sHudRX0[y], x1 = sHudRX1[y]; x < x1; x++) {
        const uint32_t p = src[x]; const uint32_t a = p >> 24;
        if (!a) continue;
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (a < 255) { /* premultiplied over: p + frame x (1 - a) */
            const uint16_t d = dst[x]; const uint32_t ia = 255 - a;
            const uint32_t dr = ((d >> 11) & 0x1F) * 255 / 31, dg = ((d >> 5) & 0x3F) * 255 / 63, db = (d & 0x1F) * 255 / 31;
            r += dr * ia / 255; g += dg * ia / 255; b += db * ia / 255;
            r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
        }
        dst[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
}

void gfx_scegu_set_phase(int is3d) {
    if ((bool)is3d == sPhase3D) return;
    gfx_soft_phase_sw++; sPhaseSwFrame++;
    if (!is3d && sHud3Pass) { hud3_end(); select_target(false); return; }
    if (is3d && hud_on() && !sUnified) { hud3_begin(); if (sHud3Pass) return; }
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
    if (sNatRing[sNatRingIdx] != sNatFB) { /* first call, or alloc_target replaced the current buffer */
        for (int i = 0; i < 3; i++) {
            if (i != sNatRingIdx) free(sNatRing[i]); /* the current one was freed by alloc_target */
            sNatRing[i] = i == sNatRingIdx ? sNatFB : (uint32_t *)calloc((size_t)sNatW * sNatH, 4);
        }
    }
    alloc_target(&sIntFB, &sIntZB, &sIntW, &sIntH, sWantIntW, sWantIntH);
    sUnified = (sIntW == sNatW && sIntH == sNatH);
    sGateScale = (sNatW > 0 && sNatH > 0) ? (float)((double)sIntW * sIntH / ((double)sNatW * sNatH)) : 1.0f;
    if (ow != sNatW || oh != sNatH || iw != sIntW || ih != sIntH) { build_maps(); sIntClean = false; }
}

#if defined(HANDHELD) && defined(HH_CM_NEON)
/* The device bench's NEON A/B (round 11; mario-kart-64 6d2bfc4): a line of its own in the display
 * cfg on the card, "neon=0", runs the scalar raster loops instead of the NEON rows (as
 * SF64_CMNEON=0, which still wins). The only NEON with a runtime switch here: the present and
 * the audio mixer choose theirs at build time. F-Zero X's NEON was slower than scalar on the A7
 * (3D 19.4 vs 18.1 ms a picture), MK64's rows ~9% faster, Paper Mario's 1-2%: only a device run
 * says which way Star Fox goes. -1: no line. */
static int cfg_neon(void) {
    const char *dir = getenv("SF64_DATA");
    char path[512], line[64];
    int n = -1;
    snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "neon=", 5) != 0) continue;
        if (line[5] == '0' || line[5] == '1') n = line[5] - '0';
        else if (strncmp(line + 5, "alt", 3) == 0) n = 2; /* alternate by PERF window */
    }
    fclose(f);
    if (n >= 0) fprintf(stderr, "neon: %s (neon= in %s: the raster rows and the present's squeeze)\n",
                        n == 2 ? "alternating each PERF window" : n ? "on" : "off", path);
    return n;
}
#endif
#if defined(HANDHELD)
/* A device A/B for the deferred backdrops (round 19): a line "nozdefer=0" (off), "=1" (gated, the default)
 * or "=2" (every frame) in the display cfg, as SF64_NOZDEFER. -1: no line. */
static int cfg_nozdefer(void) {
    const char *dir = getenv("SF64_DATA");
    char path[512], line[64];
    int n = -1;
    snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "nozdefer=", 9) == 0 && line[9] >= '0' && line[9] <= '2') n = line[9] - '0';
    fclose(f);
    if (n >= 0) fprintf(stderr, "nozdefer: %d (nozdefer= in %s)\n", n, path);
    return n;
}
#endif

static void gfx_soft_init(void) {
    { const char *e = getenv("SF64_FASTSPAN"); sFastSpans = !(e && e[0] == '0'); }
    { const char *e = getenv("SF64_DEFERCAP"); sDeferCap = !(e && e[0] == '0'); }
    { const char *e = getenv("SF64_EARLYFLUSH"); if (e && e[0] >= '0' && e[0] <= '9') sEarlyFlush = atoi(e);
      if (sEarlyFlush != EARLY_PARTIALS) port_log_always("earlyflush: %d (SF64_EARLYFLUSH)\n", sEarlyFlush); }
    { const char *e = getenv("SF64_NOZDEFER"); if (e && e[0] >= '0' && e[0] <= '2') sNozDefer = e[0] - '0';
#if defined(HANDHELD)
      else { const int n = cfg_nozdefer(); if (n >= 0) sNozDefer = n; }
#endif
    }
#if defined(HH_CM_NEON)
    { const char *e = getenv("SF64_BILCACHE"); if (e && e[0] == '0') sBilCache = 0; }
    { const char *e = getenv("SF64_NEONBLEND"); if (e && e[0] == '0') sNeonBlend = 0; }
    { const char *e = getenv("SF64_CELLSKIP"); if (e && e[0] == '0') { sCellSkip = 0; port_log_always("cellskip: off (SF64_CELLSKIP=0)\n"); } }
    { const char *e = getenv("SF64_CMNEON"); if (e && (e[0] == '0' || e[0] == '1')) sCmNeon = e[0] - '0';
#if defined(HANDHELD)
      else { const int n = cfg_neon(); if (n == 2) { sNeonAlt = 1; sCmNeon = 1; } else if (n >= 0) sCmNeon = n; }
#endif
    }
#endif
    if (!sTexArena) {
        sTexArena = aligned_alloc(16, TEXARENA_SZ);
        if (!sTexArena) { fprintf(stderr, "gfx_soft: cannot allocate the %u MB texture arena\n", (unsigned) (TEXARENA_SZ >> 20)); abort(); }
    }
    ensure_targets(); select_target(false);
}
/* The Bench (main.c, 2026-10-11): the speed audit's two bomb switches by window, each 0 off or 1 at its own value
 * (the default or env); -1 leaves it. */
static int sEarlyOwn SAVESTATE_EXCLUDE = -1, sCellOwn SAVESTATE_EXCLUDE = -1;
void gfx_soft_bench_set(int early, int cell) {
    if (sEarlyOwn < 0) sEarlyOwn = sEarlyFlush;
    if (early >= 0) sEarlyFlush = early ? sEarlyOwn : 0;
#if defined(HH_CM_NEON)
    if (sCellOwn < 0) sCellOwn = sCellSkip;
    if (cell >= 0) sCellSkip = cell ? sCellOwn : 0;
#else
    (void)cell; (void)sCellOwn;
#endif
}
static void gfx_soft_on_resize(void) { defer_flush(); ensure_targets(); select_target(sPhase3D); }
u32 gfx_soft_frames_rendered SAVESTATE_EXCLUDE; /* frames that reached end_frame (frameskip excluded) */
static void gfx_soft_start_frame(void) {
    defer_flush(); sIntDrawn = false; sTagNext = 1;
    if (sHud3Pass) hud3_end();
    if (sHudFB && sHudY0 < sHudY1) memset(sHudFB + sHudY0 * HUD_W, 0, (size_t)(sHudY1 - sHudY0) * HUD_W * 4);
    memset(sHudRX0, 0, sizeof(sHudRX0)); memset(sHudRX1, 0, sizeof(sHudRX1));
    sHudY0 = HUD_H; sHudY1 = 0; sHudAnchor = 0;
    if (sDfSpanPx) gfx_soft_defer_frames++;
    if (sDfSpanPx) { /* last deferring frame: covered = span pixels - pixels the replays drew (per span) */
        const uint64_t drawn = (uint64_t)sDfLive * PERSP_SPAN, tot = sDfSpanPx;
        sDeferPays = (tot > drawn ? tot - drawn : 0) * 10 >= tot * 3;
    }
    sDfSpanPx = sDfLive = 0;
    sDeferGate = sNozDefer == 2 || sDeferPays || (gfx_soft_frames_rendered & 15) == 0;
    ensure_targets();
    if (sNatRing[1] && sNatRing[2]) { /* this frame draws into the buffer that holds frame - 3 */
        sNatRingIdx = (sNatRingIdx + 1) % 3;
        sNatFB = sNatRing[sNatRingIdx];
    }
    PROF_BEGIN();
    if (sLazyClear < 0) { const char *e = getenv("SF64_LAZYCLEAR"); sLazyClear = !(e && e[0] == '0'); }
    if (sLazyClear) { sNatStale = true; sNatFill = 0; } /* written when native is first touched */
    else memset(sNatFB, 0, (size_t)sNatW * sNatH * 4);
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        int_rows_reset(); /* a seed left pending by a 3D pass that drew nothing must not reach this frame */
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    PROF_END(PROF_CLR);
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    if (sPhaseSwFrame > gfx_soft_phase_sw_max) gfx_soft_phase_sw_max = sPhaseSwFrame;
    sPhaseSwFrame = 0;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
static void gfx_soft_end_frame(void) {
    if (sHud3Pass) { hud3_end(); select_target(false); }
    if (!sUnified && sIntDirty) composite_internal();
    gfx_soft_last_frame_had_3d = sHad3D;
    gfx_soft_frames_rendered++;
#if !defined(HANDHELD)
    framestat_end();
#endif
    drawprof_end();
    select_target(false); /* leave native current for present/menu/dumps */
}
static void gfx_soft_finish_render(void) { }

/* -------------------------------------------------------------- shims --- */
/* gfx_pc.c calls these sceGu/texman helpers directly; software equivalents. */
void gfx_scegu_set_cull_mode(uint32_t cull) { (void)cull; /* GE-only API; winding cull lives in gfx_sp_tri1 */ }
void gfx_scegu_set_texfunc_add(bool on) { sTexAdd = on; }
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; sFogPx = false; }
void gfx_soft_set_fog(bool on, int r, int g, int b, int a) { sFogPx = on; sFogR = r; sFogG = g; sFogB = b; sFogA = a; }
void gfx_scegu_set_force_in(bool on) { sForceIn = on; }
void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b) { sTexLerp = on; sLerpR = r; sLerpG = g; sLerpB = b; }

/* Depth reads for the game (sun lens flare, light glows: z_kankyo.c /
 * z_lights.c). x, y are 320x240 game coordinates; the buffer holds the last
 * rendered 3D frame (the game asks during its update, before this frame is
 * drawn, as on the N64). Returns false off-buffer. */
static bool hh_read_depth(int32_t x, int32_t y, uint16_t *depth) {
    const bool uni = sUnified || sIntZB == NULL;
    const uint16_t *zb = uni ? sNatZB : sIntZB;
    const int w = uni ? sNatW : sIntW, h = uni ? sNatH : sIntH;
    if (zb == NULL || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= 320 || y >= 240) return false;
    *depth = zb[(y * h / 240) * w + (x * w / 320)];
    return true;
}
bool gfx_soft_depth_is_clear(int32_t x, int32_t y) {
    uint16_t d;
    return hh_read_depth(x, y, &d) && d == 0xFFFF;
}
/* projectedZ is NDC z/w; the rasterizer stores clamp(z/w, 0, 1) * 65535
 * (gfx_pc emits z/w, raster_tri clamps), so compare in the same terms. */
bool gfx_soft_depth_test(int32_t x, int32_t y, float projectedZ) {
    uint16_t d;
    float z = projectedZ;
    if (!hh_read_depth(x, y, &d)) return false;
    if (z < 0.0f) z = 0.0f;
    if (z > 1.0f) z = 1.0f;
    return (uint32_t)(z * 65535.0f) < d;
}

/* OoT's two-texture combiners (cycle 1 = lerp(TEXEL0, TEXEL1) by a constant or
 * a texel alpha): the rasterizer samples one texture, so gfx_pc pre-blends the
 * two TMEM tiles into one ARGB texture over the tile-0 texel domain and draws
 * that. Output texel x,y is tile-0 texel (base_s + x, base_t + y); tile 1 reads
 * that coordinate through its own shift and corner (uls1/ult1, integer texels).
 * rgb_mode / a_mode: 0 = tile 0, 1 = tile 1, 2 = t0 + (t1 - t0) * K,
 * 3 = t1 + (t0 - t1) * K. k_src: 0 = constants (krgb[3] / ka), 1 = tile-0
 * alpha, 2 = tile-1 alpha. Rewrites dst in place when its size matches (the
 * scrolling water re-blends every frame). Returns false if it cannot. */
static inline int hh_tile_coord(int c, int shift) {
    if (shift == 0) return c;
    if (shift <= 10) return c >> shift;
    return c << (16 - shift);
}
static inline int hh_wrap(int c, int size) {
    int m = c % size;
    return m < 0 ? m + size : m;
}
static inline int hh_lerp8(int a, int b, int k) { return a + ((b - a) * k) / 255; }
#define BLEND_MAXW 1024
static int sBlendFast SAVESTATE_EXCLUDE = -1; /* SF64_BLENDFAST=0: the per-texel loop, for A/B */
bool gfx_soft_blend_textures(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                             int uls1, int ult1, int shs1, int sht1, int rgb_mode, int a_mode,
                             int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka) {
    if (dst == 0 || dst >= MAX_TEXTURES || id0 >= MAX_TEXTURES || id1 >= MAX_TEXTURES) return false;
    const SoftTex *t0 = &sTex[id0], *t1 = &sTex[id1];
    if (!t0->valid || !t1->valid || W <= 0 || H <= 0 || W * H > 256 * 256) return false;
    uint32_t *out;
    if (sTex[dst].valid && !sTex[dst].is16 && sTex[dst].w == W && sTex[dst].h == H) {
        defer_flush(); /* a queued backdrop may hold this texture's memory */
        out = (uint32_t *)sTex[dst].px;
    } else {
        const size_t bytes = ((size_t)W * H * 4 + 15u) & ~(size_t)15;
        if (sArenaUsed + bytes > TEXARENA_SZ) return false;
        out = (uint32_t *)&sTexArena[sArenaUsed];
        sArenaUsed += bytes;
        sTex[dst].px = out; sTex[dst].is16 = false; sTex[dst].w = W; sTex[dst].h = H; sTex[dst].valid = true;
    }
    if (sBlendFast < 0) { const char *e = getenv("SF64_BLENDFAST"); sBlendFast = !(e && e[0] == '0'); }
    if (W <= BLEND_MAXW && sBlendFast) {
        /* The loop below with its per-texel work hoisted (ocarina-of-time c935b73):
         * the column wraps (two divides a texel) come from a table, the mode
         * tests pick a source and a lerp once. Same selections, same hh_lerp8:
         * identical texels. */
        static int sCol0[BLEND_MAXW] SAVESTATE_EXCLUDE, sCol1[BLEND_MAXW] SAVESTATE_EXCLUDE;
        const bool lerp_rgb = rgb_mode >= 2, swap_rgb = rgb_mode == 1 || rgb_mode == 3;
        const bool lerp_a = a_mode >= 2, swap_a = a_mode == 1 || a_mode == 3;
        for (int x = 0; x < W; x++) {
            sCol0[x] = x % t0->w;
            sCol1[x] = hh_wrap(hh_tile_coord(base_s + x, shs1) - uls1, t1->w);
        }
        for (int y = 0; y < H; y++) {
            const int r0 = (y % t0->h) * t0->w;
            const int r1 = hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w;
            uint32_t *const orow = out + y * W;
            for (int x = 0; x < W; x++) {
                const uint32_t p0 = texel_at(t0->px, t0->is16, r0 + sCol0[x]);
                const uint32_t p1 = texel_at(t1->px, t1->is16, r1 + sCol1[x]);
                const int a0 = p0 >> 24, a1 = p1 >> 24;
                const uint32_t p = swap_rgb ? p1 : p0, q = swap_rgb ? p0 : p1; /* mode 1/3: tile 1 first */
                int cr = (p >> 16) & 0xff, cg = (p >> 8) & 0xff, cb = p & 0xff, ca = swap_a ? a1 : a0;
                if (lerp_rgb) {
                    const int kt = krgb_src == 1 ? a0 : a1;
                    cr = hh_lerp8(cr, (q >> 16) & 0xff, krgb_src ? kt : krgb[0]);
                    cg = hh_lerp8(cg, (q >> 8) & 0xff, krgb_src ? kt : krgb[1]);
                    cb = hh_lerp8(cb, q & 0xff, krgb_src ? kt : krgb[2]);
                }
                if (lerp_a) {
                    ca = hh_lerp8(ca, swap_a ? a0 : a1, ka_src == 1 ? a0 : ka_src == 2 ? a1 : ka);
                }
                orow[x] = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb, (uint8_t)ca);
            }
        }
        return true;
    }
    for (int y = 0; y < H; y++) {
        const int r0 = (y % t0->h) * t0->w;
        const int r1 = hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w;
        for (int x = 0; x < W; x++) {
            const uint32_t p0 = texel_at(t0->px, t0->is16, r0 + x % t0->w);
            const uint32_t p1 = texel_at(t1->px, t1->is16, r1 + hh_wrap(hh_tile_coord(base_s + x, shs1) - uls1, t1->w));
            const int a0 = p0 >> 24, a1 = p1 >> 24;
            int c[4];
            for (int ch = 0; ch < 3; ch++) {
                const int sh = 16 - ch * 8, v0 = (p0 >> sh) & 0xff, v1 = (p1 >> sh) & 0xff;
                const int k = krgb_src == 1 ? a0 : krgb_src == 2 ? a1 : krgb[ch];
                c[ch] = rgb_mode == 0 ? v0 : rgb_mode == 1 ? v1 : rgb_mode == 2 ? hh_lerp8(v0, v1, k) : hh_lerp8(v1, v0, k);
            }
            {
                const int k = ka_src == 1 ? a0 : ka_src == 2 ? a1 : ka;
                c[3] = a_mode == 0 ? a0 : a_mode == 1 ? a1 : a_mode == 2 ? hh_lerp8(a0, a1, k) : hh_lerp8(a1, a0, k);
            }
            out[y * W + x] = argb((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], (uint8_t)c[3]);
        }
    }
    return true;
}
void gfx_scegu_sync_pending(void) { }
void gfx_scegu_upload_texture(const uint8_t *buf, int w, int h, unsigned int type) { gfx_soft_upload_texture(buf, w, h, type); }
int  gfx_vram_space_available(void) { return 1; }

int  texman_usage_percent(void) { return (int)(sArenaUsed * 100 / TEXARENA_SZ); }
int  texman_can_hold(unsigned int num, unsigned int bytes) { (void)num; return sArenaUsed + bytes <= TEXARENA_SZ; }
void texman_bind_tex(unsigned int num) { sBoundTex[sTexUploadTile] = num; }
size_t gfx_soft_arena_used(void) { return sArenaUsed; }
void texman_clear(void) { defer_flush(); sArenaUsed = 0; gfx_soft_arena16 = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; }

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
    nat_ready();
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

/* Debug: dump the framebuffer as a binary PPM (for headless verification). */
void gfx_soft_dump_ppm(const char *path) {
    if (!sNatFB) return;
    nat_ready();
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sNatW, sNatH);
    const unsigned char *g = port_vi_gamma8(); /* as the screen shows it */
    for (int i = 0; i < sNatW * sNatH; i++) {
        uint32_t p = sNatFB[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        if (g) { rgb[0] = g[rgb[0]]; rgb[1] = g[rgb[1]]; rgb[2] = g[rgb[2]]; }
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

/* Texture edges without FORCE_BL (G_RM_AA_ZB_TEX_EDGE; gfx_pc, SF64_EDGEOPAQUE, ocarina-of-time 7120396): the RDP
 * takes coverage from the texel's alpha and never blends such a pixel with memory -- it is written as the
 * texel's own colour, or not at all. Blending by alpha mixed a cutout's filtered rim with whatever was
 * behind at draw time. Triangles only (raster_tri); texture rectangles keep blending. */
/* Mode 2 (ocarina-of-time 05dd742, the narrowed rule): unblended only where nothing has been drawn yet -- the
 * 3D target is cleared to alpha 0 and every drawn pixel has 255 -- which is the case of a rim blending
 * with the clear colour; over drawn scenery the blend stays (no VI anti-aliasing here to stand in for). */
void gfx_soft_set_edge_opaque(int mode) { sEdgeOpaque = mode; }
/* SF64_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }
uint64_t gfx_soft_ss2d_mag(void) { return sSs2dMagPx; }
