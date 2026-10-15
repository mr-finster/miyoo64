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
#define HOT_NEON 1
#endif
#include "savestate.h"
#include "port.h"
extern uint32_t port_time_us(void);
extern void port_log(const char *fmt, ...);
#define PROF_BEGIN() const u32 _pt0 = port_time_us()
#define PROF_END(slot) PORT_PROF_ADD(slot, port_time_us() - _pt0)
#ifdef FZX_STAT /* the overdraw census (gfx_soft_stat.h, make STAT=1) */
static void stat_batch_begin(bool is2d); static void stat_batch_end(size_t tris);
static void stat_frame_begin(void); static void stat_frame_end(void);
static void stat_path(uint32_t px, bool is3d, int kind, bool bilin, bool zt, bool alpha, bool zw, bool fog);
static void stat_bilin(bool bilin, bool inside, bool rr, bool cc, int n, int kind, bool alpha);
static void stat_const(int kind, int th, int32_t duf, int32_t dvf, int n);
static void stat_row(bool hot, bool cut, bool own); static void stat_tri(bool hot, bool flat);
static void stat_zspan(bool ztest, int span, int n, int32_t zq, int32_t zdq, const uint16_t *zb, int zbias);
#define STAT(x) x
#else
#define STAT(x)
#endif
#include <math.h>

#include <PR/gbi.h>
#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#if defined(MIYOO)
#include <time.h>
#include "miyoo_worker.h"
#endif

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
    bool linear;        /* the draw's texture filter is bilinear, not G_TF_POINT (FZX_HUDTRI=2) */
} SoftTex;

/* Texture arena + table excluded from savestates: 12 MB would bloat every
 * state, and the cache is flushed on load so textures re-import from the
 * restored course data anyway. Byte-addressed: the N64's RGBA16/CI textures
 * (the road/kart/scenery fill that dominates the memory-bound raster) are
 * stored 16-bit rather than expanded to 32-bit, halving their cache
 * footprint; IA/I textures stay 32-bit ARGB. */
static SoftTex sTex[MAX_TEXTURES + 1] SAVESTATE_EXCLUDE; /* + DEF_TEX_ID, a deferred batch's texture copy */
static uint32_t sTexGen[MAX_TEXTURES] SAVESTATE_EXCLUDE; /* bumped whenever a texture's texels are (re)written: box row cache key */
/* Heap, not a static array: a named section is PROGBITS even when zero-
 * initialised, so the arena was 12 MB of zeros in the ELF (M6: the shipped
 * zip is code only and should be ~1 MB). Allocated once in gfx_soft_init. */
static uint8_t *sTexArena SAVESTATE_EXCLUDE;
uint32_t gfx_soft_arena_peak SAVESTATE_EXCLUDE;     /* peak texture arena use, % (PERF) */
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
    /* 5551 is unpacked to ARGB8888 here, once, instead of at every texel fetch
     * (diddy-kong-racing a0ee8b5: the per-fetch unpack was ~11% of DKR's raster
     * instructions). F-Zero X peaks at 13% of the arena with 16-bit storage,
     * so doubling those textures still leaves ample room. */
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
    { const uint32_t pct = (uint32_t)(sArenaUsed * 100u / TEXARENA_SZ); if (pct > gfx_soft_arena_peak) gfx_soft_arena_peak = pct; }
    if (src16) {
        uint32_t *d = (uint32_t *)dst; /* unpack now: fetches read plain ARGB8888 */
        for (size_t i = 0; i < n; i++) d[i] = unpack5551(((const uint16_t *)buf)[i]);
    } else { /* GU_PSM_8888: RGBA bytes -> ARGB8888 */
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = argb(buf[i*4+0], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    }
    sTex[id].px = dst; sTex[id].is16 = is16; sTex[id].w = width; sTex[id].h = height; sTex[id].valid = true;
    sTexGen[id]++;
    { /* MK64_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("FZX_TEXDUMP"); }
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
        sTex[id].linear = linear;
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

/* HUD/menu sprites are laid out for the N64's 320-wide screen and land on 240
 * columns: 4 texels to 3 pixels. Nearest drops every fourth texel column
 * (strokes vanish or double: jaggy, low-res digits), so a shrunk sprite takes
 * the area average of exactly the texels each pixel covers - every column
 * counts, edges antialias. Alpha is premultiplied so transparent texels do not
 * darken the average. FZX_HUDFILTER=0 restores nearest (A/B tests). */
int gfx_soft_hud_box SAVESTATE_EXCLUDE = -1;
static inline uint32_t texel_box(const void *px, bool t16, int rowbase, int32_t uf, int32_t duf, int tw, int ws, int mu,
                                 bool premul) {
    /* Device round 4 (M3 log + callgrind): the first version divided four
     * times per pixel and wrapped every tap - 27% of all instructions in a
     * menu-heavy run. Now: <= 3 taps (duf <= 3.0), no wrap inside the texture,
     * one reciprocal per pixel (two at a translucent edge). */
    const int32_t u0 = uf - (duf >> 1);    /* uf is the pixel centre: the footprint starts half a step left */
    const int32_t end = u0 + duf;
    const int first = u0 >> 16, last = (end - 1) >> 16;
    const bool inside = first >= 0 && last < tw;
    uint32_t cr = 0, cg = 0, cb = 0, pr = 0, pg = 0, pb = 0, sa = 0, sw = 0;
    int32_t pos = u0;
    for (int i = first; i <= last; i++) {
        int32_t next = (int32_t)((uint32_t)(i + 1) << 16);
        if (next > end) next = end;
        const uint32_t w = (uint32_t)(next - pos) >> 8; /* 8.8 coverage */
        pos = next;
        const uint32_t t = texel_at(px, t16, rowbase + (inside ? i : wrapf(i, tw, ws, mu)));
        const uint32_t a = t >> 24, r = (t >> 16) & 0xff, g = (t >> 8) & 0xff, b = t & 0xff;
        cr += r * w; cg += g * w; cb += b * w;
        if (premul) { const uint32_t aw = a * w; pr += r * aw; pg += g * aw; pb += b * aw; }
        sa += a * w; sw += w;
    }
    if (sw == 0) return 0;
    /* q = ceil(2^32 / k): (x * q) >> 32 is floor(x / k), exact for multiples of k */
    const uint32_t qw = 0xFFFFFFFFu / sw + 1u;
    const uint32_t alpha = (uint32_t)(((uint64_t)sa * qw) >> 32);
    if (!premul || sa == 255u * sw) {
        return (alpha << 24) | ((uint32_t)(((uint64_t)cr * qw) >> 32) << 16) |
               ((uint32_t)(((uint64_t)cg * qw) >> 32) << 8) | (uint32_t)(((uint64_t)cb * qw) >> 32);
    }
    if (sa == 0) return 0;
    const uint32_t qa = 0xFFFFFFFFu / sa + 1u;
    return (alpha << 24) | ((uint32_t)(((uint64_t)pr * qa) >> 32) << 16) |
           ((uint32_t)(((uint64_t)pg * qa) >> 32) << 8) | (uint32_t)(((uint64_t)pb * qa) >> 32);
}

/* float -> int32 as the device's VFP converts it (vcvt saturates, NaN gives 0), so the x86
 * proxy agrees where a plain cast is undefined: a near-degenerate triangle's colour step can
 * reach ~1e10 in 16.16, and x86's cvttss2si gives INT_MIN there (mario-kart-64 b2dff40; make
 * PLAT=pc UBSAN=1 checks float casts). */
static inline int32_t sat_i32(float f) {
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
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
/* P17 (handheld-kit k1.2, the ports lead's round 2): every textured 3D pixel bilinear, as the
 * N64 filters. The magnified-only gate (MK64 round 13, below at use_bilin) chose bilinear or
 * nearest per triangle, so a floor switched between smooth and sharp along triangle edges that
 * moved with the camera (DKR's device report: the floor "flashes"). FZX_MAGBILIN=1: the gate. */
int gfx_soft_magbilin SAVESTATE_EXCLUDE = 0;
/* Start+X (input.c): bilinear costs ~37% of the 3D raster, so the device test can compare. */

/* Boxed sprite rows kept across frames (ocarina-of-time ab65ddd, after MK64
 * b72653e). A HUD or menu sprite row is the same texels at the same scale
 * and phase frame after frame, so after its first frame a row costs one hash
 * probe instead of a texel_box per pixel. The key is everything the row
 * reads: texture data pointer + its content generation (sTexGen: uploads and
 * the TEXEL1 pre-blend, which can rewrite in place), row, start, step,
 * width, wrap, alpha mode. texman_clear (the arena reset, which recycles
 * pointers) empties the store. When it fills, rows not used this frame or
 * the last are dropped (then all but this frame's, then all). Misses run
 * texel_box itself, so a cached row is the same bytes. FZX_BOXCACHE=0 off. */
#define BOX_MAX   512
#define BOXC_PX   (128u * 1024u) /* filtered texels: 512 KB */
#define BOXC_ENTS 4096u
#define BOXC_HASH 8192u          /* power of two, 2x BOXC_ENTS */
typedef struct { uintptr_t px; int32_t rowbase, uf, duf; uint32_t misc, gen, off, frame; } BoxEnt;
static uint32_t *sBoxPx SAVESTATE_EXCLUDE;
static BoxEnt *sBoxEnt SAVESTATE_EXCLUDE;
static uint16_t *sBoxHash SAVESTATE_EXCLUDE;   /* 1 + index into sBoxEnt, 0 = empty */
static uint32_t sBoxUsed SAVESTATE_EXCLUDE, sBoxN SAVESTATE_EXCLUDE;
static int sBoxState SAVESTATE_EXCLUDE;        /* 0 = not set up yet, 1 = on, -1 = off (A/B or no memory) */
extern u32 gfx_soft_frames_rendered;

static void box_row(uint32_t *out, int n, const void *px, bool t16, int rowbase, int32_t uf, int32_t duf, int tw, int ws,
                    int mu, bool premul) {
    for (int i = 0; i < n; i++, uf += duf) out[i] = texel_box(px, t16, rowbase, uf, duf, tw, ws, mu, premul);
}
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
/* Keep the rows used within the last `age` frames, in allocation order (so
 * every kept row moves down or stays), and rebuild the index. */
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
/* A boxed row through the cache (into tmp only when the cache is off). */
static const uint32_t *box_row_cached(uint32_t *tmp, int n, uint32_t texid, const void *px, bool t16, int rowbase,
                                      int32_t uf, int32_t duf, int tw, int ws, int mu, bool premul) {
    if (sBoxState == 0) {
        const char *e = getenv("FZX_BOXCACHE");
        sBoxState = -1;
        if (!(e && e[0] == '0')) {
            sBoxPx = malloc(BOXC_PX * sizeof(*sBoxPx)); sBoxEnt = malloc(BOXC_ENTS * sizeof(*sBoxEnt));
            sBoxHash = calloc(BOXC_HASH, sizeof(*sBoxHash));
            if (sBoxPx && sBoxEnt && sBoxHash) sBoxState = 1;
            else { free(sBoxPx); free(sBoxEnt); free(sBoxHash); sBoxPx = NULL; sBoxEnt = NULL; sBoxHash = NULL; }
        }
    }
    if (sBoxState < 0) { box_row(tmp, n, px, t16, rowbase, uf, duf, tw, ws, mu, premul); return tmp; }
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
int port_filter3d(void) { return gfx_soft_bilinear; }
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
/* The bilinear blend of four taps (a b / c d) by the 8-bit fractions. */
static inline __attribute__((always_inline)) uint32_t blend4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, int fu, int fv) {
    /* even lanes = B,R ; odd lanes = G,A (shifted down 8) */
    const uint32_t te = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, fu), to = lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, fu);
    const uint32_t ue = lerp2(c & 0x00FF00FFu, d & 0x00FF00FFu, fu), uo = lerp2((c >> 8) & 0x00FF00FFu, (d >> 8) & 0x00FF00FFu, fu);
    return lerp2(te, ue, fv) | (lerp2(to, uo, fv) << 8);
}
/* Round 13 (the user, 2026-09-26: "it needs to run 60"): the N64's own three-point filter as an
 * option. The RDP splits the texel square along its diagonal and blends the three texels of the
 * half the sample lies in: a + (b - a) fu + (c - a) fv below it, d + (c - d)(1 - fu) + (b - d)(1 - fv)
 * above. Written as weights they are all >= 0 and sum to 256 (|256 - fu - fv|, then the two side
 * texels), so the two-channels-per-multiply packing of lerp2 holds (a lane stays <= 255 * 256): six
 * multiplies against blend4's twelve, and the half is chosen by selects, not a branch. The fractions
 * keep 8 bits (the RDP's are 5). On by default since round 13b (the user's pick); FZX_TAP3=0 is the 4-tap. */
int gfx_soft_tap3 SAVESTATE_EXCLUDE = -1;
static inline __attribute__((always_inline)) uint32_t blend3(uint32_t a, uint32_t b, uint32_t c, uint32_t d, int fu, int fv) {
    const int s = fu + fv - 256;
    const bool up = s >= 0;
    const uint32_t e = up ? d : a;
    const uint32_t we = (uint32_t)(up ? s : -s), wb = (uint32_t)(up ? 256 - fv : fu), wc = (uint32_t)(up ? 256 - fu : fv);
    const uint32_t lo = ((e & 0x00FF00FFu) * we + (b & 0x00FF00FFu) * wb + (c & 0x00FF00FFu) * wc) >> 8;
    const uint32_t hi = ((e >> 8 & 0x00FF00FFu) * we + (b >> 8 & 0x00FF00FFu) * wb + (c >> 8 & 0x00FF00FFu) * wc) >> 8;
    return (lo & 0x00FF00FFu) | ((hi & 0x00FF00FFu) << 8);
}
static inline bool tap3_on(void) {
    if (gfx_soft_tap3 < 0) { const char *e = getenv("FZX_TAP3"); gfx_soft_tap3 = !(e && e[0] == '0'); }
    return gfx_soft_tap3 > 0;
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
    return gfx_soft_tap3 > 0 ? blend3(a, b, c, d, fu, fv) : blend4(a, b, c, d, fu, fv);
}

/* FZX_WALLLOD (2026-10-09; the user: "fzero walls need it", after GoldenEye's wall LOD). F-Zero X never mip-maps: a
 * census of a whole autopilot race (5.1 M triangles, menus and results included) found no draw with G_TL_LOD and no
 * gSPTexture level above 0, so the RDP reads tile 0 with its 3-point filter at every pixel, as the port already does,
 * and the RDP's per-pixel LOD picks nothing. What the port does differently is the screen: its 240 columns show the
 * N64's 296 (the race's fill box, 12..308), so a wall receding across the screen is sampled ~1.23x more sparsely than
 * on the console and its stripes break up and crawl more than the N64's picture shown at 240. FZX_WALLLOD=1 samples
 * those triangles' texture where the N64's own pixel centres fall inside each port pixel (2-3 taps of the same 3-point
 * filter, weighted by how much of the N64 pixel the port pixel covers): the console's sampling, shrunk to 240 columns
 * as a box filter would. "Wall" triangles: 3D, perspective-textured, filtered, whose texture runs horizontally across
 * the screen faster than one texel a pixel and at least twice as fast as it does vertically (the receding side walls
 * and rails; the road recedes up the screen and fails the test), drawn without a depth test as the track is (the
 * machines, depth-tested, keep their pixels). Their spans whose texture runs faster than a texel a pixel across take
 * the generic loops with wall_fetch; their other spans stay on hot_span (same pixels as before).
 * Default 1 (on): against angrylion (Mute City, 20 paired race pictures) the walls' horizontal detail went from 17.3 to
 * 15.6 with the N64's at 12.7, closer in 19 of 20. 0 = off (the pick before, same pixels as before); ab = alternates every
 * 5 s (300 VIs), with an "AB walllod" line in console.log. Cost (qemu, the autopilot's race): +0.3% on the first
 * straight, +7% on the curve along the zigzag wall. */
int gfx_soft_walllod SAVESTATE_EXCLUDE = -1;
#define WALL_MAXW 1024
typedef struct { int8_t n; int16_t o[3]; uint16_t w[3]; } WallCol; /* taps: offset (1/256 px from the centre), weight /256 */
static WallCol sWallCol[WALL_MAXW] SAVESTATE_EXCLUDE;
static float sWallKey[3] SAVESTATE_EXCLUDE; /* c0, ratio, width the table was built for */
extern int gfx_frame_box(float r[4]);
extern unsigned int port_os_vi_count(void);
static bool wall_lod_active(void) {
    if (gfx_soft_walllod < 0) {
        const char *e = getenv("FZX_WALLLOD");
#if defined(MIYOO)
        /* the Miyoo Mini draws the N64's own 320 columns, one N64 pixel a pixel, with no fill: the N64's
         * sampling is already this one's (and wall_cols' box map would shift the columns there) */
        gfx_soft_walllod = !e ? 0 : (e[0] == 'a' || e[0] == 'A') ? 2 : e[0] == '0' ? 0 : 1;
#else
        gfx_soft_walllod = !e ? 1 : (e[0] == 'a' || e[0] == 'A') ? 2 : e[0] == '0' ? 0 : 1;
#endif
    }
    if (gfx_soft_walllod != 2) return gfx_soft_walllod == 1;
    static int sLast = -1;
    const int on = ((port_os_vi_count() / 300) & 1) == 0;
    if (on != sLast) { sLast = on; port_log("AB walllod %s vi %u\n", on ? "on" : "off", port_os_vi_count()); }
    return on;
}
/* The N64 columns behind each target column: port pixel j covers N64 x in [c0 + j r, c0 + (j + 1) r). */
static void wall_cols(int w) {
    float b[4];
    float c0 = 0.0f, r = 320.0f / (float)w;
    if (gfx_frame_box(b) && b[2] > b[0]) { c0 = b[0]; r = (b[2] - b[0]) / (float)w; }
    if (w > WALL_MAXW) w = WALL_MAXW;
    if (sWallKey[0] == c0 && sWallKey[1] == r && sWallKey[2] == (float)w) return;
    sWallKey[0] = c0; sWallKey[1] = r; sWallKey[2] = (float)w;
    for (int j = 0; j < w; j++) {
        const float a = c0 + j * r, e = a + r;
        WallCol *c = &sWallCol[j];
        float wf[3], of[3]; int n = 0;
        for (int k = (int)floorf(a); k < e && n < 3; k++) {
            const float lo = a > k ? a : (float)k, hi = e < k + 1 ? e : (float)(k + 1);
            if (hi - lo < 1e-4f) continue;
            wf[n] = (hi - lo) / r;
            of[n] = (k + 0.5f - c0) / r - (j + 0.5f); /* the N64 pixel's centre, in target pixels from this one's */
            n++;
        }
        int sum = 0;
        for (int k = 0; k < n; k++) {
            /* floorf(x + 0.5f), not lrintf: lrintf follows the FPU rounding mode, which gave other weights on the ARM build */
            c->o[k] = (int16_t)floorf(of[k] * 256.0f + 0.5f);
            c->w[k] = (uint16_t)(k == n - 1 ? 256 - sum : (int)floorf(wf[k] * 256.0f + 0.5f));
            sum += c->w[k];
        }
        c->n = (int8_t)n;
    }
}
static inline __attribute__((always_inline)) uint32_t
wall_fetch(const void *px, bool i16, int w, int h, int ws, int wt, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int mu, int mv,
           int col) {
    const WallCol *c = &sWallCol[col < 0 ? 0 : col >= WALL_MAXW ? WALL_MAXW - 1 : col];
    uint32_t lo = 0, hi = 0;
    for (int k = 0; k < c->n; k++) {
        const int32_t du = (int32_t)(((int64_t)duf * c->o[k]) >> 8), dv = (int32_t)(((int64_t)dvf * c->o[k]) >> 8);
        const uint32_t t = bilerp(px, i16, w, h, ws, wt, uf + du, vf + dv, mu, mv);
        lo += (t & 0x00FF00FFu) * c->w[k];
        hi += ((t >> 8) & 0x00FF00FFu) * c->w[k];
    }
    return ((lo >> 8) & 0x00FF00FFu) | (((hi >> 8) & 0x00FF00FFu) << 8);
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
    /* (TEXEL0 - X) * TEXEL0_ALPHA + X: the texture decaled over input X by its
     * own alpha (MK64 v30 bea7db7). Replacing the texel RGB with its alpha drew
     * it white - F-Zero X's whole track. */
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
static bool sTexLerp;      /* out = K + (vertex - K) * texel: OoT's (A - K) * TEXEL0 + K tints */
static int sLerpR, sLerpG, sLerpB;
static bool sForceIn;      /* 2-cycle: the vertex colour carries cycle 2, always modulate */
/* out = vertex * (B + (A - B) * texel): (A - B) * TEXEL0 + B in cycle 1 with A, B
 * constant, then COMBINED * SHADE in cycle 2 (F-Zero X's machine bodies). */
static bool sTexLerp2;
static int sL2Ar, sL2Ag, sL2Ab, sL2Br, sL2Bg, sL2Bb;
/* out = texel + (K - texel) * vertex: (K - TEXEL0) * SHADE + TEXEL0, the texture faded to a
 * constant by the shade (F-Zero X's venue floor haze; gfx_pc's FZX_TEXFOG). */
static bool sTexFog;
static int sTfR, sTfG, sTfB;
/* Opaque fog render modes: the vertex alpha carries the fog factor and the
 * fog colour is blended in after the texture (FOG * A_SHADE + IN * (1 - A_SHADE)). */
static bool sPostFog;
static int sFogR, sFogG, sFogB;

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
/* FZX_FILLRULE (2026-10-03; ocarina-of-time a7c4909, the Deku Tree's vision: a one-pixel bright diagonal across
 * Ganondorf's flames): the row solve in tri_row is biased toward inclusion, and a pixel centre exactly on an
 * edge counts as inside, so a pixel on the edge two triangles share is drawn by both. Opaque, that is
 * invisible; blended, the pixel is blended twice. The RDP without AA_EN writes a pixel only when one fixed
 * sample of it is covered (angrylion blender.c: cvbit, the mask's bit 7), and the edges split the samples
 * exactly, so exactly one triangle draws each pixel. Here, for such draws (gfx_pc: blended or alpha-tested,
 * no AA_EN), each row's ends are decided per pixel by edge functions taken in a canonical vertex order (the
 * lower y, then the lower x, first), so the two triangles of a shared edge compute the same float there and
 * only its sign differs; a centre exactly on the edge goes to the triangle the edge is a left (or top) edge
 * of. Other draws keep the inclusive solve, bit-identical. sOwnTri is render state like the rest: the recorded
 * batches (def_record, vis_record) keep it in their RastState. */
static bool sOwnTri SAVESTATE_EXCLUDE; /* gfx_soft_set_own_edges (FZX_FILLRULE) */
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
/* x / 255 truncated toward zero for either sign, as C's '/' does: the per-pixel
 * lerps (tex_lerp, tex_lerp2) take signed differences. Integer division costs
 * the Cortex-A7 ~20 cycles; this is a multiply-free shift sequence. */
static inline int div255s(int x) { return x >= 0 ? div255(x) : -div255(-x); }

/* Span rasterizer. Per row: solve the three half-plane constraints for an
 * x-interval (no per-pixel inside tests), then walk the span with
 * incrementally stepped z / shade / (u/w, v/w, 1/w). Perspective division
 * happens every PERSP_SPAN pixels with linear u,v between (the N64-plugin
 * standard; MK64's ground polygons are huge, so the per-pixel fdiv the old
 * loop did dominated the race profile). Row bases are recomputed from the
 * vertex data each row, so precision stays within one mul-add of exact and
 * shared triangle edges stay crack-free. */
#define PERSP_SPAN 16
/* During a resolve (def_raster), a span with no pixel still pending for the batch
 * (sWinPend at the target's pixel index, sWinId the batch: the track has covered it) keeps
 * its steps -- the next span's start values and the carried reciprocal -- but draws no
 * pixels. */
static const uint8_t *sWinPend SAVESTATE_EXCLUDE;
static uint8_t sWinId SAVESTATE_EXCLUDE;
static uint32_t sWinPx SAVESTATE_EXCLUDE; /* span pixels a resolve drew (PERF px) */
static inline bool span_skip(int span, int idx) {
    if (!sWinPend) return false;
    const uint8_t *p = sWinPend + idx;
    for (int i = 0; i < span; i++) {
        if (p[i] == sWinId) { sWinPx += (uint32_t)span; return false; }
    }
    return true;
}
/* Visible-only 3D (round 12, vis_flush): while a recorded 3D phase is drawn, sCutBuf holds per
 * pixel the index of the last occluder there (0: none), sCutRowMax per row the largest, and
 * sCutBMin/sCutBMax per 16-pixel block of a row a lower and an upper bound of its indices. The
 * triangle being drawn (index sCutId) draws only the pixels whose last occluder is itself or an
 * earlier triangle. A span with none of them keeps its steps, like a resolve's skipped span, and
 * skips its perspective divide: the next span that draws takes 1/sw itself (rw < 0 means not
 * taken yet), the same division of the same value. sVisPx: the span pixels drawn (PERF px). */
static const uint16_t *sCutBuf SAVESTATE_EXCLUDE;
static const uint16_t *sCutRowMax SAVESTATE_EXCLUDE, *sCutRowMin SAVESTATE_EXCLUDE; /* a row's largest index, and a lower bound */
static const uint16_t *sCutBMin SAVESTATE_EXCLUDE, *sCutBMax SAVESTATE_EXCLUDE;
static int sCutBW SAVESTATE_EXCLUDE; /* blocks per row */
#if defined(MIYOO)
/* The Miyoo Mini's second core (vis_flush, FZX_THREADS): each thread draws with its own triangle index
 * and pixel count, and while sRowSplit is set only the rows y with (y & 1) == sRowPar (raster_tri's
 * and tri_rows_t's row loops). */
static __thread uint16_t sCutId;
static __thread uint32_t sVisPx;
static __thread int sRowSplit, sRowPar;
#else
static uint16_t sCutId SAVESTATE_EXCLUDE;
static uint32_t sVisPx SAVESTATE_EXCLUDE;
#endif
#define SPAN_FULL(n) ((1u << (n)) - 1u) /* n <= PERSP_SPAN */
/* A row of a visible-only draw where an occluder after the triangle lies: its indices (by x),
 * its blocks' bounds (by x >> 4) and the triangle's index. */
typedef struct { const uint16_t *cut, *bmin, *bmax; uint16_t id; } VisRow;
int gfx_soft_visneon SAVESTATE_EXCLUDE = -1; /* FZX_VISNEON=0: the visible-only scans a pixel at a time (A/B) */
/* bit i set: pixel x + i is drawn (n <= PERSP_SPAN, so the span lies in at most two blocks) */
static inline uint32_t vis_span(const VisRow *r, int x, int n) {
    const int b0 = x >> 4, b1 = (x + n - 1) >> 4;
    const uint16_t mn = r->bmin[b0] < r->bmin[b1] ? r->bmin[b0] : r->bmin[b1];
    if (mn > r->id) return 0;
    const uint16_t mx = r->bmax[b0] > r->bmax[b1] ? r->bmax[b0] : r->bmax[b1];
    if (mx <= r->id) return SPAN_FULL(n);
    const uint16_t *c = r->cut + x;
#ifdef HOT_NEON
    if (n == PERSP_SPAN && gfx_soft_visneon) {
        /* the speed grind (2026-09-28): the sixteen compares in two vectors, each lane's result
         * masked to its own bit, then summed (the bits are disjoint, so the sum is the OR): the
         * loop's mask bit for bit. FZX_VISNEON=0: the loop. */
        static const uint16_t kBit[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
        const uint16x8_t I = vdupq_n_u16(r->id);
        const uint16x8_t m = vorrq_u16(vandq_u16(vcleq_u16(vld1q_u16(c), I), vld1q_u16(kBit)),
                                       vandq_u16(vcleq_u16(vld1q_u16(c + 8), I), vld1q_u16(kBit + 8)));
        uint16x4_t s = vpadd_u16(vget_low_u16(m), vget_high_u16(m));
        s = vpadd_u16(s, s);
        s = vpadd_u16(s, s);
        return vget_lane_u16(s, 0);
    }
#endif
    uint32_t m = 0;
    for (int i = 0; i < n; i++) m |= (uint32_t)(c[i] <= r->id) << i;
    return m;
}

/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
static bool sPhase3D SAVESTATE_EXCLUDE; /* the current batch targets the 3D pass (see the frame section) */
/* The hot span (device round 5, qemu-arm profile): 81% of raster_tri's
 * instructions on a Mute City race were ONE SPAN_LOOP variant -- the track:
 * depth write without compare, opaque, bilinear, TEXEL0-lerp x shade
 * (texlerp2), often with post-fog -- and ~60% of
 * that loop was stack traffic: the generic loop keeps ~20 values live (every
 * interpolant, step and combiner flag), the A7 has 14 registers, so GCC
 * spilled all of them and reloaded/stored each accumulator per pixel.
 * This routine does the same arithmetic in three short passes, each with few
 * live values: (1) depth test + write, collecting the visible pixels, (2) the
 * texel of each, (3) combine + store. Interpolants are rebuilt as
 * start + i*step, identical to i repeated adds (-fwrapv: two's-complement
 * wraparound, so the sums agree bit for bit). Opaque only: nothing a later
 * pass decides can discard a pixel whose depth pass 1 already wrote. */
#ifdef HOT_NEON
/* NEON lanes (B G R A, the byte order of an ARGB8888 word) with the scalar
 * integer arithmetic (ocarina-of-time cb36f29): blend4's lerp (x*(256-f) + y*f) >> 8
 * in u16 (<= 255*256), the shade clamp usat(q >> 16, 8) as vshrq 16 +
 * vqmovun_s32 + vmin 255, div255 (x + 1 + (x >> 8)) >> 8 in u16 (x <= 255*255).
 * Bit-identical. Off by default since round 12: the card6 bench of bfc1753 drew the 3D in
 * 18.3/18.4 ms scalar against 19.4/19.3 with NEON (FZX_NEON=1 turns it on, the bench's A/B). */
static int sHotNeon SAVESTATE_EXCLUDE = -1;
static int sHotNeonA SAVESTATE_EXCLUDE = -1;  /* FZX_NEONA=0: translucent spans on the scalar loop (A/B) */
static int sHotFuse SAVESTATE_EXCLUDE = -1;   /* FZX_FUSE=0: bilinear and combine as separate passes (A/B) */
/* bench.c: every NEON path of the raster and the HUD on (1) or off (0, the scalar loops);
 * the fused bilinear + combine on (1) or off (0, two passes) */
void gfx_soft_set_neon(int on) { sHotNeon = on ? 1 : 0; }
void gfx_soft_set_fuse(int on) { sHotFuse = on ? 1 : 0; }
/* The blend in u16 lanes (B G R A, each 0..255): what blend4_neon narrows and returns, and
 * what the fused loops of hot_span combine without the round trip through a word. */
/* The taps already in lanes: X = top-left, bottom-left; Y = top-right, bottom-right. */
static inline uint16x4_t blend4_neon_xy(uint32x2_t X32, uint32x2_t Y32, uint32_t fu, uint32_t fv) {
    const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));
    const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(X32));
    const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(Y32));
    const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);
    return vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2), vget_high_u16(H), wts, 3), 8);
}
static inline uint16x4_t blend4_neon_u16(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fu, uint32_t fv) {
    return blend4_neon_xy(vcreate_u32(((uint64_t)c << 32) | a), vcreate_u32(((uint64_t)d << 32) | b), fu, fv);
}
/* The taps loaded straight into lanes (round 10). Built from core registers, GCC stored
 * them to the stack and reloaded them as a vector: four stores and two loads a pixel on
 * the fused loops. Inside the texture the footprint is two adjacent pairs, one load each
 * and a transpose; elsewhere, four lane loads. The same lanes, so the same results. */
