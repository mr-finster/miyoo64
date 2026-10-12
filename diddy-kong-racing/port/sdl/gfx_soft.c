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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
#include "savestate.h"
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
static bool sEdgeOpaque SAVESTATE_EXCLUDE; /* gfx_soft_set_edge_opaque */
static bool sOwnTri SAVESTATE_EXCLUDE;     /* gfx_soft_set_own_edges (DKR_FILLRULE) */
static uint32_t *sFB SAVESTATE_EXCLUDE;  /* ARGB8888 (malloc'd -- keep live over load) */
static uint16_t *sZB SAVESTATE_EXCLUDE;  /* 0..65535, larger = farther (malloc'd) */
static int sW SAVESTATE_EXCLUDE, sH SAVESTATE_EXCLUDE;
static uint32_t *sNatFB SAVESTATE_EXCLUDE; /* native target (defined with the frame section) */


/* -------------------------------------------------------------- textures --- */
enum { WRAP_REPEAT = 0, WRAP_CLAMP = 1, WRAP_MIRROR = 2 };
#define MAX_TEXTURES 4096
#define TEXARENA_SZ  (12 * 1024 * 1024) /* generous on 64 MB; tighten later */

typedef struct {
    const void *px;     /* is16 ? uint16 RGBA5551 : uint32 ARGB8888, w*h */
    int w, h;
    int wrap_s, wrap_t;
    bool valid;
    bool is16;          /* device round 10: 5551 stored 16-bit, not expanded */
    bool point;         /* its last draw asked for G_TF_POINT (DKR_HUDTRI=2 honours it on HUD triangles) */
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
static uint32_t sTexGen[MAX_TEXTURES] SAVESTATE_EXCLUDE; /* bumped whenever a texture's texels are (re)written: box row cache key */
static void box_cache_clear(void);
static uint32_t sBoundTex[2] SAVESTATE_EXCLUDE;   /* per tile */
static int sTexUploadTile SAVESTATE_EXCLUDE;      /* tile targeted by the next upload (select_texture) */

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

/* type: GU_PSM_8888 (RGBA bytes) or GU_PSM_5551 (u16); both are stored as
 * ARGB8888. */
static void gfx_soft_upload_texture(const uint8_t *buf, int width, int height, unsigned int type) {
    PROF_BEGIN();
    uint32_t id = sBoundTex[sTexUploadTile];
    if (id == 0 || id >= MAX_TEXTURES || width <= 0 || height <= 0) return;
    const size_t n = (size_t)width * height;
    /* 5551 is unpacked to ARGB8888 here, once, rather than at every texel
     * fetch (device round 2: the per-fetch unpack was ~11% of the
     * rasterizer's instructions; DKR's textures use under 1 MB of the arena,
     * so the doubled size costs nothing). */
    const bool src16 = (type == GU_PSM_5551);
    const bool is16 = false;
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
    if (src16) {
        uint32_t *d = (uint32_t *)dst; /* unpack now: fetches read plain ARGB8888 */
        for (size_t i = 0; i < n; i++) d[i] = unpack5551(((const uint16_t *)buf)[i]);
    } else { /* GU_PSM_8888: RGBA bytes -> ARGB8888 */
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = argb(buf[i*4+0], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    }
    sTex[id].px = dst; sTex[id].is16 = is16; sTex[id].w = width; sTex[id].h = height; sTex[id].valid = true;
    sTexGen[id]++; /* box row cache key */
    { /* DKR_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("DKR_TEXDUMP"); }
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
    if (tile < 0 || tile > 1) return;
    uint32_t id = sBoundTex[tile];
    if (id && id < MAX_TEXTURES) {
        sTex[id].point = !linear; /* recorded for the HUD-triangle A/B only; filtering ignores it */
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
/* float -> int32 as the device's VFP converts (vcvt saturates, NaN gives 0), so the x86
 * proxy agrees with it where a plain cast is undefined: a near-degenerate triangle's
 * colour step can reach ~1e10 in 16.16, and x86's cvttss2si gives INT_MIN there
 * (the MK64 port b2dff40, the lead's R31; make PLAT=pc UBSAN=1 finds these). */
static inline int32_t sat_i32(float f) {
#if defined(__arm__) && defined(__ARM_FP)
    /* VFP's vcvt (round toward zero) saturates and gives 0 for NaN: the same result in the one
     * instruction the plain cast compiled to (round 17; no compares on the device) */
    int32_t i;
    __asm__("vcvt.s32.f32 %1, %1\n\tvmov %0, %1" : "=r"(i), "+t"(f));
    return i;
#else
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
#endif
}
/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}

/* 3D bilinear filtering (always on for players since round 14; DKR_BILINEAR=0 is the
 * developer SHARP switch). uf/vf are
 * 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* 0 = every textured pixel bilinear, as the N64 filters (the default since round 16).
 * 1 = the magnified-only gate below (MK64 round 13), a developer A/B (DKR_MAGBILIN=1):
 * it decides per triangle, so a floor drew smooth up close and sharp further out, and the
 * border between the two jumped from triangle to triangle while driving (device report,
 * round 16: "the texture blend scale is changing size based on distance"). Costs +3.7%
 * instructions over a stretch of Ancient Lake (qemu insnprof). */
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

/* The sprite span skip (round 18; the GoldenEye port ac941f3, the lead's round-3 batch): a blended
 * span whose every bilinear tap reads an alpha-0 texel draws nothing, so it is skipped whole.
 * Exact: such a pixel's texel alpha is 0, so its alpha (the texel's, or texel x shade) is 0 and
 * the alpha compare (amin >= 8) drops it before any colour or depth write -- the z test before
 * it only reads. raster_tri takes the skip only there: blended, the alpha from the texture, no
 * constant fog alpha in its place, bilinear. Per texture, each row's first and last texel with
 * alpha != 0, keyed by id and generation (every texel write bumps sTexGen; texman_clear never
 * resets it). DKR_SPANSKIP=0: off. */
#define AROWS_N 64   /* direct-mapped by texture id */
#define AROWS_K 7    /* row ranges up to 64 */
/* lo[k][y] / hi[k][y]: the first / last texel with alpha != 0 over rows y .. y + 2^k - 1 (a sparse
 * table: any run of up to 64 rows is two lookups). A row with none has lo = w, hi = -1. */
typedef struct { uint32_t id, gen; int w, h; bool full; int16_t lo[AROWS_K][256], hi[AROWS_K][256]; } AlphaRows;
static AlphaRows sARows[AROWS_N] SAVESTATE_EXCLUDE;
static int sSpanSkip SAVESTATE_EXCLUDE = -1;
uint64_t gfx_soft_spanskip_spans SAVESTATE_EXCLUDE, gfx_soft_spanskip_px SAVESTATE_EXCLUDE, gfx_soft_spanskip_scans SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* The Miyoo Mini draws a triangle's rows on both cores (raster_tri's split, miyoo_worker.c), so the rows'
 * span skip counts are per thread: the worker's come back with each job (tr_job), and the census below
 * (at exit, on the game thread) adds the game thread's own. */
static __thread uint64_t tSkSpans, tSkPx;
#define SK_SPANS tSkSpans
#define SK_PX    tSkPx
#define SK_CENSUS_SPANS (gfx_soft_spanskip_spans + tSkSpans)
#define SK_CENSUS_PX    (gfx_soft_spanskip_px + tSkPx)
#else
#define SK_SPANS gfx_soft_spanskip_spans
#define SK_PX    gfx_soft_spanskip_px
#define SK_CENSUS_SPANS gfx_soft_spanskip_spans
#define SK_CENSUS_PX    gfx_soft_spanskip_px
#endif
static void spanskip_census(void) { /* DKR_SPANSKIP_CENSUS=1: the totals at exit */
    extern void port_log_always(const char *fmt, ...);
    port_log_always("SPANSKIP %s: %llu spans, %llu px skipped; %llu texture scans\n", sSpanSkip ? "on" : "off",
                    (unsigned long long)SK_CENSUS_SPANS, (unsigned long long)SK_CENSUS_PX, (unsigned long long)gfx_soft_spanskip_scans);
}
static const AlphaRows *alpha_rows(uint32_t id, const SoftTex *t) {
    if (__builtin_expect(sSpanSkip < 0, 0)) {
        const char *e = getenv("DKR_SPANSKIP"); sSpanSkip = !(e && e[0] == '0');
        if ((e = getenv("DKR_SPANSKIP_CENSUS")) && e[0] == '1') atexit(spanskip_census);
    }
    if (!sSpanSkip || id == 0 || id >= MAX_TEXTURES || t->w > 4096 || t->h > 256) return NULL;
    AlphaRows *r = &sARows[id % AROWS_N];
    if (r->id == id && r->gen == sTexGen[id] && r->w == t->w && r->h == t->h) return r->full ? NULL : r;
    r->id = id; r->gen = sTexGen[id]; r->w = t->w; r->h = t->h;
    gfx_soft_spanskip_scans++;
    r->full = true;
    for (int y = 0; y < t->h; y++) {
        int lo = t->w, hi = -1;
        for (int x = 0; x < t->w; x++)
            if (texel_at(t->px, t->is16, y * t->w + x) >> 24) { if (x < lo) lo = x; hi = x; }
        r->lo[0][y] = (int16_t)lo; r->hi[0][y] = (int16_t)hi;
        if (lo != 0 || hi != t->w - 1) r->full = false;
    }
    for (int k = 1; k < AROWS_K; k++) {
        const int half = 1 << (k - 1);
        for (int y = 0; y + (1 << k) <= t->h; y++) {
            const int16_t a = r->lo[k - 1][y], b = r->lo[k - 1][y + half];
            const int16_t c = r->hi[k - 1][y], d = r->hi[k - 1][y + half];
            r->lo[k][y] = a < b ? a : b; r->hi[k][y] = c > d ? c : d;
        }
    }
    return r->full ? NULL : r;
}
/* The texel range [lo, hi] as the sampler wraps it (wrapf): one or two ranges in out[], 0 = unknown. */
static inline __attribute__((always_inline)) int wrap_range(int lo, int hi, int size, int mode, int out[4]) {
    if (mode == WRAP_CLAMP) {
        out[0] = lo < 0 ? 0 : (lo >= size ? size - 1 : lo);
        out[1] = hi < 0 ? 0 : (hi >= size ? size - 1 : hi);
        return 1;
    }
    if (mode != WRAP_REPEAT) return 0;
    if (lo >= 0 && hi < size) { out[0] = lo; out[1] = hi; return 1; }
    if (hi - lo + 1 >= size) { out[0] = 0; out[1] = size - 1; return 1; }
    const int a = ((lo % size) + size) % size, b = ((hi % size) + size) % size;
    if (a <= b) { out[0] = a; out[1] = b; return 1; }
    out[0] = a; out[1] = size - 1; out[2] = 0; out[3] = b;
    return 2;
}
/* The hull of the alpha != 0 texels over rows [y0, y1] (<= 64 rows): two table lookups. */
static inline __attribute__((always_inline)) void rows_hull(const AlphaRows *r, int y0, int y1, int *lo, int *hi) {
    const int n = y1 - y0 + 1, k = 31 - __builtin_clz((unsigned)n), y2 = y1 - (1 << k) + 1;
    const int a = r->lo[k][y0], b = r->lo[k][y2], c = r->hi[k][y0], d = r->hi[k][y2];
    *lo = a < b ? a : b; *hi = c > d ? c : d;
}
/* (A texture whose every row has alpha != 0 texels at both ends -- Whale Bay's water -- gives every
 * row range the full-width hull, so no box ever misses it: alpha_rows returns NULL for it and its
 * draws skip the check, which cost 3.6% of Whale Bay's instructions for next to no skips.) */
/* Every bilinear tap of span pixels 0..n-1, at (uf + i duf, vf + i dvf), reads alpha 0? The taps lie
 * in the box of the end pixels' 2x2 neighbourhoods (the coordinates step linearly along the span;
 * bilerp's taps are floor(u - 0.5) and the next), a span whose end points would wrap int32 never
 * skipped; a box over more than 64 rows is not tried. The test is the box's
 * columns against the hull of each row range's opaque texels: exact wherever it says clear (the hull
 * holds every alpha != 0 texel of those rows), and one or two table lookups a range. */
static bool span_clear(const AlphaRows *r, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n, int ws, int wt) {
    /* 32-bit end points (the GoldenEye port 3af0063: the 64-bit math was ~45% of this function on the
     * ARM): a span whose end point or its -0x8000 would wrap is simply not tried -- the skip is only
     * ever conservative (a span not skipped draws the same pixels) */
    int32_t dus, dvs, ub, vb;
    if (__builtin_mul_overflow(duf, n - 1, &dus) || __builtin_add_overflow(uf, dus, &ub) ||
        __builtin_mul_overflow(dvf, n - 1, &dvs) || __builtin_add_overflow(vf, dvs, &vb))
        return false;
    const int32_t lim = INT32_MIN + 0x8000;
    if (uf < lim || ub < lim || vf < lim || vb < lim) return false;
    const int ulo = ((uf < ub ? uf : ub) - 0x8000) >> 16, uhi = (((uf < ub ? ub : uf) - 0x8000) >> 16) + 1;
    const int vlo = ((vf < vb ? vf : vb) - 0x8000) >> 16, vhi = (((vf < vb ? vb : vf) - 0x8000) >> 16) + 1;
    const int w = r->w, h = r->h;
    int lo, hi;
    if (__builtin_expect(vlo >= 0 && vhi < h && ulo >= 0 && uhi < w, 1)) {
        /* the box inside the texture: no wrap either way */
        if (vhi - vlo + 1 > 64) return false;
        rows_hull(r, vlo, vhi, &lo, &hi);
        return hi < lo || uhi < lo || ulo > hi;
    }
    int cr[4], rr[4];
    const int nc = wrap_range(ulo, uhi, w, ws, cr), nr = wrap_range(vlo, vhi, h, wt, rr);
    if (nc == 0 || nr == 0) return false;
    for (int k = 0; k < nr; k++) {
        if (rr[2 * k + 1] - rr[2 * k] + 1 > 64) return false;
        rows_hull(r, rr[2 * k], rr[2 * k + 1], &lo, &hi);
        if (hi < lo) continue; /* every row transparent */
        for (int j = 0; j < nc; j++)
            if (cr[2 * j] <= hi && cr[2 * j + 1] >= lo) return false;
    }
    return true;
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
static bool sTexLerp;      /* out = K + (vertex - K) * texel: (A - K) * TEXEL0 + K tints */
static int sLerpR, sLerpG, sLerpB;
static bool sForceIn;      /* 2-cycle: the vertex colour carries cycle 2, always modulate */
/* Two textures lerped by the vertex colour, times a constant (gfx_pc gfx_update_tex2, round 18):
 * out = lerp(TEXEL0, texture sTex2Id, vertex rgb) * sTex2M, the alpha as a plain textured draw's. */
static bool sTex2;
static uint32_t sTex2Id;
static int sTex2R, sTex2G, sTex2B;
/* Per-pixel fog (gfx_pc fog_px; the fog reference, the MK64 port 7d23507, round 17): the vertex
 * alpha carries the fog factor and the pixel goes to lerp(combined, sFogR/G/B, factor) after the
 * combine, as the RDP's cycle-1 fog blend does. sFogA >= 0: the combiner's constant alpha, which
 * the factor displaced from the vertex, is the pixel's alpha. */
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
/* DKR_FILLRULE (2026-10-03; the OoT port a7c4909, the Deku Tree's vision: a one-pixel bright diagonal across
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
#define PERSP_SPAN 16
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
 * so it is bit-identical (the OoT port c935b73; DKR: qemu-arm sweep, DKR_CMNEON=0
 * vs on). In raster_tri the span setup spilled to the stack and each span paid
 * a call; flat triangles come in with zero colour steps. */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16; /* texture */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx;  /* triangle x-steps */
    float ftw, fth;                                       /* (float)tw, (float)th */
    int32x4_t dacc;                                       /* B G R A 16.16 x-steps */
    int32_t zdq; int zbias, amode, amin;
    bool persp, dmask, alpha, bilin;
    bool fog; int fga; uint16x4_t fogc;                   /* per-pixel fog (CM=2): B G R 0; fga >= 0 the constant alpha */
    const AlphaRows *arows;                               /* the sprite span skip (cm_row_neon_skip) */
    bool bf;                                              /* blended, no z write: CM_BLEND_BF */
    int ablend;                                           /* 0: texture edge without FORCE_BL, written unblended */
    bool lean;                                            /* the rows go to cm_row_lean / cm_row_lean_noz (raster_tri) */
    bool ps;                                              /* DKR_PSHADE: the shade state is colour/w, stepped per span */
    float32x4_t da, db;                                   /* DKR_SPANVEC: { cbdx, cgdx, crdx, cadx }, { zdx, uwdx, vwdx, swdx } */
    int zsk;                                              /* DKR_ZSKIP: > 0 while the triangle's spans are tested (cm_row_lean_v) */
} CmTri;
/* DKR_PSROWS (2026-10-11, the speed audit): the rows that step DKR_PSHADE's shade (cm_row_neon, _fog, _skip,
 * cm_row_lean, cm_row_lean_v) are compiled twice more, with ps a constant: one copy for perspective-shaded triangles,
 * one for the rest, chosen per row by T->ps. The runtime flag cost the shared rows a test per span and a register
 * (the device: r3d 0.1-0.7 ms above the build before PSHADE even with PSHADE off). Each copy is the row's own body
 * with ps known (PSM 0 or 1; 2 = read T->ps, the rows as before), so the same operations reach every pixel.
 * DKR_PSROWS=0: the one copy with the runtime flag (before). Same pixels. */
static int sPsRows SAVESTATE_EXCLUDE = 1;
/* FOG=1 (round 17) alone names the fog constants (fogc, rgb3, fga: in its branch of cm_row_neon):
 * CM=2's step, B G R toward the fog colour by the shade alpha (the fog factor), A kept; and the
 * constant alpha the factor displaced, if any. */
#define CM_FOG_0
#define CM_FOG_1 { const uint16_t fs = vget_lane_u16(S, 3);                                             \
                   M = vbsl_u16(rgb3, cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)(255 - fs)), fogc, fs)), M); }