static inline uint16x4_t blend4_neon_in(const uint32_t *r, int tw, uint32_t fu, uint32_t fv) {
    const uint32x2x2_t t = vtrn_u32(vld1_u32(r), vld1_u32(r + tw));
    return blend4_neon_xy(t.val[0], t.val[1], fu, fv);
}
static inline uint16x4_t blend4_neon_ld(const uint32_t *a, const uint32_t *b, const uint32_t *c, const uint32_t *d,
                                        uint32_t fu, uint32_t fv) {
    return blend4_neon_xy(vld1_lane_u32(c, vld1_dup_u32(a), 1), vld1_lane_u32(d, vld1_dup_u32(b), 1), fu, fv);
}
static inline uint32_t blend4_neon(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fu, uint32_t fv) {
    const uint16x4_t T = blend4_neon_u16(a, b, c, d, fu, fv);
    return vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(T, T))), 0);
}
static inline uint16x4_t div255_u16x4(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
#endif
#ifndef HOT_NEON
void gfx_soft_set_neon(int on) { (void)on; } /* no NEON paths on this build */
void gfx_soft_set_fuse(int on) { (void)on; }
void gfx_soft_set_pair(int on) { (void)on; }
void gfx_soft_set_quad(int on) { (void)on; }
#endif
/* HOT_SHADEA: the colour is the shade and the texture gives only the alpha (translucent:
 * the machines' glows, cc 00043600) -- SPAN_LOOP's !rgb_tex branch. */
/* HOT_TLERP (round 12): K + (shade - K) * texel, SPAN_LOOP's texlerp branch -- the sparks and
 * the particle trails, which ran the generic loops at ~250 instructions a pixel. */
enum { HOT_MOD, HOT_TL2, HOT_TEX, HOT_DECAL, HOT_SHADEA, HOT_TLERP };
typedef struct {
    const uint32_t *px; int tw, th, ws, wt, mu, mv;
    bool bilin, ztest, zwrite, postfog, alpha; int zbias;
    int amode, amin;                        /* alpha: 0 shade, 1 texel, 2 texel x shade; discard below amin */
    int kind;                               /* HOT_*: the combiner branch SPAN_LOOP would take */
    int l2br, l2bg, l2bb, l2dr, l2dg, l2db; /* texlerp2: base + delta*t/255 */
    int lkr, lkg, lkb;                      /* texlerp: the constant K */
    int pfr, pfg, pfb;                      /* post-fog colour */
} HotSpan;
#ifdef HOT_NEON
/* hot_span's fused opaque combine, two pixels at a time (FZX_PAIR): the one-pixel loop's
 * arithmetic in the same order, but the two pixels' chains sit in one basic block with no
 * branch between them, so the in-order A7 can issue one pixel's work while the other's
 * waits on a result (the device runs the raster at well under an instruction a cycle:
 * mario-kart-64 rounds 28-29). Each copy of the loop fixes the combiner kind, the post-fog
 * and the tap addressing (inside, or repeat on both axes). The post-fog blend also runs at
 * sa == 0, where it gives the colour back unchanged (div255(255 * c) == c for c <= 255).
 * FZX_PAIR=0 keeps one pixel at a time (A/B; the bench's "no pair" run). */
static int sHotPair SAVESTATE_EXCLUDE = -1;
void gfx_soft_set_pair(int on) { sHotPair = on ? 1 : 0; }
typedef struct {
    const uint32_t *px; int tw, mu, mv;
    int32_t uf, vf, duf, dvf, aq, adq;
    int32x4_t acc0, dacc, L2B;
    int16x4_t L2D;
    uint16x4_t F;
} FusePair;
static inline __attribute__((always_inline)) uint16x4_t fuse_taps(const FusePair *c, const int rr, int i) {
    const int32_t su = c->uf + i * c->duf - 0x8000, sv = c->vf + i * c->dvf - 0x8000;
    if (!rr) return blend4_neon_in(c->px + (sv >> 16) * c->tw + (su >> 16), c->tw, (su >> 8) & 0xff, (sv >> 8) & 0xff);
    const int iu0 = (su >> 16) & c->mu, iu1 = ((su >> 16) + 1) & c->mu;
    const uint32_t *r0 = c->px + ((sv >> 16) & c->mv) * c->tw, *r1 = c->px + (((sv >> 16) + 1) & c->mv) * c->tw;
    return blend4_neon_ld(r0 + iu0, r0 + iu1, r1 + iu0, r1 + iu1, (su >> 8) & 0xff, (sv >> 8) & 0xff);
}
static inline __attribute__((always_inline)) uint16x4_t fuse_px(const FusePair *c, const int kind, const int fog, uint16x4_t T, int i) {
    const uint16x4_t v255 = vdup_n_u16(255);
    const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(c->acc0, c->dacc, i), 16)), v255);
    uint16x4_t M;
    if (kind == HOT_TL2) {
        const int32x4_t P = vmull_s16(c->L2D, vreinterpret_s16_u16(T));
        const uint32x4_t A = vreinterpretq_u32_s32(vabsq_s32(P));
        const int32x4_t Q = vreinterpretq_s32_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(A, vshrq_n_u32(A, 8)), vdupq_n_u32(1)), 8));
        const int32x4_t I = vaddq_s32(c->L2B, vbslq_s32(vcltq_s32(P, vdupq_n_s32(0)), vnegq_s32(Q), Q));
        M = div255_u16x4(vmul_u16(vmovn_u32(vreinterpretq_u32_s32(I)), S));
    } else if (kind == HOT_DECAL) {
        const uint16x4_t TA = vdup_lane_u16(T, 3);
        M = div255_u16x4(vmla_u16(vmul_u16(T, TA), S, vsub_u16(v255, TA)));
    } else {
        M = div255_u16x4(vmul_u16(T, S));
    }
    if (fog) {
        const int sa = clamp8_sh16(c->aq + i * c->adq);
        M = div255_u16x4(vmla_n_u16(vmul_n_u16(M, (uint16_t)(255 - sa)), c->F, (uint16_t)sa));
    }
    return vorr_u16(M, vcreate_u16(0x00FF000000000000ull));
}
static inline __attribute__((always_inline)) void fuse_pair_loop(const FusePair *c, const int kind, const int fog, const int rr,
                                                               const uint8_t *vis, int n, uint32_t *dst) {
    int k = 0;
    for (; k + 2 <= n; k += 2) {
        const int i0 = vis[k], i1 = vis[k + 1];
        const uint16x4_t M0 = fuse_px(c, kind, fog, fuse_taps(c, rr, i0), i0);
        const uint16x4_t M1 = fuse_px(c, kind, fog, fuse_taps(c, rr, i1), i1);
        const uint32x2_t W = vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M0, M1)));
        vst1_lane_u32(dst + i0, W, 0);
        vst1_lane_u32(dst + i1, W, 1);
    }
    if (k < n) {
        const int i = vis[k];
        const uint16x4_t M = fuse_px(c, kind, fog, fuse_taps(c, rr, i), i);
        vst1_lane_u32(dst + i, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
    }
}
static void __attribute__((noinline)) fuse_pair(const FusePair *cp, int kind, bool fog, bool rr, const uint8_t *vis, int n,
                                                uint32_t *dst) {
    const FusePair c = *cp; /* a local copy: its fields stay in registers across the stores to dst */
#define FP(K)                                                                                                   \
    if (fog) { if (rr) fuse_pair_loop(&c, K, 1, 1, vis, n, dst); else fuse_pair_loop(&c, K, 1, 0, vis, n, dst); } \
    else     { if (rr) fuse_pair_loop(&c, K, 0, 1, vis, n, dst); else fuse_pair_loop(&c, K, 0, 0, vis, n, dst); }
    if (kind == HOT_TL2) { FP(HOT_TL2) } else if (kind == HOT_DECAL) { FP(HOT_DECAL) } else { FP(HOT_MOD) }
#undef FP
}
/* Round 12: hot_span's bilinear combines four pixels at a time, each channel in its own lanes:
 * texture x shade, the decal, the texture alone and the shade with the texture's alpha, opaque
 * (with the post-fog) or translucent (the alpha test and blend, the depth written for the pixels
 * kept) -- the track, the city, the clouds, the scenery, the glows. The per-pixel NEON above keeps
 * a pixel's four channels in one register, so every step waits on the one before (the A7 ran it
 * slower than the scalar loops); here one instruction does a channel of four pixels, and the
 * channels are independent. The texel addresses come from core registers, as the scalar loops
 * compute them (no NEON-to-core moves; MK64's four-pixel span read its addresses back out of NEON
 * lanes). The arithmetic is the scalar code's, in u16 lanes: the lerps (x*(256-f) + y*f) >> 8
 * along u then v, the shade usat(q >> 16, 8) as vqshrun #16 and a min, div255 as
 * (x + 1 + (x >> 8)) >> 8 with x <= 255*255, the depth usat(q >> 8, 16) as vqshrun #8. So it is
 * bit for bit the same. The fog blend also runs where the fog factor is 0 and the alpha blend where
 * the alpha is 255: both give the colour back (div255(255 * c) == c). vis holds the pixels in
 * fours: the caller repeats the last one (the same value is stored again). Off by default: the
 * round-12 device bench drew the 3D in 18.4 ms with it and 17.5 ms with hot_span's scalar loops
 * (FZX_QUAD=1 turns it on). */
static int sQuad SAVESTATE_EXCLUDE = -1;
void gfx_soft_set_quad(int on) { sQuad = on ? 1 : 0; }
enum { Q_MOD, Q_DECAL, Q_TEX, Q_SHADEA };
typedef struct {
    const uint32_t *px; int tw, mu, mv;
    int32_t uf0, vf0, duf, dvf;       /* texel coordinates less half a texel, and their x-steps */
    int32_t bq, gq, rq, aq, bdq, gdq, rdq, adq, zq, zdq;
    uint16_t pfb, pfg, pfr;           /* post-fog colour */
    int amode, amin;                  /* translucent: fa from the shade (0), texel (1) or both (2); kept at fa >= amin */
    bool zwrite;
} QuadSpan;
static inline __attribute__((always_inline)) uint16x4_t q_div255(uint16x4_t x) {
    return vshr_n_u16(vadd_u16(vsra_n_u16(x, x, 8), vdup_n_u16(1)), 8);
}
/* one channel's bilinear from its taps: X = [a x4 | b x4] (the top row), Y = [c x4 | d x4] */
static inline __attribute__((always_inline)) uint16x4_t q_bil(uint8x8_t X8, uint8x8_t Y8, uint16x8_t Wu, uint16x4_t gv, uint16x4_t fv) {
    const uint16x8_t HX = vmulq_u16(vmovl_u8(X8), Wu), HY = vmulq_u16(vmovl_u8(Y8), Wu);
    const uint16x4_t t = vshr_n_u16(vadd_u16(vget_low_u16(HX), vget_high_u16(HX)), 8);
    const uint16x4_t b = vshr_n_u16(vadd_u16(vget_low_u16(HY), vget_high_u16(HY)), 8);
    return vshr_n_u16(vmla_u16(vmul_u16(t, gv), b, fv), 8);
}
static inline __attribute__((always_inline)) void quad_loop(const QuadSpan *c, const uint8_t *vis, int n, uint32_t *dst, uint16_t *zb,
                                                            const int kind, const int alpha, const int fog, const int rr) {
    /* Setup, in two light passes (a loop with every value live spilled to the stack): per pixel
     * the texel addresses and weights (core registers, as the scalar loops), then the shade and
     * the depth, four pixels at a time. */
    const int nn = (n + 3) & ~3;
    const uint32_t *pa[PERSP_SPAN + 4], *pb[PERSP_SPAN + 4], *pc[PERSP_SPAN + 4], *pd[PERSP_SPAN + 4];
    uint8_t fu8[PERSP_SPAN + 4] __attribute__((aligned(8))), fv8[PERSP_SPAN + 4] __attribute__((aligned(8)));
    uint16_t sh[5][PERSP_SPAN + 4] __attribute__((aligned(8))); /* shade B G R A, then the depth */
    const bool need_rgb = kind != Q_SHADEA, need_ta = kind == Q_DECAL || (alpha && c->amode != 0);
    const bool need_s = kind != Q_TEX, need_sa = fog || (alpha && c->amode != 1), need_z = alpha && c->zwrite;
    {
        const uint32_t *const px = c->px;
        const int tw = c->tw, mu = c->mu, mv = c->mv;
        const int32_t uf0 = c->uf0, vf0 = c->vf0, duf = c->duf, dvf = c->dvf;
        for (int k = 0; k < nn; k++) {
            const int i = vis[k];
            const int32_t su = uf0 + i * duf, sv = vf0 + i * dvf;
            fu8[k] = (uint8_t)(su >> 8); fv8[k] = (uint8_t)(sv >> 8); /* (s >> 8) & 0xff */
            if (!rr) { /* inside the texture: each row's two taps are neighbours */
                const uint32_t *r = px + (sv >> 16) * tw + (su >> 16);
                pa[k] = r; pc[k] = r + tw;
            } else { /* repeat on both axes, power-of-two sizes */
                const int x0 = (su >> 16) & mu, x1 = ((su >> 16) + 1) & mu;
                const uint32_t *ra = px + ((sv >> 16) & mv) * tw, *rb = px + (((sv >> 16) + 1) & mv) * tw;
                pa[k] = ra + x0; pb[k] = ra + x1; pc[k] = rb + x0; pd[k] = rb + x1;
            }
        }
    }
    {
        const int32x4_t Q = { c->bq, c->gq, c->rq, c->aq }, dQ = { c->bdq, c->gdq, c->rdq, c->adq };
        const uint16x4_t v255 = vdup_n_u16(255);
        for (int k = 0; k < nn; k += 4) {
            uint32_t w4;
            memcpy(&w4, vis + k, 4);
            const int32x4_t I = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(w4))))));
            if (need_s) { /* usat(q + i * dq >> 16, 8) */
                vst1_u16(sh[0] + k, vmin_u16(vqshrun_n_s32(vmlaq_lane_s32(vdupq_lane_s32(vget_low_s32(Q), 0), I, vget_low_s32(dQ), 0), 16), v255));
                vst1_u16(sh[1] + k, vmin_u16(vqshrun_n_s32(vmlaq_lane_s32(vdupq_lane_s32(vget_low_s32(Q), 1), I, vget_low_s32(dQ), 1), 16), v255));
                vst1_u16(sh[2] + k, vmin_u16(vqshrun_n_s32(vmlaq_lane_s32(vdupq_lane_s32(vget_high_s32(Q), 0), I, vget_high_s32(dQ), 0), 16), v255));
            }
            if (need_sa)
                vst1_u16(sh[3] + k, vmin_u16(vqshrun_n_s32(vmlaq_lane_s32(vdupq_lane_s32(vget_high_s32(Q), 1), I, vget_high_s32(dQ), 1), 16), v255));
            if (need_z) /* usat(zq + i * zdq >> 8, 16) */
                vst1_u16(sh[4] + k, vqshrun_n_s32(vmlaq_n_s32(vdupq_n_s32(c->zq), I, c->zdq), 8));
        }
    }
    const uint16x4_t v255 = vdup_n_u16(255), v256 = vdup_n_u16(256), m8 = vdup_n_u16(0xff);
    const uint16x4_t F = { c->pfb, c->pfg, c->pfr, 0 };
    const uint16x4_t amin = vdup_n_u16((uint16_t)c->amin);
    const int amode = c->amode;
    for (int k = 0; k < nn; k += 4) {
        /* the taps: A = [a0..a3] (u0, v0), B (u1, v0), C (u0, v1), D (u1, v1) */
        uint32x4_t A, B, C, D;
        if (!rr) { /* one 8-byte load a row */
            const uint32x4x2_t top = vuzpq_u32(vcombine_u32(vld1_u32(pa[k]), vld1_u32(pa[k + 1])),
                                               vcombine_u32(vld1_u32(pa[k + 2]), vld1_u32(pa[k + 3])));
            const uint32x4x2_t bot = vuzpq_u32(vcombine_u32(vld1_u32(pc[k]), vld1_u32(pc[k + 1])),
                                               vcombine_u32(vld1_u32(pc[k + 2]), vld1_u32(pc[k + 3])));
            A = top.val[0]; B = top.val[1]; C = bot.val[0]; D = bot.val[1];
        } else { /* one lane a tap */
            A = vld1q_lane_u32(pa[k + 3], vld1q_lane_u32(pa[k + 2], vld1q_lane_u32(pa[k + 1], vld1q_dup_u32(pa[k]), 1), 2), 3);
            B = vld1q_lane_u32(pb[k + 3], vld1q_lane_u32(pb[k + 2], vld1q_lane_u32(pb[k + 1], vld1q_dup_u32(pb[k]), 1), 2), 3);
            C = vld1q_lane_u32(pc[k + 3], vld1q_lane_u32(pc[k + 2], vld1q_lane_u32(pc[k + 1], vld1q_dup_u32(pc[k]), 1), 2), 3);
            D = vld1q_lane_u32(pd[k + 3], vld1q_lane_u32(pd[k + 2], vld1q_lane_u32(pd[k + 1], vld1q_dup_u32(pd[k]), 1), 2), 3);
        }
        /* bytes to channels: ab.val[0] = [B of a x4, of b x4 | G ...], ab.val[1] = [R ... | A ...] */
        const uint8x16x2_t ab1 = vuzpq_u8(vreinterpretq_u8_u32(A), vreinterpretq_u8_u32(B));
        const uint8x16x2_t ab = vuzpq_u8(ab1.val[0], ab1.val[1]);
        const uint8x16x2_t cd1 = vuzpq_u8(vreinterpretq_u8_u32(C), vreinterpretq_u8_u32(D));
        const uint8x16x2_t cd = vuzpq_u8(cd1.val[0], cd1.val[1]);
        /* the weights: fu, 256 - fu along u; fv, 256 - fv along v */
        uint32_t f4;
        memcpy(&f4, fu8 + k, 4);
        const uint16x4_t fu = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(f4))));
        memcpy(&f4, fv8 + k, 4);
        const uint16x4_t fv = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(f4))));
        const uint16x8_t Wu = vcombine_u16(vsub_u16(v256, fu), fu);
        const uint16x4_t gv = vsub_u16(v256, fv);
        uint16x4_t TA = v255;
        if (need_ta) TA = q_bil(vget_high_u8(ab.val[1]), vget_high_u8(cd.val[1]), Wu, gv, fv);
        uint16x4_t MB, MG, MR;
        if (need_rgb) {
            const uint16x4_t TB = q_bil(vget_low_u8(ab.val[0]), vget_low_u8(cd.val[0]), Wu, gv, fv);
            const uint16x4_t TG = q_bil(vget_high_u8(ab.val[0]), vget_high_u8(cd.val[0]), Wu, gv, fv);
            const uint16x4_t TR = q_bil(vget_low_u8(ab.val[1]), vget_low_u8(cd.val[1]), Wu, gv, fv);
            if (kind == Q_DECAL) { /* div255(t * ta + s * (255 - ta)) */
                const uint16x4_t iTA = vsub_u16(v255, TA);
                MB = q_div255(vmla_u16(vmul_u16(TB, TA), vld1_u16(sh[0] + k), iTA));
                MG = q_div255(vmla_u16(vmul_u16(TG, TA), vld1_u16(sh[1] + k), iTA));
                MR = q_div255(vmla_u16(vmul_u16(TR, TA), vld1_u16(sh[2] + k), iTA));
            } else if (kind == Q_MOD) { /* div255(t * s) */
                MB = q_div255(vmul_u16(TB, vld1_u16(sh[0] + k)));
                MG = q_div255(vmul_u16(TG, vld1_u16(sh[1] + k)));
                MR = q_div255(vmul_u16(TR, vld1_u16(sh[2] + k)));
            } else {
                MB = TB; MG = TG; MR = TR;
            }
        } else { /* the shade, the texture giving only its alpha */
            MB = vld1_u16(sh[0] + k); MG = vld1_u16(sh[1] + k); MR = vld1_u16(sh[2] + k);
        }
        const uint16x4_t SA = need_sa ? vld1_u16(sh[3] + k) : v255;
        if (fog) {   /* div255(c * (255 - sa) + fog * sa) */
            const uint16x4_t iSA = vsub_u16(v255, SA);
            MB = q_div255(vmla_lane_u16(vmul_u16(MB, iSA), SA, F, 0));
            MG = q_div255(vmla_lane_u16(vmul_u16(MG, iSA), SA, F, 1));
            MR = q_div255(vmla_lane_u16(vmul_u16(MR, iSA), SA, F, 2));
        }
        uint32_t w4;
        memcpy(&w4, vis + k, 4);
        const int i0 = (int)(w4 & 0xff), i3 = (int)(w4 >> 24);
        const bool run = w4 == (uint32_t)i0 * 0x01010101u + 0x03020100u; /* four neighbours (the padding repeats one) */
        uint32_t *const d0 = dst + i0, *const d1 = dst + ((w4 >> 8) & 0xff), *const d2 = dst + ((w4 >> 16) & 0xff), *const d3 = dst + i3;
        uint32x4_t old = vdupq_n_u32(0), keep = vdupq_n_u32(~0u);
        if (alpha) { /* fa, the alpha test, then div255(c * fa + d * (255 - fa)) */
            const uint16x4_t FA = amode == 0 ? SA : amode == 1 ? TA : q_div255(vmul_u16(TA, SA));
            keep = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(vcge_u16(FA, amin))));
            old = run ? vld1q_u32(d0) : vld1q_lane_u32(d3, vld1q_lane_u32(d2, vld1q_lane_u32(d1, vld1q_dup_u32(d0), 1), 2), 3);
            const uint16x4_t iFA = vsub_u16(v255, FA);
            MB = q_div255(vmla_u16(vmul_u16(MB, FA), vand_u16(vmovn_u32(old), m8), iFA));
            MG = q_div255(vmla_u16(vmul_u16(MG, FA), vand_u16(vshrn_n_u32(old, 8), m8), iFA));
            MR = q_div255(vmla_u16(vmul_u16(MR, FA), vand_u16(vshrn_n_u32(old, 16), m8), iFA));
        }
        /* to ARGB8888 words: bytes B G R 255 per pixel */
        const uint8x8x2_t z1 = vzip_u8(vmovn_u16(vcombine_u16(MB, MG)), vmovn_u16(vcombine_u16(MR, v255)));
        const uint8x8x2_t z2 = vzip_u8(z1.val[0], z1.val[1]);
        uint32x4_t out = vreinterpretq_u32_u8(vcombine_u8(z2.val[0], z2.val[1]));
        if (alpha) out = vbslq_u32(keep, out, old); /* a rejected pixel keeps its colour */
        if (run) vst1q_u32(d0, out);
        else { vst1q_lane_u32(d0, out, 0); vst1q_lane_u32(d1, out, 1); vst1q_lane_u32(d2, out, 2); vst1q_lane_u32(d3, out, 3); }
        if (need_z) { /* the depth of the kept pixels */
            uint16_t *const z0 = zb + i0, *const z1p = zb + ((w4 >> 8) & 0xff), *const z2p = zb + ((w4 >> 16) & 0xff), *const z3 = zb + i3;
            const uint16x4_t oz = run ? vld1_u16(z0) : vld1_lane_u16(z3, vld1_lane_u16(z2p, vld1_lane_u16(z1p, vld1_dup_u16(z0), 1), 2), 3);
            const uint16x4_t nz = vbsl_u16(vmovn_u32(keep), vld1_u16(sh[4] + k), oz);
            if (run) vst1_u16(z0, nz);
            else { vst1_lane_u16(z0, nz, 0); vst1_lane_u16(z1p, nz, 1); vst1_lane_u16(z2p, nz, 2); vst1_lane_u16(z3, nz, 3); }
        }
    }
}
static void __attribute__((noinline)) quad_span(const QuadSpan *cp, const uint8_t *vis, int n, uint32_t *dst, uint16_t *zb, int kind,
                                                bool alpha, bool fog, bool rr) {
    const QuadSpan c = *cp;
#define QR(K, A, F) if (rr) quad_loop(&c, vis, n, dst, zb, K, A, F, 1); else quad_loop(&c, vis, n, dst, zb, K, A, F, 0);
#define QK(K) if (alpha) { QR(K, 1, 0) } else if (fog) { QR(K, 0, 1) } else { QR(K, 0, 0) }
    if (kind == Q_DECAL) { QK(Q_DECAL) }
    else if (kind == Q_MOD) { QK(Q_MOD) }
    else if (kind == Q_TEX) { QK(Q_TEX) }
    else { QR(Q_SHADEA, 1, 0) } /* (always translucent) */
#undef QK
#undef QR
}
#endif
#ifdef HOT_NEON
/* (round 13c: out of hot_span's line, which it cost ~6% while off: the compiler built the struct
 * on every span) */
static void __attribute__((noinline, cold)) quad_from_hot(const HotSpan *h, uint8_t *vis, int n, uint32_t *dst, uint16_t *zb,
                                                         int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int32_t zq0, int32_t zdq,
                                                         int32_t rq, int32_t gq, int32_t bq, int32_t aq,
                                                         int32_t rdq, int32_t gdq, int32_t bdq, int32_t adq, bool inside) {
    const QuadSpan q = { h->px, h->tw, h->mu, h->mv, uf - 0x8000, vf - 0x8000, duf, dvf, bq, gq, rq, aq, bdq, gdq, rdq, adq, zq0, zdq,
                         (uint16_t)h->pfb, (uint16_t)h->pfg, (uint16_t)h->pfr, h->amode, h->amin, h->zwrite };
    for (int k = n; k & 3; k++) vis[k] = vis[n - 1]; /* whole fours: the last pixel again */
    quad_span(&q, vis, n, dst, zb, h->kind == HOT_DECAL ? Q_DECAL : h->kind == HOT_MOD ? Q_MOD : h->kind == HOT_TEX ? Q_TEX : Q_SHADEA,
              h->alpha, h->postfog, !inside);
}
#endif
int gfx_soft_swar SAVESTATE_EXCLUDE = 1; /* FZX_SWAR=0: the opaque loops one channel at a time (A/B; round 13d) */
static void __attribute__((noinline)) hot_span(const HotSpan *h, uint32_t *dst, uint16_t *zb, int span,
                                               int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int32_t zq, int32_t zdq,
                                               int32_t rq, int32_t gq, int32_t bq, int32_t aq,
                                               int32_t rdq, int32_t gdq, int32_t bdq, int32_t adq, uint32_t pm) {
    uint8_t vis[PERSP_SPAN + 4] __attribute__((aligned(4))); /* + quad_span's padding */
    uint32_t tx[PERSP_SPAN];
    int n = 0;
    const int32_t zq0 = zq;
    {
        const int zbias = h->zbias;
        if (pm != SPAN_FULL(span)) {
            /* a visible-only draw: only the pixels in pm, each as in the loops below */
            for (int i = 0; i < span; i++, zq += zdq) {
                if (!((pm >> i) & 1u)) continue;
                const int uz = clamp16_sh8(zq);
                if (h->ztest && uz > zb[i] + zbias) continue;
                if (h->zwrite && !h->alpha) zb[i] = (uint16_t)uz;
                vis[n++] = (uint8_t)i;
            }
        } else if (!h->ztest) {
            /* F-Zero X's track: Z_UPD without Z_CMP -- every pixel lands */
            if (h->zwrite && !h->alpha) {
                int i = 0;
#ifdef HOT_NEON
                /* four at a time: usat(q >> 8, 16) is vqshrun #8, lane for lane */
                const int32_t z0[4] = { zq, zq + zdq, zq + 2 * zdq, zq + 3 * zdq };
                int32x4_t Z = vld1q_s32(z0);
                const int32x4_t dZ = vdupq_n_s32(4 * zdq);
                for (; i + 4 <= span; i += 4, Z = vaddq_s32(Z, dZ)) vst1_u16(zb + i, vqshrun_n_s32(Z, 8));
                zq += i * zdq;
#endif
                for (; i < span; i++, zq += zdq) zb[i] = (uint16_t)clamp16_sh8(zq);
            }
            _Static_assert(PERSP_SPAN == 16, "kIdent lists PERSP_SPAN indices");
            static const uint8_t kIdent[PERSP_SPAN] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
            memcpy(vis, kIdent, sizeof(kIdent)); /* every pixel: 0, 1, 2, ... (a whole-array copy) */
            n = span;
        } else if (h->zwrite && !h->alpha) {
            for (int i = 0; i < span; i++, zq += zdq) {
                const int uz = clamp16_sh8(zq);
                if (uz > zb[i] + zbias) continue;
                zb[i] = (uint16_t)uz; vis[n++] = (uint8_t)i;
            }
        } else {
            for (int i = 0; i < span; i++, zq += zdq) {
                if (clamp16_sh8(zq) > zb[i] + zbias) continue;
                vis[n++] = (uint8_t)i;
            }
        }
    }
    STAT(if (pm == SPAN_FULL(span)) stat_zspan(h->ztest, span, n, zq0, zdq, zb, h->zbias);)
    if (n == 0) return;
    {
        const uint32_t *px = h->px; const int tw = h->tw;
        /* span-constant: does every bilinear footprint of the span lie inside
         * the texture? (Endpoints in 64 bits bound the linear sequence, and
         * inside means no int32 wrap either.) Every wrap mode is the identity
         * there, so the taps index directly. */
        const int64_t su0 = (int64_t)uf - 0x8000, su1 = su0 + (int64_t)(span - 1) * duf;
        const int64_t sv0 = (int64_t)vf - 0x8000, sv1 = sv0 + (int64_t)(span - 1) * dvf;
        const bool inside = h->bilin && (su0 < su1 ? su0 : su1) >= 0 && (su0 < su1 ? su1 : su0) < (int64_t)(tw - 1) * 65536 &&
                            (sv0 < sv1 ? sv0 : sv1) >= 0 && (sv0 < sv1 ? sv1 : sv0) < (int64_t)(h->th - 1) * 65536;
        const bool rr = h->ws == WRAP_REPEAT && h->wt == WRAP_REPEAT && h->mu >= 0 && h->mv >= 0;
        const bool cc = h->ws == WRAP_CLAMP && h->wt == WRAP_CLAMP;
#define HOT_BILIN(BLEND)                                                                               \
        if (inside) { \
            for (int k = 0; k < n; k++) { \
                const int i = vis[k]; \
                const int32_t su = uf + i * duf - 0x8000, sv = vf + i * dvf - 0x8000; \
                const uint32_t *r = px + (sv >> 16) * tw + (su >> 16); \
                tx[k] = BLEND(r[0], r[1], r[tw], r[tw + 1], (su >> 8) & 0xff, (sv >> 8) & 0xff); \
            } \
        } else if (rr) { \
            const int mu = h->mu, mv = h->mv; \
            for (int k = 0; k < n; k++) { \
                const int i = vis[k]; \
                const int32_t su = uf + i * duf - 0x8000, sv = vf + i * dvf - 0x8000; \
                const int iu0 = (su >> 16) & mu, iu1 = ((su >> 16) + 1) & mu; \
                const uint32_t *r0 = px + ((sv >> 16) & mv) * tw, *r1 = px + (((sv >> 16) + 1) & mv) * tw; \
                tx[k] = BLEND(r0[iu0], r0[iu1], r1[iu0], r1[iu1], (su >> 8) & 0xff, (sv >> 8) & 0xff); \
            } \
        } else if (cc) { \
            /* clamp/clamp: the usual case at a texture's edge (desktop count: \
             * 97% of the footprints that leave the texture) */ \
            const int wm = tw - 1, hm = h->th - 1; \
            for (int k = 0; k < n; k++) { \
                const int i = vis[k]; \
                const int32_t su = uf + i * duf - 0x8000, sv = vf + i * dvf - 0x8000; \
                const int x = su >> 16, y = sv >> 16; \
                const int iu0 = x < 0 ? 0 : (x > wm ? wm : x), iu1 = x + 1 < 0 ? 0 : (x + 1 > wm ? wm : x + 1); \
                const int iv0 = y < 0 ? 0 : (y > hm ? hm : y), iv1 = y + 1 < 0 ? 0 : (y + 1 > hm ? hm : y + 1); \
                const uint32_t *r0 = px + iv0 * tw, *r1 = px + iv1 * tw; \
                tx[k] = BLEND(r0[iu0], r0[iu1], r1[iu0], r1[iu1], (su >> 8) & 0xff, (sv >> 8) & 0xff); \
            } \
        }
        STAT(stat_bilin(h->bilin, inside, rr, cc, n, h->kind, h->alpha));
        STAT(stat_const(h->kind, h->th, duf, dvf, n));
#ifdef HOT_NEON
        if (sQuad < 0) { const char *e = getenv("FZX_QUAD"); sQuad = e && e[0] == '1'; }
        if (__builtin_expect(sQuad > 0, 0) && h->bilin && (inside || rr) && h->kind != HOT_TL2 && h->kind != HOT_TLERP &&
            (h->kind != HOT_SHADEA || h->alpha) && !(duf == 0 && (dvf == 0 || h->th == 1))) {
            quad_from_hot(h, vis, n, dst, zb, uf, vf, duf, dvf, zq0, zdq, rq, gq, bq, aq, rdq, gdq, bdq, adq, inside);
            return;
        }
        /* Fused passes 2 and 3 for the NEON combines (the track, the clouds, the scenery):
         * the blended texel stays in u16 lanes into the combine, instead of being narrowed
         * to tx[k], stored, reloaded and widened again. The same arithmetic, the same
         * order: blend4_neon_u16 is blend4_neon before its narrowing (lanes <= 255). */
        if (sHotNeon && sHotFuse && h->bilin && (inside || rr || cc) && !(duf == 0 && (dvf == 0 || h->th == 1)) &&
            ((!h->alpha && (h->kind == HOT_DECAL || h->kind == HOT_MOD || h->kind == HOT_TL2)) ||
             (h->alpha && sHotNeonA && (h->kind == HOT_DECAL || h->kind == HOT_MOD || h->kind == HOT_SHADEA)))) {
#define FUSE_TAPS_INSIDE \
                const int32_t su = uf + i * duf - 0x8000, sv = vf + i * dvf - 0x8000; \
                const uint16x4_t T = blend4_neon_in(px + (sv >> 16) * tw + (su >> 16), tw, (su >> 8) & 0xff, (sv >> 8) & 0xff);
#define FUSE_TAPS_RR \
                const int32_t su = uf + i * duf - 0x8000, sv = vf + i * dvf - 0x8000; \
                const int iu0 = (su >> 16) & mu, iu1 = ((su >> 16) + 1) & mu; \
                const uint32_t *r0 = px + ((sv >> 16) & mv) * tw, *r1 = px + (((sv >> 16) + 1) & mv) * tw; \
                const uint16x4_t T = blend4_neon_ld(r0 + iu0, r0 + iu1, r1 + iu0, r1 + iu1, (su >> 8) & 0xff, (sv >> 8) & 0xff);
#define FUSE_TAPS_CC \
                const int32_t su = uf + i * duf - 0x8000, sv = vf + i * dvf - 0x8000; \
                const int x = su >> 16, y = sv >> 16; \
                const int iu0 = x < 0 ? 0 : (x > wm ? wm : x), iu1 = x + 1 < 0 ? 0 : (x + 1 > wm ? wm : x + 1); \
                const int iv0 = y < 0 ? 0 : (y > hm ? hm : y), iv1 = y + 1 < 0 ? 0 : (y + 1 > hm ? hm : y + 1); \
                const uint32_t *r0 = px + iv0 * tw, *r1 = px + iv1 * tw; \
                const uint16x4_t T = blend4_neon_ld(r0 + iu0, r0 + iu1, r1 + iu0, r1 + iu1, (su >> 8) & 0xff, (sv >> 8) & 0xff);
#define FUSE_LOOP(TAPS, BODY) for (int k = 0; k < n; k++) { const int i = vis[k]; TAPS BODY }
#define FUSE_ALL(BODY) \
            if (inside) { FUSE_LOOP(FUSE_TAPS_INSIDE, BODY) } \
            else if (rr) { const int mu = h->mu, mv = h->mv; FUSE_LOOP(FUSE_TAPS_RR, BODY) } \
            else { const int wm = tw - 1, hm = h->th - 1; FUSE_LOOP(FUSE_TAPS_CC, BODY) }
            const int32_t a0[4] = { bq, gq, rq, aq }, d0[4] = { bdq, gdq, rdq, adq };
            const int32x4_t acc0 = vld1q_s32(a0), dacc = vld1q_s32(d0);
            const uint16x4_t v255 = vdup_n_u16(255), a255 = vcreate_u16(0x00FF000000000000ull);
            const bool decal = h->kind == HOT_DECAL;
            if (!h->alpha) {
                /* as the opaque NEON loop of pass 3 */
                const uint16_t f0[4] = { (uint16_t)h->pfb, (uint16_t)h->pfg, (uint16_t)h->pfr, 0 };
                const uint16x4_t F = vld1_u16(f0);
                const bool tl2 = h->kind == HOT_TL2, fog = h->postfog;
                const int16_t l2d0[4] = { (int16_t)h->l2db, (int16_t)h->l2dg, (int16_t)h->l2dr, 0 };
                const int32_t l2b0[4] = { h->l2bb, h->l2bg, h->l2br, 0 };
                const int16x4_t L2D = vld1_s16(l2d0);
                const int32x4_t L2B = vld1q_s32(l2b0), z4 = vdupq_n_s32(0);
                const uint32x4_t one4 = vdupq_n_u32(1);
                if (sHotPair && (inside || rr)) {
                    const FusePair c = { px, tw, h->mu, h->mv, uf, vf, duf, dvf, aq, adq, acc0, dacc, L2B, L2D, F };
                    fuse_pair(&c, h->kind, fog, !inside, vis, n, dst);
                    return;
                }
                FUSE_ALL(
                    const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(acc0, dacc, i), 16)), v255);
                    uint16x4_t M;
                    if (tl2) {
                        const int32x4_t P = vmull_s16(L2D, vreinterpret_s16_u16(T));
                        const uint32x4_t A = vreinterpretq_u32_s32(vabsq_s32(P));
                        const int32x4_t Q = vreinterpretq_s32_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(A, vshrq_n_u32(A, 8)), one4), 8));
                        const int32x4_t I = vaddq_s32(L2B, vbslq_s32(vcltq_s32(P, z4), vnegq_s32(Q), Q));
                        M = div255_u16x4(vmul_u16(vmovn_u32(vreinterpretq_u32_s32(I)), S));
                    }
                    else if (decal) { const uint16x4_t TA = vdup_lane_u16(T, 3); M = div255_u16x4(vmla_u16(vmul_u16(T, TA), S, vsub_u16(v255, TA))); }
                    else       M = div255_u16x4(vmul_u16(T, S));
                    if (fog) {
                        const int sa = clamp8_sh16(aq + i * adq);
                        if (sa) M = div255_u16x4(vmla_n_u16(vmul_n_u16(M, (uint16_t)(255 - sa)), F, (uint16_t)sa));
                    }
                    M = vorr_u16(M, a255);
                    vst1_lane_u32(dst + i, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
                )
            } else {
                /* as the translucent NEON loop of pass 3 */
                const bool shade = h->kind == HOT_SHADEA;
                const int amode = h->amode, amin = h->amin;
                FUSE_ALL(
                    const int sa = clamp8_sh16(aq + i * adq);
                    const int ta = amode ? (int)vget_lane_u16(T, 3) : 0;
                    const int fa = amode == 0 ? sa : amode == 1 ? ta : div255(ta * sa);
                    if (fa < amin) continue;
                    const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(acc0, dacc, i), 16)), v255);
                    uint16x4_t M;
                    if (shade)      M = S;
                    else if (decal) { const uint16x4_t TA = vdup_lane_u16(T, 3); M = div255_u16x4(vmla_u16(vmul_u16(T, TA), S, vsub_u16(v255, TA))); }
                    else            M = div255_u16x4(vmul_u16(T, S));
                    if (fa < 255) {
                        const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(dst[i]))));
                        M = div255_u16x4(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));
                    }
                    M = vorr_u16(M, a255);
                    vst1_lane_u32(dst + i, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
                    if (h->zwrite) zb[i] = (uint16_t)clamp16_sh8(zq0 + i * zdq);
                )
            }
#undef FUSE_ALL
#undef FUSE_LOOP
#undef FUSE_TAPS_INSIDE
#undef FUSE_TAPS_RR
#undef FUSE_TAPS_CC
            return;
        }
#endif
        if (h->bilin && (inside || rr || cc) && duf == 0 && (dvf == 0 || h->th == 1) && n > 1) {
            /* Every pixel samples the same footprint: u is still and so is v, or the texture
             * is one texel tall (its two rows are the same one, and blending a row with itself
             * gives it back whatever the vertical weight). The sky gradient's spans, 13.5k
             * pixels a race picture: one blend for the span. */
            const int nn = n;
            n = 1;
            tx[0] = 0; /* (written below; keeps -Wmaybe-uninitialized quiet) */
#ifdef HOT_NEON
            if (sHotNeon) { HOT_BILIN(blend4_neon) } else
#endif
            if (tap3_on()) { HOT_BILIN(blend3) } else { HOT_BILIN(blend4) }
            n = nn;
            for (int k = 1; k < n; k++) tx[k] = tx[0];
        } else if (h->bilin && (inside || rr || cc)) {
#ifdef HOT_NEON
            if (sHotNeon) { HOT_BILIN(blend4_neon) } else
#endif
            if (tap3_on()) { HOT_BILIN(blend3) } else { HOT_BILIN(blend4) }
#undef HOT_BILIN
        } else if (h->bilin) {
            const int th = h->th, ws = h->ws, wt = h->wt, mu = h->mu, mv = h->mv;
            for (int k = 0; k < n; k++) {
                const int i = vis[k];
                tx[k] = bilerp(px, false, tw, th, ws, wt, uf + i * duf, vf + i * dvf, mu, mv);
            }
        } else if (h->ws == WRAP_REPEAT && h->wt == WRAP_REPEAT && h->mu >= 0 && h->mv >= 0) {
            const int mu = h->mu, mv = h->mv;
            for (int k = 0; k < n; k++) {
                const int i = vis[k];
                tx[k] = px[(((vf + i * dvf) >> 16) & mv) * tw + (((uf + i * duf) >> 16) & mu)];
            }
        } else {
            const int th = h->th, ws = h->ws, wt = h->wt, mu = h->mu, mv = h->mv;
            for (int k = 0; k < n; k++) {
                const int i = vis[k];
                tx[k] = px[wrapf((vf + i * dvf) >> 16, th, wt, mv) * tw + wrapf((uf + i * duf) >> 16, tw, ws, mu)];
            }
        }
    }
    /* (3) combine + store: one loop per combiner kind, post-fog as in SPAN_LOOP */
#ifdef HOT_NEON
    if (sHotNeon && h->alpha && (h->kind == HOT_DECAL || h->kind == HOT_MOD || h->kind == HOT_SHADEA) && sHotNeonA) {
        /* Translucent (the clouds, 23k span pixels a race picture): HOT_OUT_1 in the same
         * lanes -- the alpha test as a scalar (fa from the shade and/or texel alpha), the
         * blend div255(c * fa + d * (255 - fa)) in u16 lanes (<= 255 * 255), the depth
         * written for kept pixels only. FZX_NEONA=0 runs the scalar loop (A/B). */
        const int32_t a0[4] = { bq, gq, rq, aq }, d0[4] = { bdq, gdq, rdq, adq };
        const int32x4_t acc0 = vld1q_s32(a0), dacc = vld1q_s32(d0);
        const uint16x4_t v255 = vdup_n_u16(255), a255 = vcreate_u16(0x00FF000000000000ull);
        const bool decal = h->kind == HOT_DECAL, shade = h->kind == HOT_SHADEA;
        const int amode = h->amode, amin = h->amin;
        for (int k = 0; k < n; k++) {
            const int i = vis[k];
            const uint32_t t = tx[k];
            const int sa = clamp8_sh16(aq + i * adq), ta = (int)(t >> 24);
            const int fa = amode == 0 ? sa : amode == 1 ? ta : div255(ta * sa);
            if (fa < amin) continue;
            const uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(t))));
            const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(acc0, dacc, i), 16)), v255);
            uint16x4_t M;
            if (shade)      M = S;
            else if (decal) { const uint16x4_t TA = vdup_lane_u16(T, 3); M = div255_u16x4(vmla_u16(vmul_u16(T, TA), S, vsub_u16(v255, TA))); }
            else            M = div255_u16x4(vmul_u16(T, S));
            if (fa < 255) {
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(dst[i]))));
                M = div255_u16x4(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));
            }
            M = vorr_u16(M, a255);
            vst1_lane_u32(dst + i, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
            if (h->zwrite) zb[i] = (uint16_t)clamp16_sh8(zq0 + i * zdq);
        }
        return;
    }
    if (sHotNeon && !h->alpha && (h->kind == HOT_DECAL || h->kind == HOT_MOD || h->kind == HOT_TL2)) {
        const int32_t a0[4] = { bq, gq, rq, aq }, d0[4] = { bdq, gdq, rdq, adq };
        const int32x4_t acc0 = vld1q_s32(a0), dacc = vld1q_s32(d0);
        const uint16_t f0[4] = { (uint16_t)h->pfb, (uint16_t)h->pfg, (uint16_t)h->pfr, 0 };
        const uint16x4_t F = vld1_u16(f0), v255 = vdup_n_u16(255), a255 = vcreate_u16(0x00FF000000000000ull);
        const bool decal = h->kind == HOT_DECAL, tl2 = h->kind == HOT_TL2, fog = h->postfog;
        /* texlerp2: (l2b + div255s(l2d * t)) * s / 255, l2d signed: the product in 32-bit
         * lanes (|l2d * t| <= 255 * 255), div255s as div255 of the magnitude with the sign
         * put back (truncation toward zero, as C's /), the sum a lerp in 0..255 */
        const int16_t l2d0[4] = { (int16_t)h->l2db, (int16_t)h->l2dg, (int16_t)h->l2dr, 0 };
        const int32_t l2b0[4] = { h->l2bb, h->l2bg, h->l2br, 0 };
        const int16x4_t L2D = vld1_s16(l2d0);
        const int32x4_t L2B = vld1q_s32(l2b0), z4 = vdupq_n_s32(0);
        const uint32x4_t one4 = vdupq_n_u32(1);
        for (int k = 0; k < n; k++) {
            const int i = vis[k];
            const uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(tx[k]))));
            const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(vmlaq_n_s32(acc0, dacc, i), 16)), v255);
            uint16x4_t M;
            if (tl2) {
                const int32x4_t P = vmull_s16(L2D, vreinterpret_s16_u16(T));
                const uint32x4_t A = vreinterpretq_u32_s32(vabsq_s32(P));
                const int32x4_t Q = vreinterpretq_s32_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(A, vshrq_n_u32(A, 8)), one4), 8));
                const int32x4_t I = vaddq_s32(L2B, vbslq_s32(vcltq_s32(P, z4), vnegq_s32(Q), Q));
                M = div255_u16x4(vmul_u16(vmovn_u32(vreinterpretq_u32_s32(I)), S));
            }
            else if (decal) { const uint16x4_t TA = vdup_lane_u16(T, 3); M = div255_u16x4(vmla_u16(vmul_u16(T, TA), S, vsub_u16(v255, TA))); }
            else       M = div255_u16x4(vmul_u16(T, S));
            if (fog) {
                const int sa = clamp8_sh16(aq + i * adq);
                if (sa) M = div255_u16x4(vmla_n_u16(vmul_n_u16(M, (uint16_t)(255 - sa)), F, (uint16_t)sa));
            }
            M = vorr_u16(M, a255);
            vst1_lane_u32(dst + i, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
        }
        return;
    }
#endif
/* Translucent: alpha test, blend, and the depth write only for kept pixels
 * (pass 1 left it to here), as SPAN_LOOP orders them. */