#define CM_FA_0(x) (x)
/* amode 3 (DKR_FOGTEXA, OoT c7ebfea): the constant times the texel alpha */
#define CM_FA_1(x) (fga >= 0 ? (amode == 3 ? div255(vget_lane_u16(Tx, 3) * fga) : fga) : (x))
#define CM_FAV_0(x) (x)
#define CM_FAV_1(x) (fga >= 0 ? (amode == 3 ? cm_div255(vmul_n_u16(vdup_lane_u16(Tx, 3), (uint16_t)fga)) : vdup_n_u16((uint16_t)fga)) : (x))
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* The blend without a branch (speed round 2, for the Cortex-A7: its NEON is 64 bits wide, so
 * pixels in pairs gain nothing there, but every NEON-to-core move -- the scalar path's fa lane
 * read and compare per blended pixel -- stalls it). Blended draws that write no z (Whale Bay's
 * water: 73% of the screen) blend every pixel in lanes: F = fa in all four, the blend as the
 * scalar one, and the pixel as it was (D, alpha too) where fa < amin. Exact: fa = 255 gives
 * div255(M*255 + D*0) = M for every M (checked for all 256), which the branch skipped to; below
 * amin the stored bytes are D's, unchanged; no z is written either way. DKR_BLENDBF=0 off. */
#define CM_BLEND_BF(FASRC, pf)                                                                          \
        {   const uint16x4_t F = FASRC;                                                                 \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));          \
            const uint16x4_t Bl = cm_div255(vmla_u16(vmul_u16(M, F), D, vsub_u16(vdup_n_u16(255), F)));   \
            M = vbsl_u16(vclt_u16(F, vdup_n_u16((uint16_t)amin)), D, vorr_u16(Bl, a255));              \
            vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                     \
            continue; }
/* The pixel loop of one span: Tx (the texel, B G R A in u16 lanes) from the fetch (...). */
#define CM_PIXELS(ALPHA, FOG, ...)                                                                       \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz + zbias) continue;                                                                 \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M = cm_div255(vmul_u16(Tx, S));                                                      \
        CM_FOG_##FOG                                                                                    \
        if (ALPHA && bf) CM_BLEND_BF(CM_FAV_##FOG(amode == 0 ? vdup_lane_u16(M, 3) : amode == 1 ? vdup_lane_u16(Tx, 3) \
                                                  : vdup_lane_u16(S, 3)), pf)                                   \
        if (ALPHA) { /* fa: modulated (0), texel (1) or shade (2) alpha -- the scalar fa */               \
            const int fa = CM_FA_##FOG(amode == 0 ? vget_lane_u16(M, 3) : amode == 1 ? vget_lane_u16(Tx, 3) \
                                                                           : vget_lane_u16(S, 3));          \
            if (fa < amin) continue;                                                                    \
            if (fa < 255 && T->ablend) { /* div255(f*fa + d*(255-fa)), every term <= 255*255 */           \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));         \
            }                                                                                           \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (dmask) *pz = (uint16_t)uz;                                                                  \
    }
/* bilerp: four taps around (u - 0.5, v - 0.5), lerped along u then v */
#define CM_BILIN(TAP, WU, WV)                                                                           \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = WU(su >> 16), iu1 = WU((su >> 16) + 1);                                         \
        const int b0 = WV(sv >> 16) * tw, b1 = WV((sv >> 16) + 1) * tw;                                 \
        const uint32_t a = TAP(b0 + iu0), b = TAP(b0 + iu1), c = TAP(b1 + iu0), d = TAP(b1 + iu1);      \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));       \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));       \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        Tx = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2), vget_high_u16(H), wts, 3), 8);
/* FETCH_NEAREST */
/* DKR_PSHADE (2026-10-08; the GoldenEye port GE_PSHADE): a perspective triangle's shade state is colour/w; each span's
 * colours are exact at its ends (state * 1/w) and stepped linearly between, as u and v. The same operations as the
 * scalar span loop's (raster_tri): end = (c + cdx * span) * rw_b, step = (end - start) * 1/16 or / span. */
static inline int32x4_t ps_dacc(float32x4_t cq, float32x4_t cqb, int span, float fspan) {
    const float32x4_t d = vsubq_f32(cqb, cq);
    float32x4_t st;
    if (span == PERSP_SPAN) st = vmulq_n_f32(d, 1.0f / PERSP_SPAN);
    else st = (float32x4_t){ vgetq_lane_f32(d, 0) / fspan, vgetq_lane_f32(d, 1) / fspan, vgetq_lane_f32(d, 2) / fspan,
                             vgetq_lane_f32(d, 3) / fspan };
    return vcvtq_n_s32_f32(st, 16);
}
#define CM_PS_SCALAR                                                                                    \
        float32x4_t cq = { cb, cg, cr, ca };                                                            \
        int32x4_t daccp = dacc;                                                                         \
        if (ps) {                                                                                       \
            const float32x4_t cqb = vmulq_n_f32((float32x4_t){ cb + cbdx * fspan, cg + cgdx * fspan,    \
                                                               cr + crdx * fspan, ca + cadx * fspan }, rw_b); \
            cq = vmulq_n_f32(cq, rw);                                                                   \
            daccp = ps_dacc(cq, cqb, span, fspan);                                                      \
        }
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
#define CM_ROW(ALPHA, FOG, ...)                                                                          \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
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
        CM_PS_SCALAR                                                                                    \
        int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                        \
        uint32_t *const end = fb + span;                                                                \
        uint16_t *pz = zp;                                                                              \
        { const int32x4_t dacc = daccp; CM_PIXELS(ALPHA, FOG, __VA_ARGS__) }                            \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }
/* CM_ROW with the sprite span skip (round 18): a span whose taps all read alpha 0 draws nothing;
 * its interpolants step on exactly as when drawn. */
#define CM_ROW_SKIP(ALPHA, FOG, ...)                                                                     \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
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
        uint32_t *const end = fb + span;                                                                \
        if (span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                                        \
            SK_SPANS++; SK_PX += (uint64_t)span;                          \
        } else {                                                                                        \
            const float32x2_t zc = vmin_f32(vmax_f32(vdup_n_f32(zf), vdup_n_f32(-1.0f)), vdup_n_f32(2.0f)); \
            int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);              \
            CM_PS_SCALAR                                                                                \
            int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                    \
            uint16_t *pz = zp;                                                                          \
            { const int32x4_t dacc = daccp; CM_PIXELS(ALPHA, FOG, __VA_ARGS__) }                        \
        }                                                                                               \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }
static int sCmNeon SAVESTATE_EXCLUDE = 1; /* DKR_CMNEON=0: the scalar loops, for A/B */
/* The sky (the speed round, 2026-09-26): DKR draws its sky dome as big texture x shade triangles
 * without the z test -- 18-44% of the benchmark levels' 3D pixels (the path census), up to more
 * than a screen a frame, all through the scalar SPAN_LOOP(1, 0, *, *). CM_PIXELS and CM_ROW with
 * the z test and write taken out: SPAN_LOOP's ZTEST=0 draws exactly this (the modulate and the
 * alpha as CM=1's, which cm_mod guarantees). Own macros, so the z-tested rows compile as before. */
/* the deferred sky (sky_flush): a span all drawn over since */
static inline bool span_drawn(const uint32_t *fb, int n) {
    if (n == PERSP_SPAN) {
        const uint32x4_t a = vandq_u32(vandq_u32(vld1q_u32(fb), vld1q_u32(fb + 4)), vandq_u32(vld1q_u32(fb + 8), vld1q_u32(fb + 12)));
        const uint32x2_t b = vand_u32(vget_low_u32(a), vget_high_u32(a));
        return ((vget_lane_u32(b, 0) & vget_lane_u32(b, 1)) >> 24) != 0;
    }
    uint32_t m = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) m &= fb[i];
    return (m >> 24) != 0;
}
#define CM_PIXELS_NOZ(ALPHA, MASK, ...)                                                                  \
    for (uint32_t *pf = fb; pf < end; pf++, uf += duf, vf += dvf, acc = vaddq_s32(acc, dacc)) {         \
        if (MASK && (*pf >> 24)) continue; /* the deferred sky: drawn over since */                     \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M = cm_div255(vmul_u16(Tx, S));                                                      \
        if (ALPHA && bf) CM_BLEND_BF(amode == 0 ? vdup_lane_u16(M, 3) : amode == 1 ? vdup_lane_u16(Tx, 3) : vdup_lane_u16(S, 3), pf) \
        if (ALPHA) {                                                                                    \
            const int fa = amode == 0 ? vget_lane_u16(M, 3) : amode == 1 ? vget_lane_u16(Tx, 3) : vget_lane_u16(S, 3); \
            if (fa < amin) continue;                                                                    \
            if (fa < 255 && T->ablend) {                                                                \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));         \
            }                                                                                           \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
    }
/* MODE 0 plain, 1 the deferred sky (spans all drawn over since skipped, pixels likewise), 2 the
 * sprite span skip (CM_ROW_SKIP's span_clear: every tap alpha 0, nothing drawn) */
#define CM_ROW_NOZ(ALPHA, MODE, ...)                                                                     \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
        float rw_b = 0.0f;                                                                              \
        if (persp) rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;                                         \
        uint32_t *const end = fb + span;                                                                \
        if (MODE != 1 || !span_drawn(fb, span)) {                                                       \
            float u_a, v_a, du, dv;                                                                     \
            if (persp) {                                                                                \
                u_a = uw * rw; v_a = vw * rw;                                                           \
                const float nu = (uw + uwdx * fspan) * rw_b - u_a;                                      \
                const float nv = (vw + vwdx * fspan) * rw_b - v_a;                                      \
                if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); } \
                else                    { du = nu / fspan;               dv = nv / fspan; }             \
            } else {                                                                                    \
                u_a = uw; v_a = vw; du = uwdx; dv = vwdx;                                               \
            }                                                                                           \
            const float32x4_t fq = { u_a, du, v_a, dv };                                                \
            const int32x4_t tq = vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vmulq_f32(fq, tsz), vdupq_n_f32(-16000.0f)), \
                                                           vdupq_n_f32(16000.0f)), 16);                 \
            int32_t uf = vgetq_lane_s32(tq, 0), duf = vgetq_lane_s32(tq, 1);                            \
            int32_t vf = vgetq_lane_s32(tq, 2), dvf = vgetq_lane_s32(tq, 3);                            \
            if (MODE == 2 && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                       \
                SK_SPANS++; SK_PX += (uint64_t)span;                      \
            } else {                                                                                    \
                const float32x4_t cq = { cb, cg, cr, ca };                                              \
                int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                \
                CM_PIXELS_NOZ(ALPHA, MODE == 1, __VA_ARGS__)                                            \
            }                                                                                           \
        }                                                                                               \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; n -= span;                                                                            \
    }