#define HOT_OUT_1                                                                                      \
        {   const int sa = clamp8_sh16(aq + i * adq), ta = t >> 24;                                    \
            const int fa = h->amode == 0 ? sa : h->amode == 1 ? ta : div255(ta * sa);                  \
            if (fa < h->amin) continue;                                                                \
            if (fa < 255) {                                                                            \
                const uint32_t d = dst[i];                                                             \
                fr = div255(fr * fa + (int)((d >> 16) & 0xff) * (255 - fa));                           \
                fg = div255(fg * fa + (int)((d >> 8) & 0xff) * (255 - fa));                            \
                fb = div255(fb * fa + (int)(d & 0xff) * (255 - fa));                                   \
            }                                                                                          \
            dst[i] = argb((uint8_t)fr, (uint8_t)fg, (uint8_t)fb, 255);                                 \
            if (h->zwrite) zb[i] = (uint16_t)clamp16_sh8(zq0 + i * zdq);                               \
        }
#define HOT_OUT_0                                                                                      \
        if (h->postfog) {                                                                              \
            const int sa = clamp8_sh16(aq + i * adq);                                                  \
            if (sa) { fr = div255(fr * (255 - sa) + h->pfr * sa); fg = div255(fg * (255 - sa) + h->pfg * sa); \
                      fb = div255(fb * (255 - sa) + h->pfb * sa); }                                    \
        }                                                                                              \
        dst[i] = argb((uint8_t)fr, (uint8_t)fg, (uint8_t)fb, 255);
#define HOT_LOOP(ALPHA, ...)                                                                                 \
    for (int k = 0; k < n; k++) {                                                                      \
        const int i = vis[k]; const uint32_t t = tx[k];                                                \
        const int tr = (t >> 16) & 0xff, tg = (t >> 8) & 0xff, tb = t & 0xff;                          \
        int fr, fg, fb;                                                                                \
        __VA_ARGS__                                                                                    \
        HOT_OUT_##ALPHA                                                                                \
    }
#define HOT_LOOPS(...) if (h->alpha) { HOT_LOOP(1, __VA_ARGS__) } else { HOT_LOOP(0, __VA_ARGS__) }
#define HOT_SHADE const int sr = clamp8_sh16(rq + i * rdq), sg = clamp8_sh16(gq + i * gdq), sb = clamp8_sh16(bq + i * bdq);
    /* Round 13c: the opaque loops with red and blue in the lanes of one word (lerp2's packing)
     * wherever both are multiplied by the same factor -- the decal's texel alpha and the post-fog
     * blend: div255 lane by lane is (x + 1 + (x >> 8)) >> 8 with every lane <= 255 * 255 + 255,
     * so nothing carries into the next lane and each result is div255's. The pixel goes out
     * whole (no unpacking for argb). */
#define SWAR_DIV255(x) ((((x) + 0x00010001u + (((x) >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu)
#define HOT_OUT_0W                                                                                     \
        if (h->postfog) {                                                                              \
            const int sa = clamp8_sh16(aq + i * adq);                                                  \
            if (sa) { const uint32_t x = frb * (uint32_t)(255 - sa) + pfrb * (uint32_t)sa;              \
                      frb = SWAR_DIV255(x); fg = div255(fg * (255 - sa) + h->pfg * sa); }                \
        }                                                                                              \
        dst[i] = 0xFF000000u | frb | ((uint32_t)fg << 8);
#define HOT_LOOP0W(...)                                                                                \
    for (int k = 0; k < n; k++) {                                                                      \
        const int i = vis[k]; const uint32_t t = tx[k];                                                \
        uint32_t frb; int fg;                                                                          \
        __VA_ARGS__                                                                                    \
        HOT_OUT_0W                                                                                     \
    }
    if (gfx_soft_swar && !h->alpha && (h->kind == HOT_MOD || h->kind == HOT_DECAL || h->kind == HOT_TL2)) {
        const uint32_t pfrb = (uint32_t)h->pfr << 16 | (uint32_t)h->pfb;
        if (h->kind == HOT_DECAL) {
            HOT_LOOP0W(HOT_SHADE (void)sr; const uint32_t ta = t >> 24;
                       const uint32_t x = (t & 0x00FF00FFu) * ta + ((uint32_t)sr << 16 | (uint32_t)sb) * (255 - ta);
                       frb = SWAR_DIV255(x); fg = div255((int)((t >> 8) & 0xff) * (int)ta + sg * (255 - (int)ta));)
        } else if (h->kind == HOT_MOD) {
            HOT_LOOP0W(HOT_SHADE frb = (uint32_t)div255((int)((t >> 16) & 0xff) * sr) << 16 | (uint32_t)div255((int)(t & 0xff) * sb);
                       fg = div255((int)((t >> 8) & 0xff) * sg);)
        } else {
            HOT_LOOP0W(HOT_SHADE const int tr = (t >> 16) & 0xff, tg = (t >> 8) & 0xff, tb = t & 0xff;
                       frb = (uint32_t)div255((h->l2br + div255s(h->l2dr * tr)) * sr) << 16 | (uint32_t)div255((h->l2bb + div255s(h->l2db * tb)) * sb);
                       fg = div255((h->l2bg + div255s(h->l2dg * tg)) * sg);)
        }
        return;
    }
#undef HOT_LOOP0W
#undef HOT_OUT_0W
#undef SWAR_DIV255
    switch (h->kind) {
    case HOT_MOD:
        HOT_LOOPS(HOT_SHADE fr = div255(tr * sr); fg = div255(tg * sg); fb = div255(tb * sb);)
        break;
    case HOT_TL2:
        HOT_LOOPS(HOT_SHADE fr = div255((h->l2br + div255s(h->l2dr * tr)) * sr); fg = div255((h->l2bg + div255s(h->l2dg * tg)) * sg);
                 fb = div255((h->l2bb + div255s(h->l2db * tb)) * sb);)
        break;
    case HOT_TEX:
        if (!h->alpha && !h->postfog) { /* the texel as it is (the sky): argb(tr, tg, tb, 255) */
            for (int k = 0; k < n; k++) dst[vis[k]] = tx[k] | 0xFF000000u;
            break;
        }
        HOT_LOOPS(fr = tr; fg = tg; fb = tb;)
        break;
    case HOT_SHADEA:
        HOT_LOOPS(HOT_SHADE fr = sr; fg = sg; fb = sb; (void)tr; (void)tg; (void)tb;)
        break;
    case HOT_TLERP:
        HOT_LOOPS(HOT_SHADE fr = h->lkr + div255s((sr - h->lkr) * tr); fg = h->lkg + div255s((sg - h->lkg) * tg);
                  fb = h->lkb + div255s((sb - h->lkb) * tb);)
        break;
    default: /* HOT_DECAL */
        HOT_LOOPS(HOT_SHADE const int ta = t >> 24; fr = div255(tr * ta + sr * (255 - ta)); fg = div255(tg * ta + sg * (255 - ta));
                 fb = div255(tb * ta + sb * (255 - ta));)
        break;
    }
#undef HOT_SHADE
#undef HOT_LOOPS
#undef HOT_LOOP
#undef HOT_OUT_0
#undef HOT_OUT_1
}
/* A whole row of hot spans (ocarina-of-time c935b73's per-row entry): the span
 * setup raster_tri does between hot_span calls -- the same float steps in the
 * same order, then the fixed-point conversions -- with the per-triangle
 * constants in one struct instead of ~18 arguments marshalled per span. In
 * raster_tri this setup spilled and paid an FPSCR transfer per clamp. */
typedef struct {
    HotSpan hs;
    uint32_t *fbuf; uint16_t *zb;
    bool persp;
    float zdx, crdx, cgdx, cbdx, cadx, uwdx, vwdx, swdx, ftw, fth;
    int32_t zdq, rdq, gdq, bdq, adq;
#ifdef HOT_NEON
    float32x4_t da, db, m;    /* FZX_SPANVEC: { zdx, crdx, cgdx, cbdx }, { cadx, uwdx, vwdx, swdx }, { ftw, ftw, fth, fth } */
#endif
} HotTri;
/* vr: the row's cut when an occluder after this triangle lies in it, else NULL */
static void __attribute__((noinline)) hot_row(const HotTri *t, const VisRow *vr, int idx, int x, int xe, float zf, float cr,
                                              float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    while (x < xe) {
        int span = xe - x;
        if (span > PERSP_SPAN) span = PERSP_SPAN;
        const float fspan = (float)span;
        const float sw_b = sw + t->swdx * fspan;
        float rw_b = 0.0f, u_a, v_a, du, dv;
        uint32_t pm = SPAN_FULL(span);
        if (vr) {
            pm = vis_span(vr, x, span);
            if (!pm) { rw_b = -1.0f; goto next; }
            sVisPx += (uint32_t)span;
        }
        if (t->persp) {
            if (rw < 0.0f) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
            rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
            u_a = uw * rw; v_a = vw * rw;
            const float nu = (uw + t->uwdx * fspan) * rw_b - u_a;
            const float nv = (vw + t->vwdx * fspan) * rw_b - v_a;
            if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); }
            else                    { du = nu / fspan;               dv = nv / fspan; }
        } else {
            u_a = uw; v_a = vw; du = t->uwdx; dv = t->vwdx;
        }
        if (span_skip(span, idx)) goto next; /* a resolve's: nothing pending here */
        int32_t uf, vf, duf, dvf, rq, gq, bq, aq;
#ifdef HOT_NEON
        {   /* to_fix16 = clamp +-16000, then (int32_t)(f * 65536) == vcvtq_n_s32_f32(f, 16)
             * (both truncate; out-of-range saturates on ARM either way) */
            const float fa[4] = { u_a * t->ftw, du * t->ftw, v_a * t->fth, dv * t->fth };
            const float fc[4] = { cr, cg, cb, ca };
            int32_t ia[4], ic[4];
            vst1q_s32(ia, vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vld1q_f32(fa), vdupq_n_f32(-16000.0f)), vdupq_n_f32(16000.0f)), 16));
            vst1q_s32(ic, vcvtq_n_s32_f32(vld1q_f32(fc), 16));
            uf = ia[0]; duf = ia[1]; vf = ia[2]; dvf = ia[3];
            rq = ic[0]; gq = ic[1]; bq = ic[2]; aq = ic[3];
        }
#else
        uf = to_fix16(u_a * t->ftw); duf = to_fix16(du * t->ftw);
        vf = to_fix16(v_a * t->fth); dvf = to_fix16(dv * t->fth);
        rq = sat_i32(cr * 65536.0f); gq = sat_i32(cg * 65536.0f); bq = sat_i32(cb * 65536.0f); aq = sat_i32(ca * 65536.0f);
#endif
        const int32_t zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
        hot_span(&t->hs, t->fbuf + idx, t->zb + idx, span, uf, vf, duf, dvf, zq, t->zdq, rq, gq, bq, aq, t->rdq, t->gdq, t->bdq, t->adq, pm);
    next:
        idx += span;
        zf += t->zdx * fspan;
        cr += t->crdx * fspan; cg += t->cgdx * fspan; cb += t->cbdx * fspan; ca += t->cadx * fspan;
        uw += t->uwdx * fspan;
        vw += t->vwdx * fspan;
        sw = sw_b; rw = rw_b;
        x += span;
    }
}
/* FZX_SPANVEC (2026-10-07, branch speed-2026-10-07, after super-mario-64's SM64_SPANVEC; default 1, ARM only): hot_row
 * with the row's float state as two NEON vectors, A = { z, r, g, b } and B = { a, u/w, v/w, 1/w }, and the steps as
 * HotTri's da, db. hot_row kept eight floats and their eight steps, which GCC spilled around every hot_span call (the
 * steps reloaded from the HotTri, the state stored and reloaded). The same operations in the same order, lane by
 * lane: the next span's start is A + da * span and B + db * span (vmla: the product rounded, then the sum, as the
 * VFP's multiply and add round them under -ffp-contract=off), and (u/w)' is that same lane; the perspective ends
 * u/w * (1/w) and (u/w)' * (1/w)' - u, and the 1/16 step, as two-lane multiplies and a subtract; the divides (1/w and
 * a short span's step) stay VFP divides; the fixed-point conversions as hot_row makes them (vcvt #16). NEON flushes
 * denormals to zero where the VFP keeps them: no value here comes near one (screen and texture coordinates, 1/w,
 * colours 0..255, depth 0..1 and their per-pixel steps are each 0 or far above 1e-30, and a sum of two of them is 0
 * or at least the smaller one's ulp). 0: hot_row. Same pixels. */
int gfx_soft_spanvec SAVESTATE_EXCLUDE = -1;
#ifdef HOT_NEON
static void __attribute__((noinline)) hot_row_v(const HotTri *t, const VisRow *vr, int idx, int x, int xe, float32x4_t A, float32x4_t B,
                                                float rw) {
    const float32x4_t DA = t->da, DB = t->db, M = t->m;
    while (x < xe) {
        int span = xe - x;
        if (span > PERSP_SPAN) span = PERSP_SPAN;
        const float fspan = (float)span;
        const float32x4_t An = vmlaq_f32(A, DA, vdupq_n_f32(fspan)), Bn = vmlaq_f32(B, DB, vdupq_n_f32(fspan)); /* the next span's start */
        const float sw_b = vgetq_lane_f32(Bn, 3);
        float rw_b = 0.0f;
        uint32_t pm = SPAN_FULL(span);
        if (vr) {
            pm = vis_span(vr, x, span);
            if (!pm) { rw_b = -1.0f; goto next; }
            sVisPx += (uint32_t)span;
        }
        {
            const float zf = vgetq_lane_f32(A, 0);
            const int32_t zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            const float32x2_t uv = vget_low_f32(vextq_f32(B, B, 1)); /* { u/w, v/w } */
            float32x2_t ua, du;                                        /* { u_a, v_a }, { du, dv } */
            if (t->persp) {
                if (rw < 0.0f) { const float sw = vgetq_lane_f32(B, 3); rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f; }
                rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
                ua = vmul_f32(uv, vdup_n_f32(rw));
                const float32x2_t nuv = vsub_f32(vmul_f32(vget_low_f32(vextq_f32(Bn, Bn, 1)), vdup_n_f32(rw_b)), ua);
                if (span == PERSP_SPAN) du = vmul_f32(nuv, vdup_n_f32(1.0f / PERSP_SPAN));
                else du = vset_lane_f32(vget_lane_f32(nuv, 1) / fspan, vdup_n_f32(vget_lane_f32(nuv, 0) / fspan), 1);
            } else {
                ua = uv; du = vget_low_f32(vextq_f32(DB, DB, 1)); /* { uwdx, vwdx } */
            }
            if (span_skip(span, idx)) goto next; /* a resolve's: nothing pending here */
            const float32x2x2_t z = vzip_f32(ua, du);
            const float32x4_t a = vcombine_f32(z.val[0], z.val[1]); /* { u_a, du, v_a, dv } */
            const int32x4_t o = vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vmulq_f32(a, M), vdupq_n_f32(-16000.0f)), vdupq_n_f32(16000.0f)), 16);
            const int32x4_t oc = vcvtq_n_s32_f32(vextq_f32(A, B, 1), 16); /* { r, g, b, a } */
            hot_span(&t->hs, t->fbuf + idx, t->zb + idx, span, vgetq_lane_s32(o, 0), vgetq_lane_s32(o, 2), vgetq_lane_s32(o, 1),
                     vgetq_lane_s32(o, 3), zq, t->zdq, vgetq_lane_s32(oc, 0), vgetq_lane_s32(oc, 1), vgetq_lane_s32(oc, 2),
                     vgetq_lane_s32(oc, 3), t->rdq, t->gdq, t->bdq, t->adq, pm);
        }
    next:
        idx += span;
        A = An; B = Bn; rw = rw_b;
        x += span;
    }
}
#endif
int gfx_soft_hotspan SAVESTATE_EXCLUDE = -1; /* FZX_HOTSPAN=0 disables (A/B) */
static int sHotRow SAVESTATE_EXCLUDE = -1;   /* FZX_HOTROW=0: hot spans from raster_tri's own span loop (A/B) */

/* Triangle coverage, shared by raster_tri and the deferred backgrounds (def_cover): the
 * clipped bounding box, the edge constants and each row's x-span, so both see exactly
 * the same pixels. tri_edges returns false when the triangle covers none. */
/* FZX_RCPROW=1: tri_row's reciprocals (round 13; off since the 13c device bench: +0.15 ms a picture,
 * more instructions than the divides they save on the A7) */
int gfx_soft_rcprow SAVESTATE_EXCLUDE = -1;
typedef struct {
    float area, inv_area, w0dx, w1dx, w2dx;
    float r0dx, r1dx, r2dx; /* 1 / w_i dx (0 where w_i dx is 0): tri_row's quotients as products */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y;
    int minx, maxx, miny, maxy;
    bool own; OwnTri oe; /* FZX_FILLRULE: the owned-edge test (see own_edge) */
} TriEdges;
static inline __attribute__((always_inline)) bool tri_edges(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, TriEdges *t) {
    const float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) return false; /* (winding cull happens in gfx_sp_tri1; the GE cull mode is never set here) */

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
    const int minx = imax(imax(imax((int)fmin3(v0->x, v1->x, v2->x), sScX0), vpx0), 0);
    const int maxx = imin(imin(imin(iceilf (fmax3(v0->x, v1->x, v2->x)), sScX1), vpx1), sW);
    const int miny = imax(imax(imax((int)fmin3(v0->y, v1->y, v2->y), sScY0), vpy0), 0);
    const int maxy = imin(imin(imin(iceilf (fmax3(v0->y, v1->y, v2->y)), sScY1), vpy1), sH);
    if (minx >= maxx || miny >= maxy) return false;
    t->minx = minx; t->maxx = maxx; t->miny = miny; t->maxy = maxy;
    t->area = area;
    /* Barycentrics = edge/area with SIGNED area: inside is all-w >= 0 for
     * either winding, no separate sign handling needed. (Divide sits after
     * the bbox reject so off-screen triangles don't pay for it.) */
    const float inv_area = 1.0f / area;
    t->inv_area = inv_area;
    /* Barycentric x-gradients (normalized by the signed area, so inside is
     * simply all-w >= 0 whichever way the triangle winds). */
    t->w0dx = (v2->y - v1->y) * inv_area;
    t->w1dx = (v0->y - v2->y) * inv_area;
    t->w2dx = (v1->y - v0->y) * inv_area;
    t->r0dx = t->w0dx != 0.0f ? 1.0f / t->w0dx : 0.0f;
    t->r1dx = t->w1dx != 0.0f ? 1.0f / t->w1dx : 0.0f;
    t->r2dx = t->w2dx != 0.0f ? 1.0f / t->w2dx : 0.0f;
    /* Row edge bases (device round 13): edge(a,b,fx,py) is
     *   (fx - a.x)*(b.y - a.y) - (py - a.y)*(b.x - a.x)
     * and fx never changes with y, so the first product is a whole-triangle
     * constant and (b.x - a.x) is triangle-invariant. Per row only the second
     * product and the final subtract/scale remain -- the SAME operations in the
     * same order as edge() (bit-identical, still no row-to-row accumulation, so
     * shared edges stay crack-free), minus 12 vertex loads + 9 subs + 3 muls. */
    const float fx = minx + 0.5f;
    t->e0c = (fx - v1->x) * (v2->y - v1->y); t->e0x = v2->x - v1->x; t->e0y = v1->y; /* edge(v1,v2) */
    t->e1c = (fx - v2->x) * (v0->y - v2->y); t->e1x = v0->x - v2->x; t->e1y = v2->y; /* edge(v2,v0) */
    t->e2c = (fx - v0->x) * (v1->y - v0->y); t->e2x = v1->x - v0->x; t->e2y = v0->y; /* edge(v0,v1) */
    t->own = sOwnTri;
    if (t->own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&t->oe.e[0], v1, v2, s); own_edge(&t->oe.e[1], v2, v0, s); own_edge(&t->oe.e[2], v0, v1, s);
    }
    return true;
}
/* Row y's x-span [*pxs, *pxe) and its barycentric bases; false if the row is empty. */
static inline __attribute__((always_inline)) bool tri_row(const TriEdges *t, int y, int *pxs, int *pxe, float *pb0, float *pb1, float *pb2) {
    const float py = y + 0.5f;
    /* Row bases recomputed exactly each row (no row-to-row drift). */
    const float b0 = (t->e0c - (py - t->e0y) * t->e0x) * t->inv_area;
    const float b1 = (t->e1c - (py - t->e1y) * t->e1x) * t->inv_area;
    const float b2 = (t->e2c - (py - t->e2y) * t->e2x) * t->inv_area;

    /* Solve b_i + (x - minx) * w_i_dx >= 0 for the pixel interval. */
    const int minx = t->minx;
    const float w0dx = t->w0dx, w1dx = t->w1dx, w2dx = t->w2dx;
    int xs = minx, xe = t->maxx;
    {
        const float bs[3] = { b0, b1, b2 }, ds[3] = { w0dx, w1dx, w2dx }, rs[3] = { t->r0dx, t->r1dx, t->r2dx };
        for (int i = 0; i < 3; i++) {
            /* Round 13: the quotient -bs/ds as -bs * (1/ds) (a divide is ~18 cycles on the A7, and
             * this ran two or three times a row, in both visible-only passes). The product is
             * within 2 ulps of the quotient, so wherever the rounded bound is the same integer
             * across +-(|q| * 1e-6 + 1e-6) around it -- nearly always -- it is the divide's
             * bound; otherwise (and near the 1e6 clamp) the divide decides, as before. */
            if (gfx_soft_rcprow <= 0) {
            } else if (ds[i] > 0.0f && bs[i] < 0.0f) {
                /* ceil is the same across the window iff ceil(low end) >= the high end */
                const float qq = -bs[i] * rs[i] - 1e-5f, m = qq * 1e-6f + 1e-6f;
                const int c0 = iceilf(qq - m);
                if (qq + m < 1e6f && (float)c0 >= qq + m) {
                    const int a = minx + c0;
                    if (a > xs) xs = a;
                    continue;
                }
            } else if (ds[i] < 0.0f) {
                const float qq = -bs[i] * rs[i] + 1e-5f, m = (qq < 0.0f ? -qq : qq) * 1e-6f + 1e-6f;
                const int f0 = ifloorf(qq + m);
                if (qq + m < 1e6f && qq - m > -1e6f && (float)f0 <= qq - m) {
                    const int a = minx + 1 + f0;
                    if (a < xe) xe = a;
                    continue;
                }
            }
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
    if (xs >= xe) return false;
    if (t->own) { /* FZX_FILLRULE: the row's ends by the owned-edge test, one pixel either side (both solves above) */
        xs = xs > minx ? xs - 1 : minx; xe = xe < t->maxx ? xe + 1 : t->maxx;
        while (xs < xe && !own_px(&t->oe, xs, py)) xs++;
        while (xe > xs && !own_px(&t->oe, xe - 1, py)) xe--;
    } else
    /* Snap against rounding from the divisions above so the first pixel
     * really is inside per the linear edge functions. */
    while (xs < xe) {
        const float o = (float)(xs - minx);
        if (b0 + o * w0dx >= 0 && b1 + o * w1dx >= 0 && b2 + o * w2dx >= 0) break;
        xs++;
    }
    if (xs >= xe) return false;
    *pxs = xs; *pxe = xe; *pb0 = b0; *pb1 = b1; *pb2 = b2;
    return true;
}
/* FZX_FASTROWS (2026-10-07, branch speed-2026-10-07, after super-mario-64's SM64_FASTROWS; default 1, ARM only): the
 * rows of a hot triangle (every row to hot_row) in a small function of their own, tri_rows, instead of raster_tri's
 * loop, which keeps the generic span loops' ~40 values live and reloaded or spilled most of them every row. The same
 * row bases, the same edge solve (tri_row's, with the divides; the edge kinds -- w_i_dx > 0, < 0, == 0 -- taken once
 * a triangle), the same snap and the same occluder-cut scan; the snap's three tests b_i + o * w_i_dx >= 0 in three
 * NEON lanes, and the row's start values (w0*a0 + w1*a1 + w2*a2 for depth, shade, u/w, v/w, 1/w: 24 multiplies and 16
 * adds) as NEON lanes, multiply then add, each rounded as the VFP rounds them (-ffp-contract=off). NEON flushes
 * denormals to zero: no value here comes near one (see FZX_SPANVEC). Not for owned edges (FZX_FILLRULE) or
 * FZX_RCPROW=1: raster_tri's loop. 0: raster_tri's loop. Same pixels. */
int gfx_soft_fastrows SAVESTATE_EXCLUDE = -1;
#ifdef HOT_NEON
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's { z, r, g, b } and { a, u/w, v/w, 1/w } */
    float32x4_t fa, fb;                       /* flat: { -, r0, g0, b0 }, { a0, -, -, - } in place of the sums */
    const TriEdges *te;
    int ek[3];                                /* each edge's w_i_dx: 0 > 0, 1 < 0, 2 == 0 */
    int vec;                                  /* FZX_SPANVEC: hot_row_v */
    const uint16_t *cut; uint16_t cid;        /* a visible-only draw's (raster_tri's cut, cid) */
    const HotTri *ht;
} TriRows;
/* x < 0.0f for a float that is not NaN, from its bits (no VFP compare and its vmrs wait). Nothing here is NaN: the
 * vertices are finite and the area is not 0. -0.0f is not negative, as for the compare. */
static inline int f_neg(float f) { uint32_t u; memcpy(&u, &f, 4); return u > 0x80000000u; }
/* Rows miny..maxy-1 of the triangle, each as raster_tri's loop draws a hot row. Returns the span pixels drawn outside
 * an occluder cut (raster_tri's tri_px). */
static inline __attribute__((always_inline)) uint32_t tri_rows_t(const TriRows *T, const int FLAT) {
    const TriEdges *const te = T->te;
    const float e0c = te->e0c, e0x = te->e0x, e0y = te->e0y, e1c = te->e1c, e1x = te->e1x, e1y = te->e1y;
    const float e2c = te->e2c, e2x = te->e2x, e2y = te->e2y, inv_area = te->inv_area;
    const float w0dx = te->w0dx, w1dx = te->w1dx, w2dx = te->w2dx;
    const int minx = te->minx, maxx = te->maxx, maxy = te->maxy, ek0 = T->ek[0], ek1 = T->ek[1], ek2 = T->ek[2];
    const uint16_t *const cut = T->cut; const uint16_t cid = T->cid;
    const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };
    const int W = sW;
    uint32_t px = 0;
    for (int y = te->miny; y < maxy; y++) {
#if defined(MIYOO)
        if (sRowSplit && ((y ^ sRowPar) & 1)) continue; /* the other core's row (vis_flush) */
#endif
        if (cut && sCutRowMin[y] > cid) continue; /* a visible-only draw: all of the row is painted over later */
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
        /* tri_row's solve, as it is there (FZX_RCPROW=0), its ds tests made once a triangle */
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
        } else if (f_neg(BS)) continue; /* the row outside this edge: empty (tri_row's xs = xe) */
        TRI_EDGE(ek0, b0, w0dx)
        TRI_EDGE(ek1, b1, w1dx)
        TRI_EDGE(ek2, b2, w2dx)
#undef TRI_EDGE
        if (xs >= xe) continue;
        /* the snap: b_i + (xs - minx) * w_i_dx >= 0 for all three, in three lanes (vmla is not fused: the VFP's sums);
         * W ends as raster_tri's w0, w1, w2 at the row's first pixel */
        const float32x4_t B3 = vcombine_f32(vset_lane_f32(b1, vdup_n_f32(b0), 1), vset_lane_f32(0.0f, vdup_n_f32(b2), 1));
        float32x4_t Wb;
        for (;;) {
            Wb = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
            const uint32x4_t ge = vcgeq_f32(Wb, vdupq_n_f32(0.0f));
            const uint32x2_t g2 = vpmin_u32(vget_low_u32(ge), vget_high_u32(ge));
            if (vget_lane_u32(vpmin_u32(g2, g2), 0)) break;
            if (++xs >= xe) break;
        }
        if (xs >= xe) continue;
        /* a visible-only draw: as raster_tri */
        VisRow vrow;
        const VisRow *rcut = NULL;
        if (cut && cid < sCutRowMax[y]) {
            vrow.cut = cut + y * W; vrow.bmin = sCutBMin + y * sCutBW; vrow.bmax = sCutBMax + y * sCutBW; vrow.id = cid;
            int x = xs;
            if (gfx_soft_visneon) {
                const uint16x8_t I = vdupq_n_u16(cid);
                while (x < xe) {
                    if (vrow.bmin[x >> 4] > cid) { x = ((x >> 4) + 1) << 4; continue; }
                    if (x + 8 <= xe) {
                        const uint16x8_t le = vcleq_u16(vld1q_u16(vrow.cut + x), I);
                        if (vget_lane_u64(vreinterpret_u64_u16(vorr_u16(vget_low_u16(le), vget_high_u16(le))), 0)) break;
                        x += 8;
                        continue;
                    }
                    if (vrow.cut[x] > cid) { x++; continue; }
                    break;
                }
            } else
            while (x < xe) {
                if (vrow.bmin[x >> 4] > cid) { x = ((x >> 4) + 1) << 4; continue; }
                if (vrow.cut[x] > cid) { x++; continue; }
                break;
            }
            if (x >= xe) continue;
            rcut = &vrow;
        } else {
            px += (uint32_t)(xe - xs);
        }
        STAT(stat_row(true, rcut != NULL, false);)
        /* w0*z0 + w1*z1 + w2*z2 for all eight */
        float32x4_t A = vmulq_lane_f32(T->va0, vget_low_f32(Wb), 0), B = vmulq_lane_f32(T->vb0, vget_low_f32(Wb), 0);
        A = vmlaq_lane_f32(A, T->va1, vget_low_f32(Wb), 1); B = vmlaq_lane_f32(B, T->vb1, vget_low_f32(Wb), 1);
        A = vmlaq_lane_f32(A, T->va2, vget_high_f32(Wb), 0); B = vmlaq_lane_f32(B, T->vb2, vget_high_f32(Wb), 0);
        if (FLAT) { /* the shade is the vertex colour */
            static const uint32_t ma[4] = { 0, ~0u, ~0u, ~0u }, mb[4] = { ~0u, 0, 0, 0 };
            A = vbslq_f32(vld1q_u32(ma), T->fa, A); B = vbslq_f32(vld1q_u32(mb), T->fb, B);
        }
        const float sw = vgetq_lane_f32(B, 3);
        float rw = 0.0f;
        if (T->ht->persp) rw = rcut ? -1.0f : (sw > 1e-12f) ? 1.0f / sw : 0.0f;
        const int idx = y * W + xs;
        if (T->vec) hot_row_v(T->ht, rcut, idx, xs, xe, A, B, rw);
        else hot_row(T->ht, rcut, idx, xs, xe, vgetq_lane_f32(A, 0), vgetq_lane_f32(A, 1), vgetq_lane_f32(A, 2), vgetq_lane_f32(A, 3),
                     vgetq_lane_f32(B, 0), vgetq_lane_f32(B, 1), vgetq_lane_f32(B, 2), sw, rw);
    }
    return px;
}
static uint32_t __attribute__((noinline)) tri_rows_f(const TriRows *T) { return tri_rows_t(T, 1); }
static uint32_t __attribute__((noinline)) tri_rows_s(const TriRows *T) { return tri_rows_t(T, 0); }
#endif
/* A textured draw without a texture comes out as a flat block of the combiner's colour (OoT's
 * red sunset bar, a stale texture after a mid-frame cache reset). Said on the console, 4 times a
 * session; every time under FZX_TEXSTRESS (the stress sweeps count them). */
static void untex_note(const char *where) {
    static int sLogs SAVESTATE_EXCLUDE, sAll SAVESTATE_EXCLUDE = -1;
    if (sAll < 0) sAll = getenv("FZX_TEXSTRESS") != NULL;
    if (sLogs < 4 || sAll) {
        sLogs++;
        port_log("gfx: untextured draw (%s): tex %u not valid (bound %u/%u, arena %d%%)\n", where, (unsigned)sBoundTex[0],
                 (unsigned)sBoundTex[0], (unsigned)sBoundTex[1], (int)(sArenaUsed * 100 / TEXARENA_SZ));
    }
}
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
    STAT(const uint32_t stat_w0 = sWinPx;) /* a resolve counts the spans it drew */
    const uint16_t *const cut = sCutBuf; const uint16_t cid = sCutId; /* a visible-only draw's */
    const uint32_t vis0 = sVisPx;
    TriEdges te;
    if (!tri_edges(v0, v1, v2, &te)) return -1;
    const float area = te.area;
    const int minx = te.minx, miny = te.miny, maxy = te.maxy;

    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { untex_note("tri"); tex = NULL; }
    const int tmask_u = tex ? pow2mask(tex->w) : -1, tmask_v = tex ? pow2mask(tex->h) : -1;
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
    /* Only the 3D world is filtered: 2D-phase triangles (menus, HUD built from
     * triangles) stay nearest-neighbour and crisp, like the texrect sprites.
     * FZX_HUDTRI (the user's HUD-triangle A/B, before any rule change): 0 never
     * filtered (this rule), 1 smoothed like the 3D, 2 smoothed unless G_TF_POINT. */
    static int sHudTri = -1;
    if (sHudTri < 0) { const char *e = getenv("FZX_HUDTRI"); sHudTri = e ? atoi(e) : 0; }
    bool use_bilin = gfx_soft_bilinear && tex && (sPhase3D || sHudTri == 1 || (sHudTri == 2 && tex->linear));
    if (use_bilin && gfx_soft_magbilin) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
    }
    const int amin = tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
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

    const float w0dx = te.w0dx, w1dx = te.w1dx, w2dx = te.w2dx;

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
    /* FZX_WALLLOD: a wall triangle (see wall_fetch) */
    bool wall = false;
    if (tex && persp && use_bilin && sPhase3D && !ztest && wall_lod_active()) { /* the track draws without a depth test */
        const float ia = te.inv_area;
        const float w0dy = -te.e0x * ia, w1dy = -te.e1x * ia, w2dy = -te.e2x * ia;
        const float uwdy = w0dy*uw0 + w1dy*uw1 + w2dy*uw2, vwdy = w0dy*vw0 + w1dy*vw1 + w2dy*vw2, swdy = w0dy*iw0 + w1dy*iw1 + w2dy*iw2;
        const SoftVtx *vv[3] = { v0, v1, v2 };
        for (int k = 0; k < 3 && !wall; k++) {
            const float rw = 1.0f / vv[k]->invw, u = vv[k]->u, v = vv[k]->v;
            const float fxu = fabsf((uwdx - u * swdx) * rw * (float)tex->w), fxv = fabsf((vwdx - v * swdx) * rw * (float)tex->h);
            const float fyu = fabsf((uwdy - u * swdy) * rw * (float)tex->w), fyv = fabsf((vwdy - v * swdy) * rw * (float)tex->h);
            const float fx = fxu > fxv ? fxu : fxv, fy = fyu > fyv ? fyu : fyv; /* texels per pixel across, and down */
            wall = fx > 1.0f && fx > 2.0f * fy;
        }
        if (wall) wall_cols(sW);
    }
    /* Fixed-point deltas are invariant too (were rebuilt every 16-px span). */
    const int32_t zdq = (int32_t)((zdx < -1.0f ? -1.0f : zdx > 1.0f ? 1.0f : zdx) * (65535.0f * 256.0f));
    const int32_t rdq = sat_i32(crdx * 65536.0f), gdq = sat_i32(cgdx * 65536.0f), bdq = sat_i32(cbdx * 65536.0f), adq = sat_i32(cadx * 65536.0f);
    /* Flat triangles: the shade colour the pixel loop would clamp every pixel
     * (rq = (int32_t)(cr * 65536.0f) with cr == r0 on every span, and every
     * colour step 0), clamped once. (From MK64 perf round 24 via oot d267ca3.) */
    const int fsr = clamp8_sh16((int32_t)(r0 * 65536.0f)), fsg = clamp8_sh16((int32_t)(g0 * 65536.0f));
    const int fsb = clamp8_sh16((int32_t)(bl0 * 65536.0f)), fsa = clamp8_sh16((int32_t)(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const int zbias = sZDecal ? 4 : 0; const bool dmask = sDepthMask, texadd = sTexAdd;
    /* N64: Z_UPD writes depth whether or not Z_CMP compares (F-Zero X draws its
     * track that way); the kit only wrote it when testing. */
    const bool zwrite = use_z && dmask;
    const bool texlerp = sTexLerp; const int lkr = sLerpR, lkg = sLerpG, lkb = sLerpB;
    const bool texlerp2 = sTexLerp2;
    const bool texfog = sTexFog; const int tfr = sTfR, tfg = sTfG, tfb = sTfB;
    const int l2br = sL2Br, l2bg = sL2Bg, l2bb = sL2Bb, l2dr = sL2Ar - sL2Br, l2dg = sL2Ag - sL2Bg, l2db = sL2Ab - sL2Bb;
    const bool postfog = sPostFog && !sUseAlpha; const int pfr = sFogR, pfg = sFogG, pfb = sFogB;
    /* Plain texture x shade (the combiner branch chain's last case): see SPAN_LOOP_CM. */
    const bool cm_mod = rgb_tex && rgb_in && !rgb_decal && !texlerp2 && !texlerp && !texadd && !texfog && !rgb_texa;
    const bool tl2_mod = rgb_tex && rgb_in && !rgb_decal && texlerp2 && !texfog && !rgb_texa;
    const bool tex_only = rgb_tex && !rgb_in && !rgb_texa, decal = rgb_tex && rgb_in && rgb_decal && !rgb_texa;
    const bool shade_a = !rgb_tex && a_tex && sUseAlpha;
    const bool tlerp = rgb_tex && rgb_in && !rgb_decal && !texlerp2 && texlerp && !texfog && !rgb_texa;
    if (gfx_soft_hotspan < 0) { const char *e = getenv("FZX_HOTSPAN"); gfx_soft_hotspan = !(e && e[0] == '0'); }
    (void)tap3_on(); /* reads FZX_TAP3 before bilerp's first look at gfx_soft_tap3 */
    { static int swar_read; if (!swar_read) { const char *e = getenv("FZX_SWAR"); if (e && e[0] == '0') gfx_soft_swar = 0; swar_read = 1; } }
    if (gfx_soft_rcprow < 0) { const char *e = getenv("FZX_RCPROW"); gfx_soft_rcprow = e && e[0] == '1'; }
#ifdef HOT_NEON
    if (sHotNeon < 0) { const char *e = getenv("FZX_NEON"); sHotNeon = e && e[0] == '1'; }
    if (sHotNeonA < 0) { const char *e = getenv("FZX_NEONA"); sHotNeonA = !(e && e[0] == '0'); }
    if (sHotFuse < 0) { const char *e = getenv("FZX_FUSE"); sHotFuse = !(e && e[0] == '0'); }
    if (sHotPair < 0) { const char *e = getenv("FZX_PAIR"); sHotPair = !(e && e[0] == '0'); }
#endif
    const bool hot = gfx_soft_hotspan && tex && !t16 && (cm_mod || tl2_mod || tex_only || decal || shade_a || tlerp);
    HotSpan hs;
    if (hot) {
        hs.px = (const uint32_t *)texpx; hs.tw = tex->w; hs.th = tex->h; hs.ws = tex->wrap_s; hs.wt = tex->wrap_t;
        hs.mu = tmask_u; hs.mv = tmask_v; hs.bilin = use_bilin; hs.ztest = ztest; hs.zwrite = zwrite;
        hs.alpha = sUseAlpha; hs.amode = !a_tex ? 0 : (!a_in ? 1 : 2); hs.amin = amin; hs.postfog = postfog;
        hs.zbias = sZDecal ? 4 : 0; hs.pfr = pfr; hs.pfg = pfg; hs.pfb = pfb;
        hs.kind = cm_mod ? HOT_MOD : tl2_mod ? HOT_TL2 : tex_only ? HOT_TEX : decal ? HOT_DECAL : shade_a ? HOT_SHADEA : HOT_TLERP;
        hs.l2br = l2br; hs.l2bg = l2bg; hs.l2bb = l2bb; hs.l2dr = l2dr; hs.l2dg = l2dg; hs.l2db = l2db;
        hs.lkr = lkr; hs.lkg = lkg; hs.lkb = lkb;
    }
    if (sHotRow < 0) { const char *e = getenv("FZX_HOTROW"); sHotRow = !(e && e[0] == '0'); }
    /* whole rows go to hot_row when no span can take the 2D box filter */
    const bool hotrow = hot && sHotRow && !wall && !(!use_bilin && !sPhase3D && gfx_soft_hud_box > 0); /* a wall: by spans */
    HotTri ht;
    if (hotrow) {
        ht.hs = hs; ht.fbuf = fbuf; ht.zb = zb; ht.persp = persp;
        ht.zdx = zdx; ht.crdx = crdx; ht.cgdx = cgdx; ht.cbdx = cbdx; ht.cadx = cadx;
        ht.uwdx = uwdx; ht.vwdx = vwdx; ht.swdx = swdx; ht.ftw = (float)tex->w; ht.fth = (float)tex->h;
        ht.zdq = zdq; ht.rdq = rdq; ht.gdq = gdq; ht.bdq = bdq; ht.adq = adq;
#ifdef HOT_NEON
        ht.da = (float32x4_t){ zdx, crdx, cgdx, cbdx }; ht.db = (float32x4_t){ cadx, uwdx, vwdx, swdx };
        ht.m = (float32x4_t){ ht.ftw, ht.ftw, ht.fth, ht.fth };
#endif
    }
#ifdef HOT_NEON
    if (gfx_soft_spanvec < 0) { const char *e = getenv("FZX_SPANVEC"); gfx_soft_spanvec = !(e && e[0] == '0'); }
    if (gfx_soft_fastrows < 0) { const char *e = getenv("FZX_FASTROWS"); gfx_soft_fastrows = !(e && e[0] == '0'); }
    const bool spanvec = hotrow && gfx_soft_spanvec > 0;
#endif
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
    STAT(stat_tri(hotrow, flat);)

    uint32_t tri_px = 0; /* span pixels of this triangle (<= 57,600); folded into the global once below */

#ifdef HOT_NEON
    if (hotrow && gfx_soft_fastrows > 0 && !te.own && gfx_soft_rcprow <= 0) { /* FZX_FASTROWS: every row in tri_rows */
        TriRows trr;
        trr.va0 = (float32x4_t){ z0, r0, g0, bl0 }; trr.vb0 = (float32x4_t){ a0, uw0, vw0, iw0 };
        trr.va1 = (float32x4_t){ z1, r1, g1, bl1 }; trr.vb1 = (float32x4_t){ a1, uw1, vw1, iw1 };
        trr.va2 = (float32x4_t){ z2, r2, g2, bl2 }; trr.vb2 = (float32x4_t){ a2, uw2, vw2, iw2 };
        trr.fa = (float32x4_t){ 0.0f, r0, g0, bl0 }; trr.fb = (float32x4_t){ a0, 0.0f, 0.0f, 0.0f };
        trr.te = &te;
        trr.ek[0] = w0dx > 0.0f ? 0 : w0dx < 0.0f ? 1 : 2;
        trr.ek[1] = w1dx > 0.0f ? 0 : w1dx < 0.0f ? 1 : 2;
        trr.ek[2] = w2dx > 0.0f ? 0 : w2dx < 0.0f ? 1 : 2;
        trr.vec = spanvec; trr.cut = cut; trr.cid = cid; trr.ht = &ht;
        tri_px = flat ? tri_rows_f(&trr) : tri_rows_s(&trr);
    } else
#endif
    for (int y = miny; y < maxy; y++) {
#if defined(MIYOO)
        if (sRowSplit && ((y ^ sRowPar) & 1)) continue; /* the other core's row (vis_flush) */
#endif
        if (cut && sCutRowMin[y] > cid) continue; /* a visible-only draw: all of the row is painted over later */
        int xs, xe;
        float b0, b1, b2;
        if (!tri_row(&te, y, &xs, &xe, &b0, &b1, &b2)) continue;
        /* A visible-only draw: rcut when an occluder after this triangle lies in the row (its
         * spans then count in sVisPx as they are drawn); a row painted over everywhere is
         * skipped, as every row starts from the triangle's own values. */
        VisRow vrow;
        const VisRow *rcut = NULL;
        if (cut && cid < sCutRowMax[y]) {
            vrow.cut = cut + y * sW; vrow.bmin = sCutBMin + y * sCutBW; vrow.bmax = sCutBMax + y * sCutBW; vrow.id = cid;
            int x = xs;
#ifdef HOT_NEON
            if (gfx_soft_visneon) { /* the same search, eight pixels a compare (the speed grind) */
                const uint16x8_t I = vdupq_n_u16(cid);
                while (x < xe) {
                    if (vrow.bmin[x >> 4] > cid) { x = ((x >> 4) + 1) << 4; continue; }
                    if (x + 8 <= xe) {
                        const uint16x8_t le = vcleq_u16(vld1q_u16(vrow.cut + x), I);
                        if (vget_lane_u64(vreinterpret_u64_u16(vorr_u16(vget_low_u16(le), vget_high_u16(le))), 0)) break;
                        x += 8;
                        continue;
                    }
                    if (vrow.cut[x] > cid) { x++; continue; }
                    break;
                }
            } else
#endif
            while (x < xe) { /* whole blocks painted over later, then pixels */
                if (vrow.bmin[x >> 4] > cid) { x = ((x >> 4) + 1) << 4; continue; }
                if (vrow.cut[x] > cid) { x++; continue; }
                break;
            }
            if (x >= xe) continue;
            rcut = &vrow;
        } else {
            tri_px += (uint32_t)(xe - xs); /* fill/overdraw proxy (before z-reject) */
        }

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
        if (persp) rw = rcut ? -1.0f : (sw > 1e-12f) ? 1.0f / sw : 0.0f; /* -1: taken by the first span drawn */
        STAT(stat_row(hotrow, rcut != NULL, te.own);)
#ifdef HOT_NEON
        if (spanvec) { hot_row_v(&ht, rcut, idx, xs, xe, (float32x4_t){ zf, cr, cg, cb }, (float32x4_t){ ca, uw, vw, sw }, rw); continue; }
#endif
        if (hotrow) { hot_row(&ht, rcut, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw); continue; }
        int x = xs;
        while (x < xe) {
            int span = xe - x;
            if (span > PERSP_SPAN) span = PERSP_SPAN;
            const float fspan = (float)span;
            const float sw_b = sw + swdx * fspan;           /* == the old end-of-span  sw += swdx*span */
            float rw_b = 0.0f;
            uint32_t pm = SPAN_FULL(span);
            if (rcut) {
                pm = vis_span(rcut, x, span);
                if (!pm) { rw_b = -1.0f; idx += span; goto span_next; }
                sVisPx += (uint32_t)span;
            }
            const bool pmp = pm != SPAN_FULL(span); /* part of the span is painted over later */
            /* Perspective: exact u,v at span start and end, lerp between. */
            float u_a, v_a, du, dv;
            if (tex) {
                if (persp) {
                    if (rw < 0.0f) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
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
            if (span_skip(span, idx)) { idx += span; goto span_next; } /* a resolve's: nothing pending here */

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
            int32_t rq = sat_i32(cr * 65536.0f), gq = sat_i32(cg * 65536.0f), bq = sat_i32(cb * 65536.0f), aq = sat_i32(ca * 65536.0f);

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
/* FZX_WALLLOD: the N64's pixel centres inside this pixel (wall_fetch) */
#define FETCH_WALL      const uint32_t t = wall_fetch(texpx, t16, tw, th, ws, wt, uf, vf, duf, dvf, tmask_u, tmask_v, x + i);
/* 2D-phase (UI) triangles squeezed 4:3 like the sprites: area average along S
 * (as ocarina-of-time 0a3cadd does for its UI-tagged triangles). */
#define FETCH_BOX                                                                                      \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_box(texpx, t16, iv * tw, uf, duf, tw, ws, tmask_u, sUseAlpha);
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once
 * above), so the four colour accumulators drop out of the pixel loop (flat =>
 * every step is 0: same values). CM=1: the combiner is plain texture x shade
 * (cm_mod), so the per-pixel walk of the combine flags drops out. Both
 * bit-exact (MK64 perf round 24, via ocarina-of-time d267ca3). */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
#define SPAN_LOOP(TEX, ZTEST, ALPHA, BILIN) SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, 0, 0)
#define SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, CM)                                               \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq COLSTEP_##FLAT) {     \
                if (pmp && !((pm >> i) & 1u)) continue;                                                 \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias) continue; }                                      \
                const int sr = FLAT ? fsr : clamp8_sh16(rq), sg = FLAT ? fsg : clamp8_sh16(gq);         \
                const int sb = FLAT ? fsb : clamp8_sh16(bq), sa = FLAT ? fsa : clamp8_sh16(aq);         \
                int fr, fg, fb, fa;                                                                     \
                if (TEX) {                                                                              \
                    FETCH_##BILIN                                                                       \
                    const int ta = (t>>24)&0xff;                                                        \
                    const int tr = (!CM && rgb_texa) ? ta : (t>>16)&0xff, tg = (!CM && rgb_texa) ? ta : (t>>8)&0xff, tb = (!CM && rgb_texa) ? ta : t&0xff; \
                    if (CM)             { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (rgb_decal) { fr=div255(tr*ta + sr*(255-ta)); fg=div255(tg*ta + sg*(255-ta)); \
                                          fb=div255(tb*ta + sb*(255-ta)); }                             \
                    else if (texfog)    { fr=tr+div255s((tfr-tr)*sr); fg=tg+div255s((tfg-tg)*sg); fb=tb+div255s((tfb-tb)*sb); } \
                    else if (texlerp2)  { fr=div255((l2br+div255s(l2dr*tr))*sr); fg=div255((l2bg+div255s(l2dg*tg))*sg); \
                                          fb=div255((l2bb+div255s(l2db*tb))*sb); }                      \
                    else if (texlerp)   { fr=lkr+div255s((sr-lkr)*tr); fg=lkg+div255s((sg-lkg)*tg); fb=lkb+div255s((sb-lkb)*tb); } \
                    else if (texadd)    { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = !a_tex ? sa : (!a_in ? ta : div255(ta*sa));                                    \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (postfog && sa) { fr=div255(fr*(255-sa) + pfr*sa); fg=div255(fg*(255-sa) + pfg*sa);   \
                                     fb=div255(fb*(255-sa) + pfb*sa); }                                 \
                if (ALPHA) {                                                                            \
                    if (fa < amin) continue;                                                            \
                    if (fa < 255) {                                                                     \
                        const uint32_t d = fbuf[idx];                                                   \
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;                           \
                        fr = div255(fr*fa + dr*(255-fa));                                               \
                        fg = div255(fg*fa + dg*(255-fa));                                               \
                        fb = div255(fb*fa + db*(255-fa));                                               \
                    }                                                                                   \
                }                                                                                       \
                fbuf[idx] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);                              \
                if (zwrite) zb[idx] = (uint16_t)uz;                                                    \
            }
            const bool boxspan = tex && !use_bilin && !sPhase3D && gfx_soft_hud_box > 0 && duf > 66846 && duf <= 196608;
            /* FZX_WALLLOD: a wall span whose texture runs faster than a texel a pixel across; the rest as before */
            const bool wallspan = wall && (duf > 65536 || duf < -65536 || dvf > 65536 || dvf < -65536);
            if (hot && !boxspan && !wallspan) {
                hot_span(&hs, fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, rq, gq, bq, aq, rdq, gdq, bdq, adq, pm);
                idx += span;
            } else if (wallspan) {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, WALL) else SPAN_LOOP(1, 1, 0, WALL) }
                else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, WALL) else SPAN_LOOP(1, 0, 0, WALL) }
            } else if (tex && ztest && !boxspan && (cm_mod || flat)) {
                /* the hot paths: z-tested texture x shade and/or flat shade */
                if (cm_mod && flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else if (cm_mod) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 0) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 0) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 0)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 0) }
                }
            } else if (tex) {
                if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR) else SPAN_LOOP(1, 1, 0, BILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BILINEAR) else SPAN_LOOP(1, 0, 0, BILINEAR) }
                } else if (boxspan) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BOX) else SPAN_LOOP(1, 1, 0, BOX) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BOX) else SPAN_LOOP(1, 0, 0, BOX) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEAREST) else SPAN_LOOP(1, 1, 0, NEAREST) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEAREST) else SPAN_LOOP(1, 0, 0, NEAREST) }
                }
            } else if (!ztest && !zwrite && !sUseAlpha && !postfog && rdq == 0 && gdq == 0 && bdq == 0) {
                /* Constant-colour run: no z, no alpha, zero steps => every pixel
                 * is argb(clamp(rq), clamp(gq), clamp(bq), 255). */
                const uint32_t c = argb((uint8_t)clamp8_sh16(rq), (uint8_t)clamp8_sh16(gq), (uint8_t)clamp8_sh16(bq), 255);
                uint32_t *const row = fbuf + idx;
                for (int i = 0; i < span; i++) if (!pmp || ((pm >> i) & 1u)) row[i] = c;
                idx += span;
            } else {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(0, 1, 1, NEAREST) else SPAN_LOOP(0, 1, 0, NEAREST) }
                else       { if (sUseAlpha) SPAN_LOOP(0, 0, 1, NEAREST) else SPAN_LOOP(0, 0, 0, NEAREST) }
            }