#define CM_VARIANTS_NOZ(KIND, ALPHA, MODE)                                                                  \
    if (rr) { if (T->t16) { CM_ROW_NOZ(ALPHA, MODE, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }              \
              else        { CM_ROW_NOZ(ALPHA, MODE, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }            \
    else    { if (T->t16) { CM_ROW_NOZ(ALPHA, MODE, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }                \
              else        { CM_ROW_NOZ(ALPHA, MODE, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
static void __attribute__((noinline))
cm_row_neon_noz(const CmTri *T, uint32_t *fb, int n, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const bool ps = T->ps; (void)ps;
    const int amode = T->amode, amin = T->amin;
    const bool persp = T->persp, bf = T->bf;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    const AlphaRows *const arows = NULL;
    (void)th; (void)ws; (void)wt; (void)px32; (void)arows;
    if (T->bilin) { if (T->alpha) { CM_VARIANTS_NOZ(CM_BILIN, 1, 0) } else { CM_VARIANTS_NOZ(CM_BILIN, 0, 0) } }
    else          { if (T->alpha) { CM_VARIANTS_NOZ(CM_NEAR, 1, 0) }  else { CM_VARIANTS_NOZ(CM_NEAR, 0, 0) } }
}
/* The deferred sky (speed round 2, 2026-09-26): 65-87% of the sky's pixels are drawn over later in
 * the frame by opaque draws (the overdraw probe: Spaceport 56k sky px a frame, 87% covered). When
 * the sky opens the frame (after DKR's full-screen backdrop fill, which then writes alpha 0) it is
 * held back (sky_defer in raster_tri: each row's start values, exactly what cm_row_neon_noz would
 * have been called with) and drawn late, newest row first, into the pixels still undrawn: every
 * draw writes alpha 255 over a frame cleared to alpha 0, so alpha 0 is a pixel nothing drew over,
 * and newest first the sky's own later rows win as they did. A blended draw (it reads what is under
 * it) and any 2D draw first draw the held lines they cover; a readback, a texture reset and the
 * frame's end draw the rest. Opaque draws in between don't depend on what is under them (the sky
 * writes no z), so every pixel ends the same. Per pixel the integer steps run on, so a skipped
 * pixel changes nothing after it. DKR_SKYDEFER=0 off; qemu: Crescent / Spaceport -16%, race -6%. */
static void __attribute__((noinline))
cm_row_neon_noz_mask(const CmTri *T, uint32_t *fb, int n, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const bool ps = T->ps; (void)ps;
    const int amode = T->amode, amin = T->amin;
    const bool persp = T->persp, bf = T->bf;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const AlphaRows *const arows = NULL;
    (void)th; (void)ws; (void)wt; (void)px32; (void)amode; (void)amin; (void)arows;
    if (T->bilin) { CM_VARIANTS_NOZ(CM_BILIN, 0, 1) } else { CM_VARIANTS_NOZ(CM_NEAR, 0, 1) } /* opaque only */
}
/* The blended sprites without the z test (speed round 2: smoke and the like, ~5k px a frame on
 * Crescent at ~65 scalar instructions each), with the sprite span skip: the scalar loop's
 * SPAN_LOOP(1, 0, 1, BILINEAR) and its span_clear, as cm_row_neon_skip is the z-tested one's. */
static void __attribute__((noinline))
cm_row_neon_noz_skip(const CmTri *T, uint32_t *fb, int n, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const bool ps = T->ps; (void)ps;
    const int amode = T->amode, amin = T->amin;
    const bool persp = T->persp, bf = T->bf;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const AlphaRows *const arows = T->arows;
    (void)th; (void)px32;
    CM_VARIANTS_NOZ(CM_BILIN, 1, 2) /* arows: bilinear, blended (raster_tri) */
}
/* The lean rows (2026-10-04, the final speed sweep; the OoT port's cm_row_lean / cm_row_leana / cm_row_leans, whose
 * device Bench priced its opaque lean rows at 3.2 ms a frame in Kokiri Forest, and the GoldenEye port 44fa084). In
 * cm_row_neon GCC keeps the bilinear taps, the weights and half the loop's invariants on the stack: each pixel stores
 * its four taps and reloads them as two D registers (store-to-load on the pixel's dependent chain), builds the weights
 * on the core and moves them over, and reloads the z bias, the z-write flag, both texel steps and the loop end from the
 * stack (qemu: ~66 instructions a pixel in the hottest opaque block, 13 of them stack loads and stores). Here, for the
 * same values in the same order:
 *   - the taps are loaded straight into NEON registers: a 32-bit texture repeating on a power of two reads each row's
 *     pair with one 64-bit load (the pair wraps only at the last column: then lane by lane), any other wrap lane by
 *     lane from wrapf's addresses; vtrn makes [a c] [b d], the lanes CM_BILIN built on the core;
 *   - the lerp is the one-multiply form (x << 8) + (y - x) f in 16-bit lanes = x (256 - f) + y f (every term and the
 *     sum <= 255 * 256: the modular arithmetic is exact) with f by vdup, not a lane of a core-built vector;
 *   - the shade clamp is vqshrun #16 (= vqmovun(vshr #16)); x/255 adds x >> 8 by vsra (the same sum);
 *   - the fog factor stays in NEON (vdup_lane of the shade's alpha lane); the alpha lane after the fog step is not
 *     kept (vbsl): in the opaque rows nothing reads it before the store's 255;
 *   - the blend's alpha source is chosen per triangle with two masks (vbsl), its factor by vdup_lane, never a core
 *     scalar into vmul_n / vmla_n; the branch-free blend (CM_BLEND_BF) as before;
 *   - the opaque rows with no z bias and the z write on (the world's common case) have both as constants (OoT's LEANZ);
 *   - the store's bytes by one vtbl (the GoldenEye port 44fa084): every lane is <= 255, so its low byte is the value
 *     vmovn kept; the opaque rows take byte 3 from a register of 0xFF (vorr with a255 gave 255 there).
 * Taken for bilinear texture x shade rows on 32-bit textures (all of DKR's: is16 is never set): z-tested, opaque or
 * blended / alpha-tested, fogged per pixel or not (the sprite span skip included); no z, opaque (the sky as it comes and
 * the deferred sky's held rows) and blended on textures repeating on a power of two; and the fogged no-z rows, which
 * ran on the scalar CM=2 loops (the Dragon Forest boss's fogged sky: ~8% of its qemu time). Everything else (nearest,
 * the tint and add combines, two textures) stays on cm_row_neon* and the scalar loops. Bit-exact against them:
 * tools/raster_test.sh (ARM under qemu-arm and x86, every variant, every kind of row) and tools/raster_test_mut.sh
 * (every new path mutated and caught). DKR_LEANROWS=0: all off (the benchmark's lean=both); DKR_LEANZ=0 / DKR_LEANA=0 /
 * DKR_LEANS=0 / DKR_LEANF=0: the LEANZ constants / the alpha rows / the no-z rows / the fogged no-z rows alone off. */
static int sLeanRows SAVESTATE_EXCLUDE = 1, sLeanZ SAVESTATE_EXCLUDE = 1, sLeanA SAVESTATE_EXCLUDE = 1, sLeanS SAVESTATE_EXCLUDE = 1;
static int sLeanF SAVESTATE_EXCLUDE = 1;
static inline uint16x4_t cm_div255_lean(uint16x4_t m) { /* cm_div255: (m + (m >> 8) + 1) >> 8 */
    return vshr_n_u16(vadd_u16(vsra_n_u16(m, m, 8), vdup_n_u16(1)), 8);
}
/* CM_BILIN's two lerps, one multiply each: X8 = [a c], Y8 = [b d] (bytes B G R A) */
#define CM_LLERP2(X8, Y8)                                                                               \
        const uint16x8_t Hl = vshrq_n_u16(vmlaq_u16(vshll_n_u8(X8, 8), vsubl_u8(Y8, X8),                \
                                                    vdupq_n_u16((uint16_t)((su >> 8) & 0xff))), 8);      \
        Tx = vshr_n_u16(vmla_u16(vshl_n_u16(vget_low_u16(Hl), 8), vsub_u16(vget_high_u16(Hl), vget_low_u16(Hl)), \
                                 vdup_n_u16((uint16_t)((sv >> 8) & 0xff))), 8);
/* CM_BILIN(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV): a row's pair in one load unless it wraps */
#define CM_LBIL_RR                                                                                      \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = (su >> 16) & mu;                                                                \
        const int b0 = ((sv >> 16) & mv) * tw, b1 = (((sv >> 16) + 1) & mv) * tw;                       \
        uint32x2_t AB, CD;                                                                              \
        if (iu0 < mu) { AB = vld1_u32(px32 + b0 + iu0); CD = vld1_u32(px32 + b1 + iu0); }              \
        else { AB = vld1_lane_u32(px32 + b0, vld1_dup_u32(px32 + b0 + iu0), 1);                         \
               CD = vld1_lane_u32(px32 + b1, vld1_dup_u32(px32 + b1 + iu0), 1); }                       \
        const uint32x2x2_t TR = vtrn_u32(AB, CD);                                                       \
        const uint8x8_t X8 = vreinterpret_u8_u32(TR.val[0]), Y8 = vreinterpret_u8_u32(TR.val[1]);       \
        CM_LLERP2(X8, Y8)
/* CM_BILIN(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV): wrapf's taps (a clamp, or a repeat on an odd size), lane by lane */
#define CM_LBIL_GEN                                                                                     \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = CM_WRAP_GU(su >> 16), iu1 = CM_WRAP_GU((su >> 16) + 1);                         \
        const int b0 = CM_WRAP_GV(sv >> 16) * tw, b1 = CM_WRAP_GV((sv >> 16) + 1) * tw;                 \
        const uint8x8_t X8 = vreinterpret_u8_u32(vld1_lane_u32(px32 + b1 + iu0, vld1_dup_u32(px32 + b0 + iu0), 1)); \
        const uint8x8_t Y8 = vreinterpret_u8_u32(vld1_lane_u32(px32 + b1 + iu1, vld1_dup_u32(px32 + b0 + iu1), 1)); \
        CM_LLERP2(X8, Y8)
/* The pixel loop. Z: 0 no z (MASK: the deferred sky's undrawn pixels only), 1 the z test with zbias and dmask,
 * 2 no bias and the z write as constants. ALPHA, BF: cm_row_neon's (no fog with ALPHA). FOG: the opaque fog step. */
#define CM_PIXELS_L(Z, MASK, ALPHA, BF, FOG, ...)                                                        \
    for (uint32_t *pf = fb; pf < end; pf++, uf += duf, vf += dvf, acc = vaddq_s32(acc, dacc)) {         \
        int uz = 0;                                                                                     \
        if (Z) { uz = clamp16_sh8(zq); zq += zdq; pz++;                                                 \
                 if (uz > pz[-1] + (Z == 2 ? 0 : zbias)) continue; }                                    \
        if (MASK && (*pf >> 24)) continue; /* the deferred sky: drawn over since */                     \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = cm_div255_lean(vmul_u16(Tx, S));                                                 \
        uint16x4_t F = v255; (void)F;                                                                   \
        if (ALPHA) { /* the alpha source, before the fog step (cm_row_neon kept the alpha lane through it) */ \
            F = vbsl_u16(selM, M, vbsl_u16(selT, Tx, S));                                               \
            if (FOG) F = vbsl_u16(selF, vbsl_u16(selA3, cm_div255_lean(vmul_u16(Tx, fgav)), fgav), F);  \
            F = vdup_lane_u16(F, 3);                                                                    \
        }                                                                                               \
        if (FOG) {                                                                                      \
            const uint16x4_t fs = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogc, fs));                    \
        }                                                                                               \
        if (ALPHA) {                                                                                    \
            if (BF) {                                                                                   \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                const uint16x4_t Bl = cm_div255_lean(vmla_u16(vmul_u16(M, F), D, vsub_u16(v255, F)));   \
                M = vbsl_u16(vclt_u16(F, aminv), D, vorr_u16(Bl, a255));                                \
                vst1_lane_u32(pf, vreinterpret_u32_u8(vtbl1_u8(vreinterpret_u8_u16(M), lo8)), 0);         \
                continue;                                                                               \
            }                                                                                           \
            const int fa = vget_lane_u16(F, 0);                                                         \
            if (fa < amin) continue;                                                                    \
            if (fa < ablim) {                                                                           \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255_lean(vmla_u16(vmul_u16(M, F), D, vsub_u16(v255, F)));                     \
            }                                                                                           \
        }                                                                                               \
        { const uint8x8x2_t MT = { { vreinterpret_u8_u16(M), ff8 } };                                    \
          vst1_lane_u32(pf, vreinterpret_u32_u8(vtbl2_u8(MT, lo8a)), 0); }                              \
        if (Z == 2 || (Z == 1 && dmask)) pz[-1] = (uint16_t)uz;                                         \
    }
/* CM_ROW / CM_ROW_SKIP with the pixel loop above (the span skip when arows is set) */
#define CM_ROWL(Z, ALPHA, BF, FOG, ...)                                                                  \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
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
        uint32_t *const end = fb + span;                                                                \
        if (ALPHA && arows && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                      \
            SK_SPANS++; SK_PX += (uint64_t)span;                          \
        } else {                                                                                        \
            const float32x2_t zc = vmin_f32(vmax_f32(vdup_n_f32(zf), vdup_n_f32(-1.0f)), vdup_n_f32(2.0f)); \
            int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);              \
            CM_PS_SCALAR                                                                                \
            int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                    \
            uint16_t *pz = zp;                                                                          \
            { const int32x4_t dacc = daccp; CM_PIXELS_L(Z, 0, ALPHA, BF, FOG, __VA_ARGS__) }            \
        }                                                                                               \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }
/* CM_ROW_NOZ (MODE 0, 1 or 2: the deferred sky, the span skip when arows is set) with the pixel loop above */
#define CM_ROWL_NOZ(MASK, ALPHA, BF, FOG, ...)                                                                           \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
        float rw_b = 0.0f;                                                                              \
        if (persp) rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;                                         \
        uint32_t *const end = fb + span;                                                                \
        if (!MASK || !span_drawn(fb, span)) {                                                           \
            float u_a, v_a, du, dv;                                                                     \
            if (persp) {                                                                                \
                u_a = uw * rw; v_a = vw * rw;                                                           \
                const float nu = (uw + uwdx * fspan) * rw_b - u_a;                                      \
                const float nv = (vw + vwdx * fspan) * rw_b - v_a;                                      \
                if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); } \
                else                    { du = nu / fspan;               dv = nv / fspan; }             \
            } else {                                                                                    \
                u_a = uw; v_a = vw; du = uwdx; dv = vwdx;                                               \
            }                                                                                           \
            const float32x4_t fq = { u_a, du, v_a, dv };                                                \
            const int32x4_t tq = vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vmulq_f32(fq, tsz), vdupq_n_f32(-16000.0f)), \
                                                           vdupq_n_f32(16000.0f)), 16);                 \
            int32_t uf = vgetq_lane_s32(tq, 0), duf = vgetq_lane_s32(tq, 1);                            \
            int32_t vf = vgetq_lane_s32(tq, 2), dvf = vgetq_lane_s32(tq, 3);                            \
            if (ALPHA && arows && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                  \
                SK_SPANS++; SK_PX += (uint64_t)span;                      \
            } else {                                                                                    \
                const float32x4_t cq = { cb, cg, cr, ca };                                              \
                int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                \
                int32_t zq = 0; uint16_t *pz = NULL; (void)zq; (void)pz;                                \
                CM_PIXELS_L(0, MASK, ALPHA, BF, FOG, __VA_ARGS__)                                       \
            }                                                                                           \
        }                                                                                               \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; n -= span;                                                                            \
    }
#define CM_LEAN_LOCALS                                                                                  \
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull), v255 = vdup_n_u16(255);                  \
    const uint8x8_t ff8 = vdup_n_u8(0xFF);                                                              \
    const uint8x8_t lo8 = vcreate_u8(0x0000000006040200ull), lo8a = vcreate_u8(0x0000000008040200ull);   \
    const uint32_t *const px32 = (const uint32_t *)T->px;                                               \
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;                     \
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx;                                         \
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;                         \
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };                                         \
    const int32x4_t dacc = T->dacc;                                                                     \
    const bool ps = PSM == 2 ? T->ps : PSM; (void)ps;                                                   \
    const bool persp = T->persp;                                                                        \
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */    \
    const int amin = T->amin, ablim = T->ablend ? 255 : 0;                                              \
    const AlphaRows *const arows = T->arows;                                                            \
    const uint16x4_t fogc = T->fogc, aminv = vdup_n_u16((uint16_t)amin);                                \
    const uint16x4_t selM = vdup_n_u16(T->amode == 0 ? 0xFFFF : 0), selT = vdup_n_u16(T->amode == 1 ? 0xFFFF : 0); \
    const uint16x4_t selF = vdup_n_u16(T->fga >= 0 ? 0xFFFF : 0), selA3 = vdup_n_u16(T->amode == 3 ? 0xFFFF : 0); \
    const uint16x4_t fgav = vdup_n_u16((uint16_t)(T->fga >= 0 ? T->fga : 0));                          \
    (void)th; (void)a255; (void)ff8; (void)lo8; (void)lo8a; (void)amin; (void)ablim; (void)arows;     \
    (void)fogc; (void)aminv; (void)selM; (void)selT; (void)selF; (void)selA3; (void)fgav;
/* z-tested rows (raster_tri checks): opaque or alpha, fogged or not */
static inline __attribute__((always_inline)) void
cm_row_lean_b(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw, const int PSM) {
    CM_LEAN_LOCALS
    const float zdx = T->zdx;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias;
    const bool dmask = T->dmask;
    if (T->alpha) {
        if (T->fog) {
            if (T->bf) { if (rr) { CM_ROWL(1, 1, 1, 1, CM_LBIL_RR) } else { CM_ROWL(1, 1, 1, 1, CM_LBIL_GEN) } }
            else       { if (rr) { CM_ROWL(1, 1, 0, 1, CM_LBIL_RR) } else { CM_ROWL(1, 1, 0, 1, CM_LBIL_GEN) } }
        } else {
            if (T->bf) { if (rr) { CM_ROWL(1, 1, 1, 0, CM_LBIL_RR) } else { CM_ROWL(1, 1, 1, 0, CM_LBIL_GEN) } }
            else       { if (rr) { CM_ROWL(1, 1, 0, 0, CM_LBIL_RR) } else { CM_ROWL(1, 1, 0, 0, CM_LBIL_GEN) } }
        }
        return;
    }
    if (sLeanZ && zbias == 0 && dmask) {
        if (T->fog) { if (rr) { CM_ROWL(2, 0, 0, 1, CM_LBIL_RR) } else { CM_ROWL(2, 0, 0, 1, CM_LBIL_GEN) } }
        else        { if (rr) { CM_ROWL(2, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWL(2, 0, 0, 0, CM_LBIL_GEN) } }
        return;
    }
    if (T->fog) { if (rr) { CM_ROWL(1, 0, 0, 1, CM_LBIL_RR) } else { CM_ROWL(1, 0, 0, 1, CM_LBIL_GEN) } }
    else        { if (rr) { CM_ROWL(1, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWL(1, 0, 0, 0, CM_LBIL_GEN) } }
}
static void __attribute__((noinline)) cm_row_lean_r(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_lean_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 2); }
static void __attribute__((noinline)) cm_row_lean_p0(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_lean_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0); }
static void __attribute__((noinline)) cm_row_lean_p1(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_lean_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 1); }
static inline void cm_row_lean(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    if (!sPsRows) cm_row_lean_r(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else if (T->ps) cm_row_lean_p1(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else cm_row_lean_p0(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
}
/* no-z rows: the deferred sky's held rows (mask 1); as they come (mask 0): opaque, fogged or not, and blended
 * (raster_tri sends those only on textures repeating on a power of two) */
static void __attribute__((noinline))
cm_row_lean_noz(const CmTri *T, uint32_t *fb, int n, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw, int mask) {
    enum { PSM = 2 };
    CM_LEAN_LOCALS
    const int zbias = 0; const int32_t zdq = 0; const bool dmask = false;
    (void)zbias; (void)zdq; (void)dmask;
    if (mask) { if (rr) { CM_ROWL_NOZ(1, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWL_NOZ(1, 0, 0, 0, CM_LBIL_GEN) } return; }
    if (!T->alpha) {
        if (T->fog) { if (rr) { CM_ROWL_NOZ(0, 0, 0, 1, CM_LBIL_RR) } else { CM_ROWL_NOZ(0, 0, 0, 1, CM_LBIL_GEN) } }
        else        { if (rr) { CM_ROWL_NOZ(0, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWL_NOZ(0, 0, 0, 0, CM_LBIL_GEN) } }
        return;
    }
    if (T->fog) { if (T->bf) { CM_ROWL_NOZ(0, 1, 1, 1, CM_LBIL_RR) } else { CM_ROWL_NOZ(0, 1, 0, 1, CM_LBIL_RR) } }
    else        { if (T->bf) { CM_ROWL_NOZ(0, 1, 1, 0, CM_LBIL_RR) } else { CM_ROWL_NOZ(0, 1, 0, 0, CM_LBIL_RR) } }
}
/* DKR_SPANVEC (2026-10-07, the cross-port speed round; super-mario-64's SM64_SPANVEC, branch sm64-speed-2026-10-06): the
 * lean rows with the row's float state as two NEON vectors, A = { b, g, r, a } (the shade, in acc's lane order) and
 * B = { z, u/w, v/w, 1/w }, where cm_row_lean kept nine floats and nine steps that GCC moved between the core, the VFP
 * and the stack around every span. The same operations in the same order, lane by lane: the next span's start is
 * A + dA * span and B + dB * span (vmla is not fused on ARMv7 NEON: the product rounded, then the sum, as the VFP's
 * multiply then add); the perspective ends u/w * (1/w) and (u/w)' * (1/w)' - u as two-lane multiplies and a subtract,
 * the 1/16 step a two-lane multiply; the divides (the span's 1/w and a short span's step) stay VFP divides; the shade's
 * fixed point straight from A (vcvt #16, as cm_row_lean's). NEON flushes denormals to zero where the VFP keeps them: no
 * value here comes near one (screen and texture coordinates, 1/w, colours 0..255, depth 0..1 and their steps are 0 or
 * far above 1e-30). DKR_SPANVEC=0: cm_row_lean / cm_row_lean_noz as before. Same pixels.
 * DKR_ZSKIP (2026-10-07; SM64_ZSKIP; in these rows only): a z-tested span whose nearest depth is further than the
 * farthest depth stored under it plus the decal window fails the z test at every pixel (zq steps by integer adds and
 * clamp16_sh8 is monotone, so the nearest depth is at one of the span's ends), so it draws nothing: skipped whole,
 * before its texture setup and pixel loop; its float state steps on as when drawn. A triangle stops testing after four
 * misses in a row (speed only). The census: on the bench levels 9-40% of the z-tested rows' pixels lie in such spans
 * (Crescent Island 40%: its hidden track). DKR_ZSKIP=0: off. Same pixels. */
/* DKR_FASTROWS default off since 2026-10-11 (the speed audit): the device Bench (20261011-round/dkr) had all three
 * switches off faster on 5 of 7 levels by 0.2-0.4 ms, FASTROWS off -1.1 ms on Crescent Island (GoldenEye's reading:
 * NEON-to-core moves and serial vmla chains stall the A7, and DKR's triangles are too short to pay for the setup).
 * DKR_FASTROWS=1: tri_rows, as v1.3.3 shipped. Same pixels either way. */
#if defined(MIYOO)
/* The Mini keeps FASTROWS on: the second core's share of a triangle (TrJob below) is drawn in tri_rows, so with
 * FASTROWS off no triangle is split and the worker only gets the sky (qemu, both cores, Whale Bay: 0 of 141535
 * triangles split). DKR_FASTROWS=0: the handheld's default (one core for the 3D). */
static int sSpanVec SAVESTATE_EXCLUDE = 1, sZSkip SAVESTATE_EXCLUDE = 1, sFastRows SAVESTATE_EXCLUDE = 1;
#else
static int sSpanVec SAVESTATE_EXCLUDE = 1, sZSkip SAVESTATE_EXCLUDE = 1, sFastRows SAVESTATE_EXCLUDE = 0;
#endif
/* the span's u/w, v/w ends: fq = { u_a, du, v_a, dv } (CM_ROWL's float for float) */
#define CM_UVV                                                                                          \
        float32x2_t ua, duv;                                                                            \
        if (persp) {                                                                                    \
            const float32x2_t uvw = vget_low_f32(vextq_f32(B, B, 1));    /* { u/w, v/w } */             \
            ua = vmul_n_f32(uvw, rw);                                                                   \
            const float32x2_t nuv = vsub_f32(vmul_n_f32(vget_low_f32(vextq_f32(Bn, Bn, 1)), rw_b), ua); \
            if (span == PERSP_SPAN) duv = vmul_n_f32(nuv, 1.0f / PERSP_SPAN);                           \
            else duv = vset_lane_f32(vget_lane_f32(nuv, 1) / fspan, vdup_n_f32(vget_lane_f32(nuv, 0) / fspan), 1); \
        } else {                                                                                        \
            ua = vget_low_f32(vextq_f32(B, B, 1)); duv = DUV;                                           \
        }                                                                                               \
        const float32x2x2_t zuv = vzip_f32(ua, duv);                                                    \
        const float32x4_t fq = vcombine_f32(zuv.val[0], zuv.val[1]);                                    \
        const int32x4_t tq = vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vmulq_f32(fq, tsz), vdupq_n_f32(-16000.0f)), \
                                                       vdupq_n_f32(16000.0f)), 16);                     \
        int32_t uf = vgetq_lane_s32(tq, 0), duf = vgetq_lane_s32(tq, 1);                                \
        int32_t vf = vgetq_lane_s32(tq, 2), dvf = vgetq_lane_s32(tq, 3);
/* CM_ROWL with the vector state (and DKR_ZSKIP) */
#define CM_ROWLV(Z, ALPHA, BF, FOG, ...)                                                                 \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float32x4_t fsv = vdupq_n_f32(fspan);                                                     \
        const float32x4_t An = vmlaq_f32(A, DA, fsv), Bn = vmlaq_f32(B, DB, fsv); /* the next span's start */ \
        const float sw_b = vgetq_lane_f32(Bn, 3);                                                       \
        float rw_b = 0.0f;                                                                              \
        if (persp) rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;                                         \
        uint32_t *const end = fb + span;                                                                \
        const float32x2_t zc = vmin_f32(vmax_f32(vdup_lane_f32(vget_low_f32(B), 0), vdup_n_f32(-1.0f)), vdup_n_f32(2.0f)); \
        int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);                  \
        int zskip = 0;                                                                                  \
        if (zsk > 0 && span >= 4) { /* DKR_ZSKIP: the span behind the depth buffer all along */          \
            const int uz0 = clamp16_sh8(zq), uz1 = clamp16_sh8(zq + (span - 1) * zdq);                  \
            uint16x4_t m4;                                                                              \
            if (span >= 8) { const uint16x8_t m8 = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8));   \
                             m4 = vmax_u16(vget_low_u16(m8), vget_high_u16(m8)); }                      \
            else m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));                                  \
            m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);                                             \
            if ((uz0 < uz1 ? uz0 : uz1) > (int)vget_lane_u16(m4, 0) + (Z == 2 ? 0 : zbias)) { zskip = 1; zsk = 4; } \
            else zsk--;                                                                                 \
        }                                                                                               \
        if (!zskip) {                                                                                   \
            CM_UVV                                                                                      \
            if (ALPHA && arows && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                  \
                SK_SPANS++; SK_PX += (uint64_t)span;                      \
            } else {                                                                                    \
                float32x4_t cq = A; int32x4_t daccp = dacc;                                             \
                if (ps) { cq = vmulq_n_f32(A, rw); daccp = ps_dacc(cq, vmulq_n_f32(An, rw_b), span, fspan); } \
                int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                \
                uint16_t *pz = zp;                                                                      \
                { const int32x4_t dacc = daccp; CM_PIXELS_L(Z, 0, ALPHA, BF, FOG, __VA_ARGS__) }        \
            }                                                                                           \
        }                                                                                               \
        A = An; B = Bn; rw = rw_b;                                                                      \
        fb = end; zp += span; n -= span;                                                                \
    }
/* CM_ROWL_NOZ with the vector state (B's z lane and its step are 0) */
#define CM_ROWLV_NOZ(MASK, ALPHA, BF, FOG, ...)                                                          \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float32x4_t fsv = vdupq_n_f32(fspan);                                                     \
        const float32x4_t An = vmlaq_f32(A, DA, fsv), Bn = vmlaq_f32(B, DB, fsv);                       \
        const float sw_b = vgetq_lane_f32(Bn, 3);                                                       \
        float rw_b = 0.0f;                                                                              \
        if (persp) rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;                                         \
        uint32_t *const end = fb + span;                                                                \
        if (!MASK || !span_drawn(fb, span)) {                                                           \
            CM_UVV                                                                                      \
            if (ALPHA && arows && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                  \
                SK_SPANS++; SK_PX += (uint64_t)span;                      \
            } else {                                                                                    \
                int32x4_t acc = vcvtq_n_s32_f32(A, 16);                                                 \
                int32_t zq = 0; uint16_t *pz = NULL; (void)zq; (void)pz;                                \
                CM_PIXELS_L(0, MASK, ALPHA, BF, FOG, __VA_ARGS__)                                       \
            }                                                                                           \
        }                                                                                               \
        A = An; B = Bn; rw = rw_b;                                                                      \
        fb = end; n -= span;                                                                            \
    }
#define CM_LEANV_LOCALS                                                                                 \
    CM_LEAN_LOCALS                                                                                      \
    (void)swdx; (void)uwdx; (void)vwdx; (void)crdx; (void)cgdx; (void)cbdx; (void)cadx;               \
    const float32x4_t DA = T->da;                                                                       \
    const float32x2_t DUV = vget_low_f32(vextq_f32(T->db, T->db, 1)); /* { uwdx, vwdx } */