#undef SPAN_LOOP
#undef SPAN_LOOP_CM
#undef COLSTEP_0
#undef COLSTEP_1
#undef FETCH_NEAREST
#undef FETCH_BILINEAR
#undef FETCH_WALL
#undef FETCH_BOX
        span_next:
            zf += zdx * fspan;
            cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;
            uw += uwdx * fspan;
            vw += vwdx * fspan;
            sw = sw_b; rw = rw_b; /* identical to sw += swdx*span; reciprocal carried */
            x += span;
        }
    }
    tri_px += sVisPx - vis0;
    STAT(stat_path(sWinPend ? sWinPx - stat_w0 : tri_px, sPhase3D, hot ? hs.kind : (tex ? (cm_mod ? 10 : 11) : 12), use_bilin, ztest,
                   sUseAlpha, zwrite, postfog));
    return (int)tri_px;
}

static bool sIntDirty SAVESTATE_EXCLUDE, sNatDirty SAVESTATE_EXCLUDE; /* see the frame section */
static bool sScrOn SAVESTATE_EXCLUDE, sScrAll SAVESTATE_EXCLUDE; /* the composite wrote the screen (frame section) */
static void scr_mark(int x0, int x1, int y0, int y1);
static bool sHad3D SAVESTATE_EXCLUDE;         /* any 3D batch this frame */
static bool sIntClean SAVESTATE_EXCLUDE;      /* internal colour is all-zero (cleared by the last composite) */
static bool sUnified SAVESTATE_EXCLUDE;       /* internal res == native: 3D draws straight to native (see frame section) */
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */

/* ------------------------------------------------ deferred background batches --- */
/* F-Zero X opens every race picture with a full-screen sky gradient and a ground plane
 * that the track and the scenery then mostly paint over (overdraw census, Mute City GP,
 * VI 1200-1500: 71.6k of the 134k pixels a picture, 3.8k of them left visible). A batch
 * that is opaque and uses no depth at all (no test, no write) depends on nothing under
 * it, and nothing but its colour depends on it, so its drawing can wait. def_record keeps
 * a copy of its vertices, texture and render state and marks the pixels it covers as
 * pending (sPend, the latest such batch per pixel). Later batches then:
 *  - opaque without a depth test (the track): they overwrite every pixel they cover, so
 *    those pixels stop being pending (def_cover) and the background is never drawn there;
 *  - anything else (blending, alpha test, depth test): the pending pixels in the rows
 *    their triangles span are drawn first (def_resolve_rows), exactly as before.
 * The frame end and every 2D/3D switch resolve the rest. A resolve runs raster_tri on the
 * recorded batch, its state restored and its rows clipped, into a scratch buffer, and
 * copies the pixels still pending. Rows are independent in raster_tri (each recomputes
 * its edges and span setup), so clipping rows changes no pixel, and coverage and raster
 * share tri_edges/tri_row, so they agree on every span: the output is bit-identical.
 * FZX_DEFER=0 draws every batch at once (A/B). */
#define DEF_MAX 8               /* batches deferred a frame: bit k of sPendRow */
#define DEF_VTX (3 * 64)        /* vertices kept across them */
#define DEF_TEXB (256 * 1024)   /* texture bytes kept across them */
#define DEF_TEX_ID MAX_TEXTURES /* the sTex slot a resolve binds its texture copy to */
typedef struct { /* every render-state global raster_tri reads */
    bool depth_test, depth_mask, zdecal, use_alpha, tex_add, tex_lerp, force_in, tex_lerp2, tex_fog, post_fog, phase3d, own;
    int vpx, vpy, vpw, vph, scx0, scy0, scx1, scy1;
    int lerp_r, lerp_g, lerp_b, l2ar, l2ag, l2ab, l2br, l2bg, l2bb, fog_r, fog_g, fog_b, tf_r, tf_g, tf_b;
    int bilinear, magbilin;
    float gate_scale;
    uint32_t bound0;
    struct ShaderProgram *shader;
} RastState;
static void rs_save(RastState *r) {
    r->depth_test = sDepthTest; r->depth_mask = sDepthMask; r->zdecal = sZDecal; r->use_alpha = sUseAlpha;
    r->tex_add = sTexAdd; r->tex_lerp = sTexLerp; r->force_in = sForceIn; r->tex_lerp2 = sTexLerp2;
    r->tex_fog = sTexFog; r->tf_r = sTfR; r->tf_g = sTfG; r->tf_b = sTfB;
    r->post_fog = sPostFog; r->phase3d = sPhase3D; r->own = sOwnTri;
    r->vpx = sVpX; r->vpy = sVpY; r->vpw = sVpW; r->vph = sVpH;
    r->scx0 = sScX0; r->scy0 = sScY0; r->scx1 = sScX1; r->scy1 = sScY1;
    r->lerp_r = sLerpR; r->lerp_g = sLerpG; r->lerp_b = sLerpB;
    r->l2ar = sL2Ar; r->l2ag = sL2Ag; r->l2ab = sL2Ab; r->l2br = sL2Br; r->l2bg = sL2Bg; r->l2bb = sL2Bb;
    r->fog_r = sFogR; r->fog_g = sFogG; r->fog_b = sFogB;
    r->bilinear = gfx_soft_bilinear; r->magbilin = gfx_soft_magbilin; r->gate_scale = sGateScale;
    r->bound0 = sBoundTex[0]; r->shader = sCurShader;
}
static void rs_load(const RastState *r) {
    sDepthTest = r->depth_test; sDepthMask = r->depth_mask; sZDecal = r->zdecal; sUseAlpha = r->use_alpha;
    sTexAdd = r->tex_add; sTexLerp = r->tex_lerp; sForceIn = r->force_in; sTexLerp2 = r->tex_lerp2;
    sTexFog = r->tex_fog; sTfR = r->tf_r; sTfG = r->tf_g; sTfB = r->tf_b;
    sPostFog = r->post_fog; sPhase3D = r->phase3d; sOwnTri = r->own;
    sVpX = r->vpx; sVpY = r->vpy; sVpW = r->vpw; sVpH = r->vph;
    sScX0 = r->scx0; sScY0 = r->scy0; sScX1 = r->scx1; sScY1 = r->scy1;
    sLerpR = r->lerp_r; sLerpG = r->lerp_g; sLerpB = r->lerp_b;
    sL2Ar = r->l2ar; sL2Ag = r->l2ag; sL2Ab = r->l2ab; sL2Br = r->l2br; sL2Bg = r->l2bg; sL2Bb = r->l2bb;
    sFogR = r->fog_r; sFogG = r->fog_g; sFogB = r->fog_b;
    gfx_soft_bilinear = r->bilinear; gfx_soft_magbilin = r->magbilin; sGateScale = r->gate_scale;
    sBoundTex[0] = r->bound0; sCurShader = r->shader;
}
typedef struct {
    RastState st;
    struct ShaderProgram shader; /* copies: sShaders reuses its last slot when full, */
    SoftTex tex;                 /* uploads and blends rewrite textures, wrap modes change */
    int vtx0, ntris;
} DefBatch;
static DefBatch sDef[DEF_MAX] SAVESTATE_EXCLUDE;
static int sDefN SAVESTATE_EXCLUDE, sDefVtxN SAVESTATE_EXCLUDE;
static size_t sDefTexN SAVESTATE_EXCLUDE;
static SoftVtx sDefVtx[DEF_VTX] SAVESTATE_EXCLUDE;
static uint8_t *sDefTex SAVESTATE_EXCLUDE;      /* DEF_TEXB bytes */
static uint8_t *sPend SAVESTATE_EXCLUDE;        /* per pixel of the current target: pending batch + 1, 0 = none */
static uint8_t *sPendRow SAVESTATE_EXCLUDE;     /* per row: bit k = maybe pending for batch k */
static uint32_t *sDefScratch SAVESTATE_EXCLUDE; /* resolves rasterize here */
static int sDefCap SAVESTATE_EXCLUDE, sDefRows SAVESTATE_EXCLUDE;
int gfx_soft_defer SAVESTATE_EXCLUDE = -1;      /* FZX_DEFER=0: draw every batch at once (A/B) */

/* Buffers for the current target; only (re)sized while nothing is pending. */
static bool def_buffers(void) {
    if (sDefCap >= sW * sH && sDefRows >= sH && sDefTex) return true;
    if (sDefN) return false;
    free(sPend); free(sPendRow); free(sDefScratch);
    sPend = calloc((size_t)sW * sH, 1);
    sPendRow = calloc((size_t)sH, 1);
    sDefScratch = malloc((size_t)sW * sH * 4);
    if (!sDefTex) sDefTex = malloc(DEF_TEXB);
    if (!sPend || !sPendRow || !sDefScratch || !sDefTex) {
        free(sPend); free(sPendRow); free(sDefScratch);
        sPend = sPendRow = NULL; sDefScratch = NULL; sDefCap = sDefRows = 0;
        return false;
    }
    sDefCap = sW * sH; sDefRows = sH;
    return true;
}
/* Keep the batch for later and mark its pixels pending; false = draw it now. */
static bool def_record(const SoftVtx *v, size_t ntris) {
    if (sDefN >= DEF_MAX || sDefVtxN + (int)ntris * 3 > DEF_VTX || !def_buffers()) return false;
    DefBatch *d = &sDef[sDefN];
    const bool uses_tex = sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1]);
    memset(&d->tex, 0, sizeof(d->tex));
    if (uses_tex) {
        const SoftTex *t = &sTex[sBoundTex[0]];
        d->tex = *t;
        if (t->valid && t->px) {
            const size_t bytes = (size_t)t->w * t->h * (t->is16 ? 2u : 4u), at = (sDefTexN + 15u) & ~(size_t)15;
            if (at + bytes > DEF_TEXB) return false;
            memcpy(sDefTex + at, t->px, bytes);
            d->tex.px = sDefTex + at;
            sDefTexN = at + bytes;
        }
    }
    if (sCurShader) d->shader = *sCurShader;
    rs_save(&d->st);
    d->vtx0 = sDefVtxN; d->ntris = (int)ntris;
    memcpy(&sDefVtx[sDefVtxN], v, ntris * 3 * sizeof(SoftVtx));
    sDefVtxN += (int)ntris * 3;
    const uint8_t id = (uint8_t)(sDefN + 1), bit = (uint8_t)(1u << sDefN);
    sDefN++;
    for (size_t i = 0; i < ntris; i++) {
        TriEdges te;
        if (!tri_edges(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], &te)) continue;
        gfx_soft_tris++;
        for (int y = te.miny; y < te.maxy; y++) {
            int xs, xe;
            float b0, b1, b2;
            if (!tri_row(&te, y, &xs, &xe, &b0, &b1, &b2)) continue;
            memset(sPend + y * sW + xs, id, (size_t)(xe - xs));
            sPendRow[y] |= bit;
        }
    }
    return true;
}
/* An opaque batch without a depth test is about to paint every pixel it covers. */
static void def_cover(const SoftVtx *v, size_t ntris) {
    for (size_t i = 0; i < ntris; i++) {
        TriEdges te;
        if (!tri_edges(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], &te)) continue;
        for (int y = te.miny; y < te.maxy; y++) {
            int xs, xe;
            float b0, b1, b2;
            if (!sPendRow[y] || !tri_row(&te, y, &xs, &xe, &b0, &b1, &b2)) continue;
            memset(sPend + y * sW + xs, 0, (size_t)(xe - xs));
        }
    }
}
/* Draw batch k's rows [ry0, ry1) (the spans with a pixel pending for it) and keep the
 * pixels still pending: every such pixel lies in a span that was drawn. */
static void def_raster(int k, int ry0, int ry1) {
    const DefBatch *d = &sDef[k];
    RastState cur;
    uint32_t *const fb = sFB;
    rs_save(&cur);
    rs_load(&d->st);
    sCurShader = d->st.shader ? (struct ShaderProgram *)&d->shader : NULL;
    sTex[DEF_TEX_ID] = d->tex;
    sBoundTex[0] = DEF_TEX_ID;
    if (sScY0 < ry0) sScY0 = ry0;
    if (sScY1 > ry1) sScY1 = ry1;
    sWinPend = sPend; sWinId = (uint8_t)(k + 1); sWinPx = 0;
    sFB = sDefScratch;
    for (int i = 0; i < d->ntris; i++) {
        const SoftVtx *v = &sDefVtx[d->vtx0 + i * 3];
        raster_tri(&v[0], &v[1], &v[2], true);
    }
    sFB = fb;
    sWinPend = NULL;
    rs_load(&cur);
    gfx_soft_span_px += sWinPx;
    const uint8_t id = (uint8_t)(k + 1), bit = (uint8_t)(1u << k);
    for (int y = ry0; y < ry1; y++) {
        uint8_t *p = sPend + y * sW;
        const uint32_t *src = sDefScratch + y * sW;
        uint32_t *dst = sFB + y * sW;
        for (int x = 0; x < sW; x++) {
            if (p[x] == id) { dst[x] = src[x]; p[x] = 0; }
        }
        sPendRow[y] &= (uint8_t)~bit;
    }
}
/* Draw what is still pending in rows [y0, y1), batch by batch, in runs of rows. */
static void def_resolve_rows(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > sH) y1 = sH;
    for (int k = 0; k < sDefN; k++) {
        const uint8_t id = (uint8_t)(k + 1), bit = (uint8_t)(1u << k);
        int run = -1;
        for (int y = y0; y <= y1; y++) {
            bool need = false;
            if (y < y1 && (sPendRow[y] & bit)) {
                need = memchr(sPend + y * sW, id, (size_t)sW) != NULL;
                if (!need) sPendRow[y] &= (uint8_t)~bit;
            }
            if (need) {
                if (run < 0) run = y;
            } else if (run >= 0) {
                def_raster(k, run, y);
                run = -1;
            }
        }
    }
}
/* Before a batch that reads colour or depth-tests: the rows its triangles can touch. */
static void def_resolve_batch(const SoftVtx *v, size_t ntris) {
    for (size_t i = 0; i < ntris; i++) {
        TriEdges te;
        if (tri_edges(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], &te)) def_resolve_rows(te.miny, te.maxy);
    }
}
static void def_resolve_all(void) {
    if (!sDefN) return;
    PROF_BEGIN();
    def_resolve_rows(0, sH);
    sDefN = 0; sDefVtxN = 0; sDefTexN = 0;
    PROF_END(PROF_R3D);
}

/* ---------------------------------------------------- visible-only 3D (round 12) --- */
/* The deferred backgrounds save the sky and the ground where the track covers them; this goes
 * the whole way. F-Zero X paints a race picture mostly in painter's order without a depth
 * test: the sky, see-through layers over it, the city under the track, the track (writing
 * depth for the machines), then everything that tests depth. Of ~127k 3D pixels written a
 * picture, ~40k stay visible (census, Mute City GP, VI 1200-1500). So the 3D phase is
 * recorded instead of drawn (vertices, render state, shader and texture, as def_record keeps
 * them) and drawn at its end -- a switch to 2D, the frame end, or anything that reads the
 * target or rewrites what the batches use -- in two passes:
 *  1. Occluders. A triangle of an opaque batch (no blending, so no alpha test either) without
 *     a depth test writes every pixel it covers. Each writes its index (1, 2, ... in draw
 *     order) into sVisCut over its coverage (tri_edges/tri_row, as raster_tri), so every
 *     pixel ends with its last occluder. One that writes no depth (the sky, the city) takes
 *     only pixels whose depth no earlier triangle may have written (sVisZW, kept only when
 *     such a batch follows a depth-writing one).
 *  2. Every triangle in order, each drawing only the pixels whose last occluder is itself or
 *     an earlier triangle. The last occluder sets the pixel's colour, and its depth (it
 *     writes it, or nothing earlier did), whatever was drawn there before; so everything
 *     under it can go, and every pixel that is drawn meets exactly what it met before.
 * The picture is the same bit for bit. FZX_VIS=0 draws as before (the A/B). */
typedef struct {
    RastState st;
    struct ShaderProgram shader; /* copies, as in DefBatch; the texel memory is not copied: */
    SoftTex tex;                 /* uploads go to new arena memory, and texman_clear and the
                                  * in-place pre-blend draw the recorded batches first */
    uint32_t vtx0, ntris;
    bool has_shader, occl, zw;
} VisBatch;
#define VIS_MAXTRI 65535u /* triangle indices are uint16 */
static VisBatch *sVisB SAVESTATE_EXCLUDE;
static SoftVtx *sVisV SAVESTATE_EXCLUDE;
static uint32_t sVisBN SAVESTATE_EXCLUDE, sVisBCap SAVESTATE_EXCLUDE, sVisVN SAVESTATE_EXCLUDE, sVisVCap SAVESTATE_EXCLUDE;
static uint16_t *sVisCut SAVESTATE_EXCLUDE, *sVisRowMax SAVESTATE_EXCLUDE, *sVisRowMin SAVESTATE_EXCLUDE, *sVisBMin SAVESTATE_EXCLUDE,
    *sVisBMax SAVESTATE_EXCLUDE;
static uint8_t *sVisZW SAVESTATE_EXCLUDE;
static int sVisCutCap SAVESTATE_EXCLUDE, sVisRowCap SAVESTATE_EXCLUDE, sVisBlkCap SAVESTATE_EXCLUDE;
int gfx_soft_vis SAVESTATE_EXCLUDE = -1; /* FZX_VIS=0: off (A/B) */
static void vis_flush(void);

/* Keep the batch for the visible-only draw; false = draw it now. */
static bool vis_record(const SoftVtx *v, size_t ntris) {
    if (ntris == 0) return true;
    if (ntris > VIS_MAXTRI) { vis_flush(); return false; }
    if (sVisVN / 3 + ntris > VIS_MAXTRI) vis_flush();
    if (sVisBN + 1 > sVisBCap) {
        const uint32_t n = sVisBCap ? sVisBCap * 2 : 256;
        VisBatch *q = realloc(sVisB, (size_t)n * sizeof(*q));
        if (!q) { vis_flush(); return false; }
        sVisB = q; sVisBCap = n;
    }
    if (sVisVN + (uint32_t)ntris * 3 > sVisVCap) {
        uint32_t n = sVisVCap ? sVisVCap : 3 * 2048;
        while (n < sVisVN + (uint32_t)ntris * 3) n *= 2;
        SoftVtx *q = realloc(sVisV, (size_t)n * sizeof(*q));
        if (!q) { vis_flush(); return false; }
        sVisV = q; sVisVCap = n;
    }
    VisBatch *b = &sVisB[sVisBN++];
    rs_save(&b->st);
    b->has_shader = sCurShader != NULL;
    if (sCurShader) b->shader = *sCurShader;
    memset(&b->tex, 0, sizeof(b->tex));
    if (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) b->tex = sTex[sBoundTex[0]];
    b->vtx0 = sVisVN; b->ntris = (uint32_t)ntris;
    memcpy(&sVisV[sVisVN], v, ntris * 3 * sizeof(SoftVtx));
    sVisVN += (uint32_t)ntris * 3;
    b->occl = !sUseAlpha && !sDepthTest;
    b->zw = sDepthMask; /* (raster_tri always runs with use_z) */
    return true;
}
/* The cut buffers for the current target (the recorded phase's: every flush point comes
 * before a target switch). */
static bool vis_buffers(void) {
    const int n = sW * sH;
    if (sVisCutCap < n) {
        free(sVisCut); free(sVisZW);
        sVisCut = malloc((size_t)n * sizeof(*sVisCut));
        sVisZW = malloc((size_t)n);
        sVisCutCap = sVisCut && sVisZW ? n : 0;
        if (!sVisCutCap) { free(sVisCut); free(sVisZW); sVisCut = NULL; sVisZW = NULL; return false; }
    }
    if (sVisRowCap < sH) {
        free(sVisRowMax); free(sVisRowMin);
        sVisRowMax = malloc((size_t)sH * sizeof(*sVisRowMax));
        sVisRowMin = malloc((size_t)sH * sizeof(*sVisRowMin));
        sVisRowCap = sVisRowMax && sVisRowMin ? sH : 0;
        if (!sVisRowCap) { free(sVisRowMax); free(sVisRowMin); sVisRowMax = sVisRowMin = NULL; return false; }
    }
    const int nb = ((sW + 15) >> 4) * sH;
    if (sVisBlkCap < nb) {
        free(sVisBMin); free(sVisBMax);
        sVisBMin = malloc((size_t)nb * sizeof(*sVisBMin));
        sVisBMax = malloc((size_t)nb * sizeof(*sVisBMax));
        sVisBlkCap = sVisBMin && sVisBMax ? nb : 0;
        if (!sVisBlkCap) { free(sVisBMin); free(sVisBMax); sVisBMin = sVisBMax = NULL; return false; }
    }
    return true;
}
/* An occluder's row: its index over [xs, xe), and the blocks' bounds (a block it covers whole
 * gets it as its lower bound too; ids only grow, so it is every touched block's upper bound). */
static inline void vis_fill(uint16_t *c, uint16_t *bmin, uint16_t *bmax, int xs, int xe, int w, uint16_t id) {
    uint16_t *p = c + xs;
    int n = xe - xs;
#ifdef HOT_NEON
    const uint16x8_t V = vdupq_n_u16(id);
    for (; n >= 8; n -= 8, p += 8) vst1q_u16(p, V);
#endif
    for (; n > 0; n--) *p++ = id;
    for (int b = xs >> 4, be = (xe - 1) >> 4; b <= be; b++) {
        const int x0 = b << 4, x1 = x0 + 16 < w ? x0 + 16 : w;
        bmax[b] = id;
        if (x0 >= xs && x1 <= xe) bmin[b] = id;
    }
}
#if defined(MIYOO)
/* One core's share of a batch (vis_flush): rows of parity par of each triangle. */
typedef struct {
    const SoftVtx *v;
    uint32_t n, id0, tris, px;
    int par;
    uint64_t cpu_ns; /* FZX_MTPROF: the share's thread CPU time */
} VisJob;
/* FZX_MTPROF=1 (a measuring switch, for qemu, where wall time on a busy host says nothing): the 3D's
 * thread CPU time, split or not, for the MT line: what each core spent drawing, and the slower share
 * of each batch summed (the frame's critical path through the 3D). */
static int sMtProf SAVESTATE_EXCLUDE = -1;
static uint64_t sMtOne SAVESTATE_EXCLUDE, sMtEven SAVESTATE_EXCLUDE, sMtOdd SAVESTATE_EXCLUDE, sMtMax SAVESTATE_EXCLUDE;
static uint64_t thread_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void gfx_soft_mtprof(uint64_t *one, uint64_t *even, uint64_t *odd, uint64_t *mx) {
    *one = sMtOne; *even = sMtEven; *odd = sMtOdd; *mx = sMtMax;
    sMtOne = sMtEven = sMtOdd = sMtMax = 0;
}
static void vis_job(void *arg) {
    VisJob *j = arg;
    uint32_t tris = 0, px = 0;
    const uint64_t t0 = sMtProf > 0 ? thread_ns() : 0;
    sRowSplit = 1; sRowPar = j->par;
    for (uint32_t i = 0; i < j->n; i++) {
        sCutId = (uint16_t)(j->id0 + i + 1);
        const int p = raster_tri(&j->v[i * 3], &j->v[i * 3 + 1], &j->v[i * 3 + 2], true);
        if (p >= 0) { tris++; px += (uint32_t)p; }
    }
    sRowSplit = 0;
    j->tris = tris; j->px = px;
    j->cpu_ns = sMtProf > 0 ? thread_ns() - t0 : 0;
}
static uint32_t sVisFlushes SAVESTATE_EXCLUDE, sVisSplits SAVESTATE_EXCLUDE;
/* Split this flush: the worker runs (FZX_THREADS, two cores online), and not the process's first flush,
 * which sets raster_tri's lazily read switches (getenv) on this thread alone. */
static bool vis_split_ok(void) {
    return sVisFlushes > 0 && miyoo_worker_ready();
}
/* port_main.c's MT line: the 3D flushes shared with the second core, out of all, since the last call. */
void gfx_soft_split_counts(unsigned *split, unsigned *flushes) {
    static uint32_t s0 SAVESTATE_EXCLUDE, f0 SAVESTATE_EXCLUDE;
    *split = sVisSplits - s0; *flushes = sVisFlushes - f0;
    s0 = sVisSplits; f0 = sVisFlushes;
}
#endif
static void vis_bind(const VisBatch *b) {
    rs_load(&b->st);
    sCurShader = b->has_shader ? (struct ShaderProgram *)&b->shader : NULL;
    sTex[DEF_TEX_ID] = b->tex;
    sBoundTex[0] = DEF_TEX_ID;
}
static void vis_flush(void) {
    if (!sVisBN) return;
    if (gfx_soft_visneon < 0) { const char *e = getenv("FZX_VISNEON"); gfx_soft_visneon = !(e && e[0] == '0'); }
    PROF_BEGIN();
    RastState cur;
    rs_save(&cur);
    const bool cutok = vis_buffers();
    const int W = sW, BW = (sW + 15) >> 4;
    if (cutok) { /* 1. every pixel's last occluder */
        memset(sVisCut, 0, (size_t)W * sH * sizeof(*sVisCut));
        memset(sVisRowMax, 0, (size_t)sH * sizeof(*sVisRowMax));
        memset(sVisBMin, 0, (size_t)BW * sH * sizeof(*sVisBMin));
        memset(sVisBMax, 0, (size_t)BW * sH * sizeof(*sVisBMax));
        bool guard = false, zseen = false;
        for (uint32_t k = 0; k < sVisBN; k++) {
            if (sVisB[k].occl && !sVisB[k].zw && zseen) guard = true;
            if (sVisB[k].zw) zseen = true;
        }
        if (guard) memset(sVisZW, 0, (size_t)W * sH);
        uint32_t id = 0;
        for (uint32_t k = 0; k < sVisBN; k++) {
            const VisBatch *b = &sVisB[k];
            const bool mark = guard && b->zw, take = !guard || b->zw;
            if (!b->occl && !mark) { id += b->ntris; continue; }
            vis_bind(b); /* tri_edges reads the viewport and scissor */
            const SoftVtx *v = &sVisV[b->vtx0];
            for (uint32_t i = 0; i < b->ntris; i++) {
                const uint16_t tid = (uint16_t)++id;
                TriEdges te;
                if (!tri_edges(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], &te)) continue;
                for (int y = te.miny; y < te.maxy; y++) {
                    int xs, xe;
                    float b0, b1, b2;
                    if (!tri_row(&te, y, &xs, &xe, &b0, &b1, &b2)) continue;
                    if (b->occl) {
                        uint16_t *c = sVisCut + y * W, *bmin = sVisBMin + y * BW, *bmax = sVisBMax + y * BW;
                        if (take) vis_fill(c, bmin, bmax, xs, xe, W, tid);
                        else { /* only where no depth was written: the blocks' lower bounds stay */
                            const uint8_t *z = sVisZW + y * W;
                            for (int x = xs; x < xe; x++) if (!z[x]) c[x] = tid;
                            for (int bb = xs >> 4; bb <= (xe - 1) >> 4; bb++) bmax[bb] = tid;
                        }
                        sVisRowMax[y] = tid; /* ids only grow: the row's largest */
                    }
                    if (mark) memset(sVisZW + y * W + xs, 1, (size_t)(xe - xs));
                }
            }
        }
    }
    if (cutok) /* a lower bound of each row's indices: a row whose bound is above a triangle's index is all painted over */
        for (int y = 0; y < sH; y++) {
            const uint16_t *bmin = sVisBMin + y * BW;
            uint16_t m = bmin[0];
            for (int b = 1; b < BW; b++) m = bmin[b] < m ? bmin[b] : m;
            sVisRowMin[y] = m;
        }
    /* 2. draw, each triangle only where nothing later paints over it */
    sCutBuf = cutok ? sVisCut : NULL; sCutRowMax = sVisRowMax; sCutRowMin = sVisRowMin; sCutBMin = sVisBMin; sCutBMax = sVisBMax; sCutBW = BW;
    uint32_t id = 0;
#if defined(MIYOO)
    if (vis_split_ok()) {
        /* The Miyoo Mini's second core: batch by batch, both cores draw every triangle of the batch, this
         * one the even rows and the worker the odd ones (sRowSplit, sRowPar), and the next batch starts
         * when both are done. Rows are independent in raster_tri (each recomputes its edges and span
         * starts from the triangle's own values; see def_raster), a pixel only ever meets the triangles
         * of its own row, in the same order, and everything both read (the batch's state, its texture,
         * the cut buffers) is fixed while they draw: the picture is the same bit for bit (qemu: frames
         * with FZX_THREADS=1 and 2 compared). */
        if (wall_lod_active()) wall_cols(sW); /* its table is built lazily: never by two threads at once */
        for (uint32_t k = 0; k < sVisBN; k++) {
            const VisBatch *b = &sVisB[k];
            VisJob odd, even;
            vis_bind(b);
            odd.v = even.v = &sVisV[b->vtx0];
            odd.n = even.n = b->ntris;
            odd.id0 = even.id0 = id;
            odd.par = 1; even.par = 0;
            miyoo_worker_run(vis_job, &odd);
            vis_job(&even);
            miyoo_worker_wait();
            if (sMtProf > 0) {
                sMtEven += even.cpu_ns; sMtOdd += odd.cpu_ns;
                sMtMax += even.cpu_ns > odd.cpu_ns ? even.cpu_ns : odd.cpu_ns;
            }
            gfx_soft_tris += even.tris; /* a triangle's bounding box is the same for both: counted once */
            gfx_soft_span_px += even.px + odd.px;
            id += b->ntris;
        }
        sVisSplits++;
    } else
#endif
#if defined(MIYOO)
    {
    if (sMtProf < 0) { const char *e = getenv("FZX_MTPROF"); sMtProf = e && e[0] == '1'; }
    const uint64_t mt0 = sMtProf > 0 ? thread_ns() : 0;
#endif
    for (uint32_t k = 0; k < sVisBN; k++) {
        const VisBatch *b = &sVisB[k];
        vis_bind(b);
        const SoftVtx *v = &sVisV[b->vtx0];
        for (uint32_t i = 0; i < b->ntris; i++) {
            sCutId = (uint16_t)++id;
            const int px = raster_tri(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], true);
            if (px >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)px; }
        }
    }
#if defined(MIYOO)
    if (sMtProf > 0) sMtOne += thread_ns() - mt0;
    }
#endif
    sCutBuf = NULL; sCutRowMax = sCutRowMin = NULL; sCutBMin = sCutBMax = NULL;
    rs_load(&cur);
    sVisBN = 0; sVisVN = 0;
#if defined(MIYOO)
    sVisFlushes++;
#endif
    PROF_END(PROF_R3D);
}

/* FZX_PROBE=x,y (debug): log every triangle that changes that pixel of the
 * current target, with gfx_pc's note on the batch's texture state. */
extern unsigned int port_os_vi_count(void);
extern int gfx_probe_on;
extern char gfx_probe_note[256];
static int sProbeX, sProbeY;

static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    STAT(stat_batch_begin(!sPhase3D));
    PROF_BEGIN();
    if (gfx_probe_on < 0) {
        const char *e = getenv("FZX_PROBE");
        gfx_probe_on = e && sscanf(e, "%d,%d", &sProbeX, &sProbeY) == 2;
    }
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
    if (sScrOn && !sPhase3D) { /* 2D-phase triangles into native: their bounding box */
        float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
        for (size_t i = 0; i < num_tris * 3; i++) {
            bx0 = v[i].x < bx0 ? v[i].x : bx0; bx1 = v[i].x > bx1 ? v[i].x : bx1;
            by0 = v[i].y < by0 ? v[i].y : by0; by1 = v[i].y > by1 ? v[i].y : by1;
        }
        if (num_tris) scr_mark(bx0 < -1e6f ? -1000000 : (int)bx0 - 1, bx1 > 1e6f ? 1000000 : (int)bx1 + 2,
                               by0 < -1e6f ? -1000000 : (int)by0 - 1, by1 > 1e6f ? 1000000 : (int)by1 + 2);
    }
    if (gfx_soft_defer < 0) {
        const char *e = getenv("FZX_DEFER");
#ifdef FZX_STAT
        gfx_soft_defer = e && e[0] == '1'; /* the census measures the display lists' own overdraw */
#else
        gfx_soft_defer = !(e && e[0] == '0');
#endif
    }
    if (gfx_soft_vis < 0) {
        const char *e = getenv("FZX_VIS");
#ifdef FZX_STAT
        gfx_soft_vis = e && e[0] == '1'; /* as the deferral: the census measures the display lists' own overdraw */
#else
        gfx_soft_vis = !(e && e[0] == '0');
#endif
    }
    const bool probing = gfx_probe_on > 0 && sProbeX < sW && sProbeY < sH; /* a probe wants every triangle drawn */
    if (gfx_soft_vis && sPhase3D && !probing) {
        if (vis_record(v, num_tris)) {
            STAT(stat_batch_end(num_tris));
            PROF_END(PROF_R3D);
            return;
        }
        /* not kept (vis_record drew what it held first): drawn now; nothing is deferred */
    } else {
        vis_flush();
        if (gfx_soft_defer && sPhase3D && !probing) {
            if (!sUseAlpha && !sDepthTest) {
                if (!sDepthMask && def_record(v, num_tris)) {
                    STAT(stat_batch_end(num_tris));
                    PROF_END(PROF_R3D);
                    return;
                }
                if (sDefN) def_cover(v, num_tris);
            } else if (sDefN) {
                def_resolve_batch(v, num_tris);
            }
        } else {
            def_resolve_all();
        }
    }
    for (size_t i = 0; i < num_tris; i++) {
        const int probe = gfx_probe_on && sProbeX < sW && sProbeY < sH;
        const uint32_t before = probe ? sFB[sProbeY * sW + sProbeX] : 0;
        const int px = raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
        if (probe && sFB[sProbeY * sW + sProbeX] != before) {
            const SoftTex *t = sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1]) ? &sTex[sBoundTex[0]] : NULL;
            port_log("probe vi %u %06X: tex %dx%d uv (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) xy (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) | %s\n",
                     (unsigned) port_os_vi_count(), (unsigned) (sFB[sProbeY * sW + sProbeX] & 0xFFFFFF), t ? t->w : 0, t ? t->h : 0,
                     v[i*3].u, v[i*3].v, v[i*3+1].u, v[i*3+1].v, v[i*3+2].u, v[i*3+2].v,
                     v[i*3].x, v[i*3].y, v[i*3+1].x, v[i*3+1].y, v[i*3+2].x, v[i*3+2].y, gfx_probe_note);
        }
        /* Profiling counters are 3D-only: this path also serves the ortho
         * (2D-phase) batches, whose time lands in PROF_R2D, and counting their
         * pixels here understated the GFX3D line's ns/px (round 13). */
        if (sPhase3D && px >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)px; }
    }
    STAT(stat_batch_end(num_tris));
    PROF_END(sPhase3D ? PROF_R3D : PROF_R2D);
}
/* gfx_pc tells us when a batch is real perspective geometry (the sky quads
 * also render on the internal target but must not count as "3D present"). */
void gfx_scegu_note_persp(void) { sHad3D = true; }

/* 2D path (HUD, menus, logo): gfx_pc.c emits PSP GU_SPRITES — TWO corners per
 * rect in a VertexColor {u16 u,v; RGBA; u16 x,y,z}: screen pixels + TEXEL UVs
 * (tile-origin already subtracted), combiner colour pre-folded by
 * gfx_eval_vertex_color. GU takes the sprite colour from the second vertex. */
typedef struct __attribute__((aligned(16))) {
    int16_t u, v; /* signed: left of / above the tile origin gives -1, not 65535 */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* Magnified 2D sprites (the lead's matrix X14, from super-smash-bros 872636f): the N64 filters texture
 * rectangles like triangles, so a sprite drawn bigger than its texture comes out smooth. The port
 * sampled them nearest: F-Zero X's race-results portraits (32x32, flown in at up to 7x) came out in
 * big square blocks. gfx_pc sets gfx_soft_2d_bilin from the draw's G_TF_BILERP per sprite; a sprite
 * magnified along either axis then takes the 3D path's filter (bilerp: the 3-tap by default) at the
 * pixel centre. Shrunk sprites (the HUD at 240) keep the area filter, 1:1 ones the copy.
 * FZX_2DBILIN=0: nearest, as before. */
bool gfx_soft_2d_bilin SAVESTATE_EXCLUDE;
static int sStaticFade SAVESTATE_EXCLUDE = -1; /* FZX_STATICFADE (the solid-rect loop of the 2D path) */
static int sBilin2DOn SAVESTATE_EXCLUDE = -1;
int gfx_soft_hudswar SAVESTATE_EXCLUDE = -1; /* FZX_HUDSWAR=0: the HUD box loop with /255 and a channel at a time (A/B) */
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { untex_note("2d"); tex = NULL; }
    const bool modulate = !sCurShader || sCurShader->cc.num_inputs > 0;
    sNatDirty = true; /* gfx_pc switches to the 2D phase before calling us */
    vis_flush();
    def_resolve_all();
    STAT(stat_batch_begin(true));
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
        if (sScrOn) scr_mark(cx0, cx1, cy0, cy1);
        const int cr = b->r, cg = b->g, cb2 = b->b, ca = b->a; /* GU: colour of 2nd vertex */
        /* Per-sprite constants hoisted out of the pixel loop. amin folds the
         * texture-edge (0x55) and near-zero (8) alpha rejects. "plain" = the
         * texel is the final colour (no combiner input, or a white opaque
         * prim, which is what nearly every HUD sprite and menu tile uses). */
        const bool use_alpha = sUseAlpha;
        const int amin = use_alpha ? ((sCurShader && sCurShader->cc.opt_texture_edge) ? 0x55 : 8) : 0;
        if (sStaticFade < 0) { const char *e = getenv("FZX_STATICFADE"); sStaticFade = !(e && e[0] == '0'); }
        const bool dither_ac = sStaticFade && use_alpha && sCurShader && sCurShader->cc.opt_noise;
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa, rgb_decal = sCurShader && sCurShader->rgb_decal;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (gfx_soft_hud_box < 0) { const char *e = getenv("FZX_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
        /* shrunk 1..3x along S: area-average (see texel_box) */
        const bool boxf = tex && gfx_soft_hud_box && dudx > 1.02f && dudx <= 3.0f;
        if (sBilin2DOn < 0) { const char *e = getenv("FZX_2DBILIN"); sBilin2DOn = !(e && e[0] == '0'); }
        const bool mag2 = tex && sBilin2DOn && gfx_soft_2d_bilin && !boxf && (dudx < 0.999f || dvdy < 0.999f);
        const bool plain = !boxf && !mag2 && tex && !t16 && rgb_tex && a_tex && !rgb_texa &&
                           ((!rgb_in && !a_in) || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        const bool boxc = boxf && cx1 > cx0 && cx1 - cx0 <= BOX_MAX; /* a row at a time through the cache */
#ifdef HOT_NEON
        /* the general loop's combiner, for its NEON form: 0 colour, 1 texel, 2 decal, 3 texel x
         * colour; -1 (texel alpha as RGB, TEXEL-lerp, add) stays scalar */
        const int hud_mode = rgb_texa ? -1 : !rgb_tex ? 0 : !rgb_in ? 1 : rgb_decal ? 2 : (!sTexLerp && !sTexAdd) ? 3 : -1;
        if (sHotNeon < 0) { const char *e = getenv("FZX_NEON"); sHotNeon = e && e[0] == '1'; }
#endif
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
                    const uint32_t c = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255);
                    for (int x = cx0; x < cx1; x++) row[x] = c;
                } else if (ca >= amin && dither_ac) {
                    /* FZX_STATICFADE: alpha compare against the RDP's random value (G_AC_DITHER): a pixel is
                     * blended only where its alpha is at least a fresh 8-bit random number, so the static
                     * fade (transition.c, machine settings -> race) eats the picture away in specks, and
                     * the rest stays untouched, as on the N64 (angrylion). A counter hash per pixel and
                     * frame keeps fixed-clock runs repeatable. FZX_STATICFADE=0: a plain even fade. */
                    const uint32_t fr = (uint32_t)port_os_vi_count() * 0x9E3779B1u;
                    for (int x = cx0; x < cx1; x++) {
                        uint32_t h = fr ^ ((uint32_t)y * 0x85EBCA77u) ^ ((uint32_t)x * 0xC2B2AE3Du);
                        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
                        if (ca < (int)(h & 0xFF)) continue;
                        const uint32_t d = row[x];
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                        row[x] = argb((uint8_t)((cr*ca + dr*(255-ca))/255), (uint8_t)((cg*ca + dg*(255-ca))/255), (uint8_t)((cb2*ca + db*(255-ca))/255), 255);
                    }
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
            uint32_t boxtmp[BOX_MAX];
            const uint32_t *boxrow = boxc ? box_row_cached(boxtmp, cx1 - cx0, (uint32_t)(tex - sTex), texpx, t16, trow_base, uf,
                                                           duf, tw, ws2, mu, use_alpha)
                                          : NULL;
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
#ifdef HOT_NEON
            if (sHotNeon && hud_mode >= 0) {
                /* The general loop below in u16 lanes (B G R A) for its common combiners (the
                 * HUD's texture, texture x colour and decal sprites): the same alpha test and
                 * blend, div255 == /255 for every value here (<= 255 * 255). */
                const uint16x4_t v255 = vdup_n_u16(255), a255 = vcreate_u16(0x00FF000000000000ull);
                const uint16_t c0[4] = { (uint16_t)cb2, (uint16_t)cg, (uint16_t)cr, (uint16_t)ca };
                const uint16x4_t C = vld1_u16(c0);
                for (int x = cx0; x < cx1; x++, uf += duf) {
                    const uint32_t t = boxrow ? boxrow[x - cx0]
                                     : boxf ? texel_box(texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha)
                                            : mag2 ? bilerp(texpx, t16, tw, th, ws2, wt2, uf, vf_row, mu, mv)
                                            : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                    const int ta = (int)(t >> 24);
                    const int fa = !a_tex ? ca : (!a_in ? ta : div255(ta * ca));
                    if (use_alpha && fa < amin) continue;
                    const uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(t))));
                    uint16x4_t M;
                    if (hud_mode == 0)      M = C;
                    else if (hud_mode == 1) M = T;
                    else if (hud_mode == 2) { const uint16x4_t TA = vdup_lane_u16(T, 3); M = div255_u16x4(vmla_u16(vmul_u16(T, TA), C, vsub_u16(v255, TA))); }
                    else                    M = div255_u16x4(vmul_u16(T, C));
                    if (use_alpha && fa < 255) {
                        const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(row[x]))));
                        M = div255_u16x4(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));
                    }
                    M = vorr_u16(M, a255);
                    vst1_lane_u32(row + x, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
                }
                continue;
            }
#endif
            /* Round 13: the general loop below with its combiner and fetch fixed, for the common
             * case (a cached box row, the race HUD's sprites): the same expressions, one loop per
             * combiner, instead of the whole choice chain on every pixel. */
            if (gfx_soft_hudswar < 0) { const char *e = getenv("FZX_HUDSWAR"); gfx_soft_hudswar = !(e && e[0] == '0'); }
            if (boxrow && gfx_soft_hudswar && !rgb_texa && (!rgb_tex || !rgb_in || rgb_decal || (!sTexLerp && !sTexAdd))) {
                /* The speed grind (2026-09-28): the loop below with each /255 as div255 (the same
                 * floor for every x <= 255 * 255: every product here) and red and blue in the lanes of
                 * one word wherever they share a factor (the decal's texel alpha, the alpha blend),
                 * SWAR_DIV255's packing. Bit for bit the loop below; FZX_HUDSWAR=0 runs that one. */
                const uint32_t *br = boxrow - cx0;
                const uint32_t crb = (uint32_t)cr << 16 | (uint32_t)cb2;
#define HUD_DIV255X2(x) ((((x) + 0x00010001u + (((x) >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu)
#define HUD_BOX_LOOP2(COMB)                                                                         \
                for (int x = cx0; x < cx1; x++) {                                                   \
                    const uint32_t t = br[x];                                                       \
                    const int ta = (int)(t >> 24);                                                  \
                    uint32_t frb; int fg;                                                           \
                    COMB                                                                            \
                    if (use_alpha) {                                                                \
                        const int fa = !a_tex ? ca : (!a_in ? ta : div255(ta * ca));                \
                        if (fa < amin) continue;                                                    \
                        if (fa < 255) {                                                             \
                            const uint32_t d = row[x];                                              \
                            frb = HUD_DIV255X2(frb * (uint32_t)fa + (d & 0x00FF00FFu) * (uint32_t)(255 - fa)); \
                            fg = div255(fg * fa + (int)((d >> 8) & 0xff) * (255 - fa));             \
                        }                                                                           \
                    }                                                                               \
                    row[x] = 0xFF000000u | frb | (uint32_t)fg << 8;                                 \
                }
                if (!rgb_tex)       { HUD_BOX_LOOP2(frb = crb; fg = cg;) }
                else if (!rgb_in)   { HUD_BOX_LOOP2(frb = t & 0x00FF00FFu; fg = (int)((t >> 8) & 0xff);) }
                else if (rgb_decal) { HUD_BOX_LOOP2(frb = HUD_DIV255X2((t & 0x00FF00FFu) * (uint32_t)ta + crb * (uint32_t)(255 - ta));
                                                    fg = div255((int)((t >> 8) & 0xff) * ta + cg * (255 - ta));) }
                else                { HUD_BOX_LOOP2(frb = (uint32_t)div255((int)((t >> 16) & 0xff) * cr) << 16 | (uint32_t)div255((int)(t & 0xff) * cb2);
                                                    fg = div255((int)((t >> 8) & 0xff) * cg);) }
#undef HUD_BOX_LOOP2
#undef HUD_DIV255X2
                continue;
            }
            if (boxrow && !rgb_texa && (!rgb_tex || !rgb_in || rgb_decal || (!sTexLerp && !sTexAdd))) {
                const uint32_t *br = boxrow - cx0;
#define HUD_BOX_LOOP(COMB)                                                                          \
                for (int x = cx0; x < cx1; x++) {                                                   \
                    const uint32_t t = br[x];                                                       \
                    const int ta = (t>>24)&0xff, tr = (t>>16)&0xff, tg = (t>>8)&0xff, tb = t&0xff;  \
                    int fr, fg, fb, fa;                                                             \
                    (void)tr; (void)tg; (void)tb;                                                   \
                    COMB                                                                            \
                    fa = !a_tex ? ca : (!a_in ? ta : (ta*ca)/255);                                  \
                    if (use_alpha) {                                                                \
                        if (fa < amin) continue;                                                    \
                        if (fa < 255) {                                                             \
                            const uint32_t d = row[x];                                              \
                            const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;                    \
                            fr = (fr*fa + dr*(255-fa))/255;                                         \
                            fg = (fg*fa + dg*(255-fa))/255;                                         \
                            fb = (fb*fa + db*(255-fa))/255;                                         \
                        }                                                                           \
                    }                                                                               \
                    row[x] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);                         \
                }
                if (!rgb_tex)       { HUD_BOX_LOOP(fr=cr; fg=cg; fb=cb2;) }
                else if (!rgb_in)   { HUD_BOX_LOOP(fr=tr; fg=tg; fb=tb;) }
                else if (rgb_decal) { HUD_BOX_LOOP(fr=(tr*ta + cr*(255-ta))/255; fg=(tg*ta + cg*(255-ta))/255; fb=(tb*ta + cb2*(255-ta))/255;) }
                else                { HUD_BOX_LOOP(fr=(tr*cr)/255; fg=(tg*cg)/255; fb=(tb*cb2)/255;) }
#undef HUD_BOX_LOOP
                continue;
            }
            /* General: per-channel combiner (RGB and alpha each from the
             * texel, the input colour, or their product). */
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = boxrow ? boxrow[x - cx0]
                                 : boxf ? texel_box(texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha)
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
    STAT(stat_batch_end(num_sprites));
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
/* Round 13 (the user's pick, 2026-09-26: the 3D at 80% + the 3-tap filter, to reach 60): the
 * composite scales the 3D up BILINEARLY (pixel centres: s = (x + 0.5) * iw / nw - 0.5, clamped),
 * sLin* per native column and row: the first source pixel, the second (clamped) and the 8-bit
 * weight of the second. */
static uint16_t sLinX0[MAP_MAX] SAVESTATE_EXCLUDE, sLinX1[MAP_MAX] SAVESTATE_EXCLUDE, sLinY0[MAP_MAX] SAVESTATE_EXCLUDE,
                sLinY1[MAP_MAX] SAVESTATE_EXCLUDE;
static uint8_t sLinXF[MAP_MAX] SAVESTATE_EXCLUDE, sLinYF[MAP_MAX] SAVESTATE_EXCLUDE;
static void lin_map(uint16_t *m0, uint16_t *m1, uint8_t *mf, int n, int src) {
    for (int i = 0; i < n && i < MAP_MAX; i++) {
        const int s8 = ((2 * i + 1) * src * 256 / n - 256) / 2; /* the source centre in 1/256 px */
        int i0 = s8 < 0 ? 0 : s8 >> 8, f = s8 < 0 ? 0 : s8 & 255;
        if (i0 >= src - 1) { i0 = src - 1; f = 0; }
        m0[i] = (uint16_t)i0; m1[i] = (uint16_t)(i0 + 1 < src ? i0 + 1 : i0); mf[i] = (uint8_t)f;
    }
}
/* The native target holds last frame's picture until something writes it (round 13): the
 * frame's clear is left to the first 2D draw (nat_ensure), and the composite, when it comes
 * first, writes every pixel instead -- in a race the full-size 230 KB clear was a wasted pass. */
static bool sNatStale SAVESTATE_EXCLUDE;
/* 80% across (192 -> 240): the columns repeat every 5 native pixels, 4 internal ones on (the
 * source centre steps 0.8 px, so five steps are exactly 4). sLin45 checks that on the tables
 * themselves, from column 5 (the first whose left tap is not clamped) to sLin45End, and keeps the
 * five weights, so a NEON loop can do 20 columns with constant weights and give lin_row's bytes. */
static int sLin45 SAVESTATE_EXCLUDE, sLin45End SAVESTATE_EXCLUDE;
static uint8_t sLin45W[5] SAVESTATE_EXCLUDE;
static uint8_t sLin45WA[5][16] __attribute__((aligned(16))) SAVESTATE_EXCLUDE, sLin45WB[5][16] __attribute__((aligned(16))) SAVESTATE_EXCLUDE; /* per 4-column vector */
static void build_maps(void) {
    if (sNatW <= 0 || sNatH <= 0 || sIntW <= 0 || sIntH <= 0) return;
    lin_map(sLinX0, sLinX1, sLinXF, sNatW, sIntW);
    lin_map(sLinY0, sLinY1, sLinYF, sNatH, sIntH);
    sLin45 = 0; sLin45End = 5;
    if (sIntW * 5 == sNatW * 4 && sNatW >= 30 && sNatW <= MAP_MAX) {
        int ok = 1, end = 5;
        for (int i = 5; i < 10; i++) {
            sLin45W[i - 5] = sLinXF[i];
            ok &= sLinXF[i] != 0 && sLinX0[i] == sLinX0[5] + (i - 5) && sLinX1[i] == sLinX0[i] + 1;
        }
        /* the longest run of whole 20-column groups from column 5 that keeps the pattern */
        while (ok && end + 20 <= sNatW) {
            for (int i = end; i < end + 20 && ok; i++)
                ok = sLinX1[i] == sLinX0[i] + 1 && sLinXF[i] == sLin45W[(i - 5) % 5] && (i < 10 || sLinX0[i] == sLinX0[i - 5] + 4);
            if (ok) end += 20;
        }
        sLin45End = end;
        sLin45 = end > 5;
        for (int q = 0; q < 5; q++)
            for (int b = 0; b < 16; b++) {
                const int j = (4 * q + b / 4) % 5;
                sLin45WB[q][b] = sLin45W[j]; sLin45WA[q][b] = (uint8_t)(256 - sLin45W[j]);
            }
    }
    for (int x = 0; x < sNatW && x < MAP_MAX; x++) sN2I_x[x] = (uint16_t)(x * sIntW / sNatW);
    for (int y = 0; y < sNatH && y < MAP_MAX; y++) sN2I_y[y] = (uint16_t)(y * sIntH / sNatH);
    for (int x = 0; x < sIntW && x < MAP_MAX; x++) sI2N_x[x] = (uint16_t)(x * sNatW / sIntW);
    for (int y = 0; y < sIntH && y < MAP_MAX; y++) sI2N_y[y] = (uint16_t)(y * sNatH / sIntH);
}
/* Presented image: always the native target (composited at end_frame). */
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }
/* A caller that wrote the whole native picture itself, mid-frame (gfx_pc.c's boot logo, the N64
 * logo the CPU draws): it is the frame's picture now, so the lazy clear must not wipe it (the
 * logo was lost at 80% from b0e60cc to 39d780f: the frame's end cleared the "stale" target). */
void gfx_soft_native_written(void) { sNatStale = false; sNatDirty = true; }

static void alloc_target(uint32_t **fb, uint16_t **zb, int *w, int *h, int nw, int nh) {
    if (*fb && *w == nw && *h == nh) return;
    free(*fb); free(*zb);
    *w = nw; *h = nh;
    *fb = (uint32_t *)malloc((size_t)nw * nh * 4);
    *zb = (uint16_t *)malloc((size_t)nw * nh * 2);
}

static void nat_ensure(void) {
    if (!sNatStale) return;
    sNatStale = false;
    if (sNatFB) memset(sNatFB, 0, (size_t)sNatW * sNatH * 4);
}
static void select_target(bool is3d) {
    extern struct GfxDimensions gfx_current_dimensions;
    if (!is3d || sUnified) nat_ensure();
    sPhase3D = is3d;
    if (is3d && !sUnified) { sFB = sIntFB; sZB = sIntZB; sW = sIntW; sH = sIntH; }
    else                   { sFB = sNatFB; sZB = sNatZB; sW = sNatW; sH = sNatH; }
    sScX0 = 0; sScY0 = 0; sScX1 = sW; sScY1 = sH;
    sVpX = 0; sVpY = 0; sVpW = sW; sVpH = sH;
    gfx_current_dimensions.width = (uint32_t)sW;
    gfx_current_dimensions.height = (uint32_t)sH;
    gfx_current_dimensions.aspect_ratio = sH ? (float)sW / (float)sH : 1.0f;
}

/* The scaled composite (round 13): every internal row is scaled across once, into a ring of
 * two native-width rows, and zeroed while it is still in the cache (the old composite cleared the
 * whole internal buffer in a pass of its own afterwards); each native row is then the vertical
 * blend of two ring rows. Channels blend as lerp2 does, alpha too: where it comes out 0 no 3D is
 * there. A native target still stale takes every pixel (0 where no 3D: the clear it skipped).
 * Otherwise only the covered pixels are written -- and then the internal buffer was seeded with
 * the 2D beneath (the only way the native target is dirty when the 3D is composited), so the
 * blended colour already holds that backdrop at the 3D's edges. */
/* The composite straight into the screen (round 13b): when it writes every native pixel (a race
 * frame), it also converts each finished row into the display surface the present would have
 * written (gfx_wm_screen_acquire: the same wait for the last flip first), and any 2D drawn after
 * it marks its 16-pixel blocks in sScrBlk, so the present converts only those (the HUD) instead
 * of the whole 230 KB picture. A second composite, or no screen, sends the frame through the full
 * present (sScrAll). gfx_wm_row_to_565 is the present's own conversion, so the screen gets the
 * same bytes either way. */
extern uint16_t *gfx_wm_screen_acquire(int *pitch_px);
extern void gfx_wm_row_to_565(uint16_t *dst, const uint32_t *src, int n);
static uint16_t sScrBlk[MAP_MAX] SAVESTATE_EXCLUDE;
/* The blocks the last frame's 2D covered (the HUD barely moves): the composite leaves them to
 * the present, which converts them with this frame's marks, so a block is written to the screen
 * once. A frame whose 2D covers less just converts those blocks at the present. */
static uint16_t sScrPrev[MAP_MAX] SAVESTATE_EXCLUDE;
static bool sScrPrevOk SAVESTATE_EXCLUDE;
static void scr_row(uint16_t *d, const uint32_t *r, uint32_t skip, int w) {
    if (!skip) { gfx_wm_row_to_565(d, r, w); return; }
    uint32_t m = ~skip & ((1u << ((w + 15) >> 4)) - 1u);
    while (m) {
        const int b0 = __builtin_ctz(m);
        int b1 = b0;
        while ((m >> (b1 + 1)) & 1u) b1++;
        m &= ~(((2u << b1) - 1u) & ~((1u << b0) - 1u));
        const int x0 = b0 * 16, x1 = (b1 + 1) * 16 > w ? w : (b1 + 1) * 16;
        gfx_wm_row_to_565(d + x0, r + x0, x1 - x0);
    }
}
static void scr_mark(int x0, int x1, int y0, int y1) {
    if (!sScrOn || sScrAll) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sNatW) x1 = sNatW;
    if (y1 > sNatH) y1 = sNatH;
    if (x0 >= x1 || y0 >= y1) return;
    const uint32_t m = ((2u << ((x1 - 1) >> 4)) - 1u) & ~((1u << (x0 >> 4)) - 1u);
    for (int y = y0; y < y1; y++) sScrBlk[y] |= (uint16_t)m;
}
/* gfx_sdl12.c's present: 1 and the blocks when only they need converting, else 0 (all of it) */
int gfx_soft_screen_blocks(const uint16_t **blk) {
    if (!sScrOn || sScrAll || sNatW > 256) { sScrPrevOk = false; return 0; }
    for (int y = 0; y < sNatH; y++) { /* this frame's marks go on as the next frame's guess */
        const uint16_t m = sScrBlk[y];
        sScrBlk[y] = m | (sScrPrevOk ? sScrPrev[y] : 0); /* what the composite left + this frame's 2D */
        sScrPrev[y] = m;
    }
    sScrPrevOk = true;
    *blk = sScrBlk;
    return 1;
}
static uint32_t sLinRow[2][MAP_MAX] SAVESTATE_EXCLUDE;
static int sLinRowY[2] SAVESTATE_EXCLUDE;
static const uint32_t *lin_row(int iy) {
    uint32_t *r = sLinRow[iy & 1];
    if (sLinRowY[iy & 1] == iy) return r;
    sLinRowY[iy & 1] = iy;
    uint32_t *src = sIntFB + iy * sIntW;
    int x = 0;
#ifdef HOT_NEON
    if (sLin45) {
        for (; x < 5; x++) { /* the clamped left edge, from the tables */
            const uint32_t a = src[sLinX0[x]], b = src[sLinX1[x]];
            const int f = sLinXF[x];
            r[x] = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, f) | lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, f) << 8;
        }
        /* 20 columns a pass from column 5k: column 5k + c blends src[t + c - c / 5] and the pixel
         * after it (t = sLinX0[5k]) by w[c % 5]. Four columns a vector: their left taps are the
         * source pixels at 0 1 2 3 | 4 4 5 6 | 7 8 8 9 | 10 11 12 12 | 13 14 15 16 (from t):
         * one 16-byte load each, the repeats made by vext/vdup; the right taps the same from t + 1.
         * The blend is lerp2's (x * (256 - w) + y * w) >> 8 in byte lanes (no weight is 0). */
        for (; x < sLin45End; x += 20) {
            const uint8_t *a8 = (const uint8_t *)(src + sLinX0[x]);
            uint8_t *o8 = (uint8_t *)(r + x);
#define LIN45_Q(q, TAPS)                                                                                 \
            {   const uint8_t *p = a8; uint8x16_t A, B; TAPS(A, p) p += 4; TAPS(B, p)                   \
                const uint8x16_t WA = vld1q_u8(sLin45WA[q]), WB = vld1q_u8(sLin45WB[q]);                 \
                const uint8x8_t lo = vshrn_n_u16(vmlal_u8(vmull_u8(vget_low_u8(A), vget_low_u8(WA)), vget_low_u8(B), vget_low_u8(WB)), 8); \
                const uint8x8_t hi = vshrn_n_u16(vmlal_u8(vmull_u8(vget_high_u8(A), vget_high_u8(WA)), vget_high_u8(B), vget_high_u8(WB)), 8); \
                vst1q_u8(o8 + 16 * q, vcombine_u8(lo, hi)); }
#define T0(T, p) T = vld1q_u8(p);
#define T1(T, p) { const uint8x16_t P = vld1q_u8(p + 16); \
                   T = vextq_u8(vreinterpretq_u8_u32(vdupq_lane_u32(vget_low_u32(vreinterpretq_u32_u8(P)), 0)), P, 12); }
#define T2(T, p) { const uint8x16_t P = vld1q_u8(p + 28); T = vcombine_u8(vget_low_u8(P), vext_u8(vget_low_u8(P), vget_high_u8(P), 4)); }
#define T3(T, p) { const uint8x16_t P = vld1q_u8(p + 40); \
                   T = vcombine_u8(vget_low_u8(P), vreinterpret_u8_u32(vdup_lane_u32(vreinterpret_u32_u8(vget_high_u8(P)), 0))); }
#define T4(T, p) T = vld1q_u8(p + 52);
            LIN45_Q(0, T0) LIN45_Q(1, T1) LIN45_Q(2, T2) LIN45_Q(3, T3) LIN45_Q(4, T4)
#undef LIN45_Q
#undef T0
#undef T1
#undef T2
#undef T3
#undef T4
        }
    }
    if (!sLin45) {
        /* Any ratio (the screen filled: 177.6 internal columns over 240), four columns a pass: the
         * taps loaded lane by lane from the tables, each column's weight repeated over its four
         * bytes. lerp2's (a * (256 - f) + b * f) >> 8 as a * 256 - a * f + b * f in u16 lanes: the
         * true value is 0..65280, so the wrap-around arithmetic lands on it exactly. */
        static const uint8_t kRep[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 };
        const uint8x8_t ilo = vld1_u8(kRep), ihi = vld1_u8(kRep + 8);
        for (; x + 4 <= sNatW; x += 4) {
            uint32x4_t A = vld1q_dup_u32(src + sLinX0[x]), B = vld1q_dup_u32(src + sLinX1[x]);
            A = vld1q_lane_u32(src + sLinX0[x + 1], A, 1); B = vld1q_lane_u32(src + sLinX1[x + 1], B, 1);
            A = vld1q_lane_u32(src + sLinX0[x + 2], A, 2); B = vld1q_lane_u32(src + sLinX1[x + 2], B, 2);
            A = vld1q_lane_u32(src + sLinX0[x + 3], A, 3); B = vld1q_lane_u32(src + sLinX1[x + 3], B, 3);
            uint32_t f4;
            memcpy(&f4, sLinXF + x, 4);
            const uint8x8_t F = vreinterpret_u8_u32(vdup_n_u32(f4));
            const uint8x8_t Flo = vtbl1_u8(F, ilo), Fhi = vtbl1_u8(F, ihi);
            const uint8x16_t a8 = vreinterpretq_u8_u32(A), b8 = vreinterpretq_u8_u32(B);
            uint16x8_t lo = vshll_n_u8(vget_low_u8(a8), 8), hi = vshll_n_u8(vget_high_u8(a8), 8);
            lo = vmlal_u8(vmlsl_u8(lo, vget_low_u8(a8), Flo), vget_low_u8(b8), Flo);
            hi = vmlal_u8(vmlsl_u8(hi, vget_high_u8(a8), Fhi), vget_high_u8(b8), Fhi);
            vst1q_u8((uint8_t *)(r + x), vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
        }
    }
#endif
    for (; x < sNatW; x++) {
        const uint32_t a = src[sLinX0[x]], b = src[sLinX1[x]];
        const int f = sLinXF[x];
        r[x] = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, f) | lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, f) << 8;
    }
    memset(src, 0, (size_t)sIntW * 4);
    return r;
}
/* The screen filled (gfx_pc.c gfx_frame_crop: a 1P race's TV-safe box, x 12..308 y 16..224): the
 * composite stretches that box of the internal 3D image over the whole native screen, and seeding
 * the 3D from 2D drawn first maps through it too. The maps follow the box, rebuilt when it changes
 * (the 4:5 NEON column path doesn't apply: the columns are no longer 4 internal to 5 native). */
extern int gfx_frame_crop(float r[4]);
static int sMapCropOn SAVESTATE_EXCLUDE;
static float sMapCrop[4] SAVESTATE_EXCLUDE;
static int sCropLoY SAVESTATE_EXCLUDE, sCropHiY SAVESTATE_EXCLUDE; /* the internal rows the box covers */
/* n native pixels from the source range [a, b) (source px), bilinear at pixel centres, taps kept to
 * the pixels whose centres lie in the range */
static void lin_map_sub(uint16_t *m0, uint16_t *m1, uint8_t *mf, int n, int src, float a, float b, int *plo, int *phi) {
    int lo = (int) ceilf(a - 0.5f), hi = (int) floorf(b - 0.5f);
    if (lo < 0) lo = 0;
    if (hi > src - 1) hi = src - 1;
    if (hi < lo) hi = lo;
    for (int i = 0; i < n && i < MAP_MAX; i++) {
        const float sc = a + ((float) i + 0.5f) * (b - a) / (float) n - 0.5f;
        const int s8 = (int) floorf(sc * 256.0f);
        int i0 = s8 >> 8, f = s8 & 255;
        if (i0 < lo) { i0 = lo; f = 0; }
        if (i0 >= hi) { i0 = hi; f = 0; }
        m0[i] = (uint16_t) i0; m1[i] = (uint16_t) (i0 + 1 <= hi ? i0 + 1 : i0); mf[i] = (uint8_t) f;
    }
    if (plo) *plo = lo;
    if (phi) *phi = hi;
}
static void maps_follow_crop(void) {
    float r[4];
    const int on = gfx_frame_crop(r);
    if (on == sMapCropOn && (!on || memcmp(r, sMapCrop, sizeof(r)) == 0)) return;
    sMapCropOn = on;
    if (!on) { build_maps(); return; }
    memcpy(sMapCrop, r, sizeof(r));
    const float ax = r[0] * (float) sIntW / 320.0f, bx = r[2] * (float) sIntW / 320.0f;
    const float ay = r[1] * (float) sIntH / 240.0f, by = r[3] * (float) sIntH / 240.0f;
    lin_map_sub(sLinX0, sLinX1, sLinXF, sNatW, sIntW, ax, bx, NULL, NULL);
    lin_map_sub(sLinY0, sLinY1, sLinYF, sNatH, sIntH, ay, by, &sCropLoY, &sCropHiY);
    sLin45 = 0;
    for (int x = 0; x < sNatW && x < MAP_MAX; x++) { int v = (int) (ax + ((float) x + 0.5f) * (bx - ax) / (float) sNatW); sN2I_x[x] = (uint16_t) (v < 0 ? 0 : v >= sIntW ? sIntW - 1 : v); }
    for (int y = 0; y < sNatH && y < MAP_MAX; y++) { int v = (int) (ay + ((float) y + 0.5f) * (by - ay) / (float) sNatH); sN2I_y[y] = (uint16_t) (v < 0 ? 0 : v >= sIntH ? sIntH - 1 : v); }
    for (int x = 0; x < sIntW && x < MAP_MAX; x++) { int v = (int) floorf((((float) x + 0.5f) - ax) * (float) sNatW / (bx - ax)); sI2N_x[x] = (uint16_t) (v < 0 ? 0 : v >= sNatW ? sNatW - 1 : v); }
    for (int y = 0; y < sIntH && y < MAP_MAX; y++) { int v = (int) floorf((((float) y + 0.5f) - ay) * (float) sNatH / (by - ay)); sI2N_y[y] = (uint16_t) (v < 0 ? 0 : v >= sNatH ? sNatH - 1 : v); }
}
int gfx_soft_compneon SAVESTATE_EXCLUDE = -1; /* FZX_COMPNEON=0: the composite over 2D a pixel at a time (A/B) */
static uint32_t sCompWhole SAVESTATE_EXCLUDE, sCompOver SAVESTATE_EXCLUDE; /* composites of each kind (the exit line) */
void gfx_soft_comp_counts(uint32_t *whole, uint32_t *over) { *whole = sCompWhole; *over = sCompOver; }
static void composite_bilinear(void) {
    if (gfx_soft_compneon < 0) { const char *e = getenv("FZX_COMPNEON"); gfx_soft_compneon = !(e && e[0] == '0'); }
    maps_follow_crop();
    const bool stale = sNatStale;
    sNatStale = false;
    sLinRowY[0] = sLinRowY[1] = -1;
    uint16_t *scr = NULL;
    int sp = 0;
    if (sScrOn) sScrAll = true; /* a second composite this frame: the full present */
    else if (stale && sNatW <= 256 && (scr = gfx_wm_screen_acquire(&sp)) != NULL) {
        sScrOn = true; sScrAll = false;
        memset(sScrBlk, 0, sizeof(sScrBlk));
    }
    const uint16_t *skip = scr && sScrPrevOk ? sScrPrev : NULL;
    for (int y = 0; y < sNatH; y++) {
        const int y0 = sLinY0[y], y1 = sLinY1[y], f = sLinYF[y];
        uint32_t *dst = sNatFB + y * sNatW;
        const uint32_t *r0 = lin_row(y0), *r1 = f ? lin_row(y1) : r0;
        if (stale) {
            if (!f) { memcpy(dst, r0, (size_t)sNatW * 4); if (scr) scr_row(scr + y * sp, dst, skip ? skip[y] : 0, sNatW); continue; }
            int x = 0;
#ifdef HOT_NEON
            const uint8x8_t w0 = vdup_n_u8((uint8_t)(256 - f)), w1 = vdup_n_u8((uint8_t)f);
            for (; x + 4 <= sNatW; x += 4) {
                const uint8x16_t a = vld1q_u8((const uint8_t *)(r0 + x)), b = vld1q_u8((const uint8_t *)(r1 + x));
                const uint8x8_t lo = vshrn_n_u16(vmlal_u8(vmull_u8(vget_low_u8(a), w0), vget_low_u8(b), w1), 8);
                const uint8x8_t hi = vshrn_n_u16(vmlal_u8(vmull_u8(vget_high_u8(a), w0), vget_high_u8(b), w1), 8);
                vst1q_u8((uint8_t *)(dst + x), vcombine_u8(lo, hi));
            }
#endif
            for (; x < sNatW; x++) {
                const uint32_t a = r0[x], b = r1[x];
                dst[x] = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, f) | lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, f) << 8;
            }
            if (scr) scr_row(scr + y * sp, dst, skip ? skip[y] : 0, sNatW);
        } else {
            int x = 0;
#ifdef HOT_NEON
            if (gfx_soft_compneon) {
                /* Over 2D drawn first (the speed grind, 2026-09-28: a device run spent ~4.4 ms a
                 * picture here, against ~1.3 for the whole-frame path above): the same lerp in byte
                 * lanes as that path (lerp2's per lane), then the 3D pixels (alpha != 0) put in
                 * with vbsl over the 2D ones. Bit for bit the loop below; FZX_COMPNEON=0 runs it. */
                const uint32x4_t amask = vdupq_n_u32(0xFF000000u);
                if (!f) {
                    for (; x + 4 <= sNatW; x += 4) {
                        const uint32x4_t p = vld1q_u32(r0 + x);
                        vst1q_u32(dst + x, vbslq_u32(vtstq_u32(p, amask), vorrq_u32(p, amask), vld1q_u32(dst + x)));
                    }
                } else {
                    const uint8x8_t w0 = vdup_n_u8((uint8_t)(256 - f)), w1 = vdup_n_u8((uint8_t)f);
                    for (; x + 4 <= sNatW; x += 4) {
                        const uint8x16_t a = vld1q_u8((const uint8_t *)(r0 + x)), b = vld1q_u8((const uint8_t *)(r1 + x));
                        const uint8x8_t lo = vshrn_n_u16(vmlal_u8(vmull_u8(vget_low_u8(a), w0), vget_low_u8(b), w1), 8);
                        const uint8x8_t hi = vshrn_n_u16(vmlal_u8(vmull_u8(vget_high_u8(a), w0), vget_high_u8(b), w1), 8);
                        const uint32x4_t p = vreinterpretq_u32_u8(vcombine_u8(lo, hi));
                        vst1q_u32(dst + x, vbslq_u32(vtstq_u32(p, amask), vorrq_u32(p, amask), vld1q_u32(dst + x)));
                    }
                }
            }
#endif
            for (; x < sNatW; x++) {
                const uint32_t a = r0[x], b = r1[x];
                const uint32_t p = !f ? a : lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, f) | lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, f) << 8;
                if (p >> 24) dst[x] = p | 0xFF000000u;
            }
        }
    }
    if (sMapCropOn) { /* the internal rows outside the box were never read (lin_row clears the rows it reads) */
        if (sCropLoY > 0) memset(sIntFB, 0, (size_t) sCropLoY * sIntW * 4);
        if (sCropHiY + 1 < sIntH) memset(sIntFB + (size_t) (sCropHiY + 1) * sIntW, 0, (size_t) (sIntH - sCropHiY - 1) * sIntW * 4);
    }
    if (stale) sCompWhole++; else sCompOver++;
    /* every internal row was scaled (and zeroed): the native size is >= the internal one, so
     * consecutive native rows step at most one internal row */
}

/* Put the internal 3D image into native where the 3D wrote (scaled: composite_bilinear)
 * (alpha == 255), then clear it for any later 3D batch this frame. */
static void composite_internal(void) {
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    PROF_BEGIN();
    if (sIntW == sNatW && sIntH == sNatH) {
        for (int i = 0; i < sNatW * sNatH; i++) if (sIntFB[i] >> 24) sNatFB[i] = sIntFB[i];
    } else {
        composite_bilinear();
        sIntDirty = false; sNatDirty = true; sIntClean = true; /* (composite_bilinear cleared it) */
        PROF_END(PROF_COMP);
        return;
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
    maps_follow_crop();
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
uint32_t gfx_soft_phase_switches SAVESTATE_EXCLUDE; /* 2D<->3D target switches (PERF) */
void gfx_scegu_set_phase(int is3d) {
    if ((bool)is3d == sPhase3D) return;
    vis_flush(); /* before the composite or seed reads a target */
    def_resolve_all();
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
    if (!sTexArena) {
        void* arena = NULL;
        /* posix_memalign: gnu99 hides C11's aligned_alloc (it was implicitly declared) */
        sTexArena = posix_memalign(&arena, 16, TEXARENA_SZ) == 0 ? arena : NULL;
        if (!sTexArena) { dprintf(2, "gfx_soft: cannot allocate the %u MB texture arena\n", (unsigned) (TEXARENA_SZ >> 20)); abort(); }
    }
    ensure_targets(); select_target(false);
}
static void gfx_soft_on_resize(void) { vis_flush(); def_resolve_all(); ensure_targets(); select_target(sPhase3D); }
u32 gfx_soft_frames_rendered SAVESTATE_EXCLUDE; /* frames that reached end_frame (frameskip excluded) */
static void gfx_soft_start_frame(void) {
    if (sDefN) { /* a frame that never ended: its pending pixels go with it */
        memset(sPend, 0, (size_t)sDefCap); memset(sPendRow, 0, (size_t)sDefRows);
        sDefN = 0; sDefVtxN = 0; sDefTexN = 0;
    }
    sVisBN = 0; sVisVN = 0; /* and its recorded batches */
    ensure_targets();
    PROF_BEGIN();
    if (sUnified) memset(sNatFB, 0, (size_t)sNatW * sNatH * 4);
    else sNatStale = true; /* cleared by the first 2D draw, or written whole by the composite */
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    PROF_END(PROF_CLR);
    STAT(stat_frame_begin());
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    sScrOn = false; sScrAll = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
static void gfx_soft_end_frame(void) {
    vis_flush();
    def_resolve_all();
    STAT(stat_frame_end());
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
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; }
void gfx_scegu_set_force_in(bool on) { sForceIn = on; }
/* FZX_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }
void gfx_scegu_set_tex_lerp2(bool on, const uint8_t a[3], const uint8_t b[3]) {
    sTexLerp2 = on;
    sL2Ar = a[0]; sL2Ag = a[1]; sL2Ab = a[2];
    sL2Br = b[0]; sL2Bg = b[1]; sL2Bb = b[2];
}
void gfx_scegu_set_post_fog(bool on, uint8_t r, uint8_t g, uint8_t b) { sPostFog = on; sFogR = r; sFogG = g; sFogB = b; }
void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b) { sTexLerp = on; sLerpR = r; sLerpG = g; sLerpB = b; }
void gfx_scegu_set_tex_fog(bool on, uint8_t r, uint8_t g, uint8_t b) { sTexFog = on; sTfR = r; sTfG = g; sTfB = b; }

/* Depth reads for the game (sun lens flare, light glows: z_kankyo.c /
 * z_lights.c). x, y are 320x240 game coordinates; the buffer holds the last
 * rendered 3D frame (the game asks during its update, before this frame is
 * drawn, as on the N64). Returns false off-buffer. */
static bool hh_read_depth(int32_t x, int32_t y, uint16_t *depth) {
    vis_flush(); /* (the game asks between frames: normally nothing is recorded) */
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
bool gfx_soft_blend_textures(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                             int uls1, int ult1, int shs1, int sht1, int rgb_mode, int a_mode,
                             int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka) {
    if (dst == 0 || dst >= MAX_TEXTURES || id0 >= MAX_TEXTURES || id1 >= MAX_TEXTURES) return false;
    vis_flush(); /* it may rewrite texels a recorded batch draws with */
    const SoftTex *t0 = &sTex[id0], *t1 = &sTex[id1];
    if (!t0->valid || !t1->valid || W <= 0 || H <= 0 || W * H > 256 * 256) return false;
    uint32_t *out;
    sTexGen[dst]++; /* rewritten, possibly in place: box row cache key */
    if (sTex[dst].valid && !sTex[dst].is16 && sTex[dst].w == W && sTex[dst].h == H) {
        out = (uint32_t *)sTex[dst].px;
    } else {
        const size_t bytes = ((size_t)W * H * 4 + 15u) & ~(size_t)15;
        if (sArenaUsed + bytes > TEXARENA_SZ) return false;
        out = (uint32_t *)&sTexArena[sArenaUsed];
        sArenaUsed += bytes;
    { const uint32_t pct = (uint32_t)(sArenaUsed * 100u / TEXARENA_SZ); if (pct > gfx_soft_arena_peak) gfx_soft_arena_peak = pct; }
        sTex[dst].px = out; sTex[dst].is16 = false; sTex[dst].w = W; sTex[dst].h = H; sTex[dst].valid = true;
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
void texman_clear(void) { vis_flush(); sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; box_cache_clear(); }

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
    vis_flush(); /* (normally nothing is recorded: the game asks between frames) */
    const uint32_t *fb = sNatFB; const int fw = sNatW, fh = sNatH;
    if (!fb || !target || w <= 0 || h <= 0 || w > 320 || h > 240 || fw <= 0 || fh <= 0) return;
    { extern void port_fb_tile_note(void *target, unsigned int bytes); port_fb_tile_note(target, (unsigned int)(w * h * 2)); } /* rewritten every frame: keeps its content hash */
    float cr[4]; /* the last frame's box, if it filled the screen (a menu without 3D has no composite) */
    const int crop = gfx_frame_crop(cr);
    for (int ty = 0; ty < h; ty++) {
        /* (the screen filled: the last frame's box was stretched over it, maps_follow_crop) */
        int fy = crop ? (int) floorf(((float) (y + ty) + 0.5f - cr[1]) * (float) fh / (cr[3] - cr[1]))
                            : (y + ty) * fh / 240;
        if (fy < 0) fy = 0;
        if (fy >= fh) fy = fh - 1;
        const uint32_t *row = &fb[fy * fw];
        for (int tx = 0; tx < w; tx++) {
            int fx = crop ? (int) floorf(((float) (x + tx) + 0.5f - cr[0]) * (float) fw / (cr[2] - cr[0]))
                                : (x + tx) * fw / 320;
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
#ifdef FZX_STAT
#include "gfx_soft_stat.h"
#endif