/* cm_row_lean (z-tested) with the vector state: A = { cb, cg, cr, ca }, B = { zf, uw, vw, sw } */
static inline __attribute__((always_inline)) void
cm_row_lean_v_b(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float32x4_t A, float32x4_t B, float rw, const int PSM) {
    CM_LEANV_LOCALS
    const float32x4_t DB = T->db;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias;
    const bool dmask = T->dmask;
    int zsk = T->zsk;
    if (T->alpha) {
        if (T->fog) {
            if (T->bf) { if (rr) { CM_ROWLV(1, 1, 1, 1, CM_LBIL_RR) } else { CM_ROWLV(1, 1, 1, 1, CM_LBIL_GEN) } }
            else       { if (rr) { CM_ROWLV(1, 1, 0, 1, CM_LBIL_RR) } else { CM_ROWLV(1, 1, 0, 1, CM_LBIL_GEN) } }
        } else {
            if (T->bf) { if (rr) { CM_ROWLV(1, 1, 1, 0, CM_LBIL_RR) } else { CM_ROWLV(1, 1, 1, 0, CM_LBIL_GEN) } }
            else       { if (rr) { CM_ROWLV(1, 1, 0, 0, CM_LBIL_RR) } else { CM_ROWLV(1, 1, 0, 0, CM_LBIL_GEN) } }
        }
    } else if (sLeanZ && zbias == 0 && dmask) {
        if (T->fog) { if (rr) { CM_ROWLV(2, 0, 0, 1, CM_LBIL_RR) } else { CM_ROWLV(2, 0, 0, 1, CM_LBIL_GEN) } }
        else        { if (rr) { CM_ROWLV(2, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWLV(2, 0, 0, 0, CM_LBIL_GEN) } }
    } else {
        if (T->fog) { if (rr) { CM_ROWLV(1, 0, 0, 1, CM_LBIL_RR) } else { CM_ROWLV(1, 0, 0, 1, CM_LBIL_GEN) } }
        else        { if (rr) { CM_ROWLV(1, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWLV(1, 0, 0, 0, CM_LBIL_GEN) } }
    }
    ((CmTri *)T)->zsk = zsk; /* the triangle's run of misses, for its next row */
}
static void __attribute__((noinline)) cm_row_lean_v_r(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float32x4_t A, float32x4_t B, float rw) { cm_row_lean_v_b(T, fb, zp, n, A, B, rw, 2); }
static void __attribute__((noinline)) cm_row_lean_v_p0(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float32x4_t A, float32x4_t B, float rw) { cm_row_lean_v_b(T, fb, zp, n, A, B, rw, 0); }
static void __attribute__((noinline)) cm_row_lean_v_p1(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float32x4_t A, float32x4_t B, float rw) { cm_row_lean_v_b(T, fb, zp, n, A, B, rw, 1); }
static inline void cm_row_lean_v(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float32x4_t A, float32x4_t B, float rw) {
    if (!sPsRows) cm_row_lean_v_r(T, fb, zp, n, A, B, rw);
    else if (T->ps) cm_row_lean_v_p1(T, fb, zp, n, A, B, rw);
    else cm_row_lean_v_p0(T, fb, zp, n, A, B, rw);
}
/* cm_row_lean_noz with the vector state: A = { cb, cg, cr, ca }, B = { 0, uw, vw, sw } */
static void __attribute__((noinline))
cm_row_lean_noz_v(const CmTri *T, uint32_t *fb, int n, float32x4_t A, float32x4_t B, float rw, int mask) {
    enum { PSM = 2 };
    CM_LEANV_LOCALS
    const float32x4_t DB = vsetq_lane_f32(0.0f, T->db, 0);
    const int zbias = 0; const int32_t zdq = 0; const bool dmask = false;
    (void)zbias; (void)zdq; (void)dmask;
    if (mask) { if (rr) { CM_ROWLV_NOZ(1, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWLV_NOZ(1, 0, 0, 0, CM_LBIL_GEN) } }
    else if (!T->alpha) {
        if (T->fog) { if (rr) { CM_ROWLV_NOZ(0, 0, 0, 1, CM_LBIL_RR) } else { CM_ROWLV_NOZ(0, 0, 0, 1, CM_LBIL_GEN) } }
        else        { if (rr) { CM_ROWLV_NOZ(0, 0, 0, 0, CM_LBIL_RR) } else { CM_ROWLV_NOZ(0, 0, 0, 0, CM_LBIL_GEN) } }
    } else {
        if (T->fog) { if (T->bf) { CM_ROWLV_NOZ(0, 1, 1, 1, CM_LBIL_RR) } else { CM_ROWLV_NOZ(0, 1, 0, 1, CM_LBIL_RR) } }
        else        { if (T->bf) { CM_ROWLV_NOZ(0, 1, 1, 0, CM_LBIL_RR) } else { CM_ROWLV_NOZ(0, 1, 0, 0, CM_LBIL_RR) } }
    }
}
/* the lean rows from scalar start values (raster_tri's own loop, the deferred sky's held rows): DKR_SPANVEC's or not */
static inline void cm_lean_row(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                               float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    if (sSpanVec) cm_row_lean_v(T, fb, zp, n, (float32x4_t){ cb, cg, cr, ca }, (float32x4_t){ zf, uw, vw, sw }, rw);
    else cm_row_lean(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
}
static inline void cm_lean_row_noz(const CmTri *T, uint32_t *fb, int n,
                                   float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw, int mask) {
    if (sSpanVec) cm_row_lean_noz_v(T, fb, n, (float32x4_t){ cb, cg, cr, ca }, (float32x4_t){ 0.0f, uw, vw, sw }, rw, mask);
    else cm_row_lean_noz(T, fb, n, cr, cg, cb, ca, uw, vw, sw, rw, mask);
}
#undef CM_LEANV_LOCALS
#undef CM_ROWLV
#undef CM_ROWLV_NOZ
#undef CM_UVV
#undef CM_LEAN_LOCALS
#undef CM_ROWL
#undef CM_ROWL_NOZ
#undef CM_PIXELS_L
#undef CM_LBIL_RR
#undef CM_LBIL_GEN
#undef CM_LLERP2
typedef struct { uint32_t off; int32_t n, tri, next; float cr, cg, cb, ca, uw, vw, sw, rw; } SkyRow;
#define SKY_TRIS 512
#define SKY_ROWS 8192
static CmTri sSkyTri[SKY_TRIS] SAVESTATE_EXCLUDE;
static SkyRow sSkyRow[SKY_ROWS] SAVESTATE_EXCLUDE;
static int sSkyNT SAVESTATE_EXCLUDE, sSkyNR SAVESTATE_EXCLUDE;
static bool sSkyWin SAVESTATE_EXCLUDE;      /* nothing but held sky drawn yet this frame */
static int sSkyDefer SAVESTATE_EXCLUDE = -1; /* DKR_SKYDEFER=0: the sky drawn as it comes (A/B) */
static uint32_t *sSkyFB SAVESTATE_EXCLUDE;  /* the target the rows index */
/* Held rows by screen line, newest first (a pixel is on one line, so a line's rows can be drawn
 * apart from the rest): a blended draw draws only the lines it covers first. */
#define SKY_LINES 512
static int32_t sSkyHead[SKY_LINES] SAVESTATE_EXCLUDE;
static int sSkyY0 SAVESTATE_EXCLUDE = SKY_LINES, sSkyY1 SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* The Mini's second core (see TrJob below): the held sky is drawn line by line, and a line's rows touch only
 * that line, so a flush of enough lines is shared, this core the even lines from y0, the worker the odd
 * ones, and finished before anything else draws. */
#include <unistd.h>
#include "miyoo_worker.h"
#include "miyoo_watchdog.h"
#define SKY_SPLIT_LINES 8
static uint64_t sTrPx SAVESTATE_EXCLUDE, sTrSplitPx SAVESTATE_EXCLUDE; /* the PERF line's split share of the drawn pixels */
static uint32_t sky_lines_step(int y0, int y1, int ystep) { /* sky_flush_lines' loop, every ystep-th line; returns its pixels */
    uint32_t px = 0;
    for (int y = y0; y < y1; y += ystep) {
        for (int i = sSkyHead[y]; i >= 0; i = sSkyRow[i].next) {
            const SkyRow *r = &sSkyRow[i];
            const CmTri *const T = &sSkyTri[r->tri];
            if (T->lean) cm_lean_row_noz(T, sSkyFB + r->off, r->n, r->cr, r->cg, r->cb, r->ca, r->uw, r->vw, r->sw, r->rw, 1);
            else cm_row_neon_noz_mask(T, sSkyFB + r->off, r->n, r->cr, r->cg, r->cb, r->ca, r->uw, r->vw, r->sw, r->rw);
            px += (uint32_t)r->n;
        }
        sSkyHead[y] = -1;
    }
    return px;
}
typedef struct { int y0, y1; uint32_t px; uint64_t sk_spans, sk_px; } SkyJob;
static void sky_job(void *arg) {
    SkyJob *const j = (SkyJob *)arg;
    const uint64_t s0 = tSkSpans, p0 = tSkPx;
    j->px = sky_lines_step(j->y0, j->y1, 2);
    j->sk_spans = tSkSpans - s0; j->sk_px = tSkPx - p0;
}
#endif
static void sky_flush_lines(int y0, int y1) {
    if (y0 < sSkyY0) y0 = sSkyY0;
    if (y1 > sSkyY1) y1 = sSkyY1;
#if defined(MIYOO)
    {
        uint32_t px;
        if (miyoo_worker_ready() && y1 - y0 >= SKY_SPLIT_LINES) {
            SkyJob job = { y0 + 1, y1, 0, 0, 0 };
            miyoo_worker_run(sky_job, &job);
            px = sky_lines_step(y0, y1, 2);
            gMiyooWdGame = WD_SKY_WAIT;
            miyoo_worker_wait();
            gMiyooWdGame = WD_RUN;
            px += job.px; tSkSpans += job.sk_spans; tSkPx += job.sk_px;
            sTrSplitPx += px;
        } else px = sky_lines_step(y0, y1, 1);
        sTrPx += px;
        return;
    }
#endif
    for (int y = y0; y < y1; y++) {
        for (int i = sSkyHead[y]; i >= 0; i = sSkyRow[i].next) {
            const SkyRow *r = &sSkyRow[i];
            const CmTri *const T = &sSkyTri[r->tri];
            if (T->lean) cm_lean_row_noz(T, sSkyFB + r->off, r->n, r->cr, r->cg, r->cb, r->ca, r->uw, r->vw, r->sw, r->rw, 1);
            else cm_row_neon_noz_mask(T, sSkyFB + r->off, r->n, r->cr, r->cg, r->cb, r->ca, r->uw, r->vw, r->sw, r->rw);
        }
        sSkyHead[y] = -1;
    }
}
static void sky_flush(void) {
    sSkyWin = false;
    if (sSkyNR) sky_flush_lines(sSkyY0, sSkyY1);
    sSkyNR = sSkyNT = 0; sSkyY0 = SKY_LINES; sSkyY1 = 0;
}
static int sBlendBF SAVESTATE_EXCLUDE = -1; /* DKR_BLENDBF=0: the blend with its branch (A/B) */
static int sNozNeon SAVESTATE_EXCLUDE = -1; /* DKR_NOZNEON=0: the sky through the scalar loop (A/B) */

/* The rows, twice (round 17): cm_row_neon, round 16's plain rows token for token (an always-
 * inline body shared with the fog rows changed its register allocation: +0.5% on the race), and
 * cm_row_neon_fog, the fogged texture x shade rows with CM=2's step. */
#define CM_VARIANTS(KIND, ALPHA, FOG)                                                                   \
    if (rr) { if (T->t16) { CM_ROW(ALPHA, FOG, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }             \
              else        { CM_ROW(ALPHA, FOG, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }           \
    else    { if (T->t16) { CM_ROW(ALPHA, FOG, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }               \
              else        { CM_ROW(ALPHA, FOG, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
static inline __attribute__((always_inline)) void
cm_row_neon_b(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw, const int PSM) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const bool ps = PSM == 2 ? T->ps : PSM; (void)ps;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin;
    const bool persp = T->persp, dmask = T->dmask, bf = T->bf;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    (void)th; (void)ws; (void)wt; (void)px32;
    if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 1, 0) } else { CM_VARIANTS(CM_BILIN, 0, 0) } }
    else          { if (T->alpha) { CM_VARIANTS(CM_NEAR, 1, 0) }  else { CM_VARIANTS(CM_NEAR, 0, 0) } }
}
static void __attribute__((noinline)) cm_row_neon_r(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 2); }
static void __attribute__((noinline)) cm_row_neon_p0(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0); }
static void __attribute__((noinline)) cm_row_neon_p1(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 1); }
static inline void cm_row_neon(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    if (!sPsRows) cm_row_neon_r(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else if (T->ps) cm_row_neon_p1(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else cm_row_neon_p0(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
}
static inline __attribute__((always_inline)) void
cm_row_neon_fog_b(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw, const int PSM) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const bool ps = PSM == 2 ? T->ps : PSM; (void)ps;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin;
    const bool persp = T->persp, dmask = T->dmask, bf = T->bf;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    (void)th; (void)ws; (void)wt; (void)px32;
    const uint16x4_t rgb3 = vcreate_u16(0x0000FFFFFFFFFFFFull); /* lanes 0-2 (B G R): the fog step's */
    const uint16x4_t fogc = T->fogc;
    const int fga = T->fga;
    if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 1, 1) } else { CM_VARIANTS(CM_BILIN, 0, 1) } }
    else          { if (T->alpha) { CM_VARIANTS(CM_NEAR, 1, 1) }  else { CM_VARIANTS(CM_NEAR, 0, 1) } }
}
static void __attribute__((noinline)) cm_row_neon_fog_r(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_fog_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 2); }
static void __attribute__((noinline)) cm_row_neon_fog_p0(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_fog_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0); }
static void __attribute__((noinline)) cm_row_neon_fog_p1(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_fog_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 1); }
static inline void cm_row_neon_fog(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    if (!sPsRows) cm_row_neon_fog_r(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else if (T->ps) cm_row_neon_fog_p1(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else cm_row_neon_fog_p0(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
}
/* The blended sprite rows with the span skip (round 18): only raster_tri's skip triangles come
 * here (bilinear, blended, the alpha from the texture), so the two row kernels above stay as
 * they were. */
#define CM_VARIANTS_SKIP(KIND, ALPHA, FOG)                                                              \
    if (rr) { if (T->t16) { CM_ROW_SKIP(ALPHA, FOG, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }        \
              else        { CM_ROW_SKIP(ALPHA, FOG, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }      \
    else    { if (T->t16) { CM_ROW_SKIP(ALPHA, FOG, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }          \
              else        { CM_ROW_SKIP(ALPHA, FOG, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
static inline __attribute__((always_inline)) void
cm_row_neon_skip_b(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                 float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw, const int PSM) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const bool ps = PSM == 2 ? T->ps : PSM; (void)ps;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin;
    const bool persp = T->persp, dmask = T->dmask, bf = T->bf;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    const AlphaRows *const arows = T->arows;
    (void)th; (void)px32;
    CM_VARIANTS_SKIP(CM_BILIN, 1, 0)
}
static void __attribute__((noinline)) cm_row_neon_skip_r(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                 float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_skip_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 2); }
static void __attribute__((noinline)) cm_row_neon_skip_p0(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                 float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_skip_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0); }
static void __attribute__((noinline)) cm_row_neon_skip_p1(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                 float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) { cm_row_neon_skip_b(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw, 1); }
static inline void cm_row_neon_skip(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                 float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    if (!sPsRows) cm_row_neon_skip_r(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else if (T->ps) cm_row_neon_skip_p1(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    else cm_row_neon_skip_p0(T, fb, zp, n, zf, cr, cg, cb, ca, uw, vw, sw, rw);
}
#undef CM_VARIANTS_SKIP
#undef CM_ROW_SKIP
#undef CM_VARIANTS
#undef CM_PIXELS
#undef CM_FOG_0
#undef CM_FOG_1
#undef CM_FA_0
#undef CM_FA_1
#undef CM_BILIN
#undef CM_NEAR
#undef CM_TAP32
#undef CM_TAP16
#undef CM_WRAP_RRU
#undef CM_WRAP_RRV
#undef CM_WRAP_GU
#undef CM_WRAP_GV
#undef CM_ROW
/* DKR_FASTROWS (2026-10-07, the cross-port speed round; super-mario-64's SM64_FASTROWS): raster_tri's row loop, for
 * the triangles whose rows all go to a NEON row (cm_row_lean*, cm_row_neon*, the deferred sky's held rows), in a small
 * function of its own (tri_rows): raster_tri keeps ~45 values live across its row loop and GCC reloaded or spilled
 * most of them every row (the census: 4,000-8,000 such rows a frame on the bench levels). The same bases, edge solve,
 * owned-edge ends and snap, float for float; the edge kinds (w_i_dx > 0, < 0, == 0) once a triangle; the snap's three
 * tests in NEON lanes; the row's start values (shade, depth, u/w, v/w, 1/w) as NEON lanes, multiply then add, each
 * rounded as the VFP rounds them (vmla is not fused on ARMv7 NEON; denormals as DKR_SPANVEC's note). They go to the
 * DKR_SPANVEC rows as they are. DKR_FASTROWS=0: raster_tri's own loop. Same pixels. */
enum { TR_LEAN, TR_FOG, TR_SKIP, TR_NEON, TR_LEANNOZ, TR_SKY, TR_NOZSKIP, TR_NOZ };
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's { b, g, r, a } and { z, u/w, v/w, 1/w } */
    float32x4_t fa;                           /* flat: the shade { b0, g0, r0, a0 } in place of the sums */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    int minx, maxx, sw, persp, tgt, skytri;
    int ek[3];                                /* each edge's w_i_dx: 0 > 0, 1 < 0, 2 == 0 */
    const OwnTri *own;                        /* DKR_FILLRULE's edges, or NULL */
    uint32_t *fbuf; uint16_t *zb;
    const CmTri *cmt;
} TriRows;
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
/* Rows y..maxy-1 of the triangle, each exactly as raster_tri's loop draws it; returns the span pixels */
static inline __attribute__((always_inline)) uint32_t tri_rows_t(const TriRows *__restrict T, int y, const int maxy, const int FLAT TR_STEP_PARAM) {
    const float e0c = T->e0c, e0x = T->e0x, e0y = T->e0y, e1c = T->e1c, e1x = T->e1x, e1y = T->e1y;
    const float e2c = T->e2c, e2x = T->e2x, e2y = T->e2y, inv_area = T->inv_area;
    const float w0dx = T->w0dx, w1dx = T->w1dx, w2dx = T->w2dx;
    const int minx = T->minx, maxx = T->maxx, ek0 = T->ek[0], ek1 = T->ek[1], ek2 = T->ek[2];
    const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };
    const CmTri *const cmt = T->cmt;
    const int tgt = T->tgt;
    uint32_t px = 0;
    for (; y < maxy; TR_YNEXT) {
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
        /* raster_tri's solve, its ds tests made once a triangle (a row outside an edge with ds == 0: nothing) */
#define TRI_EDGE(EK, BS, DS)                                                                            \
        if (EK == 0) {                                                                                  \
            if ((BS) < 0.0f) {                                                                          \
                float q = -(BS) / (DS) - 1e-5f; if (q > 1e6f) q = 1e6f;                                 \
                const int a = minx + iceilf(q);                                                         \
                if (a > xs) xs = a;                                                                     \
            }                                                                                           \
        } else if (EK == 1) {                                                                           \
            float q = -(BS) / (DS) + 1e-5f; if (q > 1e6f) q = 1e6f;                                     \
            const int a = minx + 1 + ifloorf(q);                                                        \
            if (a < xe) xe = a;                                                                         \
        } else if ((BS) < 0.0f) continue;
        TRI_EDGE(ek0, b0, w0dx)
        TRI_EDGE(ek1, b1, w1dx)
        TRI_EDGE(ek2, b2, w2dx)
#undef TRI_EDGE
        if (xs >= xe) continue;
        const float32x4_t B3 = { b0, b1, b2, 0.0f };
        float32x4_t W;
        if (T->own) { /* DKR_FILLRULE: the row's ends by the owned-edge test, one pixel either side */
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;
            while (xs < xe && !own_px(T->own, xs, py)) xs++;
            while (xe > xs && !own_px(T->own, xe - 1, py)) xe--;
            if (xs >= xe) continue;
            W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
        } else {
            /* the snap: b_i + (xs - minx) * w_i_dx >= 0 for all three, in three lanes */
            for (;;) {
                W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
                const uint32x4_t ge = vcgeq_f32(W, vdupq_n_f32(0.0f));
                const uint32x2_t g2 = vpmin_u32(vget_low_u32(ge), vget_high_u32(ge));
                if (vget_lane_u32(vpmin_u32(g2, g2), 0)) break;
                if (++xs >= xe) break;
            }
            if (xs >= xe) continue;
        }
        const int n = xe - xs;
        px += (uint32_t)n;
        /* w0*z0 + w1*z1 + w2*z2 for all eight (the shade: the vertex colour when flat) */
        float32x4_t A, B = vmulq_lane_f32(T->vb0, vget_low_f32(W), 0);
        B = vmlaq_lane_f32(B, T->vb1, vget_low_f32(W), 1);
        B = vmlaq_lane_f32(B, T->vb2, vget_high_f32(W), 0);
        if (FLAT) A = T->fa;
        else {
            A = vmulq_lane_f32(T->va0, vget_low_f32(W), 0);
            A = vmlaq_lane_f32(A, T->va1, vget_low_f32(W), 1);
            A = vmlaq_lane_f32(A, T->va2, vget_high_f32(W), 0);
        }
        const float sw = vgetq_lane_f32(B, 3);
        float rw = 0.0f;
        if (T->persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
        const int idx = y * T->sw + xs;
        uint32_t *const fb = T->fbuf + idx;
        if (tgt == TR_LEAN && sSpanVec) { cm_row_lean_v(cmt, fb, T->zb + idx, n, A, B, rw); continue; }
        if (tgt == TR_LEANNOZ && sSpanVec) { cm_row_lean_noz_v(cmt, fb, n, A, B, rw, 0); continue; }
        const float zf = vgetq_lane_f32(B, 0), uw = vgetq_lane_f32(B, 1), vw = vgetq_lane_f32(B, 2);
        const float cb = vgetq_lane_f32(A, 0), cg = vgetq_lane_f32(A, 1), cr = vgetq_lane_f32(A, 2), ca = vgetq_lane_f32(A, 3);
        switch (tgt) {
        case TR_LEAN: cm_row_lean(cmt, fb, T->zb + idx, n, zf, cr, cg, cb, ca, uw, vw, sw, rw); break;
        case TR_FOG: cm_row_neon_fog(cmt, fb, T->zb + idx, n, zf, cr, cg, cb, ca, uw, vw, sw, rw); break;
        case TR_SKIP: cm_row_neon_skip(cmt, fb, T->zb + idx, n, zf, cr, cg, cb, ca, uw, vw, sw, rw); break;
        case TR_NEON: cm_row_neon(cmt, fb, T->zb + idx, n, zf, cr, cg, cb, ca, uw, vw, sw, rw); break;
        case TR_LEANNOZ: cm_row_lean_noz(cmt, fb, n, cr, cg, cb, ca, uw, vw, sw, rw, 0); break;
        case TR_SKY: {
            SkyRow *const r = &sSkyRow[sSkyNR];
            r->off = (uint32_t)idx; r->n = n; r->tri = T->skytri; r->next = sSkyHead[y]; sSkyHead[y] = sSkyNR++;
            r->cr = cr; r->cg = cg; r->cb = cb; r->ca = ca; r->uw = uw; r->vw = vw; r->sw = sw; r->rw = rw;
            break; }
        case TR_NOZSKIP: cm_row_neon_noz_skip(cmt, fb, n, cr, cg, cb, ca, uw, vw, sw, rw); break;
        default: cm_row_neon_noz(cmt, fb, n, cr, cg, cb, ca, uw, vw, sw, rw); break;
        }
    }
    return px;
}
static uint32_t __attribute__((noinline)) tri_rows_f(const TriRows *T, int y, int maxy TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, 1 TR_STEP_ARG(ystep)); }
static uint32_t __attribute__((noinline)) tri_rows_s(const TriRows *T, int y, int maxy TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, 0 TR_STEP_ARG(ystep)); }
#if defined(MIYOO)
/* The Mini's second core (miyoo_worker.c, the shared miyoo/kit KM2): a 3D triangle whose rows all go to
 * the NEON rows (DKR_FASTROWS' tri_rows) and that has enough rows is drawn by both cores at once, this one
 * its even rows from miny, the worker the odd ones. Exact: tri_rows computes each row from the triangle's
 * constants and its own y alone (the bases, the edge solve and the row's start values are recomputed per
 * row; nothing steps from one row to the next), so a row comes out the same whichever core draws it; the
 * shares write disjoint rows of colour and depth; and the triangle is finished (miyoo_worker_wait) before
 * the next one starts. All of the setup is done first, by this core, and the worker only reads it (the
 * TriRows and the owned edges); what the rows write besides pixels is per share: the worker has its own
 * copy of the CmTri (DKR_ZSKIP's run of misses, carried row to row, only decides which spans are tested,
 * and a tested span is skipped only when it draws nothing), its own span skip counts (SK_SPANS) and its
 * own pixel count, both added up after the wait. Not split: the deferred sky (TR_SKY), whose rows are
 * queued in order on shared lists instead of drawn. DKR_THREADS=1 (or threads=1 in dkr_display.cfg): one
 * core, as before (gfx_sdl12.c starts the worker). DKR_SPLIT=<px>: the smallest triangle split, in pixels
 * of area (default TR_SPLIT_PX). */
#define TR_SPLIT_PX   64 /* below this, the handoff costs about what it saves */
#define TR_SPLIT_ROWS 4
typedef struct { TriRows trr; CmTri cmt; int flat, y, maxy; uint32_t px; uint64_t sk_spans, sk_px; } TrJob;
volatile int gMiyooStallWorker SAVESTATE_EXCLUDE; /* DKR_STALL_TEST=worker:<iteration>[:<s>] (main.c): the next split's worker share sleeps <s> s */
static void tr_job(void *arg) {
    TrJob *const j = (TrJob *)arg;
    if (gMiyooStallWorker) { const int s = gMiyooStallWorker; gMiyooStallWorker = 0; for (int i = 0; i < s * 10; i++) usleep(100000); }
    const uint64_t s0 = tSkSpans, p0 = tSkPx;
    j->px = j->flat ? tri_rows_f(&j->trr, j->y, j->maxy, 2) : tri_rows_s(&j->trr, j->y, j->maxy, 2);
    j->sk_spans = tSkSpans - s0; j->sk_px = tSkPx - p0;
}
static int sTrMinPx SAVESTATE_EXCLUDE = -1;
static uint32_t sTrTris SAVESTATE_EXCLUDE, sTrSplits SAVESTATE_EXCLUDE; /* the PERF line's split count */
/* main.c's PERF line: this window's split triangles, the split share of the pixels drawn, the worker's jobs and sleeps */
int gfx_soft_mt_line(char *buf, int n) {
    unsigned jobs = 0, sleeps = 0;
    unsigned long long spins = 0;
    miyoo_worker_stats(&jobs, &sleeps, &spins);
    const int r = snprintf(buf, n, " | mt %s split %u/%u tris %.0f%% px, jobs %u, sleeps %u", miyoo_worker_ready() ? "on" : "off",
                           (unsigned)sTrSplits, (unsigned)sTrTris, sTrPx ? 100.0 * (double)sTrSplitPx / (double)sTrPx : 0.0, jobs, sleeps);
    sTrSplits = sTrTris = 0;
    sTrPx = sTrSplitPx = 0;
    return r;
}
#endif
#define HH_CM_NEON 1
#else
static void sky_flush(void) { }
static void sky_flush_lines(int y0, int y1) { (void)y0; (void)y1; }
static bool sSkyWin; static const int sSkyNR = 0;
#endif

/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
static void int_rows_touch(int y0, int y1); /* internal target: seed/dirty rows (see the frame section) */
static uint32_t *sIntFB; /* defined with the targets below */
/* HUD triangles -- textured triangles drawn in the 2D phase: the race HUD's ortho quads,
 * the menus' wood panels -- for the cross-port A/B (the lead, 2026-09-24; DKR_HUDTRI):
 * 1 = smoothed like the 3D (today's rule, MK64's; the default), 0 = never bilinear,
 * 2 = honour the game's G_TF_POINT. DKR_HUDTRI_CENSUS=1 logs each frame's count. */
static int sHudTri SAVESTATE_EXCLUDE = -1, sHudCensus SAVESTATE_EXCLUDE;
static uint32_t sHudTris SAVESTATE_EXCLUDE, sHudPoint SAVESTATE_EXCLUDE, sHudBilin SAVESTATE_EXCLUDE, sHudExact SAVESTATE_EXCLUDE;
static bool soft_phase3d(void); /* sPhase3D is defined with the targets */
/* A textured draw whose texture isn't there comes out as a flat block of the combiner's colour (OoT's
 * v35 sunset bar): said on the console a few times a session, so a device run catches it;
 * DKR_UNTEX_CENSUS=1 prints the count at exit. */
static uint64_t sUntexDraws SAVESTATE_EXCLUDE;
static void untex_census(void) { extern void port_log_always(const char *fmt, ...); port_log_always("UNTEX %llu untextured draws\n", (unsigned long long)sUntexDraws); }
static void untextured_draw(void) {
    static int sLogs SAVESTATE_EXCLUDE, sInit SAVESTATE_EXCLUDE;
    if (!sInit) { sInit = 1; const char *e = getenv("DKR_UNTEX_CENSUS"); if (e && e[0] == '1') atexit(untex_census); }
    sUntexDraws++;
    if (sLogs < 4) {
        sLogs++;
        port_log("gfx: untextured draw: tex %u not valid (bound %u/%u, arena %d%%)\n", (unsigned)sBoundTex[0],
                 (unsigned)sBoundTex[0], (unsigned)sBoundTex[1], (int)(sArenaUsed * 100 / TEXARENA_SZ));
    }
}
static int sPShadeSw SAVESTATE_EXCLUDE = -1;
static unsigned sPShadeFrame SAVESTATE_EXCLUDE;
void gfx_soft_set_pshade(int on) { sPShadeSw = on ? 1 : 0; } /* the Bench's windows */
static bool pshade_frame(void) {
    if (__builtin_expect(sPShadeSw < 0, 0)) {
        const char *e = getenv("DKR_PSHADE");
        sPShadeSw = !e || !e[0] ? 1 : e[0] == 'a' ? 2 : e[0] != '0';
    }
    return sPShadeSw == 1 || (sPShadeSw == 2 && ((sPShadeFrame / 150) & 1));
}
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
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
    if (sFB == sIntFB) int_rows_touch(miny, maxy);
    /* Barycentrics = edge/area with SIGNED area: inside is all-w >= 0 for
     * either winding, no separate sign handling needed. (Divide sits after
     * the bbox reject so off-screen triangles don't pay for it.) */
    const float inv_area = 1.0f / area;

    const SoftTex *tex = (sCurShader && sCurShader->cc.used_textures[0]) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { untextured_draw(); tex = NULL; }
    const int tmask_u = tex ? pow2mask(tex->w) : -1, tmask_v = tex ? pow2mask(tex->h) : -1;
    const SoftTex *const tex1 = (__builtin_expect(sTex2, 0) && tex && sTex2Id && sTex2Id < MAX_TEXTURES && sTex[sTex2Id].valid &&
                                 sTex[sTex2Id].w == tex->w && sTex[sTex2Id].h == tex->h) ? &sTex[sTex2Id] : NULL;
    const bool tex_edge = sUseAlpha && sCurShader && sCurShader->cc.opt_texture_edge;
    const bool ztest = use_z && sDepthTest;
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
    if (tex && !soft_phase3d()) {
        if (sHudTri < 0) {
            const char *e = getenv("DKR_HUDTRI");
            sHudTri = (e && (e[0] == '0' || e[0] == '2')) ? e[0] - '0' : 1;
            e = getenv("DKR_HUDTRI_CENSUS");
            sHudCensus = e && e[0] == '1';
        }
        if (use_bilin && (sHudTri == 0 || (sHudTri == 2 && tex->point))) use_bilin = false;
        /* at exactly 1:1 (the unsquished race HUD at 240): one texel a pixel, so the texel itself -- the
         * N64 samples its fonts and digits G_TF_POINT, and bilinear, half a texel off, spreads every
         * 1-texel stroke over two pixels at half strength (the matrix's X27, SM64 eb0a164; gfx_pc places
         * these on whole pixels). Scaled or squeezed HUD quads keep their smoothing. DKR_HUDEXACT=0: off. */
        if (use_bilin) {
            extern int gfx_hud_exact_on(void);
            const float a2 = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
            if (a2 != 0.0f && gfx_hud_exact_on()) {
                const float dudx = ((v1->u - v0->u) * (v2->y - v0->y) - (v2->u - v0->u) * (v1->y - v0->y)) / a2;
                const float dvdx = ((v1->v - v0->v) * (v2->y - v0->y) - (v2->v - v0->v) * (v1->y - v0->y)) / a2;
                const float su = fabsf(dudx * (float) tex->w), sv = fabsf(dvdx * (float) tex->h);
                if ((fabsf(su - 1.0f) <= 0.02f && sv < 1.0f / 64.0f) || (fabsf(sv - 1.0f) <= 0.02f && su < 1.0f / 64.0f)) {
                    use_bilin = false;
                    if (sHudCensus) sHudExact++;
                }
            }
        }
        if (sHudCensus) { sHudTris++; sHudPoint += tex->point; sHudBilin += use_bilin; }
    }
    const int amin = tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
    const int ablend = !(tex_edge && sEdgeOpaque); /* 0: the texel's own colour past amin (gfx_soft_set_edge_opaque) */
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
    const float iw0 = v0->invw, iw1 = v1->invw, iw2 = v2->invw;

    const bool persp = tex && iw0 > 0.0f && iw1 > 0.0f && iw2 > 0.0f;
    /* DKR_PSHADE (2026-10-08, the GoldenEye port GE_PSHADE; the user: the ground's lighting shifting with the camera): the
     * RDP steps the vertex shade linearly on screen, so on a big floor triangle reaching into the distance the baked
     * lighting is stretched toward the far corner and slides as the view turns and the clipper recuts it. A textured,
     * depth-tested, perspective, smooth-shaded triangle carries colour/w (the vertex colours times 1/w) and each 16-px
     * span takes its colours exact at both ends, linear between, as u and v. The no-depth draws (the sky) keep the
     * RDP's rule. DKR_PSHADE=0: the RDP's screen-linear shade (before); ab: alternates every 150 frames. */
    const bool ps = persp && ztest && !flat && pshade_frame();
    const float pm0 = ps ? iw0 : 1.0f, pm1 = ps ? iw1 : 1.0f, pm2 = ps ? iw2 : 1.0f;
    const float r0 = ps ? v0->r * pm0 : v0->r, r1 = ps ? v1->r * pm1 : v1->r, r2 = ps ? v2->r * pm2 : v2->r;
    const float g0 = ps ? v0->g * pm0 : v0->g, g1 = ps ? v1->g * pm1 : v1->g, g2 = ps ? v2->g * pm2 : v2->g;
    const float bl0 = ps ? v0->b * pm0 : v0->b, bl1 = ps ? v1->b * pm1 : v1->b, bl2 = ps ? v2->b * pm2 : v2->b;
    const float a0 = ps ? v0->a * pm0 : v0->a, a1 = ps ? v1->a * pm1 : v1->a, a2 = ps ? v2->a * pm2 : v2->a;
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
    const int32_t rdq_t = rdq, gdq_t = gdq, bdq_t = bdq, adq_t = adq; /* the scalar span loop's (DKR_PSHADE replaces them per span) */
    /* Flat triangles: the shade colour the pixel loop would clamp every pixel
     * (rq = (int32_t)(cr * 65536.0f) with cr == r0 on every span, since every
     * colour step is 0), clamped once. (MK64 perf round 24 via the OoT port
     * d267ca3; bit-exact.) */
    const int fsr = clamp8_sh16((int32_t)(r0 * 65536.0f)), fsg = clamp8_sh16((int32_t)(g0 * 65536.0f));
    const int fsb = clamp8_sh16((int32_t)(bl0 * 65536.0f)), fsa = clamp8_sh16((int32_t)(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const int zbias = sZDecal ? 4 : 0; const bool dmask = sDepthMask, texmix = sTexAdd || rgb_decal || sTexLerp; /* the rare combines share one test */
    const bool texlerp = sTexLerp; const int lkr = sLerpR, lkg = sLerpG, lkb = sLerpB;
    /* Fogged per pixel (CM=2): a triangle with some fog, or a constant alpha (the lead's
     * correction via Star Fox: a blended constant-alpha draw near the camera, all factors 0,
     * vanished on the plain loops, whose alpha is the vertex's). */
    const bool fogtri = __builtin_expect(sFogPx, 0) && ((v0->a | v1->a | v2->a) != 0 || sFogA >= 0);
    const bool cm_mod = rgb_tex && rgb_in && !texmix && !rgb_texa && !fogtri && !tex1; /* texture x shade: see SPAN_LOOP_CM */
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
    /* the sprite span skip (span_clear): blended, the alpha from the texture, bilinear, and no
     * constant fog alpha in its place (CM=2's fga); not tried under 24 px wide, where a span's
     * check costs about what it would save */
    const AlphaRows *const arows = (tex && sUseAlpha && a_tex && use_bilin && !(fogtri && sFogA >= 0) && !tex1 && maxx - minx >= 24) ? alpha_rows(sBoundTex[0], tex) : NULL;
#if defined(HH_CM_NEON)
    /* The NEON rows (cm_row_neon): z-tested texture x shade, one call a row. */
    /* fogged texture x shade (the fogged scenery) rides the NEON rows too, with the fog step */
    const bool cm_fog = fogtri && rgb_tex && rgb_in && !texmix && !rgb_texa && !tex1;
    const bool cm_rows = sCmNeon && tex && ztest && (cm_mod || cm_fog);
    if (__builtin_expect(sNozNeon < 0, 0)) { const char *e = getenv("DKR_NOZNEON"); sNozNeon = !(e && e[0] == '0'); }
    const bool cm_noz = sCmNeon && sNozNeon && tex && !ztest && cm_mod; /* the sky (cm_row_neon_noz), no-z sprites (_skip) */
    /* the fogged no-z rows (the scalar CM=2 loops before; the lean rows only, DKR_LEANF): a blended one only on a
     * texture repeating on a power of two */
    const bool rrtex = tex && ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0;
    const bool cm_nozf = sCmNeon && sLeanRows && sLeanF && tex && !ztest && cm_fog && use_bilin && !t16 && (!sUseAlpha || rrtex);
    CmTri cmt;
    if (cm_rows || cm_noz || cm_nozf) {
        const int32_t d[4] = { bdq, gdq, rdq, adq };
        cmt.px = texpx; cmt.w = tw; cmt.h = th; cmt.ws = ws; cmt.wt = wt; cmt.mu = tmask_u; cmt.mv = tmask_v; cmt.t16 = t16;
        cmt.swdx = swdx; cmt.uwdx = uwdx; cmt.vwdx = vwdx; cmt.zdx = zdx;
        cmt.crdx = crdx; cmt.cgdx = cgdx; cmt.cbdx = cbdx; cmt.cadx = cadx;
        cmt.ftw = (float)tw; cmt.fth = (float)th;
        cmt.dacc = vld1q_s32(d); cmt.zdq = zdq; cmt.zbias = zbias;
        cmt.amode = !a_tex ? 2 : !a_in ? 1 : 0; /* fa = !a_tex ? sa : !a_in ? ta : div255(ta*sa) */
        if (cm_fog && sFogA >= 0 && a_tex) cmt.amode = 3; /* fa = div255(ta*fga): a fogged texel x constant alpha */
        cmt.amin = amin; cmt.persp = persp; cmt.dmask = dmask; cmt.alpha = sUseAlpha; cmt.bilin = use_bilin;
        cmt.fog = cm_fog; cmt.fga = sFogA;
        cmt.fogc = vcreate_u16((uint64_t)(uint16_t)sFogB | (uint64_t)(uint16_t)sFogG << 16 | (uint64_t)(uint16_t)sFogR << 32);
        cmt.arows = arows;
        if (__builtin_expect(sBlendBF < 0, 0)) { const char *e = getenv("DKR_BLENDBF"); sBlendBF = !(e && e[0] == '0'); }
        cmt.bf = sBlendBF && sUseAlpha && !(ztest && dmask) && ablend; /* CM_BLEND_BF: no z write */
        cmt.ablend = ablend;
        cmt.da = (float32x4_t){ cbdx, cgdx, crdx, cadx }; cmt.db = (float32x4_t){ zdx, uwdx, vwdx, swdx };
        cmt.ps = ps;
        cmt.zsk = sZSkip ? 4 : 0;
        /* the lean rows (cm_row_lean): bilinear; z-tested opaque (fog or not) or alpha without fog; no-z opaque */
        cmt.lean = sLeanRows && use_bilin && !t16 &&
                   (cm_rows ? (!sUseAlpha || sLeanA) : cm_noz ? (sLeanS && (!sUseAlpha || (sLeanA && rrtex))) : true);
    }
    /* the deferred sky (sky_flush): opaque, and nothing else drawn yet this frame */
    const bool sky_defer = cm_noz && !sUseAlpha && sSkyWin && fbuf == sNatFB && sSkyNT < SKY_TRIS && sSkyNR + (maxy - miny) <= SKY_ROWS && maxy <= SKY_LINES;
    if (sky_defer) {
        sSkyTri[sSkyNT] = cmt; sSkyFB = fbuf;
        if (miny < sSkyY0) sSkyY0 = miny;
        if (maxy > sSkyY1) sSkyY1 = maxy;
    } else sSkyWin = false;
    if (sUseAlpha && sSkyNR) sky_flush_lines(miny, maxy); /* a blend reads what is under it */
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
    OwnTri oe; /* DKR_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    if (own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&oe.e[0], v1, v2, s); own_edge(&oe.e[1], v2, v0, s); own_edge(&oe.e[2], v0, v1, s);
    }

#if defined(HH_CM_NEON)
    if (sFastRows && (cm_rows || cm_nozf || cm_noz)) { /* DKR_FASTROWS: every row in tri_rows */
        TriRows trr;
        trr.va0 = (float32x4_t){ bl0, g0, r0, a0 }; trr.vb0 = (float32x4_t){ z0, uw0, vw0, iw0 };
        trr.va1 = (float32x4_t){ bl1, g1, r1, a1 }; trr.vb1 = (float32x4_t){ z1, uw1, vw1, iw1 };
        trr.va2 = (float32x4_t){ bl2, g2, r2, a2 }; trr.vb2 = (float32x4_t){ z2, uw2, vw2, iw2 };
        trr.fa = (float32x4_t){ bl0, g0, r0, a0 };
        trr.e0c = e0c; trr.e0x = e0x; trr.e0y = e0y; trr.e1c = e1c; trr.e1x = e1x; trr.e1y = e1y;
        trr.e2c = e2c; trr.e2x = e2x; trr.e2y = e2y; trr.inv_area = inv_area;
        trr.w0dx = w0dx; trr.w1dx = w1dx; trr.w2dx = w2dx;
        trr.minx = minx; trr.maxx = maxx; trr.sw = sW; trr.persp = persp;
        trr.ek[0] = w0dx > 0.0f ? 0 : w0dx < 0.0f ? 1 : 2;
        trr.ek[1] = w1dx > 0.0f ? 0 : w1dx < 0.0f ? 1 : 2;
        trr.ek[2] = w2dx > 0.0f ? 0 : w2dx < 0.0f ? 1 : 2;
        trr.own = own ? &oe : NULL; trr.fbuf = fbuf; trr.zb = zb; trr.cmt = &cmt; trr.skytri = sSkyNT;
        trr.tgt = cm_rows ? (cmt.lean ? TR_LEAN : cmt.fog ? TR_FOG : arows ? TR_SKIP : TR_NEON)
                : cm_nozf ? TR_LEANNOZ
                : sky_defer ? TR_SKY : cmt.lean ? TR_LEANNOZ : arows ? TR_NOZSKIP : TR_NOZ;
#if defined(MIYOO)
        if (sTrMinPx < 0) { const char *e = getenv("DKR_SPLIT"); sTrMinPx = e && e[0] ? atoi(e) : TR_SPLIT_PX; }
        if (miyoo_worker_ready() && trr.tgt != TR_SKY && maxy - miny >= TR_SPLIT_ROWS &&
            (area < 0.0f ? -area : area) >= 2.0f * (float)sTrMinPx) {
            TrJob job; /* the worker's share: the odd rows, with its own CmTri (see TrJob) */
            job.trr = trr; job.cmt = cmt; job.trr.cmt = &job.cmt;
            job.flat = flat; job.y = miny + 1; job.maxy = maxy; job.px = 0; job.sk_spans = job.sk_px = 0;
            miyoo_worker_run(tr_job, &job);
            tri_px = flat ? tri_rows_f(&trr, miny, maxy, 2) : tri_rows_s(&trr, miny, maxy, 2);
            gMiyooWdGame = WD_WORKER_WAIT;
            miyoo_worker_wait();
            gMiyooWdGame = WD_RUN;
            tri_px += job.px; tSkSpans += job.sk_spans; tSkPx += job.sk_px;
            sTrSplits++; sTrSplitPx += tri_px;
        } else
#endif
        tri_px = flat ? tri_rows_f(&trr, miny, maxy TR_STEP_ARG(1)) : tri_rows_s(&trr, miny, maxy TR_STEP_ARG(1));
#if defined(MIYOO)
        sTrTris++;
        if (trr.tgt != TR_SKY) sTrPx += tri_px; /* the held sky's pixels count when they are drawn (sky_flush_lines) */
#endif
        if (sky_defer) sSkyNT++;
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
        if (own) { /* DKR_FILLRULE: the row's ends by the owned-edge test, one pixel either side */
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
            if (cmt.lean) cm_lean_row(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else if (__builtin_expect(cmt.fog, 0)) cm_row_neon_fog(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else if (arows) cm_row_neon_skip(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else cm_row_neon(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            continue;
        }
        if (cm_nozf) { cm_lean_row_noz(&cmt, fbuf + idx, xe - xs, cr, cg, cb, ca, uw, vw, sw, rw, 0); continue; }
        if (cm_noz) {
            if (sky_defer) {
                SkyRow *const r = &sSkyRow[sSkyNR];
                r->off = (uint32_t)idx; r->n = xe - xs; r->tri = sSkyNT; r->next = sSkyHead[y]; sSkyHead[y] = sSkyNR++;
                r->cr = cr; r->cg = cg; r->cb = cb; r->ca = ca; r->uw = uw; r->vw = vw; r->sw = sw; r->rw = rw;
                continue;
            }
            if (cmt.lean) cm_lean_row_noz(&cmt, fbuf + idx, xe - xs, cr, cg, cb, ca, uw, vw, sw, rw, 0);
            else if (arows) cm_row_neon_noz_skip(&cmt, fbuf + idx, xe - xs, cr, cg, cb, ca, uw, vw, sw, rw);
            else cm_row_neon_noz(&cmt, fbuf + idx, xe - xs, cr, cg, cb, ca, uw, vw, sw, rw);
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
            int32_t zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            int32_t rq, gq, bq, aq, rdq = rdq_t, gdq = gdq_t, bdq = bdq_t, adq = adq_t;
            if (ps) { /* DKR_PSHADE: the span's colours exact at both ends (the NEON rows' ps_dacc, op for op) */
                const float sr = cr * rw, sg = cg * rw, sb = cb * rw, sa = ca * rw;
                const float er = (cr + crdx * fspan) * rw_b, eg = (cg + cgdx * fspan) * rw_b;
                const float eb = (cb + cbdx * fspan) * rw_b, ea = (ca + cadx * fspan) * rw_b;
                float dr = er - sr, dg = eg - sg, db = eb - sb, da = ea - sa;
                if (span == PERSP_SPAN) { dr *= 1.0f / PERSP_SPAN; dg *= 1.0f / PERSP_SPAN; db *= 1.0f / PERSP_SPAN; da *= 1.0f / PERSP_SPAN; }
                else { dr /= fspan; dg /= fspan; db /= fspan; da /= fspan; }
                rq = (int32_t)(sr * 65536.0f); gq = (int32_t)(sg * 65536.0f); bq = (int32_t)(sb * 65536.0f); aq = (int32_t)(sa * 65536.0f);
                rdq = sat_i32(dr * 65536.0f); gdq = sat_i32(dg * 65536.0f); bdq = sat_i32(db * 65536.0f); adq = sat_i32(da * 65536.0f);
            } else {
                rq = (int32_t)(cr * 65536.0f); gq = (int32_t)(cg * 65536.0f); bq = (int32_t)(cb * 65536.0f); aq = (int32_t)(ca * 65536.0f);
            }

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once per
 * triangle above) -- the colour accumulators and their steps drop out of the
 * pixel loop; same values, since flat => every step is 0.
 * CM=1: the combiner is plain texture x shade (cm_mod), so the per-pixel walk
 * of the combine flags drops out. (MK64 perf round 24, bit-exact.) */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
/* CM=2 (fogged, round 17) alone names the fog constants, so they live in its branch only and the
 * other loops compile as before: the pixel alpha (the constant the factor displaced, if any) and
 * the fog step, lerp(colour, fog, shade alpha). */
#define FA_TEX_0 (!a_tex ? sa : (!a_in ? ta : div255(ta*sa)))
#define FA_TEX_1 FA_TEX_0
#define FA_TEX_2 (fga >= 0 ? (a_tex && ta < 255 ? div255(ta*fga) : fga) : FA_TEX_0) /* a_tex: texel x constant (DKR_FOGTEXA) */
#define FA_SH_0 sa
#define FA_SH_1 sa
#define FA_SH_2 (fga >= 0 ? fga : sa)
#define FOGSTEP_0
#define FOGSTEP_1
#define FOGSTEP_2 { fr=div255(fr*(255-sa) + fgr*sa); fg=div255(fg*(255-sa) + fgg*sa); fb=div255(fb*(255-sa) + fgb*sa); }
#define SPAN_LOOP(TEX, ZTEST, ALPHA, BILIN) SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, 0, 0)
#define SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, CM)                                               \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq COLSTEP_##FLAT) {     \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias) continue; }                                      \
                const int sr = FLAT ? fsr : clamp8_sh16(rq), sg = FLAT ? fsg : clamp8_sh16(gq);         \
                const int sb = FLAT ? fsb : clamp8_sh16(bq), sa = FLAT ? fsa : clamp8_sh16(aq);         \
                int fr, fg, fb, fa;                                                                     \
                if (TEX) {                                                                              \
                    FETCH_##BILIN                                                                       \
                    const int ta = (t>>24)&0xff;                                                        \
                    const int tr = (CM != 1 && rgb_texa) ? ta : (t>>16)&0xff, tg = (CM != 1 && rgb_texa) ? ta : (t>>8)&0xff, tb = (CM != 1 && rgb_texa) ? ta : t&0xff; \
                    if (CM == 1)        { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (texmix) {                                                          \
                        if (rgb_decal)  { fr=div255(tr*ta + sr*(255-ta)); fg=div255(tg*ta + sg*(255-ta)); \
                                          fb=div255(tb*ta + sb*(255-ta)); }                             \
                        else if (texlerp) { fr=lkr+((sr-lkr)*tr)/255; fg=lkg+((sg-lkg)*tg)/255;         \
                                          fb=lkb+((sb-lkb)*tb)/255; }                                   \
                        else            { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    }                                                                                   \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = FA_TEX_##CM;                                                                   \
                } else { fr=sr; fg=sg; fb=sb; fa=FA_SH_##CM; }                                          \
                FOGSTEP_##CM                                                                            \
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
            if (__builtin_expect(tex1 != NULL, 0)) {
                /* two textures lerped by the vertex colour, times sTex2R/G/B (gfx_update_tex2): the
                 * trophy race flag. Rare, so one plain loop; the alpha as SPAN_LOOP's FA_TEX_0. */
                const void *const t1px = tex1->px; const bool t116 = tex1->is16;
                const int ws1 = tex1->wrap_s, wt1 = tex1->wrap_t, mr = sTex2R, mg = sTex2G, mb = sTex2B;
                for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq, rq += rdq, gq += gdq, bq += bdq, aq += adq) {
                    const int uz = clamp16_sh8(zq);
                    if (ztest && uz > zb[idx] + zbias) continue;
                    const int sr = clamp8_sh16(rq), sg = clamp8_sh16(gq), sb = clamp8_sh16(bq), sa = clamp8_sh16(aq);
                    uint32_t t, u;
                    if (use_bilin) {
                        t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
                        u = bilerp(t1px, t116, tw, th, ws1, wt1, uf, vf, tmask_u, tmask_v);
                    } else {
                        const int iu = wrapf(uf >> 16, tw, ws, tmask_u), iv = wrapf(vf >> 16, th, wt, tmask_v);
                        const int ju = wrapf(uf >> 16, tw, ws1, tmask_u), jv = wrapf(vf >> 16, th, wt1, tmask_v);
                        t = texel_at(texpx, t16, iv * tw + iu);
                        u = texel_at(t1px, t116, jv * tw + ju);
                    }
                    const int ta = (t >> 24) & 0xff, tr0 = (t >> 16) & 0xff, tg0 = (t >> 8) & 0xff, tb0 = t & 0xff;
                    const int tr = tr0 + ((((int)(u >> 16) & 0xff) - tr0) * sr) / 255;
                    const int tg = tg0 + ((((int)(u >> 8) & 0xff) - tg0) * sg) / 255;
                    const int tb = tb0 + ((((int)u & 0xff) - tb0) * sb) / 255;
                    int fr = div255(tr * mr), fg = div255(tg * mg), fb = div255(tb * mb);
                    const int fa = !a_tex ? sa : (!a_in ? ta : div255(ta * sa));
                    if (sUseAlpha) {
                        if (fa < amin) continue;
                        if (fa < 255 && ablend) {
                            const uint32_t dd = fbuf[idx];
                            const int dr = (dd >> 16) & 0xff, dg = (dd >> 8) & 0xff, db = dd & 0xff;
                            fr = div255(fr * fa + dr * (255 - fa));
                            fg = div255(fg * fa + dg * (255 - fa));
                            fb = div255(fb * fa + db * (255 - fa));
                        }
                    }
                    fbuf[idx] = argb((uint8_t)fr, (uint8_t)fg, (uint8_t)fb, 255);
                    if (ztest && dmask) zb[idx] = (uint16_t)uz;
                }
            } else if (arows && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {
                idx += span; /* every tap alpha 0: the span draws nothing */
                SK_SPANS++; SK_PX += (uint64_t)span;
            } else if (__builtin_expect(fogtri, 0)) {
                /* fogged per pixel (CM=2): the general combine, then the fog lerp. Texture x shade
                 * takes the NEON rows above; this is the rest (and the x86 proxy). */
                const int fgr = sFogR, fgg = sFogG, fgb = sFogB, fga = sFogA;
                if (tex) {
                    if (use_bilin) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 2) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 2) }
                        else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, BILINEAR, 0, 2) else SPAN_LOOP_CM(1, 0, 0, BILINEAR, 0, 2) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 2) }
                        else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(1, 0, 0, NEAREST, 0, 2) }
                    }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(0, 1, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(0, 1, 0, NEAREST, 0, 2) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(0, 0, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(0, 0, 0, NEAREST, 0, 2) }
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
                /* Constant-colour run: no z, no alpha and zero steps, so every
                 * pixel of the span is argb(clamp(rq), clamp(gq), clamp(bq), 255). */
                const uint32_t c = argb((uint8_t)clamp8_sh16(rq), (uint8_t)clamp8_sh16(gq), (uint8_t)clamp8_sh16(bq), 255);
                uint32_t *const row = fbuf + idx;
                for (int i = 0; i < span; i++) row[i] = c;
                idx += span;
            } else {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(0, 1, 1, NEAREST) else SPAN_LOOP(0, 1, 0, NEAREST) }
                else       { if (sUseAlpha) SPAN_LOOP(0, 0, 1, NEAREST) else SPAN_LOOP(0, 0, 0, NEAREST) }
            }
#undef SPAN_LOOP
#undef SPAN_LOOP_CM
#undef COLSTEP_0
#undef COLSTEP_1
#undef FA_TEX_0
#undef FA_TEX_1
#undef FA_TEX_2
#undef FA_SH_0
#undef FA_SH_1
#undef FA_SH_2
#undef FOGSTEP_0
#undef FOGSTEP_1
#undef FOGSTEP_2
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
#if defined(HH_CM_NEON)
    if (sky_defer) sSkyNT++;
#endif
#if defined(MIYOO)
    sTrTris++; /* raster_tri's own loop: one core (the split takes the NEON rows' triangles) */
    if (!sky_defer) sTrPx += tri_px; /* the held sky's pixels count when they are drawn (sky_flush_lines) */
#endif
    return (int)tri_px;
}

static bool sPhase3D SAVESTATE_EXCLUDE, sIntDirty SAVESTATE_EXCLUDE, sNatDirty SAVESTATE_EXCLUDE; /* see the frame section */
static bool soft_phase3d(void) { return sPhase3D; }
static bool sHad3D SAVESTATE_EXCLUDE;         /* any 3D batch this frame */
static bool sIntClean SAVESTATE_EXCLUDE;      /* internal colour is all-zero (cleared by the last composite) */
static bool sUnified SAVESTATE_EXCLUDE;       /* internal res == native: 3D draws straight to native (see frame section) */
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */
/* Lazy native clear (MK64 perf round 24 via the OoT port d267ca3): a frame that
 * opens with a full-screen opaque fill needn't clear native first. The colour
 * is cleared just before its first write or read, unless that write covers
 * the whole target. */
static bool sNatStale SAVESTATE_EXCLUDE;
static void nat_ready(void);
static void target_ready(void);

static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    PROF_BEGIN();
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    target_ready();
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
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
    int16_t u, v; /* signed, as VertexColor: a sprite starting at s = -1 is texel -1, not 65535 */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* HUD/menu sprites are laid out for the N64's 320-wide screen and land on 240
 * columns in CONVERT mode: 4 texels to 3 pixels. Nearest drops every fourth
 * texel column (strokes vanish or double: jaggy, low-res text and digits), so a
 * shrunk sprite takes the area average of exactly the texels each pixel covers -
 * every column counts, edges antialias. Alpha is premultiplied so transparent
 * texels do not darken the average. DKR_HUDFILTER=0 restores nearest (A/B).
 * (the F-Zero X port ca8ef33, the MK64 port cd479d8.) */
int gfx_soft_hud_box SAVESTATE_EXCLUDE = -1;
static inline uint32_t texel_box(const void *px, bool t16, int rowbase, int32_t uf, int32_t duf, int tw, int ws, int mu,
                                 bool premul) {
    int32_t u = uf - duf / 2;      /* uf is the pixel centre: the footprint starts half a step left */
    const int32_t end = u + duf;
    uint32_t sr = 0, sg = 0, sb = 0, sa = 0, sw = 0;
    while (u < end) {
        int32_t next = (int32_t)(((uint32_t)(u >> 16) + 1u) << 16);
        if (next > end) next = end;
        const uint32_t w = (uint32_t)(next - u) >> 8; /* 8.8 coverage */
        const uint32_t t = texel_at(px, t16, rowbase + wrapf(u >> 16, tw, ws, mu));
        const uint32_t a = t >> 24, k = premul ? a * w : 255u * w;
        sr += ((t >> 16) & 0xff) * k; sg += ((t >> 8) & 0xff) * k; sb += (t & 0xff) * k;
        sa += a * w; sw += w;
        u = next;
    }
    if (sw == 0) return 0;
    const uint32_t kd = premul ? sa : 255u * sw;
    if (kd == 0) return 0;
    return ((sa / sw) << 24) | ((sr / kd) << 16) | ((sg / kd) << 8) | (sb / kd);
}

/* A 2D sprite row at a time (MK64 708fd2c via the OoT port ab65ddd): pixel i
 * covers texels [u0 + i*duf, u0 + (i+1)*duf), u0 = uf - duf/2. Walked once
 * left to right, each texel is fetched once -- not once per pixel it touches
 * -- and the wrap is stepped instead of recomputed. Every pixel equals
 * texel_box's. */
static void box_row(uint32_t *out, int n, const void *px, bool t16, int rowbase, int32_t uf, int32_t duf, int tw, int ws,
                    int mu, bool premul) {
    int32_t u = uf - duf / 2;
    int ti = u >> 16;                     /* the texel under u, unwrapped ... */
    int wi = wrapf(ti, tw, ws, mu);       /* ... and its column in the texture */
    uint32_t t = texel_at(px, t16, rowbase + wi);
    for (int i = 0; i < n; i++) {
        const int32_t end = u + duf;
        uint32_t sr = 0, sg = 0, sb = 0, sa = 0, sw = 0;
        for (;;) {
            int32_t next = (int32_t)(((uint32_t)ti + 1u) << 16);
            if (next > end) next = end;
            const uint32_t w = (uint32_t)(next - u) >> 8;
            const uint32_t a = t >> 24, k = premul ? a * w : 255u * w;
            sr += ((t >> 16) & 0xff) * k; sg += ((t >> 8) & 0xff) * k; sb += (t & 0xff) * k;
            sa += a * w; sw += w;
            u = next;
            if (u >= end) break;
            ti++; /* into the next texel */
            wi = ws == WRAP_REPEAT ? (wi + 1 == tw ? 0 : wi + 1) : (ti <= 0 ? 0 : ti >= tw ? tw - 1 : ti);
            t = texel_at(px, t16, rowbase + wi);
        }
        const uint32_t kd = premul ? sa : 255u * sw;
        out[i] = (sw == 0 || kd == 0) ? 0 : ((sa / sw) << 24) | ((sr / kd) << 16) | ((sg / kd) << 8) | (sb / kd);
    }
}

/* Filtered rows are kept across frames (MK64 b72653e via the OoT port
 * ab65ddd): a HUD or menu sprite row is the same texels at the same scale and
 * phase frame after frame, so after its first frame a row costs one hash
 * probe. The key is everything box_row reads -- texture data pointer + its
 * content generation, row, start, step, width, wrap, formats; texman_clear
 * (the arena reset) empties the store. When full, rows not used this frame or
 * the last are dropped (then all but this frame's, then all).
 * DKR_BOXCACHE=0 walks every row (A/B: must be 0 px different). */
#define BOXC_PX   (192u * 1024u) /* filtered texels: 768 KB */
#define BOXC_ENTS 4096u
#define BOXC_HASH 8192u          /* power of two, 2x BOXC_ENTS */
typedef struct { uintptr_t px; int32_t rowbase, uf, duf; uint32_t misc, gen, off, frame; } BoxEnt;
static uint32_t *sBoxPx SAVESTATE_EXCLUDE;
static BoxEnt *sBoxEnt SAVESTATE_EXCLUDE;
static uint16_t *sBoxHash SAVESTATE_EXCLUDE;   /* 1 + index into sBoxEnt, 0 = empty */
static uint32_t sBoxUsed SAVESTATE_EXCLUDE, sBoxN SAVESTATE_EXCLUDE;
static int sBoxState SAVESTATE_EXCLUDE;        /* 0 = not set up yet, 1 = on, -1 = off (A/B or no memory) */
extern u32 gfx_soft_frames_rendered;

static void box_cache_clear(void) {
    sBoxUsed = sBoxN = 0;
    if (sBoxHash) memset(sBoxHash, 0, BOXC_HASH * sizeof(*sBoxHash));
}
static inline uint32_t box_hash(uintptr_t px, int32_t rowbase, int32_t uf, int32_t duf, uint32_t misc, uint32_t gen) {
    uint32_t h = (uint32_t)px * 0x9E3779B1u ^ (uint32_t)rowbase * 0x85EBCA77u ^ (uint32_t)uf * 0xC2B2AE3Du ^
                 (uint32_t)duf * 0x27D4EB2Fu ^ misc ^ gen * 0x165667B1u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h & (BOXC_HASH - 1);
}
static void box_cache_compact(uint32_t now, uint32_t age) {
    uint32_t used = 0, n = 0;
    for (uint32_t i = 0; i < sBoxN; i++) {
        BoxEnt b = sBoxEnt[i];
        if (now - b.frame > age) continue;
        const uint32_t len = b.misc & 1023u;
        if (b.off != used) memmove(&sBoxPx[used], &sBoxPx[b.off], len * sizeof(*sBoxPx));
        b.off = used; used += len; sBoxEnt[n++] = b;
    }
    sBoxUsed = used; sBoxN = n;
    memset(sBoxHash, 0, BOXC_HASH * sizeof(*sBoxHash));
    for (uint32_t i = 0; i < n; i++) {
        const BoxEnt *b = &sBoxEnt[i];
        uint32_t h = box_hash(b->px, b->rowbase, b->uf, b->duf, b->misc, b->gen);
        while (sBoxHash[h]) h = (h + 1) & (BOXC_HASH - 1);
        sBoxHash[h] = (uint16_t)(i + 1);
    }
}
static const uint32_t *box_row_cached(uint32_t *tmp, int n, uint32_t texid, const void *px, bool t16, int rowbase,
                                      int32_t uf, int32_t duf, int tw, int ws, int mu, bool premul) {
    if (sBoxState == 0) {
        const char *e = getenv("DKR_BOXCACHE");
        sBoxState = -1;
        if (!(e && e[0] == '0')) {
            sBoxPx = malloc(BOXC_PX * sizeof(*sBoxPx)); sBoxEnt = malloc(BOXC_ENTS * sizeof(*sBoxEnt));
            sBoxHash = calloc(BOXC_HASH, sizeof(*sBoxHash));
            if (sBoxPx && sBoxEnt && sBoxHash) sBoxState = 1;
            else { free(sBoxPx); free(sBoxEnt); free(sBoxHash); sBoxPx = NULL; sBoxEnt = NULL; sBoxHash = NULL; }
        }
    }
    if (sBoxState < 0 || n > 1023) { box_row(tmp, n, px, t16, rowbase, uf, duf, tw, ws, mu, premul); return tmp; }
    const uintptr_t key = (uintptr_t)px;
    const uint32_t gen = texid < MAX_TEXTURES ? sTexGen[texid] : 0;
    const uint32_t misc = (uint32_t)n | (uint32_t)ws << 10 | (uint32_t)premul << 12 | (uint32_t)t16 << 13 | (uint32_t)tw << 16;
    const uint32_t now = gfx_soft_frames_rendered;
    uint32_t h = box_hash(key, rowbase, uf, duf, misc, gen);
    for (uint32_t e; (e = sBoxHash[h]) != 0; h = (h + 1) & (BOXC_HASH - 1)) {
        BoxEnt *b = &sBoxEnt[e - 1];
        if (b->px == key && b->rowbase == rowbase && b->uf == uf && b->duf == duf && b->misc == misc && b->gen == gen) {
            b->frame = now;
            return &sBoxPx[b->off];
        }
    }
    if (sBoxUsed + (uint32_t)n > BOXC_PX || sBoxN >= BOXC_ENTS) {
        box_cache_compact(now, 1);
        if (sBoxUsed + (uint32_t)n > BOXC_PX || sBoxN >= BOXC_ENTS) box_cache_compact(now, 0);
        if (sBoxUsed + (uint32_t)n > BOXC_PX || sBoxN >= BOXC_ENTS) box_cache_clear();
        h = box_hash(key, rowbase, uf, duf, misc, gen);
        while (sBoxHash[h]) h = (h + 1) & (BOXC_HASH - 1);
    }
    BoxEnt *b = &sBoxEnt[sBoxN];
    b->px = key; b->rowbase = rowbase; b->uf = uf; b->duf = duf; b->misc = misc; b->gen = gen; b->off = sBoxUsed; b->frame = now;
    sBoxHash[h] = (uint16_t)++sBoxN;
    uint32_t *out = &sBoxPx[sBoxUsed];
    sBoxUsed += (uint32_t)n;
    box_row(out, n, px, t16, rowbase, uf, duf, tw, ws, mu, premul);
    return out;
}

/* Magnified 2D sprites (the matrix's X14, Smash 872636f): gfx_pc sets gfx_soft_2d_bilin from each draw's
 * texture filter; a sprite drawn bigger than its texture on either axis then takes bilerp at the pixel centre,
 * as the RDP filters texture rectangles. Shrunk sprites (most of the HUD at 240) keep the area filter, 1:1
 * ones the copy. DKR_2DBILIN=0: nearest, as before; DKR_2DBILIN_CENSUS=1 counts them (printed at exit). */
bool gfx_soft_2d_bilin SAVESTATE_EXCLUDE;
static int sBilin2DOn SAVESTATE_EXCLUDE = -1;
static bool sBilin2DCensus SAVESTATE_EXCLUDE;
static unsigned long sMag2Sprites SAVESTATE_EXCLUDE, sMag2Bilerp SAVESTATE_EXCLUDE;
static unsigned long long sMag2Px SAVESTATE_EXCLUDE;
__attribute__((destructor)) static void mag2_census_report(void) {
    if (sBilin2DCensus) fprintf(stderr, "MAG2 magnified 2D sprites %lu (asking G_TF_BILERP %lu), %llu screen px\n", sMag2Sprites, sMag2Bilerp, sMag2Px);
}
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && sCurShader->cc.used_textures[0]) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { untextured_draw(); tex = NULL; }
    const bool modulate = !sCurShader || sCurShader->cc.num_inputs > 0;
    sNatDirty = true; /* gfx_pc switches to the 2D phase before calling us */
    PROF_BEGIN();

    for (size_t s = 0; s < num_sprites; s++) {
        const Vtx2D *a = &q[s*2+0], *b = &q[s*2+1];
        {
            extern int gfx_trace_frames;
            extern void port_log(const char *fmt, ...);
            if (gfx_trace_frames > 0)
                port_log("  2d.s: (%u,%u)-(%u,%u) uv(%d,%d)-(%d,%d) col %02X%02X%02X%02X tex %u(%dx%d) ua=%d te=%d mod=%d add=%d\n",
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
        const int cr = b->r, cg = b->g, cb2 = b->b, ca = b->a; /* GU: colour of 2nd vertex */
        /* Per-sprite constants hoisted out of the pixel loop. amin folds the
         * texture-edge (0x55) and near-zero (8) alpha rejects. "plain" = the
         * texel is the final colour (no combiner input, or a white opaque
         * prim, which is what nearly every HUD sprite and menu tile uses). */
        const bool use_alpha = sUseAlpha;
        const int amin = use_alpha ? ((sCurShader && sCurShader->cc.opt_texture_edge) ? 0x55 : 8) : 0;
        const bool full = !tex && (!use_alpha || ca >= 255) && cx0 == 0 && cy0 == 0 && cx1 == sW && cy1 == sH && sFB == sNatFB;
        /* the deferred sky: the frame's opening full-screen fill (DKR's backdrop colour) is the
         * background, written with alpha 0, the mark for undrawn; nothing reads a pixel's alpha but
         * sky_flush, where unified (sSkyWin) */
        const bool bgfill = full && sSkyWin && !sSkyNR;
        if (!bgfill) { sSkyWin = false; if (sSkyNR) sky_flush_lines(cy0, cy1); }
        if (full) {
            sNatStale = false; /* this solid rect overwrites the whole target */
        } else {
            target_ready();
        }
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa, rgb_decal = sCurShader && sCurShader->rgb_decal;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (gfx_soft_hud_box < 0) { const char *e = getenv("DKR_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
        /* shrunk 1..3x along S: area-average (see texel_box) */
        const bool boxf = tex && gfx_soft_hud_box && dudx > 1.02f && dudx <= 3.0f;
        /* magnified along either axis, and the draw asks for G_TF_BILERP (gfx_soft_2d_bilin): bilerp at the
         * pixel centre, as the RDP filters a texture rectangle (Smash 872636f, the matrix's X14) */
        if (sBilin2DOn < 0) { const char *e = getenv("DKR_2DBILIN"); sBilin2DOn = !(e && e[0] == '0');
                              sBilin2DCensus = getenv("DKR_2DBILIN_CENSUS") != NULL; }
        const bool magnified = tex && !boxf && (dudx < 0.999f || dvdy < 0.999f);
        const bool mag2 = sBilin2DOn && gfx_soft_2d_bilin && magnified;
        if (sBilin2DCensus && magnified) { sMag2Sprites++; sMag2Bilerp += gfx_soft_2d_bilin; sMag2Px += (unsigned long long)(cx1 > cx0 ? cx1 - cx0 : 0) * (cy1 > cy0 ? cy1 - cy0 : 0); }
        const bool plain = !boxf && !mag2 && tex && !t16 && rgb_tex && a_tex && !rgb_texa &&
                           ((!rgb_in && !a_in) || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
                    const uint32_t c = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, bgfill ? 0 : 255);
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
            const int32_t vf_row = mag2 ? to_fix16(v) : 0; /* bilerp: the texel coordinate of the pixel centre */
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
                            row[x] = argb((uint8_t)((((t>>16)&0xff)*ta + dr*(255-ta))/255), (uint8_t)((((t>>8)&0xff)*ta + dg*(255-ta))/255), (uint8_t)(((t&0xff)*ta + db*(255-ta))/255), 255);
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
                    row[x] = argb((uint8_t)((((t>>16)&0xff)*ta + dr*(255-ta))/255), (uint8_t)((((t>>8)&0xff)*ta + dg*(255-ta))/255), (uint8_t)(((t&0xff)*ta + db*(255-ta))/255), 255);
                }
                continue;
            }
            /* General: per-channel combiner (RGB and alpha each from the
             * texel, the input colour, or their product). */
            const uint32_t *brow = NULL;
            if (boxf && cx1 > cx0) { /* the row's filtered texels, walked once (and kept across frames); a sprite
                                    * clipped to nothing never reaches the cache (a negative n rewound it) */
                static uint32_t sBoxTmp[1024] SAVESTATE_EXCLUDE;
                const int bn = cx1 - cx0 > 1024 ? 1024 : cx1 - cx0;
                brow = box_row_cached(sBoxTmp, bn, (uint32_t)(tex - sTex), texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha);
            }
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = boxf ? brow[x - cx0 < 1024 ? x - cx0 : 1023]
                                 : mag2 ? bilerp(texpx, t16, tw, th, ws2, wt2, uf, vf_row, mu, mv)
                                        : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                const int ta = (t>>24)&0xff;
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff;
                int fr, fg, fb, fa;
                if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }
                else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }
                else if (rgb_decal) { fr=(tr*ta + cr*(255-ta))/255; fg=(tg*ta + cg*(255-ta))/255; fb=(tb*ta + cb2*(255-ta))/255; }
                else if (sTexLerp) { fr=sLerpR+((cr-sLerpR)*tr)/255; fg=sLerpG+((cg-sLerpG)*tg)/255; fb=sLerpB+((cb2-sLerpB)*tb)/255; }
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
static void nat_ready(void) {
    if (sNatStale && sNatFB) { memset(sNatFB, 0, (size_t)sNatW * sNatH * 4); sNatStale = false; }
}
/* Before writing the current target (only native is cleared lazily). */
static void target_ready(void) { if (sFB == sNatFB) nat_ready(); }
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { sky_flush(); nat_ready(); if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }

static void alloc_target(uint32_t **fb, uint16_t **zb, int *w, int *h, int nw, int nh) {
    if (*fb && *w == nw && *h == nh) return;
    free(*fb); free(*zb);
    *w = nw; *h = nh;
    *fb = (uint32_t *)malloc((size_t)nw * nh * 4);
    *zb = (uint16_t *)malloc((size_t)nw * nh * 2);
}

static void select_target(bool is3d) {
    extern struct GfxDimensions gfx_current_dimensions;
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
/* Row bookkeeping for the internal target (device round 2: DKR switches
 * 2D<->3D a few times per frame, and a full-screen seed + composite + clear
 * per switch cost ~2-5 ms on the device). The seed is lazy -- a row is copied
 * from native the first time a triangle touches it -- and the composite and
 * clear cover only the rows seeded or drawn since the last composite. The
 * output is identical: untouched rows hold no drawn (alpha > 0) pixel. */
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
    if (sSeedPending) {
        for (int y = y0; y < y1 && y < MAP_MAX; y++) {
            if (sRowGen[y] != sSeedGen) { sRowGen[y] = sSeedGen; seed_row(y); }
        }
    }
}
static void composite_internal(void) {
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    PROF_BEGIN();
    nat_ready();
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
                if (p >> 24) dst[x] = p;
            }
        }
        memset(sIntFB + ty0 * sIntW, 0, (size_t)sIntW * (ty1 - ty0) * 4);
    }
    sTouchY0 = 1 << 30; sTouchY1 = -1;
    sSeedPending = false;
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
        for (int i = 0; i < sIntW * sIntH; i++) sIntFB[i] = sNatFB[i] & 0x00FFFFFFu;
        return;
    }
    /* lazy: rows are copied as triangles reach them (int_rows_touch) */
    sSeedPending = true;
    if (++sSeedGen == 0) { memset(sRowGen, 0, sizeof(sRowGen)); sSeedGen = 1; }
}

/* gfx_pc direct-call: switch the draw target for the next batch. */
u32 gfx_soft_phase_switches SAVESTATE_EXCLUDE; /* PERF: 2D<->3D target switches */
void gfx_scegu_set_phase(int is3d) {
    if ((bool)is3d == sPhase3D) return;
    gfx_soft_phase_switches++;
    if (!sUnified) sky_flush(); /* the composite reads the target (never held there: fbuf == sNatFB) */
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

/* The benchmark's NEON-off run (main.c: a "benchmark" trigger file holding neon=0; the
 * cross-port lead's round 2): the NEON rows against the scalar loops, on the device. */
void gfx_soft_set_neon(int on) {
#if defined(HH_CM_NEON)
    sCmNeon = on ? 1 : 0;
#else
    (void) on;
#endif
}

/* The benchmark's sky=both (main.c): the deferred sky on or off (from the next frame). */
void gfx_soft_set_skydefer(int on) {
#if defined(HH_CM_NEON)
    sSkyDefer = on ? 1 : 0;
#else
    (void) on;
#endif
}

/* The benchmark's bf=both (main.c): the branch-free blend on or off. */
void gfx_soft_set_blendbf(int on) {
#if defined(HH_CM_NEON)
    sBlendBF = on ? 1 : 0;
#else
    (void) on;
#endif
}

/* The benchmark's lean=both (main.c): the lean rows (cm_row_lean, 2026-10-04) on or off. */
void gfx_soft_set_lean(int on) {
#if defined(HH_CM_NEON)
    sLeanRows = on ? 1 : 0;
#else
    (void) on;
#endif
}

/* The benchmark's speed-round windows (main.c kind 7, 2026-10-07): DKR_FASTROWS, DKR_SPANVEC, DKR_ZSKIP (from the next
 * triangle; the bench reloads the level between windows). */
void gfx_soft_set_xspeed(int fastrows, int spanvec, int zskip) {
#if defined(HH_CM_NEON)
    sFastRows = fastrows ? 1 : 0; sSpanVec = spanvec ? 1 : 0; sZSkip = zskip ? 1 : 0;
#else
    (void) fastrows; (void) spanvec; (void) zskip;
#endif
}

/* The Bench's 2026-10-11 windows (main.c kind 7): DKR_FASTROWS and DKR_PSROWS (from the next triangle / row). */
void gfx_soft_set_rows4(int fastrows, int psrows) {
#if defined(HH_CM_NEON)
    sFastRows = fastrows ? 1 : 0; sPsRows = psrows ? 1 : 0;
#else
    (void) fastrows; (void) psrows;
#endif
}

/* The benchmark's noz=both (main.c): the sky's NEON rows on or off. */
void gfx_soft_set_noz(int on) {
#if defined(HH_CM_NEON)
    sNozNeon = on ? 1 : 0;
#else
    (void) on;
#endif
}

static void gfx_soft_init(void) {
#if defined(HH_CM_NEON)
    memset(sSkyHead, 0xFF, sizeof(sSkyHead)); /* -1: no held rows */
    { const char *e = getenv("DKR_CMNEON"); if (e && e[0] == '0') sCmNeon = 0; }
    { const char *e = getenv("DKR_LEANROWS"); if (e && e[0] == '0') sLeanRows = 0; } /* the lean rows (cm_row_lean) */
    { const char *e = getenv("DKR_LEANZ"); if (e && e[0] == '0') sLeanZ = 0; }
    { const char *e = getenv("DKR_LEANA"); if (e && e[0] == '0') sLeanA = 0; }
    { const char *e = getenv("DKR_LEANS"); if (e && e[0] == '0') sLeanS = 0; }
    { const char *e = getenv("DKR_LEANF"); if (e && e[0] == '0') sLeanF = 0; }
    { const char *e = getenv("DKR_FASTROWS"); if (e && e[0]) sFastRows = e[0] != '0'; } /* the cross-port speed round (2026-10-07); default off since 2026-10-11 */
    { const char *e = getenv("DKR_PSROWS"); if (e && e[0] == '0') sPsRows = 0; } /* DKR_PSROWS (2026-10-11) */
    { const char *e = getenv("DKR_SPANVEC"); if (e && e[0] == '0') sSpanVec = 0; }
    { const char *e = getenv("DKR_ZSKIP"); if (e && e[0] == '0') sZSkip = 0; }
#endif
    if (!sTexArena) {
        sTexArena = aligned_alloc(16, TEXARENA_SZ);
        if (!sTexArena) { fprintf(stderr, "gfx_soft: cannot allocate the %u MB texture arena\n", (unsigned) (TEXARENA_SZ >> 20)); abort(); }
    }
    ensure_targets(); select_target(false);
}
static void gfx_soft_on_resize(void) { sky_flush(); ensure_targets(); select_target(sPhase3D); }
u32 gfx_soft_frames_rendered SAVESTATE_EXCLUDE; /* frames that reached end_frame (frameskip excluded) */
static void gfx_soft_start_frame(void) {
    sky_flush();
    ensure_targets();
    PROF_BEGIN();
    sNatStale = true; /* cleared lazily, or never if the frame's first draw covers it */
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    PROF_END(PROF_CLR);
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
#if defined(HH_CM_NEON)
    if (sSkyDefer < 0) { const char *e = getenv("DKR_SKYDEFER"); sSkyDefer = !(e && e[0] == '0'); }
    sSkyWin = sSkyDefer && sUnified; /* cleared to alpha 0: nothing drawn yet */
#endif
}
static void gfx_soft_end_frame(void) {
    sPShadeFrame++;
    sky_flush();
    nat_ready(); /* nothing reached native this frame: present it cleared */
    if (!sUnified && sIntDirty) composite_internal();
    gfx_soft_last_frame_had_3d = sHad3D;
    if (sHudCensus && sHudTris) /* the frame number matches DKR_DUMP's */
        port_log("hudtri: frame %u: %u textured HUD triangles, %u asked for G_TF_POINT, %u drawn bilinear, %u exact 1:1 (point)\n",
                 (unsigned) gfx_soft_frames_rendered, (unsigned) sHudTris, (unsigned) sHudPoint, (unsigned) sHudBilin, (unsigned) sHudExact);
    sHudTris = sHudPoint = sHudBilin = sHudExact = 0;
    gfx_soft_frames_rendered++;
    select_target(false); /* leave native current for present/menu/dumps */
}
static void gfx_soft_finish_render(void) { }

/* -------------------------------------------------------------- shims --- */
/* gfx_pc.c calls these sceGu/texman helpers directly; software equivalents. */
void gfx_scegu_set_cull_mode(uint32_t cull) { (void)cull; /* GE-only API; winding cull lives in gfx_sp_tri1 */ }
void gfx_scegu_set_texfunc_add(bool on) { sTexAdd = on; }
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; }
void gfx_scegu_set_force_in(bool on) { sForceIn = on; }
void gfx_soft_set_fog(bool on, int r, int g, int b, int a) { sFogPx = on; sFogR = r; sFogG = g; sFogB = b; sFogA = on ? a : -1; }
void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b) { sTexLerp = on; sLerpR = r; sLerpG = g; sLerpB = b; }
void gfx_soft_set_tex2(bool on, uint32_t id1, uint8_t r, uint8_t g, uint8_t b) { sTex2 = on; sTex2Id = id1; sTex2R = r; sTex2G = g; sTex2B = b; }
void gfx_scegu_sync_pending(void) { }
void gfx_scegu_upload_texture(const uint8_t *buf, int w, int h, unsigned int type) { gfx_soft_upload_texture(buf, w, h, type); }
int  gfx_vram_space_available(void) { return 1; }

int  texman_usage_percent(void) { return (int)(sArenaUsed * 100 / TEXARENA_SZ); }
int  texman_can_hold(unsigned int num, unsigned int bytes) {
    (void)num;
    /* 16-bit sources are stored as ARGB8888: twice the source bytes (bytes
     * is sized 2 per texel; 32-bit sources are over-counted, so it flushes
     * early rather than late). */
    return sArenaUsed + (size_t) bytes * 2u <= TEXARENA_SZ;
}
void texman_bind_tex(unsigned int num) { sBoundTex[sTexUploadTile] = num; }
void texman_clear(void) { sky_flush(); /* the held rows point into the arena */ sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; box_cache_clear(); }

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
    sky_flush();
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
    sky_flush();
    nat_ready();
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

/* Texture edges without FORCE_BL (G_RM_AA_ZB_TEX_EDGE and kin; gfx_pc, DKR_EDGEOPAQUE; OoT 7120396): the
 * RDP takes coverage from the texel's alpha and never blends such a pixel with memory -- it is written
 * as the texel's own colour, or not at all (the VI's AA softens the edge later). 3D triangles only; the
 * 2D path (rectangles, sprites) keeps its blend. */
void gfx_soft_set_edge_opaque(bool on) { sEdgeOpaque = on; }
/* DKR_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }

