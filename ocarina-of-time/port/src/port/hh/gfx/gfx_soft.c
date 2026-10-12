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
#include "port.h"
extern uint32_t port_time_us(void);
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
static bool sEdgeOpaque SAVESTATE_EXCLUDE; /* gfx_soft_set_edge_opaque */
static bool sOwnTri SAVESTATE_EXCLUDE;     /* gfx_soft_set_own_edges (OOT_FILLRULE) */
/* OOT_CRACKFILL (crack_fill): a byte a pixel of the 3D target, set where a draw that can leave colour without depth
 * covered the pixel's centre (raster_tri's cov, cov_rect); sCovMark is the mask while the 3D target sCovFB is current */
static uint8_t *sCov SAVESTATE_EXCLUDE, *sCovMark SAVESTATE_EXCLUDE;
static int sCovSz SAVESTATE_EXCLUDE;
static const uint32_t *sCovFB SAVESTATE_EXCLUDE;
static const uint32_t *sCovTarget SAVESTATE_EXCLUDE; /* the target the 3D draws to (select_target) */
static bool sSkyTag SAVESTATE_EXCLUDE; /* between the sky's tags (gfx_scegu_set_sky_pass): the sky covers nothing here */
/* OOT_EDGETINT (edge_vi): the TEX_EDGE edge pixels written unblended over a pixel without depth, for the VI's pass */
static int sEdgeTint SAVESTATE_EXCLUDE = 1;
enum { EN_MAX = 8192 };
static int32_t sEnIdx[EN_MAX] SAVESTATE_EXCLUDE;
static uint16_t sEnZ[EN_MAX] SAVESTATE_EXCLUDE;
static uint8_t sEnA[EN_MAX] SAVESTATE_EXCLUDE;
static int sEnN SAVESTATE_EXCLUDE;
static uint32_t *sEnFB SAVESTATE_EXCLUDE; /* the target they were written to */
static uint8_t *sEnMark SAVESTATE_EXCLUDE;  /* a byte a pixel: a noted pixel, still partly covered (edge_vi's own) */
static int sEnMarkSz SAVESTATE_EXCLUDE;
uint32_t gfx_soft_edge_vi SAVESTATE_EXCLUDE; /* noted edge pixels (dl_test's count) */
#if defined(MIYOO)
/* A split triangle's notes (see split_row): the worker's share notes into its own list, appended after the wait, so the
 * list holds the upper rows' notes then the lower rows', in row order, cut at EN_MAX, as one core would have made it. */
typedef struct { int n; int32_t idx[EN_MAX]; uint16_t z[EN_MAX]; uint8_t a[EN_MAX]; } EnShare;
static __thread EnShare *tEnShare; /* set on the worker for the share it draws; NULL elsewhere */
static EnShare sEnWk SAVESTATE_EXCLUDE;
#endif
static void __attribute__((noinline)) edge_note(const uint32_t *pf, int fa, int z) {
#if defined(MIYOO)
    EnShare *const es = tEnShare;
    if (es) {
        if (es->n < EN_MAX) { es->idx[es->n] = (int32_t)(pf - sEnFB); es->z[es->n] = (uint16_t)z; es->a[es->n++] = (uint8_t)fa; }
        return;
    }
#endif
    if (sEnN < EN_MAX) { sEnIdx[sEnN] = (int32_t)(pf - sEnFB); sEnZ[sEnN] = (uint16_t)z; sEnA[sEnN++] = (uint8_t)fa; }
}
static void sky_settle(void); /* OOT_SKYDEFER: the kept sky drawn now, if any (see raster_tri's rows) */
static void sky_texels_change(const void *px); /* the same, if a kept sky triangle reads the texels at px */
static uint32_t sNatFill SAVESTATE_EXCLUDE; /* the frame's clear colour (see the lazy native clear) */
static bool sUnified SAVESTATE_EXCLUDE; /* internal res == native (see the frame section) */

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
    bool linear;        /* the game's filter for it: not G_TF_POINT (OOT_HUDTRI=2) */
} SoftTex;
static uint32_t sTexGen[MAX_TEXTURES] SAVESTATE_EXCLUDE; /* bumped whenever a texture's texels are (re)written: box row cache key */

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
/* unpack5551 as two table lookups, one per byte of the texel: every output
 * bit depends on one byte only (r5 and alpha sit in one byte each, b5 in the
 * other, and G's replication splits cleanly: with g5 = gl | gh<<3, the byte
 * gl<<3 | gl>>2 | gh<<6 | gh<<1 has no overlapping terms), so
 * unpack5551(c) == lut[c & 0xff] | lut[256 + (c >> 8)] for all 65536 c
 * (checked exhaustively). 5 ops instead of 11 on the raster's hottest path.
 * Filled once in gfx_soft_init. (MK64 perf round 24.) */
static uint32_t sUnpackLut[512] SAVESTATE_EXCLUDE;
static void unpack_lut_init(void) {
    for (int x = 0; x < 256; x++) {
        sUnpackLut[x] = unpack5551((uint16_t)x);
        sUnpackLut[256 + x] = unpack5551((uint16_t)(x << 8));
    }
}
/* TEX32: expand 16-bit textures to 8888 at upload so the pixel loops never
 * unpack (bit-identical: the same unpack5551, once per texel), for twice the
 * arena bytes. 1 = every 16-bit texture, 2 = only small ones (<= TEX32_SMALL
 * texels: the room and actor textures most pixels sample; backdrops stay
 * 16-bit). OOT_TEX32=0/1/2 (or debug.cfg tex32=) overrides PORT_TEX32. */
#ifndef PORT_TEX32
#define PORT_TEX32 2
#endif
/* 4096 again since round 59: round 53's 8192 was measured over frames 100..160 of the basement bench view,
 * which are still the Deku Tree's entrance cutscene -- in the water room itself (frames 230..290) 4096 and
 * 8192 cost the same (2,327.8M qemu insns / 60 frames), and the device bench showed no gain. OOT_TEX32SMALL
 * overrides. */
#define TEX32_SMALL 4096
int gfx_soft_tex32_mode(void) {
    static int mode SAVESTATE_EXCLUDE = -1;
    if (mode < 0) { const char *e = getenv("OOT_TEX32"); mode = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : PORT_TEX32; }
    return mode;
}
static bool tex32_expand(size_t texels) {
    const int m = gfx_soft_tex32_mode();
    static long small SAVESTATE_EXCLUDE = -1; /* OOT_TEX32SMALL=<texels>: mode 2's limit (A/B) */
    if (small < 0) { const char *e = getenv("OOT_TEX32SMALL"); small = e ? atol(e) : TEX32_SMALL; }
    return m == 1 || (m == 2 && (long)texels <= small);
}
/* One texel as ARGB8888 from either storage format. */
static inline uint32_t texel_at(const void *px, bool is16, int idx) {
    if (!is16) return ((const uint32_t *)px)[idx];
    const uint32_t c = ((const uint16_t *)px)[idx];
    return sUnpackLut[c & 0xff] | sUnpackLut[256 + (c >> 8)];
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

/* type: GU_PSM_8888 (RGBA bytes) or GU_PSM_5551 (u16). 5551 is stored 16-bit
 * as-is; 8888 is normalised to ARGB8888. */
static void gfx_soft_upload_texture(const uint8_t *buf, int width, int height, unsigned int type) {
    /* (OOT_SKYDEFER: an upload takes new arena memory, never a kept sky triangle's texels; texman_clear settles) */
    PROF_BEGIN();
    uint32_t id = sBoundTex[sTexUploadTile];
    if (id == 0 || id >= MAX_TEXTURES || width <= 0 || height <= 0) return;
    const size_t n = (size_t)width * height;
    const bool src16 = (type == GU_PSM_5551);
    const bool is16 = src16 && !tex32_expand(n);
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
    if (is16) {
        memcpy(dst, buf, n * 2); /* store 5551 verbatim, unpacked lazily at fetch */
    } else if (src16) { /* TEX32: expanded once here */
        uint32_t *d = (uint32_t *)dst; const uint16_t *s16 = (const uint16_t *)buf;
        for (size_t i = 0; i < n; i++) d[i] = unpack5551(s16[i]);
    } else { /* GU_PSM_8888: RGBA bytes -> ARGB8888 */
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = argb(buf[i*4+0], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    }
    sTex[id].px = dst; sTex[id].is16 = is16; sTex[id].w = width; sTex[id].h = height; sTex[id].valid = true;
    sTexGen[id]++; /* box row cache key */
    { /* OOT_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("OOT_TEXDUMP"); }
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
 * columns on the handheld: 4 texels to 3 pixels. Nearest drops every fourth
 * texel column (strokes vanish or double: jaggy, low-res text and digits), so a
 * shrunk sprite takes the area average of exactly the texels each pixel covers -
 * every column counts, edges antialias. Alpha is premultiplied so transparent
 * texels do not darken the average. OOT_HUDFILTER=0 restores nearest (A/B).
 * (MK64 hud-area-filter cd479d8, from f-zero-x ca8ef33.) */
int gfx_soft_hud_box SAVESTATE_EXCLUDE = -1;
/* HUD/menu triangles (UI-tagged, ortho): OOT_HUDTRI=0 never bilinear (the rule
 * today), 1 smoothed like the 3D, 2 bilinear unless the game set G_TF_POINT.
 * A developer switch for the user's A/B (cross-port lead item 9); the rule
 * changes only when the user picks. */
static int sHudTri SAVESTATE_EXCLUDE = -1;
int gHhSceneId SAVESTATE_EXCLUDE = -1; /* hh_main.c: the scene being drawn (-1 outside play) */
static int gfx_soft_fast_water(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("OOT_FASTWATER"); on = e && e[0] == '1'; } /* OFF by default since 2026-10-01 */
    return on;
}
int gfx_soft_water_grain(void) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("OOT_WATERGRAIN"); on = !(e && e[0] == '0'); }
    return on;
}
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

/* A 2D sprite row at a time (MK64 708fd2c): pixel i covers texels
 * [u0 + i*duf, u0 + (i+1)*duf), u0 = uf - duf/2 (uf is the first pixel's
 * centre). Walked once left to right, each texel is fetched and unpacked once
 * -- not once per pixel it touches -- and the wrap is stepped instead of
 * recomputed. Coverage is 8.8 per texel; every pixel equals texel_box's. */
#define BOX_MAX 512
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

/* Boxed rows are kept across frames (MK64 b72653e). A HUD or menu sprite row
 * is the same texels at the same scale and phase frame after frame, so after
 * its first frame a row costs one hash probe instead of the walk. The key is
 * everything box_row reads -- texture data pointer + its content generation,
 * row, start, step, width, wrap, formats. Uploads bump-allocate (a new
 * pointer); the TEXEL1 pre-blend rewrites a texture in place, so it bumps the
 * generation (sTexGen); texman_clear, the arena reset, empties the store.
 * When the store fills, rows not used this frame or the last are dropped
 * (then all but this frame's, then all). OOT_BOXCACHE=0 walks every row. */
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
/* box_row through the cache: the row's texels (into tmp only when the cache is off). */
static const uint32_t *box_row_cached(uint32_t *tmp, int n, uint32_t texid, const void *px, bool t16, int rowbase,
                                      int32_t uf, int32_t duf, int tw, int ws, int mu, bool premul) {
    if (sBoxState == 0) {
        const char *e = getenv("OOT_BOXCACHE");
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

/* n pixels of colour c */
static void fill32(uint32_t *d, uint32_t c, int n) {
    int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32x4_t v = vdupq_n_u32(c);
    for (; i + 8 <= n; i += 8) { vst1q_u32(d + i, v); vst1q_u32(d + i + 4, v); }
#endif
    for (; i < n; i++) d[i] = c;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define HH_2D_NEON 1
/* The 2D blends 8 pixels at a time (round 30: the 2D path was ~11% of the v28 device profile,
 * mostly its per-pixel blends). Every product is <= 255 * 255, where (x + 1 + (x >> 8)) >> 8
 * equals x / 255 exactly (checked over the whole range), so they match the scalar loops bit for
 * bit. Each returns the pixels it did (a multiple of 8); the scalar loop finishes the row.
 * OOT_NEON2D=0: the scalar loops (A/B). */
static int sNeon2D SAVESTATE_EXCLUDE = -1;
static inline uint8x8_t n2d_div255(uint16x8_t x) {
    return vshrn_n_u16(vaddq_u16(vaddq_u16(x, vdupq_n_u16(1)), vshrq_n_u16(x, 8)), 8);
}
/* the solid rectangle with alpha ca: d = div255(c * ca + d * (255 - ca)) per channel, opaque */
static int blend_solid_n2d(uint32_t *d, int n, int cr, int cg, int cb, int ca) {
    const uint8x8_t k = vdup_n_u8((uint8_t)(255 - ca));
    const uint16x8_t sr = vdupq_n_u16((uint16_t)(cr * ca)), sg = vdupq_n_u16((uint16_t)(cg * ca)),
                     sb = vdupq_n_u16((uint16_t)(cb * ca));
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        uint8x8x4_t p = vld4_u8((const uint8_t *)(d + i)); /* B G R A */
        p.val[0] = n2d_div255(vmlal_u8(sb, p.val[0], k));
        p.val[1] = n2d_div255(vmlal_u8(sg, p.val[1], k));
        p.val[2] = n2d_div255(vmlal_u8(sr, p.val[2], k));
        p.val[3] = vdup_n_u8(255);
        vst4_u8((uint8_t *)(d + i), p);
    }
    return i;
}
/* texels t over d by their alpha: kept below amin, the texel at 255 (the blend gives it
 * exactly), the blend between */
static int blend_texel_n2d(uint32_t *d, const uint32_t *t, int n, int amin) {
    const uint8x8_t vamin = vdup_n_u8((uint8_t)amin), v255 = vdup_n_u8(255);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const uint8x8x4_t s = vld4_u8((const uint8_t *)(t + i));
        uint8x8x4_t p = vld4_u8((const uint8_t *)(d + i));
        const uint8x8_t a = s.val[3], ia = vsub_u8(v255, a), keep = vclt_u8(a, vamin);
        const uint8x8_t o0 = n2d_div255(vmlal_u8(vmull_u8(s.val[0], a), p.val[0], ia));
        const uint8x8_t o1 = n2d_div255(vmlal_u8(vmull_u8(s.val[1], a), p.val[1], ia));
        const uint8x8_t o2 = n2d_div255(vmlal_u8(vmull_u8(s.val[2], a), p.val[2], ia));
        p.val[0] = vbsl_u8(keep, p.val[0], o0);
        p.val[1] = vbsl_u8(keep, p.val[1], o1);
        p.val[2] = vbsl_u8(keep, p.val[2], o2);
        p.val[3] = vbsl_u8(keep, p.val[3], v255);
        vst4_u8((uint8_t *)(d + i), p);
    }
    return i;
}
/* the general loop's texture x colour: f = div255(t * c) per channel, fa = ca (amode 0), ta (1)
 * or div255(ta * ca) (2), then blended as blend_texel_n2d when use_alpha, else opaque */
static int combine_mod_n2d(uint32_t *d, const uint32_t *t, int n, int cr, int cg, int cb, int ca, int amode,
                           bool use_alpha, int amin) {
    const uint8x8_t vr = vdup_n_u8((uint8_t)cr), vg = vdup_n_u8((uint8_t)cg), vb = vdup_n_u8((uint8_t)cb);
    const uint8x8_t vca = vdup_n_u8((uint8_t)ca), vamin = vdup_n_u8((uint8_t)amin), v255 = vdup_n_u8(255);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const uint8x8x4_t s = vld4_u8((const uint8_t *)(t + i));
        const uint8x8_t fb = n2d_div255(vmull_u8(s.val[0], vb)), fg = n2d_div255(vmull_u8(s.val[1], vg));
        const uint8x8_t fr = n2d_div255(vmull_u8(s.val[2], vr));
        uint8x8x4_t p;
        if (!use_alpha) {
            p.val[0] = fb; p.val[1] = fg; p.val[2] = fr; p.val[3] = v255;
        } else {
            const uint8x8_t fa = amode == 0 ? vca : amode == 1 ? s.val[3] : n2d_div255(vmull_u8(s.val[3], vca));
            const uint8x8_t ia = vsub_u8(v255, fa), keep = vclt_u8(fa, vamin);
            p = vld4_u8((const uint8_t *)(d + i));
            const uint8x8_t o0 = n2d_div255(vmlal_u8(vmull_u8(fb, fa), p.val[0], ia));
            const uint8x8_t o1 = n2d_div255(vmlal_u8(vmull_u8(fg, fa), p.val[1], ia));
            const uint8x8_t o2 = n2d_div255(vmlal_u8(vmull_u8(fr, fa), p.val[2], ia));
            p.val[0] = vbsl_u8(keep, p.val[0], o0);
            p.val[1] = vbsl_u8(keep, p.val[1], o1);
            p.val[2] = vbsl_u8(keep, p.val[2], o2);
            p.val[3] = vbsl_u8(keep, p.val[3], v255);
        }
        vst4_u8((uint8_t *)(d + i), p);
    }
    return i;
}
/* The general loop's (A - K) * TEXEL0 + K tint (OOT_2DLERP, 2026-10-01): f = k + ((c - k) * t) / 255 per channel, C's
 * division toward zero, as k + div255(sat(c - k) * t) - div255(sat(k - c) * t): one of the two saturating differences
 * is 0, so it is k + floor(|c - k| t / 255) above k and k - floor(|c - k| t / 255) below it (toward zero), between k
 * and c (no wrap in u8); the products are <= 255 * 255 (n2d_div255 exact). Alpha and the blend as combine_mod_n2d. */
static int combine_lerp_n2d(uint32_t *d, const uint32_t *t, int n, int cr, int cg, int cb, int ca, int amode,
                            bool use_alpha, int amin, int kr, int kg, int kb) {
    const uint8x8_t pr = vdup_n_u8((uint8_t)(cr > kr ? cr - kr : 0)), qr = vdup_n_u8((uint8_t)(kr > cr ? kr - cr : 0));
    const uint8x8_t pg = vdup_n_u8((uint8_t)(cg > kg ? cg - kg : 0)), qg = vdup_n_u8((uint8_t)(kg > cg ? kg - cg : 0));
    const uint8x8_t pb = vdup_n_u8((uint8_t)(cb > kb ? cb - kb : 0)), qb = vdup_n_u8((uint8_t)(kb > cb ? kb - cb : 0));
    const uint8x8_t vkr = vdup_n_u8((uint8_t)kr), vkg = vdup_n_u8((uint8_t)kg), vkb = vdup_n_u8((uint8_t)kb);
    const uint8x8_t vca = vdup_n_u8((uint8_t)ca), vamin = vdup_n_u8((uint8_t)amin), v255 = vdup_n_u8(255);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const uint8x8x4_t s = vld4_u8((const uint8_t *)(t + i));
        const uint8x8_t fb = vsub_u8(vadd_u8(vkb, n2d_div255(vmull_u8(s.val[0], pb))), n2d_div255(vmull_u8(s.val[0], qb)));
        const uint8x8_t fg = vsub_u8(vadd_u8(vkg, n2d_div255(vmull_u8(s.val[1], pg))), n2d_div255(vmull_u8(s.val[1], qg)));
        const uint8x8_t fr = vsub_u8(vadd_u8(vkr, n2d_div255(vmull_u8(s.val[2], pr))), n2d_div255(vmull_u8(s.val[2], qr)));
        uint8x8x4_t p;
        if (!use_alpha) {
            p.val[0] = fb; p.val[1] = fg; p.val[2] = fr; p.val[3] = v255;
        } else {
            const uint8x8_t fa = amode == 0 ? vca : amode == 1 ? s.val[3] : n2d_div255(vmull_u8(s.val[3], vca));
            const uint8x8_t ia = vsub_u8(v255, fa), keep = vclt_u8(fa, vamin);
            p = vld4_u8((const uint8_t *)(d + i));
            const uint8x8_t o0 = n2d_div255(vmlal_u8(vmull_u8(fb, fa), p.val[0], ia));
            const uint8x8_t o1 = n2d_div255(vmlal_u8(vmull_u8(fg, fa), p.val[1], ia));
            const uint8x8_t o2 = n2d_div255(vmlal_u8(vmull_u8(fr, fa), p.val[2], ia));
            p.val[0] = vbsl_u8(keep, p.val[0], o0);
            p.val[1] = vbsl_u8(keep, p.val[1], o1);
            p.val[2] = vbsl_u8(keep, p.val[2], o2);
            p.val[3] = vbsl_u8(keep, p.val[3], v255);
        }
        vst4_u8((uint8_t *)(d + i), p);
    }
    return i;
}
#endif
static int s2dLerp SAVESTATE_EXCLUDE = 1; /* combine_lerp_n2d (2026-10-01); OOT_2DLERP=0: the tint in the scalar loop */

/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}
/* float -> int32 as the device's VFP converts (vcvt saturates, NaN gives 0), so the x86 proxy
 * agrees with it where a plain cast is undefined: a near-degenerate triangle's colour step can
 * reach ~1e10 in 16.16, and x86's cvttss2si gives INT_MIN there (make PLAT=pc UBSAN=1 finds it).
 * (MK64 b2dff40, the lead's R31.) */
static inline int32_t sat_i32(float f) {
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
}

/* OOT_NEARW (default on; 0 = the W_MIN 2 clip and the span depth before it, 2026-10-04; goldeneye-007 c20b9ee):
 * with NoN (OoT's F3DZEX2 NoN draws everything in front of the eye, w > 0) gfx_pc.c's clipToNearW now keeps
 * everything from w = 1/64 on, not from w = 2. Faces that near reach raster_tri with depths far below 0 -- z/w * 0.5
 * + 0.5 is about 1 - 10 / w with the near plane at 10: -639 at w = 1/64 -- and the span setup lost them: each
 * 16-pixel span started from its depth clamped to [-8, 2] (round 49's NoN) and stepped by a slope clamped to +-1 a
 * pixel, so a span starting at z = -40 and rising got the nearest depth (0) many pixels too soon, in front of things
 * it lies behind. Now a span that starts below -1 and rises (or above 2 and falls) starts instead at the first pixel
 * inside [-1, 2], from that pixel's exact depth, and steps back to the start; the slope clamp is +-6. Every pixel then
 * stores what its exact depth clamps to (the RDP's 0..max), give or take 1: below -1 and above 2 both directions
 * agree, and past a step of 6 the depth crosses the whole range in one pixel either way. The values stay within 98
 * units (16 steps of 6 from 2), inside 24.8's int32 range. A span starting inside [-1, 2] is set up exactly as
 * before. raster_test nearz checks it against the exact depth. */
static int sNearW SAVESTATE_EXCLUDE = -1;
int gfx_soft_nearw_on(void) {
    if (sNearW < 0) { const char *e = getenv("OOT_NEARW"); sNearW = !(e && e[0] == '0'); }
    return sNearW;
}
#define ZQ_SCALE (65535.0f * 256.0f)
/* the depth slope as 24.8 a pixel: clamped to +-1 (old) or +-6 (OOT_NEARW) */
static inline int32_t span_zdq(float zdx) {
    const float m = gfx_soft_nearw_on() ? 6.0f : 1.0f;
    return (int32_t)((zdx < -m ? -m : zdx > m ? m : zdx) * ZQ_SCALE);
}
/* A span's first depth, zf outside [-1, 2] with OOT_NEARW on (above). span: its pixels (<= 16). */
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
/* true: span_zq_far sets this span up (zf outside [-1, 2], not NaN, OOT_NEARW on) */
static inline bool span_z_out(float zf) {
    return (zf < -1.0f || zf > 2.0f) && gfx_soft_nearw_on();
}
/* OOT_PSHADE's span setup without the sat_i32 chains (2026-10-01): 1/16 as the exact constant on full spans
 * (1.0f / 16.0f is 0.0625 exactly, as the divide gives it) and, on ARM, the eight conversions as two NEON vcvt (round
 * toward zero, NaN -> 0, saturating: sat_i32's definition, which costs three VFP compares and FPSCR transfers each on
 * the A7). cm_row_lean's since bfe7b5d; cm_row_neon's and the scalar loops' with this. OOT_PSVCVT=0: the old setup. */
static int sPsVcvt SAVESTATE_EXCLUDE = 1;

/* 3D bilinear filtering: SMOOTH, the only player setting (user, 2026-09-24: no
 * 3D options in any game; bilinear=0, SHARP, stays a developer A/B switch).
 * uf/vf are 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* 0 = every textured pixel bilinear (default since v26); 1 = bilinear only when magnified (the old
 * MK64 round 13 gate, debug.cfg magbilin=1). The per-triangle smooth/sharp switch is visible: floors
 * flash as the camera moves (the user on DKR's device; the lead's P17, kit k1.2). In Navi's flight
 * the ground's frame-to-frame edge-energy jump drops 31% (worst 4.0 -> 1.4); qemu Hyrule Field with
 * the grain +4.6% instructions. */
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

/* -------------------------------------------------------------- detail --- */
/* Tile 1 sampled per pixel beside tile 0 (gfx_pc.c, gfx_detail_tiles): a tile 1 that
 * repeats at another scale than tile 0 can't live in a pre-blend over tile 0's texels.
 * Hyrule Field's grass grain is 16x finer than the ground under it and came out as one
 * flat colour. Tile 1's 16.16 coordinate is tile 0's scaled by 2^e per axis (the tiles'
 * relative shift) plus an offset, taken modulo its power-of-two period:
 *   u1 = ((((uint32_t)(uf >> srs) << sls) + offs) & ((w1 << 16) - 1))
 * (e > 0: sls = e, and the scaled value is exact mod 2^32, so mod the period too;
 * e < 0: srs = -e). The two texels mix per channel as the RDP's combiner rounds a lerp:
 *   (t0 * (256 - w) + t1 * w + 128) >> 8, w = tile 1's weight, 0..256
 * (a constant K is w = K; t1 + (t0 - t1) * K is w = 256 - K; tile 1 alone is w = 256). */
typedef struct {
    uint32_t id;             /* tile 1's texture, 0 = off */
    int sls, srs, slt, srt;  /* the scale, as shifts per axis */
    uint32_t offs, offt;     /* 16.16 offsets in tile-1 texels, inside the period */
    uint16_t w[4];           /* tile 1's weight per channel: B G R A */
    float area;              /* 2^(es + et): tile-1 texels per tile-0 texel, by area */
} SoftDetail;
static SoftDetail sDetail SAVESTATE_EXCLUDE;
static inline uint32_t detail_mix(uint32_t t0, uint32_t t1, const uint16_t w[4]) {
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t a = (t0 >> (i * 8)) & 0xff, b = (t1 >> (i * 8)) & 0xff;
        out |= ((a * (256u - w[i]) + b * w[i] + 128u) >> 8) << (i * 8);
    }
    return out;
}
/* A texture's imported size (gfx_pc.c's detail rebase step). */
bool gfx_soft_tex_size(uint32_t id, int *w, int *h) {
    if (id == 0 || id >= MAX_TEXTURES || !sTex[id].valid) return false;
    *w = sTex[id].w;
    *h = sTex[id].h;
    return true;
}
/* id1 = 0 turns it off. False (and off) if tile 1's texture can't be sampled this way. */
bool gfx_soft_set_detail(uint32_t id1, int es, int et, int32_t offs, int32_t offt, const uint16_t w[4]) {
    sDetail.id = 0;
    if (id1 == 0) return true;
    if (id1 >= MAX_TEXTURES || es < -15 || es > 15 || et < -15 || et > 15) return false;
    const SoftTex *t = &sTex[id1];
    if (!t->valid || pow2mask(t->w) < 0 || pow2mask(t->h) < 0 || t->w > 32768 || t->h > 32768) return false;
    sDetail.sls = es > 0 ? es : 0;
    sDetail.srs = es < 0 ? -es : 0;
    sDetail.slt = et > 0 ? et : 0;
    sDetail.srt = et < 0 ? -et : 0;
    sDetail.offs = (uint32_t)offs & (((uint32_t)t->w << 16) - 1u);
    sDetail.offt = (uint32_t)offt & (((uint32_t)t->h << 16) - 1u);
    for (int i = 0; i < 4; i++) sDetail.w[i] = w[i] > 256 ? 256 : w[i];
    const int e = es + et;
    sDetail.area = e >= 0 ? (float)(1u << e) : 1.0f / (float)(1u << -e);
    sDetail.id = id1;
    return true;
}

/* -------------------------------------------------------------- shaders --- */
/* Per-channel texel use, derived from the combiner (device round 6b): the
 * cloud sprites are I4 textures used ONLY for alpha -- RGB = SHADE (white).
 * Multiplying the texel's grey intensity into RGB painted the soft edge grey. */
struct ShaderProgram { uint32_t shader_id; struct CCFeatures cc; bool rgb_tex, rgb_in, a_tex, a_in, rgb_texa; };
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
    p->rgb_tex = p->rgb_in = p->a_tex = p->a_in = p->rgb_texa = false;
    for (int i = 0; i < 4; i++) {
        const int c0 = p->cc.c[0][i], c1 = p->cc.c[1][i];
        if (c0 == SHADER_TEXEL0 || c0 == SHADER_TEXEL1) p->rgb_tex = true;
        if (c0 == SHADER_TEXEL0A) { p->rgb_tex = true; p->rgb_texa = true; }
        if (c0 >= SHADER_INPUT_1 && c0 <= SHADER_INPUT_4) p->rgb_in = true;
        if (c1 == SHADER_TEXEL0 || c1 == SHADER_TEXEL0A || c1 == SHADER_TEXEL1) p->a_tex = true;
        if (c1 >= SHADER_INPUT_1 && c1 <= SHADER_INPUT_4) p->a_in = true;
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
static bool sTexLerpV;     /* with sTexLerp, OOT_SHADELERP: out = vertex + (K - vertex) * texel (gfx_scegu_set_tex_lerpv) */
static int sLerpR, sLerpG, sLerpB;
/* Per-pixel fog (gfx_pc gfx_fog_plan, MK64 7d23507's reference): the vertex alpha is the fog
 * factor; the pixel goes to lerp(colour, sFogR/G/B, factor) after the combine, as the RDP's
 * cycle-1 fog blend does. sFogA >= 0: the combiner's constant alpha, which the factor displaced. */
static bool sFogPx;
static bool sFogZ SAVESTATE_EXCLUDE; static float sFogZA SAVESTATE_EXCLUDE, sFogZB SAVESTATE_EXCLUDE; /* round 74 */
static int sFogR, sFogG, sFogB, sFogA = -1;
static bool sForceIn;      /* 2-cycle: the vertex colour carries cycle 2, always modulate */
static bool sUiMode;       /* HUD/menu triangles (game UI tags): not bilinear (F-Zero X's rule -- MK64 smooths them since the user's A/B; OoT's A/B pending); area-filtered when squeezed */

static void gfx_soft_set_depth_test(bool d) { sDepthTest = d; }
static void gfx_soft_set_depth_mask(bool m) { sDepthMask = m; }
static void gfx_soft_set_zmode_decal(bool d) { sZDecal = d; }
static void gfx_soft_set_use_alpha(bool a) { sUseAlpha = a; }
extern int gfx_trace_frames;
static bool sSkyPass; /* half-resolution sky (gfx_scegu_set_sky_pass) */
static int sSkyFull[8];  /* the full-resolution viewport/scissor the pass stands in for */
static void gfx_soft_set_viewport(int x, int y, int w, int h) {
    if (sSkyPass) { sSkyFull[0] = x; sSkyFull[1] = y; sSkyFull[2] = w; sSkyFull[3] = h; x /= 2; y /= 2; w = (w + 1) / 2; h = (h + 1) / 2; }
    sVpX=x; sVpY=y; sVpW=w; sVpH=h;
    if (gfx_trace_frames > 0) port_log("  soft: viewport %d,%d %dx%d (target %dx%d %s)\n", x, y, w, h, sW, sH, sFB == sNatFB ? "native" : "internal");
}
static void gfx_soft_set_scissor(int x, int y, int w, int h) {
    if (sSkyPass) { sSkyFull[4] = x; sSkyFull[5] = y; sSkyFull[6] = w; sSkyFull[7] = h; x /= 2; y /= 2; w = (w + 1) / 2; h = (h + 1) / 2; }
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
/* OOT_FILLRULE (2026-10-03, the Deku Tree's vision: a one-pixel bright diagonal across Ganondorf's flames): the row
 * solve below is biased toward inclusion, and a pixel centre exactly on an edge counts as inside, so a pixel on the
 * edge two triangles share is drawn by both. Opaque, that is invisible; blended, the pixel is blended twice. The
 * flames are full-screen quads (gEffFire1DL, G_RM_PASS + G_RM_ZB_CLD_SURF2: FORCE_BL, no AA_EN) whose guard-band
 * clip fan has a diagonal from corner (-120,-120) to (360,360): every pixel centre of y = x lies on it. The RDP
 * without AA_EN writes a pixel only when one fixed sample of it is covered (angrylion blender.c: cvbit, the mask's
 * bit 7), and the edges split the samples exactly, so exactly one triangle draws each pixel. Here, for such draws
 * (gfx_pc: blended or alpha-tested, no AA_EN), each row's ends are decided per pixel by edge functions taken in a
 * canonical vertex order (the lower y, then the lower x, first), so the two triangles of a shared edge compute the
 * same float there and only its sign differs; a centre exactly on the edge goes to the triangle the edge is a left
 * (or top) edge of. Other draws keep the inclusive solve, bit-identical. */
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
/* One row of raster_tri: the row's edge bases, its pixel interval [xs, xe) (continue when empty), the span pixels
 * counted, and the interpolants at its first pixel (zf, cr..ca, uw, vw, sw, rw) and its index idx. raster_tri's loop
 * and the deferred sky's flush (OOT_SKYDEFER) run this same text over the same values. */
/* (In the edge solve both bounds are biased toward inclusion: a 1-px overlap between neighbouring triangles is
 * invisible, a 1-px crack is not, and the snap loop restores exactness on the left edge. A near-horizontal edge
 * (|ds| ~ 1 ulp) on a large off-screen polygon makes the quotient ~1e11: the int conversion saturated to INT_MAX and
 * `minx + 1 + INT_MAX` wrapped negative, so the whole triangle vanished for a frame (round 13); the float is clamped
 * first, exact for every quotient that fits. The snap loop: the first pixel really inside per the linear edge
 * functions, against rounding in the divisions. rw: 1/w at the row's start; each span's end reciprocal is the next
 * span's start, carried, so one divide a span.) */
#define TRI_ROW_SETUP                                                                                   \
        const float py = y + 0.5f;                                                                      \
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;                                           \
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;                                           \
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;                                           \
        int xs = minx, xe = maxx;                                                                       \
        {                                                                                               \
            const float bs[3] = { b0, b1, b2 }, ds[3] = { w0dx, w1dx, w2dx };                           \
            for (int i = 0; i < 3; i++) {                                                               \
                if (ds[i] > 0.0f) {                                                                     \
                    if (bs[i] < 0.0f) {                                                                 \
                        float q = -bs[i] / ds[i] - 1e-5f; if (q > 1e6f) q = 1e6f;                       \
                        int a = minx + iceilf(q);                                                       \
                        if (a > xs) xs = a;                                                             \
                    }                                                                                   \
                } else if (ds[i] < 0.0f) {                                                              \
                    float q = -bs[i] / ds[i] + 1e-5f; if (q > 1e6f) q = 1e6f;                           \
                    int a = minx + 1 + ifloorf(q);                                                      \
                    if (a < xe) xe = a;                                                                 \
                } else if (bs[i] < 0.0f) {                                                              \
                    xs = xe; /* row entirely outside this edge */                                       \
                    break;                                                                              \
                }                                                                                       \
            }                                                                                           \
        }                                                                                               \
        if (xs >= xe) continue;                                                                         \
        if (own) { /* OOT_FILLRULE: the row's ends by the owned-edge test, one pixel either side */     \
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;                             \
            while (xs < xe && !own_px(&oe, xs, py)) xs++;                                               \
            while (xe > xs && !own_px(&oe, xe - 1, py)) xe--;                                           \
        } else                                                                                          \
        while (xs < xe) {                                                                               \
            const float o = (float)(xs - minx);                                                         \
            if (b0 + o * w0dx >= 0 && b1 + o * w1dx >= 0 && b2 + o * w2dx >= 0) break;                  \
            xs++;                                                                                       \
        }                                                                                               \
        if (xs >= xe) continue;                                                                         \
        tri_px += (uint32_t)(xe - xs); /* fill/overdraw proxy (before z-reject) */                      \
        const float o = (float)(xs - minx);                                                             \
        float w0 = b0 + o * w0dx;                                                                       \
        float w1 = b1 + o * w1dx;                                                                       \
        float w2 = b2 + o * w2dx;                                                                       \
        float zf = w0*z0 + w1*z1 + w2*z2;                                                               \
        float cr, cg, cb, ca;                                                                           \
        if (flat) { cr = r0; cg = g0; cb = bl0; ca = a0; } /* constant: no barycentric build */         \
        else { cr = w0*r0 + w1*r1 + w2*r2; cg = w0*g0 + w1*g1 + w2*g2; cb = w0*bl0 + w1*bl1 + w2*bl2;   \
               ca = w0*a0 + w1*a1 + w2*a2; }                                                            \
        float uw = w0*uw0 + w1*uw1 + w2*uw2;                                                            \
        float vw = w0*vw0 + w1*vw1 + w2*vw2;                                                            \
        float sw = w0*iw0 + w1*iw1 + w2*iw2;                                                            \
        int idx = y * sW + xs;                                                                          \
        float rw = 0.0f;                                                                                \
        if (persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
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
 * call; flat triangles come in with zero colour steps. OOT_CMNEON=0 keeps the
 * scalar loops. */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16; /* texture */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx;  /* triangle x-steps */
    float ftw, fth;                                       /* (float)tw, (float)th */
    int32x4_t dacc;                                       /* B G R A 16.16 x-steps */
    bool pshade;                                          /* colours are c/w: rebuilt per span (OOT_PSHADE) */
    int tinta; uint16x4_t tavg; bool tglass;              /* fastwater tint: strength 0..255; the texture's mean, or the glass filter (B G R A) */
    int32_t zdq; int zbias, amode, amin, fgac, ablend; uint32_t clr; /* amode 3: the fogged constant alpha fgac (the scalar fga); ablend 0: TEX_EDGE, written unblended */
    uint32_t cmask; bool enote;                           /* OOT_EDGETINT: the colour test's mask (0: any pixel without depth), edge_note */
    bool persp, dmask, alpha, bilin;
    bool texonly; /* MODE 1: texture only, no z, opaque (the sky) */
    int extra;    /* round 69: blends after the first on the one shaded source -- 1: the same triangle drawn twice in a
                   * row (see raster_tri); 3: fastwater's top layer standing in for both water layers (round 73) */
    /* The detail layer (sDetail): z-tested texture x shade, tile 0 repeating on a power of two */
    const void *dpx; int dw, dmu, dmv, dsls, dsrs, dslt, dsrt;
    uint32_t dpmu, dpmv, doffs, dofft;
    uint16x4_t dwt, dwinv;   /* tile 1's weight per lane, and 256 - it */
    bool detail, dbilin;
    /* Per-pixel fog (fogtri, no constant alpha): after the modulate, lerp to the fog colour
     * (B G R lanes) by the shade alpha, the fog factor -- SPAN_LOOP_CM's CM=3 */
    bool fog; uint16x4_t fogv;
    bool lean;    /* the rows go to cm_row_lean (OOT_WATERLEAN / OOT_OPAQUELEAN; raster_tri checks the conditions) */
    uint8_t lean2; /* 1: cm_row_leana (OOT_ALPHALEAN), 2: cm_row_leans (OOT_SKYLEAN), 3: cm_row_leanl (OOT_LERPLEAN);
                    * raster_tri checks the conditions */
    uint16x4_t lk; /* cm_row_leanl: the (A - K) * TEXEL0 + K tint's K (B G R lanes, 0 in A) */
    int zskip;     /* OOT_ZSKIP (CM_ROW): 4 while the triangle's spans are tested, counting down on misses; 0: off */
} CmTri;
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* The pixel loop of one span: Tx (the texel, B G R A in u16 lanes) from the fetch (...).
 * MODE 0: z-tested texture x shade (SPAN_LOOP_CM); MODE 1: texture only (the
 * combiner's rgb input off), no z test or write, opaque -- SPAN_LOOP(1, 0, 0, *)
 * with rgb_in clear: the sky. */
#define CM_PIXELS(MODE, ALPHA, DETAIL, FOG, ...)                                                         \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        int uz = 0;                                                                                     \
        if (MODE == 0) { uz = clamp16_sh8(zq); if (uz > *pz + zbias) continue; }                        \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_DETAIL_##DETAIL                                                                              \
        uint16x4_t M = Tx;                                                                              \
        if (MODE == 0) {                                                                                \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        M = cm_div255(vmul_u16(Tx, S));                                                                 \
        if (FOG) { /* div255(c*(255-f) + fog*f), the sum <= 255*255; lane 3 keeps the modulated alpha */      \
            const uint16_t fs = vget_lane_u16(S, 3);                                                    \
            M = vset_lane_u16(vget_lane_u16(M, 3),                                                      \
                              cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)(255 - fs)), fogv, fs)), 3); \
        }                                                                                               \
        if (ALPHA) { /* fa: modulated (0), texel (1) or shade (2) alpha -- the scalar fa */               \
            const int fa = (FOG && amode == 3) ? fgac                                                   \
                         : (FOG && amode == 4) ? div255(vget_lane_u16(Tx, 3) * fgac) : amode == 0 ? vget_lane_u16(M, 3) \
                         : amode == 1 ? vget_lane_u16(Tx, 3) : vget_lane_u16(S, 3);                     \
            if (fa < amin) continue;                                                                    \
            const uint16x4_t Ms = M; /* the source, for twice */                                        \
            if (fa < 255 && (ablend || *pz != 0xFFFF || (*pf & cmask) != clr)) { /* div255(f*fa + d*(255-fa)) */ \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));         \
            } else if (enote && fa < 255) edge_note(pf, fa, dmask ? uz : 0xFFFF);                         \
            for (int kx = 0; kx < extra; kx++) { /* the same source over the last draw's pixel, same steps */ \
                const uint32_t w1 = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vorr_u16(M, a255), M))), 0); \
                M = Ms;                                                                                 \
                if (fa < 255 && (ablend || *pz != 0xFFFF || (w1 & 0xFFFFFFu) != clr)) {                  \
                    const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(w1))));    \
                    M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));     \
                }                                                                                       \
            }                                                                                           \
            if (tinta) { /* fastwater tint */                                                          \
                uint16x4_t Mt;                                                                          \
                if (tglass) Mt = cm_div255(vmul_u16(M, tavg)); /* glass: the pixel through a teal filter */ \
                else {       /* solid: toward the texture's mean, shaded and fogged like the pixel */     \
                    Mt = cm_div255(vmul_u16(tavg, S));                                                  \
                    if (FOG) {                                                                          \
                        const uint16_t fs = vget_lane_u16(S, 3);                                        \
                        Mt = cm_div255(vmla_n_u16(vmul_n_u16(Mt, (uint16_t)(255 - fs)), fogv, fs));     \
                    }                                                                                   \
                }                                                                                       \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)(255 - tinta)), Mt, (uint16_t)tinta)); \
            }                                                                                           \
        }                                                                                               \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (MODE == 0 && dmask) *pz = (uint16_t)uz;                                                     \
    }
/* The two lerps of a bilinear texel, one multiply each (SM64 68a56a9): floor((x*(256-f) + y*f) / 256)
 * as floor((x*256 + (y-x)*f) / 256). The sum is at most 255*256, so the wrapping 16-bit arithmetic
 * gives it exactly. X8 = {a, c}, Y8 = {b, d} (the taps' bytes), wts lane 1 = fu, lane 3 = fv. */
#define CM_LERP2(X8, Y8)                                                                                \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vshll_n_u8(X8, 8), vsubl_u8(Y8, X8), wts, 1), 8); \
        Tx = vshr_n_u16(vmla_lane_u16(vshl_n_u16(vget_low_u16(H), 8),                                   \
                                      vsub_u16(vget_high_u16(H), vget_low_u16(H)), wts, 3), 8);
/* bilerp: four taps around (u - 0.5, v - 0.5), lerped along u then v */
#define CM_BILIN(TAP, WU, WV)                                                                           \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = WU(su >> 16), iu1 = WU((su >> 16) + 1);                                         \
        const int b0 = WV(sv >> 16) * tw, b1 = WV((sv >> 16) + 1) * tw;                                 \
        const uint32_t a = TAP(b0 + iu0), b = TAP(b0 + iu1), c = TAP(b1 + iu0), d = TAP(b1 + iu1);      \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u64(vcreate_u64(((uint64_t)fv << 48) | ((uint64_t)fu << 16))); \
        const uint8x8_t X8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a));                 \
        const uint8x8_t Y8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b));                 \
        CM_LERP2(X8, Y8)
/* CM_BILIN for a 32-bit texture repeating on a power of two (mask MU, texels PX): a tap's
 * horizontal neighbour is the next word unless the column wraps (iu0 == MU), so each row's two
 * taps come in one 64-bit load, and vtrn puts a,c and b,d in the lanes CM_BILIN's vcreates did --
 * the same values, bit for bit (SM64 6c307c2, the lead's round-3 list). OOT_BIL64=0: CM_BILIN. */
#define CM_BILIN_RR32(PX, MU, WV)                                                                       \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = (su >> 16) & (MU);                                                              \
        const int b0 = WV(sv >> 16) * tw, b1 = WV((sv >> 16) + 1) * tw;                                 \
        uint32x2_t AB, CD;                                                                              \
        if (iu0 < (MU)) { AB = vld1_u32((PX) + b0 + iu0); CD = vld1_u32((PX) + b1 + iu0); }            \
        else { AB = vcreate_u32(((uint64_t)(PX)[b0] << 32) | (PX)[b0 + iu0]);                           \
               CD = vcreate_u32(((uint64_t)(PX)[b1] << 32) | (PX)[b1 + iu0]); }                         \
        const uint32x2x2_t TR = vtrn_u32(AB, CD);                                                       \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u64(vcreate_u64(((uint64_t)fv << 48) | ((uint64_t)fu << 16))); \
        const uint8x8_t X8 = vreinterpret_u8_u32(TR.val[0]), Y8 = vreinterpret_u8_u32(TR.val[1]);       \
        CM_LERP2(X8, Y8)
/* FETCH_NEAREST */
#define CM_NEAR(TAP, WU, WV)                                                                            \
        const uint32_t t1 = TAP(WV(vf >> 16) * tw + WU(uf >> 16));                                      \
        Tx = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(t1))));
/* The detail layer's tile 1 (sDetail): its coordinate from tile 0's, then CM_BILIN or CM_NEAR
 * on it in an inner scope (uf, vf, tw and Tx shadowed), then the lerp, as detail_mix:
 *   (t0 * (256 - w) + t1 * w + 128) >> 8   in u16 (the two products sum to <= 255*256) */
#define CM_DETAIL_0
#define CM_DETAIL_1                                                                                     \
        {                                                                                               \
            const int32_t du1 = (int32_t)((((uint32_t)(uf >> dsrs) << dsls) + doffs) & dpmu);           \
            const int32_t dv1 = (int32_t)((((uint32_t)(vf >> dsrt) << dslt) + dofft) & dpmv);           \
            uint16x4_t T1x;                                                                             \
            {                                                                                           \
                const int32_t uf = du1, vf = dv1; const int tw = dw1; uint16x4_t Tx;                    \
                if (dbilin && bil64) { CM_BILIN_RR32(dpx32, dmu, CM_DWRAPV) T1x = Tx; }                  \
                else if (dbilin) { CM_BILIN(CM_DTAP, CM_DWRAPU, CM_DWRAPV) T1x = Tx; }                  \
                else        { CM_NEAR(CM_DTAP, CM_DWRAPU, CM_DWRAPV) T1x = Tx; }                        \
            }                                                                                           \
            Tx = vrshr_n_u16(vmla_u16(vmul_u16(Tx, dwinv), T1x, dwt), 8);                               \
        }
#define CM_DTAP(i) dpx32[i]
#define CM_DWRAPU(x) ((x) & dmu)
#define CM_DWRAPV(x) ((x) & dmv)
#define CM_TAP32(i) px32[i]
#define CM_TAP16(i) texel_at(px, true, (i))
#define CM_WRAP_RRU(x) ((x) & mu)
#define CM_WRAP_RRV(x) ((x) & mv)
#define CM_WRAP_GU(x) wrapf((x), tw, ws, mu)
#define CM_WRAP_GV(x) wrapf((x), th, wt, mv)
/* OOT_ZSKIP (2026-10-07, from SM64's SM64_ZSKIP; default 1, OOT_ZSKIP=0: off): in a row with a depth test (MODE 0 and
 * T->zskip, which raster_tri leaves 0 for the rows without one), a span whose nearest depth is further than the
 * farthest depth stored under it plus the decal window fails the depth test at every pixel: each pixel's test,
 * uz > *pz + zbias, is the first thing its loop does (every CM_PIXELS* with a z test), and uz = clamp16_sh8(zq + i * zdq)
 * is monotonic in i, so its least value is at one end of the span. Such a span is stepped over whole -- its float
 * state stepped as drawn (the same sums), no texel or shade setup, no pixel loop (each hidden pixel cost its depth
 * load and compare and six steps). A sufficient test, never a guess: every other span is drawn as before. T->zbias is
 * the largest bias any row applies (cm_row_lean's water rows test with 0, and raster_tri takes them only with zbias
 * 0). A triangle stops testing after four misses running (speed only; any hit restarts the count). 4-pixel spans and
 * longer (the max over [zp, zp + span) by two overlapping loads). CM_ROW_SKIPPED(s): what a stepped-over span steps
 * besides fb and zp (OOT_SKYDEFER's marking rows: their mask pointer). Same pixels. */
#define CM_ROW_SKIPPED(s)
#define CM_ROW_ZSK 1 /* 0: the half-resolution layer's rows (gfx_soft_halfwater.inc.c: their own depth buffer) */
#define CM_ZSKIP_HIDDEN(zq, span)                                                                       \
        ({ const int uz0_ = clamp16_sh8(zq), uz1_ = clamp16_sh8((zq) + ((span) - 1) * zdq);            \
           uint16x4_t m4_;                                                                              \
           if ((span) >= 8) { const uint16x8_t m8_ = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + (span) - 8)); \
                              m4_ = vmax_u16(vget_low_u16(m8_), vget_high_u16(m8_)); }                  \
           else m4_ = vmax_u16(vld1_u16(zp), vld1_u16(zp + (span) - 4));                                \
           m4_ = vpmax_u16(m4_, m4_); m4_ = vpmax_u16(m4_, m4_);                                        \
           (uz0_ < uz1_ ? uz0_ : uz1_) > (int)vget_lane_u16(m4_, 0) + T->zbias; })
/* One row: raster_tri's span loop for this path, float for float (fetch = ...). The span's depth comes first, for
 * OOT_ZSKIP's test (a pure value: the order changes nothing). */
#define CM_ROW(MODE, ALPHA, DETAIL, FOG, ...)                                                            \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float sw_b = sw + swdx * fspan;                                                           \
        const float32x2_t zc = vmin_f32(vmax_f32(vdup_n_f32(zf), vdup_n_f32(-8.0f)), vdup_n_f32(2.0f));   \
        int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);                  \
        if (span_z_out(zf)) zq = span_zq_far(zf, zdx, span, zdq); /* OOT_NEARW (span_zq_far) */          \
        if (MODE == 0 && CM_ROW_ZSK && T->zskip > 0 && span >= 4) { /* OOT_ZSKIP */                     \
            if (CM_ZSKIP_HIDDEN(zq, span)) {                                                            \
                ((CmTri *)T)->zskip = 4;                                                                \
                zf += zdx * fspan;                                                                      \
                cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;         \
                uw += uwdx * fspan;                                                                     \
                vw += vwdx * fspan;                                                                     \
                sw = sw_b; rw = persp ? ((sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f) : 0.0f;                  \
                CM_ROW_SKIPPED(span)                                                                    \
                fb += span; zp += span; n -= span;                                                      \
                continue;                                                                               \
            }                                                                                           \
            ((CmTri *)T)->zskip--;                                                                      \
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
        int32x4_t dacc = dacc0;                                                                         \
        if (pshade) { /* colour/w at both span ends (rw, rw_b), linear between (OOT_PSHADE) */          \
            CM_ROW_PSHADE                                                                               \
        }                                                                                               \
        uint32_t *const end = fb + span;                                                                \
        uint16_t *pz = zp;                                                                              \
        CM_ROW_PIXELS(MODE, ALPHA, DETAIL, FOG, __VA_ARGS__)                                            \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }
static int sCmNeon SAVESTATE_EXCLUDE = 1; /* OOT_CMNEON=0 (debug.cfg cmneon=0): the scalar loops, for A/B */
static int sZSkip SAVESTATE_EXCLUDE = 1;    /* CM_ROW's hidden-span skip (2026-10-07); OOT_ZSKIP=0: off */
static int sFastRows SAVESTATE_EXCLUDE = 1; /* tri_rows (2026-10-07); OOT_FASTROWS=0: raster_tri's own row loop */
static int sCmNeonAlt SAVESTATE_EXCLUDE;  /* cmneon=alt: on and off by PERF window (gfx_soft_cmneon_alt_step) */
static int sBil64 SAVESTATE_EXCLUDE = 1;  /* OOT_BIL64=0 (debug.cfg bil64=0): the four-load taps; bil64=alt flips per window */
static int sBil64Alt SAVESTATE_EXCLUDE;
static int sCmSpec SAVESTATE_EXCLUDE = 1; /* OOT_CMSPEC=0: no constant-folded water rows (round 61) */
static int sWaterLean SAVESTATE_EXCLUDE = 1;  /* the lean water rows (cm_row_lean, 2026-10-01); OOT_WATERLEAN=0: off */
static int sOpaqueLean SAVESTATE_EXCLUDE = 1; /* the lean opaque rows (cm_row_lean, 2026-10-01); OOT_OPAQUELEAN=0: off */
#define CM_ROW_PIXELS CM_PIXELS /* the pixel loop CM_ROW runs (cm_row_lean swaps in its own) */
/* The OOT_PSHADE span setup with the eight sat_i32 conversions as two NEON vcvt (round toward zero, NaN -> 0,
 * saturating: sat_i32's definition, which on the A7's VFP cost three compares and FPSCR transfers each, 24 a span),
 * the same float values in the same order (VFP, scalar), and 1/16 as the exact constant on full spans (1.0f / 16.0f
 * is 0.0625 exactly, as the divide gives it). cm_row_lean's since bfe7b5d; cm_row_neon's too since 2026-10-01
 * (OOT_PSVCVT=0: its old setup, the sat_i32 chain). water_test ops: vcvt = sat_i32 on every class of float. */
#define CM_PSHADE_VCVT                                                                                  \
            const float is = span == PERSP_SPAN ? 1.0f / PERSP_SPAN : 1.0f / fspan;                      \
            const float ar = cr * rw, ag = cg * rw, ab = cb * rw, aa = ca * rw;                         \
            const float32x4_t a4 = { ab * 65536.0f, ag * 65536.0f, ar * 65536.0f, aa * 65536.0f };      \
            const float32x4_t d4 = { ((cb + cbdx * fspan) * rw_b - ab) * is * 65536.0f,                 \
                                     ((cg + cgdx * fspan) * rw_b - ag) * is * 65536.0f,                 \
                                     ((cr + crdx * fspan) * rw_b - ar) * is * 65536.0f,                 \
                                     ((ca + cadx * fspan) * rw_b - aa) * is * 65536.0f };               \
            acc = vcvtq_s32_f32(a4);                                                                    \
            dacc = vcvtq_s32_f32(d4);
/* CM_ROW's OOT_PSHADE span setup (cm_row_lean swaps in its own) */
#define CM_ROW_PSHADE                                                                                   \
            if (sPsVcvt) { CM_PSHADE_VCVT } else {                                                      \
            const float is = 1.0f / fspan;                                                              \
            const float ar = cr * rw, ag = cg * rw, ab = cb * rw, aa = ca * rw;                         \
            acc = (int32x4_t){ sat_i32(ab * 65536.0f), sat_i32(ag * 65536.0f),                          \
                               sat_i32(ar * 65536.0f), sat_i32(aa * 65536.0f) };                        \
            dacc = (int32x4_t){ sat_i32(((cb + cbdx * fspan) * rw_b - ab) * is * 65536.0f),             \
                                sat_i32(((cg + cgdx * fspan) * rw_b - ag) * is * 65536.0f),             \
                                sat_i32(((cr + crdx * fspan) * rw_b - ar) * is * 65536.0f),             \
                                sat_i32(((ca + cadx * fspan) * rw_b - aa) * is * 65536.0f) }; }
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
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin, fgac = T->fgac, ablend = T->ablend;
    const uint32_t clr = T->clr, cmask = T->cmask; const bool enote = T->enote;
    const bool persp = T->persp, dmask = T->dmask;
    const int extra = T->extra;
    const int tinta = T->tinta; const uint16x4_t tavg = T->tavg; const bool tglass = T->tglass;
    const bool fog = T->fog; const uint16x4_t fogv = T->fogv;
    /* the fog step as a compile-time variant: a per-pixel test of the flag cost the unfogged rows
     * ~6% of cm_row_neon under qemu (Kokiri) */
#define CM_ROWF(MODE, ALPHA, DETAIL, ...)                                                                \
    if ((MODE) == 0 && fog) { CM_ROW(MODE, ALPHA, DETAIL, 1, __VA_ARGS__) }                             \
    else                    { CM_ROW(MODE, ALPHA, DETAIL, 0, __VA_ARGS__) }
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    const bool bil64 = sBil64 != 0;
    (void)th; (void)ws; (void)wt; (void)px32;
    if (T->detail) { /* z-tested, both textures 32-bit, tile 0 repeating on a power of two (raster_tri) */
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        const bool dbilin = T->dbilin;
        /* Round 61: the Deku Tree water's case -- fogged at the constant alpha (amode 3), both layers bilinear
         * 64-bit taps -- with amode, dbilin and ablend constants, so the per-pixel branch chains fold away
         * (cm_row_neon's hottest detail block tested amode, dbilin and ablend on every pixel). The same code,
         * the same order: exact. OOT_CMSPEC=0: the generic rows (A/B). */
        if (sCmSpec && T->bilin && bil64 && T->alpha && fog && amode == 3 && dbilin) {
            const int amode = 3; const bool dbilin = true; (void)amode; (void)dbilin;
            if (ablend) { const int ablend = 1; (void)ablend; CM_ROW(0, 1, 1, 1, CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            else        { const int ablend = 0; (void)ablend; CM_ROW(0, 1, 1, 1, CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            return;
        }
        if (T->bilin && bil64) { if (T->alpha) { CM_ROWF(0, 1, 1, CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)) }
                                 else          { CM_ROWF(0, 0, 1, CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)) } }
        else if (T->bilin) { if (T->alpha) { CM_ROWF(0, 1, 1, CM_BILIN(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) }
                        else          { CM_ROWF(0, 0, 1, CM_BILIN(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }
        else          { if (T->alpha) { CM_ROWF(0, 1, 1, CM_NEAR(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) }
                        else          { CM_ROWF(0, 0, 1, CM_NEAR(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }
        return;
    }
#define CM_KIND_RR32_CM_BILIN CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)
#define CM_KIND_RR32_CM_NEAR  CM_NEAR(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)
#define CM_VARIANTS(KIND, MODE, ALPHA)                                                                  \
    if (rr) { if (T->t16) { CM_ROWF(MODE, ALPHA, 0, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }        \
              else if (bil64) { CM_ROWF(MODE, ALPHA, 0, CM_KIND_RR32_##KIND) }                          \
              else        { CM_ROWF(MODE, ALPHA, 0, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }      \
    else    { if (T->t16) { CM_ROWF(MODE, ALPHA, 0, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }          \
              else        { CM_ROWF(MODE, ALPHA, 0, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
    /* round 61: the water's other layer (no detail), the same constants (above) */
    if (sCmSpec && !T->texonly && T->bilin && T->alpha && fog && amode == 3 && rr && !T->t16 && bil64) {
        const int amode = 3; (void)amode;
        if (ablend) { const int ablend = 1; (void)ablend; CM_ROW(0, 1, 0, 1, CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        else        { const int ablend = 0; (void)ablend; CM_ROW(0, 1, 0, 1, CM_BILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        return;
    }
    if (T->texonly) { if (T->bilin) { CM_VARIANTS(CM_BILIN, 1, 0) } else { CM_VARIANTS(CM_NEAR, 1, 0) } }
    else if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 0, 1) } else { CM_VARIANTS(CM_BILIN, 0, 0) } }
    else               { if (T->alpha) { CM_VARIANTS(CM_NEAR, 0, 1) }  else { CM_VARIANTS(CM_NEAR, 0, 0) } }
#undef CM_VARIANTS
#undef CM_KIND_RR32_CM_BILIN
#undef CM_KIND_RR32_CM_NEAR
#undef CM_ROWF
}
/* OOT_WATERLEAN=1 (debug.cfg waterlean=1; 2026-10-01, the basement water audit): the Deku Tree basement water's two
 * constant-folded rows (round 61: fogged, constant alpha fga, amode 3, ablend 1, bilinear 32-bit repeating taps; with
 * and without the detail layer) as their own function, with the per-pixel chain kept in NEON registers. In cm_row_neon
 * GCC built each pixel's bilinear weights, the fog factor and the blend factors through the stack (two str + vldr, and
 * vldr / vmov.16 lane / vstr / vldr per scalar operand) and moved the first blend's result to a core register and
 * back for the second blend -- store-to-load and NEON-to-core round trips on the pixel's dependent chain, the A7's
 * weak spot (round 72). Here, for the same values in the same order:
 *   - the weights and the per-triangle factors are whole vectors (vdup), multiplied lane by lane: x * w[lane] and
 *     x * dup(w) are the same 16-bit products;
 *   - the fog factor stays in NEON (vdup_lane of the shade's alpha lane); the alpha lane after the fog is not kept:
 *     with amode 3 nothing reads it (the blend's alpha lane is overwritten by the store's 255);
 *   - the twice blend (extra 1) blends over the first blend's lanes directly: they are <= 255, so the old narrow to
 *     bytes, core move and widen gave the same B G R values (its alpha lane, 255 there, is again overwritten);
 *     the source x fga product is taken once for both blends;
 *   - the shade clamp is vqshrun #16 (= vqmovun(vshr #16)); x/255 adds x >> 8 with vsra (= vshr + vadd);
 *   - a wrapping tap pair is loaded lane by lane (vld1 lane) instead of through a 64-bit core value.
 * Taken only where every per-pixel test of the folded rows is decided per triangle (raster_tri: fga in [amin, 254],
 * no z write, no decal bias, no fastwater tint, extra 0 or 1); anything else stays on cm_row_neon. Bit-exact against
 * it and the scalar loops (the test harness: random water-mode triangles, ARM under qemu). Default off until
 * the device Bench decides ("Deku Tree basement water, exact (fastwater OFF)" vs "..., exact + waterlean"). */
static inline uint16x4_t cm_div255_lean(uint16x4_t m) { /* cm_div255: (m + (m >> 8) + 1) >> 8 */
    return vshr_n_u16(vadd_u16(vsra_n_u16(m, m, 8), vdup_n_u16(1)), 8);
}
/* CM_LERP2 with the weights as whole vectors (su, sv from the fetch's scope) */
#define CM_LLERP2(X8, Y8)                                                                               \
        const uint16x8_t H = vshrq_n_u16(vmlaq_u16(vshll_n_u8(X8, 8), vsubl_u8(Y8, X8),                 \
                                                   vdupq_n_u16((uint16_t)((su >> 8) & 0xff))), 8);       \
        Tx = vshr_n_u16(vmla_u16(vshl_n_u16(vget_low_u16(H), 8), vsub_u16(vget_high_u16(H), vget_low_u16(H)), \
                                 vdup_n_u16((uint16_t)((sv >> 8) & 0xff))), 8);
#define CM_LBILIN_RR32(PX, MU, WV)                                                                      \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = (su >> 16) & (MU);                                                              \
        const int b0 = WV(sv >> 16) * tw, b1 = WV((sv >> 16) + 1) * tw;                                 \
        uint32x2_t AB, CD;                                                                              \
        if (iu0 < (MU)) { AB = vld1_u32((PX) + b0 + iu0); CD = vld1_u32((PX) + b1 + iu0); }            \
        else { AB = vld1_lane_u32((PX) + b0, vld1_dup_u32((PX) + b0 + iu0), 1);                         \
               CD = vld1_lane_u32((PX) + b1, vld1_dup_u32((PX) + b1 + iu0), 1); }                       \
        const uint32x2x2_t TR = vtrn_u32(AB, CD);                                                       \
        const uint8x8_t X8 = vreinterpret_u8_u32(TR.val[0]), Y8 = vreinterpret_u8_u32(TR.val[1]);       \
        CM_LLERP2(X8, Y8)
/* CM_BILIN(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV) with CM_LLERP2: the taps are unpacked on the core (the LUT) */
#define CM_LBILIN16                                                                                     \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = (su >> 16) & mu, iu1 = ((su >> 16) + 1) & mu;                                   \
        const int b0 = ((sv >> 16) & mv) * tw, b1 = (((sv >> 16) + 1) & mv) * tw;                       \
        const uint32_t a = CM_TAP16(b0 + iu0), b = CM_TAP16(b0 + iu1), c = CM_TAP16(b1 + iu0), d = CM_TAP16(b1 + iu1); \
        const uint8x8_t X8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a));                 \
        const uint8x8_t Y8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b));                 \
        CM_LLERP2(X8, Y8)
#define CM_LDETAIL_0
#define CM_LDETAIL_1                                                                                    \
        {                                                                                               \
            const int32_t du1 = (int32_t)((((uint32_t)(uf >> dsrs) << dsls) + doffs) & dpmu);           \
            const int32_t dv1 = (int32_t)((((uint32_t)(vf >> dsrt) << dslt) + dofft) & dpmv);           \
            uint16x4_t T1x;                                                                             \
            {                                                                                           \
                const int32_t uf = du1, vf = dv1; const int tw = dw1; uint16x4_t Tx;                    \
                CM_LBILIN_RR32(dpx32, dmu, CM_DWRAPV) T1x = Tx;                                          \
            }                                                                                           \
            Tx = vrshr_n_u16(vmla_u16(vmul_u16(Tx, dwinv), T1x, dwt), 8);                               \
        }
/* MODE 0 always. ALPHA 1: the water (FOG 1, amode 3, the blend always, no z write; LX, a constant in each branch, =
 * extra, 0 or 1). ALPHA 0: an opaque row (OOT_OPAQUELEAN): z bias and z write as cm_row_neon; the alpha lane after the
 * fog is not kept there either -- nothing reads it before the store's 255 */
#define CM_PIXELS_LEAN(MODE, ALPHA, DETAIL, FOG, ...)                                                    \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz + (ALPHA ? 0 : zbias)) continue;                                                   \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_LDETAIL_##DETAIL                                                                             \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = cm_div255_lean(vmul_u16(Tx, S));                                                 \
        if (FOG) {                                                                                      \
            const uint16x4_t fs = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs));                    \
        }                                                                                               \
        if (ALPHA) {                                                                                    \
            const uint16x4_t SF = vmul_u16(M, vfa);                                                     \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));         \
            M = cm_div255_lean(vmla_u16(SF, D, vifa));                                                  \
            if (LX) M = cm_div255_lean(vmla_u16(SF, M, vifa));                                          \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (!ALPHA && dmask) *pz = (uint16_t)uz;                                                        \
    }
/* OOT_SKYDEFER's marking rows: CM_PIXELS_LEAN's opaque LEANZ pixel (no z bias, z written), and a byte of the pending
 * sky's mask set where the pixel is written */
#define CM_PIXELS_LEANM(MODE, ALPHA, DETAIL, FOG, ...)                                                   \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, pm++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz) continue;                                                                         \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_LDETAIL_##DETAIL                                                                             \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = cm_div255_lean(vmul_u16(Tx, S));                                                 \
        if (FOG) {                                                                                      \
            const uint16x4_t fs = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs));                    \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        *pz = (uint16_t)uz;                                                                             \
        *pm = 1;                                                                                        \
    }
static bool sDsMarking SAVESTATE_EXCLUDE;  /* a deferred sky is pending: cm_row_lean's LEANZ rows mark what they write */
static uint8_t *sDsMask SAVESTATE_EXCLUDE; /* a byte a pixel of the pending sky's target (sDsFB): 1 = written since */
static uint32_t *sDsFB SAVESTATE_EXCLUDE;
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
/* the PSHADE setup by vcvt (CM_PSHADE_VCVT, above), always */
#undef CM_ROW_PSHADE
#define CM_ROW_PSHADE CM_PSHADE_VCVT
static int sLeanZ SAVESTATE_EXCLUDE = 1; /* cm_row_lean's opaque rows with zbias 0 / z write as constants; OOT_LEANZ=0: off */
static void __attribute__((noinline))
cm_row_lean(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
            float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t v255 = vdup_n_u16(255), vfa = vdup_n_u16((uint16_t)T->fgac), vifa = vdup_n_u16((uint16_t)(255 - T->fgac));
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const int tw = T->w, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp, dmask = T->dmask, fog = T->fog;
    const int zbias = T->zbias;
    const uint16x4_t fogv = T->fogv;
    (void)px; (void)vfa; (void)vifa; (void)dmask; (void)zbias;
    if (T->detail) {
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        if (T->alpha) {
            if (T->extra) { const int LX = 1; CM_ROW(0, 1, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            else          { const int LX = 0; CM_ROW(0, 1, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        } else if (sLeanZ && zbias == 0 && dmask && sDsMarking) { /* the same, marking (OOT_SKYDEFER) */
            const int LX = 0; (void)LX; const int zbias = 0; const bool dmask = true; (void)zbias; (void)dmask;
            uint8_t *pm = sDsMask + (fb - sDsFB);
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANM
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s) pm += (s);
            if (fog) { CM_ROW(0, 0, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            else     { CM_ROW(0, 0, 1, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
        } else if (sLeanZ && zbias == 0 && dmask) { /* OOT_LEANZ: no z bias, z written, as constants */
            const int LX = 0; (void)LX; const int zbias = 0; const bool dmask = true; (void)zbias; (void)dmask;
            if (fog) { CM_ROW(0, 0, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            else     { CM_ROW(0, 0, 1, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        } else {
            const int LX = 0; (void)LX;
            if (fog) { CM_ROW(0, 0, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            else     { CM_ROW(0, 0, 1, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        }
        return;
    }
    if (T->alpha) {
        if (T->extra) { const int LX = 1; CM_ROW(0, 1, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        else          { const int LX = 0; CM_ROW(0, 1, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        return;
    }
    const int LX = 0; (void)LX;
    if (sLeanZ && zbias == 0 && dmask) { /* OOT_LEANZ (2026-10-01): the opaque world's common case -- no z bias (not a
                                          * decal), z written -- with both as constants: two fewer live values in the
                                          * pixel loop, which GCC kept on the stack (and the z pointer with them) */
        const int zbias = 0; const bool dmask = true; (void)zbias; (void)dmask;
        if (sDsMarking) { /* OOT_SKYDEFER: the same rows, marking */
            uint8_t *pm = sDsMask + (fb - sDsFB);
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANM
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s) pm += (s);
            if (T->t16) {
                if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILIN16) }
                else     { CM_ROW(0, 0, 0, 0, CM_LBILIN16) }
            } else {
                if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
                else     { CM_ROW(0, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
            return;
        }
        if (T->t16) {
            if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILIN16) }
            else     { CM_ROW(0, 0, 0, 0, CM_LBILIN16) }
        } else {
            if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
            else     { CM_ROW(0, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        }
        return;
    }
    if (T->t16) {
        if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILIN16) }
        else     { CM_ROW(0, 0, 0, 0, CM_LBILIN16) }
    } else {
        if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        else     { CM_ROW(0, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
    }
}
/* The second set of lean rows (2026-10-01, the FPS hunt): the rows the profile still finds in cm_row_neon, with the
 * per-pixel chain kept in NEON registers as cm_row_lean's (the bilinear weights by vdup, the fog factor by vdup_lane,
 * no stack round trips), the same values in the same order as cm_row_neon -- bit-exact against it and the scalar loops
 * (tools/handheld/raster_test.sh):
 *   - cm_row_leana (OOT_ALPHALEAN): the blended / alpha-tested z-tested texture x shade rows -- foliage, fences,
 *     translucent surfaces -- with the alpha source a compile-time constant (AM = cm_row_neon's amode: 0 the modulated
 *     alpha, 1 the texel's, 2 the shade's, 3 the fogged constant fgac, 4 the texel's times fgac), on every texture
 *     cm_row_neon takes bilinear: 32-bit or 16-bit, repeating on a power of two, clamped or odd-sized (CM_LBILING:
 *     wrapf's taps), the detail tile on the 32-bit repeating ones; and the opaque rows cm_row_lean leaves (clamped or
 *     odd-sized textures). Only rows with no blend-twice (extra 0) and no fastwater tint; the water stays on cm_row_lean.
 *   - cm_row_leans (OOT_SKYLEAN): the sky's texture-only rows (MODE 1: no z, no shade, opaque), bilinear: the texel is
 *     the pixel. No z or colour stepping per pixel (MODE 1 reads neither).
 * In the alpha rows the fog step runs after the alpha test: it has no side effect, and the test reads the alpha from
 * before it (cm_row_neon keeps the modulated alpha's lane through the fog for that). */
/* CM_BILIN(TAP, CM_WRAP_GU, CM_WRAP_GV) with CM_LLERP2: the taps by wrapf (clamp, or a repeat on an odd size) */
#define CM_LBILING(TAP)                                                                                 \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int iu0 = CM_WRAP_GU(su >> 16), iu1 = CM_WRAP_GU((su >> 16) + 1);                         \
        const int b0 = CM_WRAP_GV(sv >> 16) * tw, b1 = CM_WRAP_GV((sv >> 16) + 1) * tw;                 \
        const uint32_t a = TAP(b0 + iu0), b = TAP(b0 + iu1), c = TAP(b1 + iu0), d = TAP(b1 + iu1);      \
        const uint8x8_t X8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a));                 \
        const uint8x8_t Y8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b));                 \
        CM_LLERP2(X8, Y8)
/* OOT_CLAMPTAP (2026-10-04, the flame overlays): CM_LBILING with the inside taps read straight. When 0 <= x0 < tw - 1
 * and 0 <= y0 < th - 1 (x0, y0 the top-left tap: su >> 16, sv >> 16) all four taps are in the texture, and wrapf gives
 * each coordinate back unchanged in every mode (clamp: inside the range; repeat: c & mask = c, c % size = c), so the
 * taps are i0, i0 + 1, i0 + tw, i0 + tw + 1 with i0 = y0 * tw + x0 -- the same texels; anywhere else wrapf's taps as
 * before. itw = tw - 1 and ith = th - 1 as unsigned (itw = 0 when the switch is off: the test never passes). The
 * flames (gEffFire1DL: the Deku Tree's vision, Volvagia's lava holes; tile 0 mirrored and clamped, a 64x128 texture)
 * and Barinade's room's blended layers (clamped 32x64 / 32x32) sat in wrapf's mode tests and clamps, 2 x 4 a pixel.
 * OOT_ALPHASKIP (same day): SKIP 1 -- the alpha rows whose fa is at most the texel's alpha (AM 0: div255(ta * sa) or,
 * in the tint rows, k's alpha lane 0 + div255(sa * ta); AM 1: ta; AM 4: div255(ta * fgac); never with a detail layer,
 * which mixes tile 1's alpha in) -- take the alpha test before the lerps: the bilinear alpha is at most the largest of
 * the four taps' (each lerp stays between its two ends: (x * 256 + (y - x) * f) >> 8 <= max(x, y) * 256 >> 8), and
 * (a | b | c | d) >> 24 is at least each tap's alpha, so (a | b | c | d) < askip = amin << 24 means fa < amin: the pixel
 * the alpha test rejects anyway, rejected before the lerps, the tint, the fog step and the blend (none of which writes
 * anything for it). askip = 0 when the switch is off. The flames' see-through texels: ~40-80k pixels a frame of the
 * vision. */
#define CM_LBILINI(TAP, SKIP)                                                                           \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int x0 = su >> 16, y0 = sv >> 16;                                                         \
        uint32_t a, b, c, d;                                                                            \
        if ((unsigned)x0 < itw && (unsigned)y0 < ith) {                                                 \
            const int i0 = y0 * tw + x0;                                                                \
            a = TAP(i0); b = TAP(i0 + 1); c = TAP(i0 + tw); d = TAP(i0 + tw + 1);                       \
        } else {                                                                                        \
            const int iu0 = CM_WRAP_GU(x0), iu1 = CM_WRAP_GU(x0 + 1);                                   \
            const int b0 = CM_WRAP_GV(y0) * tw, b1 = CM_WRAP_GV(y0 + 1) * tw;                           \
            a = TAP(b0 + iu0); b = TAP(b0 + iu1); c = TAP(b1 + iu0); d = TAP(b1 + iu1);                 \
        }                                                                                               \
        if ((SKIP) && (a | b | c | d) < askip) continue;                                                \
        const uint8x8_t X8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a));                 \
        const uint8x8_t Y8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b));                 \
        CM_LLERP2(X8, Y8)
/* CM_LBILINI on a 32-bit texture: the inside taps as two 64-bit loads and vtrn (CM_LBILIN_RR32's lanes: X8 = {a, c},
 * Y8 = {b, d}), not through core registers and the stack; with SKIP the four words are read on the core first for the
 * test (the passing pixels read them again into NEON: L1 hits) */
#define CM_LBILINI32(SKIP)                                                                              \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        const int x0 = su >> 16, y0 = sv >> 16;                                                         \
        uint8x8_t X8, Y8;                                                                               \
        if ((unsigned)x0 < itw && (unsigned)y0 < ith) {                                                 \
            const uint32_t *const p0 = px32 + y0 * tw + x0, *const p1 = p0 + tw;                        \
            if ((SKIP) && (p0[0] | p0[1] | p1[0] | p1[1]) < askip) continue;                            \
            const uint32x2x2_t TR = vtrn_u32(vld1_u32(p0), vld1_u32(p1));                               \
            X8 = vreinterpret_u8_u32(TR.val[0]); Y8 = vreinterpret_u8_u32(TR.val[1]);                   \
        } else {                                                                                        \
            const int iu0 = CM_WRAP_GU(x0), iu1 = CM_WRAP_GU(x0 + 1);                                   \
            const int b0 = CM_WRAP_GV(y0) * tw, b1 = CM_WRAP_GV(y0 + 1) * tw;                           \
            const uint32_t a = px32[b0 + iu0], b = px32[b0 + iu1], c = px32[b1 + iu0], d = px32[b1 + iu1]; \
            if ((SKIP) && (a | b | c | d) < askip) continue;                                            \
            X8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a));                             \
            Y8 = vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b));                             \
        }                                                                                               \
        CM_LLERP2(X8, Y8)
/* the alpha sources whose fa is at most the texel's alpha (OOT_ALPHASKIP), in an AM's scope */
#define CM_ASKIP_AM (AM == 0 || AM == 1 || AM == 4)
static int sClampTap SAVESTATE_EXCLUDE = 1;  /* CM_LBILINI's inside taps (2026-10-04); OOT_CLAMPTAP=0: wrapf's taps always */
static int sAlphaSkip SAVESTATE_EXCLUDE = 1; /* CM_LBILINI's early alpha test (2026-10-04); OOT_ALPHASKIP=0: off */
/* MODE 0, ALPHA 1: cm_row_neon's CM_PIXELS(0, 1, DETAIL, FOG, ...) with amode = AM and extra 0, tinta 0 */
#define CM_PIXELS_LEANA(MODE, ALPHA, DETAIL, FOG, ...)                                                   \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz + zbias) continue;                                                                 \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_LDETAIL_##DETAIL                                                                             \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = cm_div255_lean(vmul_u16(Tx, S));                                                 \
        const int fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 3)          \
                     : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \
        if (fa < amin) continue;                                                                        \
        if (FOG) {                                                                                      \
            const uint16x4_t fs = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs));                    \
        }                                                                                               \
        if (fa < 255 && (ablend || *pz != 0xFFFF || (*pf & cmask) != clr)) {                            \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));         \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa)))); \
        } else if (enote && fa < 255) edge_note(pf, fa, dmask ? uz : 0xFFFF);                             \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (dmask) *pz = (uint16_t)uz;                                                                  \
    }
static int sAlphaLean SAVESTATE_EXCLUDE = 1; /* cm_row_leana (2026-10-01); OOT_ALPHALEAN=0: those rows on cm_row_neon */
static int sSkyLean SAVESTATE_EXCLUDE = 1;   /* cm_row_leans (2026-10-01); OOT_SKYLEAN=0: the sky on cm_row_neon */
static void __attribute__((noinline))
cm_row_leana(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
             float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t v255 = vdup_n_u16(255);
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp, dmask = T->dmask, fog = T->fog;
    const int zbias = T->zbias, amin = T->amin, fgac = T->fgac, ablend = T->ablend;
    const uint32_t clr = T->clr, cmask = T->cmask; const bool enote = T->enote;
    const uint16x4_t fogv = T->fogv;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const uint16x4_t vfa = v255, vifa = v255; /* (CM_PIXELS_LEAN's water blend: not reached with ALPHA 0) */
    const int LX = 0; (void)LX; (void)th; (void)px; (void)vfa; (void)vifa;
    const unsigned itw = sClampTap ? (unsigned)(tw - 1) : 0u, ith = (unsigned)(th - 1); /* OOT_CLAMPTAP (CM_LBILINI) */
    const uint32_t askip = sAlphaSkip ? (uint32_t)amin << 24 : 0u;                         /* OOT_ALPHASKIP */
    if (!T->alpha && T->detail) { /* OOT_DETCLAMP: opaque, tile 0 32-bit clamped or odd-sized, tile 1 bilinear */
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        if (fog) { CM_ROW(0, 0, 1, 1, CM_LBILINI32(0)) } else { CM_ROW(0, 0, 1, 0, CM_LBILINI32(0)) }
        return;
    }
    if (!T->alpha) { /* opaque, clamped or odd-sized (cm_row_lean has the repeating ones) */
        if (T->t16) { if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(0, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } }
        else        { if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILINI32(0)) } else { CM_ROW(0, 0, 0, 0, CM_LBILINI32(0)) } }
        return;
    }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANA
/* the alpha rows of one fetch for each alpha source: AM 3 and 4 only fogged (raster_tri: amode 3/4 come with fogtri) */
#define CM_LEANA_AM(DETAIL, FETCH)                                                                      \
    if (fog) {                                                                                          \
        switch (T->amode) {                                                                             \
        case 0: { const int AM = 0; CM_ROW(0, 1, DETAIL, 1, FETCH) } break;                             \
        case 1: { const int AM = 1; CM_ROW(0, 1, DETAIL, 1, FETCH) } break;                             \
        case 2: { const int AM = 2; CM_ROW(0, 1, DETAIL, 1, FETCH) } break;                             \
        case 3: { const int AM = 3; CM_ROW(0, 1, DETAIL, 1, FETCH) } break;                             \
        default: { const int AM = 4; CM_ROW(0, 1, DETAIL, 1, FETCH) } break;                            \
        }                                                                                               \
    } else {                                                                                            \
        switch (T->amode) {                                                                             \
        case 0: { const int AM = 0; CM_ROW(0, 1, DETAIL, 0, FETCH) } break;                             \
        case 1: { const int AM = 1; CM_ROW(0, 1, DETAIL, 0, FETCH) } break;                             \
        default: { const int AM = 2; CM_ROW(0, 1, DETAIL, 0, FETCH) } break;                            \
        }                                                                                               \
    }
    if (T->detail) { /* tile 0 32-bit repeating on a power of two, tile 1 bilinear (raster_tri's dneon + dbilin) */
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        if (rr) { CM_LEANA_AM(1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        else    { CM_LEANA_AM(1, CM_LBILINI32(0)) } /* OOT_DETCLAMP: tile 0 clamped or odd-sized */
        return;
    }
    if (rr) { if (T->t16) { CM_LEANA_AM(0, CM_LBILIN16) } else { CM_LEANA_AM(0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }
    else    { if (T->t16) { CM_LEANA_AM(0, CM_LBILINI(CM_TAP16, CM_ASKIP_AM)) } else { CM_LEANA_AM(0, CM_LBILINI32(CM_ASKIP_AM)) } }
#undef CM_LEANA_AM
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
}
/* OOT_LERPLEAN (2026-10-02, FPS round 2): the z-tested bilinear rows whose colour is the (A - K) * TEXEL0 + K tint
 * (texlerp: the shade toward the constant K by the texel) in NEON rows. Before, raster_tri sent them to its scalar loops
 * (SPAN_LOOP_CM's CM 0, or CM 4 when fogged): in the device profile of the Deku Tree basement water view (build
 * 23f4901f) the flat alpha-tested one alone was ~7% of the frame -- the generic span loop, with every combine flag,
 * the tint constants and the per-pixel state on the stack (rgb_tex, rgb_in and texlerp tested every pixel). Here, per
 * pixel, the scalar loop's integer steps in lanes (B G R A), the same values in the same order:
 *   - the texel: the same bilinear taps and lerps as the other lean rows (CM_LBILIN_RR32 / CM_LBILIN16 / CM_LBILING);
 *   - the shade: clamp8_sh16 per lane = vqshrun #16 + vmin 255 (cm_row_neon's rule);
 *   - the tint k + ((s - k) * t) / 255 toward zero = k + div255(sat(s - k) t) - div255(sat(k - s) t) (one saturating
 *     difference is 0; products <= 255 * 255: ui_tint_n's identity); with K's alpha lane 0 the alpha lane comes out
 *     div255(sa * ta), the scalar fa for a_tex && a_in (AM 0);
 *   - fa by the compile-time alpha source (cm_row_leana's AM: 0 that lane, 1 the texel's, 2 the shade's; fogged with
 *     the constant combiner alpha: 3 fga, 4 div255(ta * fga) -- the scalar fta < 255 ? div255(fta * fga) : fga, since
 *     div255(255 * fga) = fga), read before the fog step as the scalar loop does;
 *   - the fog step (CM 4) div255(c (255 - sa) + fog sa) on B G R; the alpha test, the blend and the z write as
 *     cm_row_leana's.
 * Taken (raster_tri) only for: z-tested, bilinear, rgb_tex && rgb_in && texlerp && !rgb_texa (the scalar loops'
 * texlerp branch), no detail tile, not a doubled triangle's blend-twice draw; tile 0 any texture cm_row_leana takes
 * (32-bit repeating on a power of two with OOT_BIL64, 16-bit, clamped or odd-sized). raster_test.sh (kind lerp, and in
 * gen) compares it with the scalar loops byte for byte. OOT_LERPLEAN=0: the scalar loops. */
#define CM_PIXELS_LEANL(MODE, ALPHA, DETAIL, FOG, ...)                                                   \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz + zbias) continue;                                                                 \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_LDETAIL_##DETAIL                                                                             \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = vsub_u16(vadd_u16(lk, cm_div255_lean(vmul_u16(vqsub_u16(S, lk), Tx))),             \
                                cm_div255_lean(vmul_u16(vqsub_u16(lk, S), Tx)));                        \
        int fa = 255;                                                                                   \
        if (ALPHA) {                                                                                    \
            fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 3)                \
               : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \
            if (fa < amin) continue;                                                                    \
        }                                                                                               \
        if (FOG) {                                                                                      \
            const uint16x4_t fs = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs));                    \
        }                                                                                               \
        if (ALPHA && fa < 255 && (ablend || *pz != 0xFFFF || (*pf & cmask) != clr)) {                   \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));         \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa)))); \
        } else if (ALPHA && enote && fa < 255) edge_note(pf, fa, dmask ? uz : 0xFFFF);                  \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (dmask) *pz = (uint16_t)uz;                                                                  \
    }
static int sLerpLean SAVESTATE_EXCLUDE = 1; /* cm_row_leanl (2026-10-02); OOT_LERPLEAN=0: those rows in the scalar loops */
static int sDetClamp SAVESTATE_EXCLUDE = 1; /* the detail layer over a clamped / odd tile 0 in cm_row_leana (2026-10-04); OOT_DETCLAMP=0: scalar */
static int sDetLerp SAVESTATE_EXCLUDE = 1;  /* the detail layer under the tint in cm_row_leanl (2026-10-04); OOT_DETLERP=0: scalar */
static void __attribute__((noinline))
cm_row_leanl(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
             float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t v255 = vdup_n_u16(255), lk = T->lk;
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp, dmask = T->dmask, fog = T->fog;
    const int zbias = T->zbias, amin = T->amin, fgac = T->fgac, ablend = T->ablend;
    const uint32_t clr = T->clr, cmask = T->cmask; const bool enote = T->enote; /* OOT_EDGETINT, as cm_row_leana */
    const uint16x4_t fogv = T->fogv;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const unsigned itw = sClampTap ? (unsigned)(tw - 1) : 0u, ith = (unsigned)(th - 1); /* OOT_CLAMPTAP (CM_LBILINI) */
    const uint32_t askip = sAlphaSkip ? (uint32_t)amin << 24 : 0u;                         /* OOT_ALPHASKIP */
    (void)th; (void)px;
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANL
/* the rows of one fetch: opaque, or each alpha source (AM 3 and 4 only fogged: raster_tri's amode) */
#define CM_LEANL_AM(D, FETCH)                                                                              \
    if (!T->alpha) { /* AM unread without the alpha test; 2 keeps CM_ASKIP_AM (OOT_ALPHASKIP) off */          \
        const int AM = 2; (void)AM;                                                                     \
        if (fog) { CM_ROW(0, 0, D, 1, FETCH) } else { CM_ROW(0, 0, D, 0, FETCH) }                       \
    } else if (fog) {                                                                                   \
        switch (T->amode) {                                                                             \
        case 0: { const int AM = 0; CM_ROW(0, 1, D, 1, FETCH) } break;                                  \
        case 1: { const int AM = 1; CM_ROW(0, 1, D, 1, FETCH) } break;                                  \
        case 2: { const int AM = 2; CM_ROW(0, 1, D, 1, FETCH) } break;                                  \
        case 3: { const int AM = 3; CM_ROW(0, 1, D, 1, FETCH) } break;                                  \
        default: { const int AM = 4; CM_ROW(0, 1, D, 1, FETCH) } break;                                 \
        }                                                                                               \
    } else {                                                                                            \
        switch (T->amode) {                                                                             \
        case 0: { const int AM = 0; CM_ROW(0, 1, D, 0, FETCH) } break;                                  \
        case 1: { const int AM = 1; CM_ROW(0, 1, D, 0, FETCH) } break;                                  \
        default: { const int AM = 2; CM_ROW(0, 1, D, 0, FETCH) } break;                                 \
        }                                                                                               \
    }
    if (T->detail) { /* OOT_DETLERP: tile 1 over a 32-bit tile 0, both bilinear, as cm_row_lean / cm_row_leana mix them */
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        if (rr) { CM_LEANL_AM(1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } else { CM_LEANL_AM(1, CM_LBILINI32(0)) }
        return;
    }
    if (rr) { if (T->t16) { CM_LEANL_AM(0, CM_LBILIN16) } else { CM_LEANL_AM(0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }
    else    { if (T->t16) { CM_LEANL_AM(0, CM_LBILINI(CM_TAP16, CM_ASKIP_AM)) } else { CM_LEANL_AM(0, CM_LBILINI32(CM_ASKIP_AM)) } }
#undef CM_LEANL_AM
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
}
/* OOT_NOZLEAN (2026-10-04): the blended texture x shade rows drawn without a z test (raster_tri's scalar SPAN_LOOP with
 * ZTEST 0 and the plain modulate: the inside of Ganon's Castle, ~40,000 px a frame of a 64x128 layer) in NEON rows:
 * CM_PIXELS_LEANA's pixel without the depth test, the depth write and the decal bias (the scalar loop makes none of
 * them without ZTEST); the blend's clear-pixel test and the edge note are kept as the loop has them, though without z
 * ablend is always 1 (raster_tri: !(tex_edge && sEdgeOpaque && ztest)), so every pixel under 255 blends. Not fogged, no detail layer, bilinear (raster_tri). */
#define CM_PIXELS_LEANZ(MODE, ALPHA, DETAIL, FOG, ...)                                                   \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, acc = vaddq_s32(acc, dacc)) {   \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = cm_div255_lean(vmul_u16(Tx, S));                                                 \
        const int fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 3)          \
                     : (int)vget_lane_u16(S, 3);                                                        \
        if (fa < amin) continue;                                                                        \
        if (fa < 255 && (ablend || *pz != 0xFFFF || (*pf & cmask) != clr)) {                            \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));         \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa)))); \
        } else if (enote && fa < 255) edge_note(pf, fa, 0xFFFF);                                        \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
    }
static int sNoZLean SAVESTATE_EXCLUDE = 1; /* cm_row_leanz (2026-10-04); OOT_NOZLEAN=0: those rows in the scalar loops */
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANZ
static void __attribute__((noinline))
cm_row_leanz(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
             float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t v255 = vdup_n_u16(255);
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp;
    const int amin = T->amin, ablend = T->ablend;
    const uint32_t clr = T->clr, cmask = T->cmask; const bool enote = T->enote;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const unsigned itw = sClampTap ? (unsigned)(tw - 1) : 0u, ith = (unsigned)(th - 1); /* OOT_CLAMPTAP (CM_LBILINI) */
    const uint32_t askip = sAlphaSkip ? (uint32_t)amin << 24 : 0u;                         /* OOT_ALPHASKIP */
    (void)th; (void)px; (void)zdq;
#define CM_LEANZ_AM(FETCH)                                                                              \
    switch (T->amode) {                                                                                 \
    case 0: { const int AM = 0; CM_ROW(0, 1, 0, 0, FETCH) } break;                                      \
    case 1: { const int AM = 1; CM_ROW(0, 1, 0, 0, FETCH) } break;                                      \
    default: { const int AM = 2; CM_ROW(0, 1, 0, 0, FETCH) } break;                                     \
    }
    if (rr) { if (T->t16) { CM_LEANZ_AM(CM_LBILIN16) } else { CM_LEANZ_AM(CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }
    else    { if (T->t16) { CM_LEANZ_AM(CM_LBILINI(CM_TAP16, CM_ASKIP_AM)) } else { CM_LEANZ_AM(CM_LBILINI32(CM_ASKIP_AM)) } }
#undef CM_LEANZ_AM
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
}
/* MODE 1 (the sky): cm_row_neon's CM_PIXELS(1, 0, 0, 0, ...) -- the texel, alpha 255 */
#define CM_PIXELS_LEANS(MODE, ALPHA, DETAIL, FOG, ...)                                                   \
    for (uint32_t *pf = fb; pf < end; pf++, uf += duf, vf += dvf) {                                     \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vorr_u16(Tx, a255), Tx))), 0);       \
    }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANS
static void __attribute__((noinline))
cm_row_leans(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
             float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const void *const px = T->px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = false; /* MODE 1 reads no shade */
    const int32_t zdq = T->zdq;
    const bool persp = T->persp;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const unsigned itw = sClampTap ? (unsigned)(tw - 1) : 0u, ith = (unsigned)(th - 1); /* OOT_CLAMPTAP (CM_LBILINI) */
    const uint32_t askip = 0; /* (no alpha test: CM_LBILINI's SKIP 0) */
    (void)th; (void)px; (void)zdq; (void)dacc0; (void)askip;
    if (rr) { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILIN16) } else { CM_ROW(1, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }
    else    { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(1, 0, 0, 0, CM_LBILINI32(0)) } }
}
/* OOT_UINEON (2026-10-01): raster_tri's CM 5 span (the UI's (A - K) * TEXEL0 + K tint, nearest, inside the texture,
 * no z) a pixel at a time in NEON lanes (B G R A), the scalar loop's integer steps: the shade clamp vqshrun + vmin
 * (= clamp8_sh16), the tint k + ((s - k) * t) / 255 toward zero as k + div255(sat(s - k) * t) - div255(sat(k - s) * t)
 * (one of the saturating differences is 0), fa and the blend with div255 as the scalar loop's (all products <= 255 *
 * 255); the alpha lane of the colour is not read, and the store's 255 replaces it. */
static void __attribute__((noinline))
ui_tint_n(uint32_t *fb, const uint16_t *zb, int n, const void *texpx, bool t16, int tw, int32_t uf, int32_t duf,
          int32_t vf, int32_t dvf, int32x4_t acc, int32x4_t dacc, uint16x4_t k, int amode, bool alpha, int amin,
          bool ablend, uint32_t clr) {
    const uint16x4_t v255 = vdup_n_u16(255), a255 = vcreate_u16(0x00FF000000000000ull);
    for (int i = 0; i < n; i++, uf += duf, vf += dvf, acc = vaddq_s32(acc, dacc)) {
        const int ti = (vf >> 16) * tw + (uf >> 16);
        const uint32_t t = t16 ? texel_at(texpx, true, ti) : ((const uint32_t *)texpx)[ti];
        const uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(t))));
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);
        uint16x4_t M = vsub_u16(vadd_u16(k, cm_div255_lean(vmul_u16(vqsub_u16(S, k), T))),
                                cm_div255_lean(vmul_u16(vqsub_u16(k, S), T)));
        if (alpha) {
            const int fa = amode == 2 ? (int)vget_lane_u16(S, 3) : amode == 1 ? (int)(t >> 24)
                         : div255((int)(t >> 24) * (int)vget_lane_u16(S, 3));
            if (fa < amin) continue;
            if (fa < 255 && (ablend || zb[i] != 0xFFFF || (fb[i] & 0xFFFFFFu) != clr)) {
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(fb + i))));
                M = cm_div255_lean(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa))));
            }
        }
        M = vorr_u16(M, a255);
        vst1_lane_u32(fb + i, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);
    }
}
/* OOT_SKYDEFER (2026-10-01): the full-resolution sky drawn last, where nothing overwrote it. OoT draws the sky first
 * (no z, opaque, texture only: cm_row_leans), then the world over most of it. Here the sky's triangles are kept (their
 * CmTri and row values, everything their rows read but the texels and the target) instead of drawn; while they wait,
 * the opaque LEANZ rows -- writes that read nothing of the colour buffer -- mark each pixel they write in a byte mask;
 * every other draw, colour read, target change, texture change and the frame's end first draws the kept sky
 * (sky_settle), skipping the marked pixels. Per pixel the result is the original's: the sky's colour depends on the
 * sky alone; a marked pixel was overwritten after the sky by a write that did not read it (so the sky's colour there
 * was never seen); an unmarked one shows the sky (its later writers, if any, come after the settle). The sky writes no
 * z, so z is the same at every moment. A sky triangle kept after a marking write settles the earlier ones first (it
 * would have overwritten that write). On qemu it saved no instructions -- a skipped pixel still runs its span's setup,
 * and the marking rows cost more a pixel -- but the device Bench of 23f4901f (2026-10-02) has Hyrule Field day at
 * render 31.35 ms deferred vs 34.15 in place (2 runs each), so it is the default; OOT_SKYDEFER=0 draws it in place. */
#define CM_PIXELS_LEANSM(MODE, ALPHA, DETAIL, FOG, ...)                                                  \
    for (uint32_t *pf = fb; pf < end; pf++, pm++, uf += duf, vf += dvf) {                               \
        if (*pm) continue;                                                                              \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vorr_u16(Tx, a255), Tx))), 0);       \
    }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEANSM
static void __attribute__((noinline))
cm_row_leans_m(const CmTri *T, uint32_t *fb, uint16_t *zp, int n, float zf, float cr, float cg, float cb, float ca,
               float uw, float vw, float sw, float rw, const uint8_t *pm) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const void *const px = T->px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = false;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    const unsigned itw = sClampTap ? (unsigned)(tw - 1) : 0u, ith = (unsigned)(th - 1); /* OOT_CLAMPTAP (CM_LBILINI) */
    const uint32_t askip = 0; /* (no alpha test: CM_LBILINI's SKIP 0) */
    (void)th; (void)px; (void)zdq; (void)dacc0; (void)askip;
    if (rr) { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILIN16) } else { CM_ROW(1, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }
    else    { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(1, 0, 0, 0, CM_LBILINI32(0)) } }
}
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_LEAN
#undef CM_ROW_SKIPPED
#define CM_ROW_SKIPPED(s)
typedef struct { /* a kept sky triangle's row values (raster_tri's names, TRI_ROW_SETUP's inputs) */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    float z0, z1, z2, r0, r1, r2, g0, g1, g2, bl0, bl1, bl2, a0, a1, a2, uw0, uw1, uw2, vw0, vw1, vw2, iw0, iw1, iw2;
    int minx, maxx, miny, maxy, stride;
    bool flat, persp, p3d;
    uint16_t *zbuf; /* the target's z (the sky's rows step its pointer, never read it) */
} DsRows;
#define DS_MAX 192
static struct { CmTri cmt; DsRows R; } sDsTri[DS_MAX] SAVESTATE_EXCLUDE;
static int sDsN SAVESTATE_EXCLUDE;          /* sky triangles kept */
static bool sDsMarked SAVESTATE_EXCLUDE;    /* a marking row drew since the last kept triangle */
static int sDsMaskSz SAVESTATE_EXCLUDE;
static int sSkyDefer SAVESTATE_EXCLUDE = 1; /* OOT_SKYDEFER=0: drawn in place (deferred is exact; on since the 2026-10-02 Bench) */
static bool sDsTag SAVESTATE_EXCLUDE;       /* between the sky's tags (gfx_scegu_set_sky_pass), full resolution */
static bool sDsAllow SAVESTATE_EXCLUDE, sDsP3D SAVESTATE_EXCLUDE; /* gfx_soft_draw_triangles: no OOT_PICK; the 3D phase */
#if defined(MIYOO)
/* The Mini's second core (kit miyoo_worker, 2026-10-09): a big triangle's rows are split in two shares, the worker
 * drawing the lower one while this thread draws the upper, then waiting (raster_rows below; the deferred sky's
 * triangles here). Only the row range differs between the shares: see raster_rows for why the pixels are the same. */
#include "miyoo_worker.h"
static int sSplitPx SAVESTATE_EXCLUDE = -1;  /* OOT_SPLITPX: the estimated span pixels a triangle needs to be split */
static int sSplitRows SAVESTATE_EXCLUDE = 8; /* OOT_SPLITROWS: and its rows */
uint64_t gfx_soft_split_px SAVESTATE_EXCLUDE; /* span pixels drawn in split triangles (the PERF line) */
static void split_init(void) { /* on the first big triangle: the game thread, after the audio and boot threads started
                                * (they keep both cores; threads started after this one is pinned would inherit core 0) */
    miyoo_worker_start();
    const char *e = getenv("OOT_SPLITPX");
    sSplitPx = e ? atoi(e) : 800;
    e = getenv("OOT_SPLITROWS");
    if (e) sSplitRows = atoi(e);
    if (sSplitRows < 2) sSplitRows = 2;
}
/* The row at which a triangle's span pixels reach half (the upper share is miny..ym-1), or -1: too few rows or pixels,
 * or no worker. Each row's pixel interval is estimated from the same edge bases as TRI_ROW_SETUP's (unsnapped, by
 * reciprocals): it only balances the shares, it never decides a pixel. */
static int split_row(float e0c, float e0x, float e0y, float e1c, float e1x, float e1y, float e2c, float e2x, float e2y,
                     float inv_area, float w0dx, float w1dx, float w2dx, int minx, int maxx, int miny, int maxy) {
    enum { MAXROWS = 1024 };
    float cum[MAXROWS];
    const int rows = maxy - miny;
    if (sSplitPx < 0) split_init();
    if (rows < sSplitRows || rows > MAXROWS || !miyoo_worker_ready()) return -1;
    const float ec[3] = { e0c, e1c, e2c }, ex[3] = { e0x, e1x, e2x }, ey[3] = { e0y, e1y, e2y }, ds[3] = { w0dx, w1dx, w2dx };
    float ids[3];
    for (int i = 0; i < 3; i++) ids[i] = ds[i] != 0.0f ? 1.0f / ds[i] : 0.0f;
    const float span = (float)(maxx - minx);
    float tot = 0.0f;
    for (int k = 0; k < rows; k++) {
        const float py = (float)(miny + k) + 0.5f;
        float lo = 0.0f, hi = span;
        for (int i = 0; i < 3; i++) {
            const float b = (ec[i] - (py - ey[i]) * ex[i]) * inv_area;
            if (ds[i] > 0.0f) { const float q = -b * ids[i]; if (q > lo) lo = q; }
            else if (ds[i] < 0.0f) { const float q = -b * ids[i] + 1.0f; if (q < hi) hi = q; }
            else if (b < 0.0f) hi = lo;
        }
        if (hi > lo) tot += hi - lo;
        cum[k] = tot;
    }
    if (!(tot >= (float)sSplitPx)) return -1;
    int k = 0;
    while (k < rows - 1 && cum[k] < tot * 0.5f) k++;
    const int ym = miny + k + 1;
    return ym > miny && ym < maxy ? ym : -1;
}
/* A kept sky triangle's rows miny..maxy-1 (sky_flush's loop) */
static uint32_t __attribute__((noinline)) sky_rows(const CmTri *T, const DsRows *RP, int miny, int maxy) {
    const DsRows R = *RP;
    const float e0c = R.e0c, e0x = R.e0x, e0y = R.e0y, e1c = R.e1c, e1x = R.e1x, e1y = R.e1y;
    const float e2c = R.e2c, e2x = R.e2x, e2y = R.e2y, inv_area = R.inv_area;
    const float w0dx = R.w0dx, w1dx = R.w1dx, w2dx = R.w2dx, z0 = R.z0, z1 = R.z1, z2 = R.z2;
    const float r0 = R.r0, r1 = R.r1, r2 = R.r2, g0 = R.g0, g1 = R.g1, g2 = R.g2;
    const float bl0 = R.bl0, bl1 = R.bl1, bl2 = R.bl2, a0 = R.a0, a1 = R.a1, a2 = R.a2;
    const float uw0 = R.uw0, uw1 = R.uw1, uw2 = R.uw2, vw0 = R.vw0, vw1 = R.vw1, vw2 = R.vw2;
    const float iw0 = R.iw0, iw1 = R.iw1, iw2 = R.iw2;
    const int minx = R.minx, maxx = R.maxx, sW = R.stride;
    const bool flat = R.flat, persp = R.persp;
    uint32_t *const fbuf = sDsFB; uint16_t *const zb = R.zbuf;
    uint32_t tri_px = 0;
    const bool own = false; OwnTri oe; (void)oe; /* the sky: opaque, the inclusive solve */
    for (int y = miny; y < maxy; y++) {
        TRI_ROW_SETUP
        cm_row_leans_m(T, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw, sDsMask + idx);
    }
    return tri_px;
}
typedef struct { const CmTri *T; const DsRows *R; int y0, y1; uint32_t px; } SkyJob;
static void sky_job(void *arg) { SkyJob *j = (SkyJob *)arg; j->px = sky_rows(j->T, j->R, j->y0, j->y1); }
static uint32_t sky_tri(const CmTri *T, const DsRows *R) {
    const int ym = split_row(R->e0c, R->e0x, R->e0y, R->e1c, R->e1x, R->e1y, R->e2c, R->e2x, R->e2y, R->inv_area,
                             R->w0dx, R->w1dx, R->w2dx, R->minx, R->maxx, R->miny, R->maxy);
    if (ym < 0) return sky_rows(T, R, R->miny, R->maxy);
    SkyJob j = { T, R, ym, R->maxy, 0 };
    miyoo_worker_run(sky_job, &j);
    const uint32_t px = sky_rows(T, R, R->miny, ym);
    miyoo_worker_wait();
    gfx_soft_split_px += px + j.px;
    return px + j.px;
}
#endif /* MIYOO */
static void sky_flush(void) {
    const int n = sDsN;
    sDsN = 0; sDsMarking = false; sDsMarked = false;
    for (int k = 0; k < n; k++) {
        const CmTri *const T = &sDsTri[k].cmt;
        const DsRows R = sDsTri[k].R;
        const float e0c = R.e0c, e0x = R.e0x, e0y = R.e0y, e1c = R.e1c, e1x = R.e1x, e1y = R.e1y;
        const float e2c = R.e2c, e2x = R.e2x, e2y = R.e2y, inv_area = R.inv_area;
        const float w0dx = R.w0dx, w1dx = R.w1dx, w2dx = R.w2dx, z0 = R.z0, z1 = R.z1, z2 = R.z2;
        const float r0 = R.r0, r1 = R.r1, r2 = R.r2, g0 = R.g0, g1 = R.g1, g2 = R.g2;
        const float bl0 = R.bl0, bl1 = R.bl1, bl2 = R.bl2, a0 = R.a0, a1 = R.a1, a2 = R.a2;
        const float uw0 = R.uw0, uw1 = R.uw1, uw2 = R.uw2, vw0 = R.vw0, vw1 = R.vw1, vw2 = R.vw2;
        const float iw0 = R.iw0, iw1 = R.iw1, iw2 = R.iw2;
        const int minx = R.minx, maxx = R.maxx, sW = R.stride;
        const bool flat = R.flat, persp = R.persp;
        uint32_t *const fbuf = sDsFB; uint16_t *const zb = R.zbuf;
        uint32_t tri_px = 0;
        const bool own = false; OwnTri oe; (void)oe; /* the sky: opaque, the inclusive solve */
#if defined(MIYOO)
        tri_px = sky_tri(T, &sDsTri[k].R); /* the same rows, in two shares when it is big (sky_rows) */
#else
        for (int y = R.miny; y < R.maxy; y++) {
            TRI_ROW_SETUP
            cm_row_leans_m(T, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw, sDsMask + idx);
        }
#endif
        if (R.p3d) gfx_soft_span_px += tri_px; /* the stat raster_tri's caller would have added */
    }
}
#undef CM_ROW_ZSK
#define CM_ROW_ZSK 0 /* the half-resolution layer's rows: no OOT_ZSKIP (their own depth buffer) */
#include "gfx_soft_halfwater.inc.c" /* OOT_HALFWATER (2026-10-02, a look trade; default on since 2026-10-03) */
static void sky_settle(void) { if (sDsN) sky_flush(); hfx_settle(); }
static void sky_texels_change(const void *px) {
    for (int k = 0; k < sDsN; k++) if (sDsTri[k].cmt.px == px) { sky_flush(); return; }
}
#undef CM_PIXELS_LEANSM
#undef CM_PIXELS_LEANM
#undef CM_PIXELS_LEANA
#undef CM_PIXELS_LEANS
#undef CM_LBILING
#undef CM_ROW_PIXELS
#undef CM_ROW_PSHADE
#undef CM_PSHADE_VCVT
#undef CM_PIXELS_LEAN
#undef CM_LLERP2
#undef CM_LBILIN_RR32
#undef CM_LBILIN16
#undef CM_LDETAIL_0
#undef CM_LDETAIL_1
#undef CM_PIXELS
#undef CM_BILIN
#undef CM_BILIN_RR32
#undef CM_NEAR
#undef CM_DETAIL_0
#undef CM_DETAIL_1
#undef CM_DTAP
#undef CM_DWRAPU
#undef CM_DWRAPV
#undef CM_TAP32
#undef CM_TAP16
#undef CM_WRAP_RRU
#undef CM_WRAP_RRV
#undef CM_WRAP_GU
#undef CM_WRAP_GV
#undef CM_ROW
#undef CM_ROW_SKIPPED
#undef CM_ROW_ZSK
#undef CM_ZSKIP_HIDDEN
#define HH_CM_NEON 1
#else
static void sky_settle(void) {}
static void sky_texels_change(const void *px) { (void)px; }
#endif

static int sDecalDz SAVESTATE_EXCLUDE = -1; /* OOT_DECALDZ, see raster_tri */
static int sUiLoop SAVESTATE_EXCLUDE = 1;    /* raster_tri's CM 5 tint loops (2026-10-01); OOT_UILOOP=0: the generic loops */
static int sNearIn SAVESTATE_EXCLUDE = 1;    /* raster_tri's in-texture nearest spans (2026-10-01); OOT_NEARIN=0: wrapf always */
static int sUiNeon SAVESTATE_EXCLUDE = 1;    /* the UI tint span in NEON (ui_tint_n, 2026-10-01); OOT_UINEON=0: the scalar CM 5 loop */
/* OOT_PLD=1 (2026-10-02, FPS round 2; default off, a device trial): at each row of a triangle, a prefetch of the next
 * row's z and colour lines over this row's length. The device profile of the basement water view (23f4901f) has the
 * pixel loops' samples bunched right after the row's z load (the opaque lean row: 21 of its 202 samples on the
 * instruction after the ldrh) and the water's colour load: cache misses on the 230 KB colour and 115 KB z buffers,
 * which do not fit the A7's 256 KB L2 with the textures. A prefetch changes no data (PLD has no architectural effect
 * and never faults; the addresses stay inside the target: the next row is inside the triangle's box), so the pixels
 * are the same whatever it does to the time; the Bench view "..., prefetch ON" decides. */
static int sPld SAVESTATE_EXCLUDE;
static int sBlendNeon SAVESTATE_EXCLUDE = 1; /* gfx_soft_blend_textures's NEON loops (OOT_BLENDNEON, 2026-10-04) */
static int sTpeRows SAVESTATE_EXCLUDE = 1; /* tpe_eval_fast's row reads (OOT_TPEROWS, 2026-10-04) */
static int sTpeFast SAVESTATE_EXCLUDE = 1; /* gfx_soft_eval_texels's operand sources (OOT_TPEFAST, 2026-10-02; see tpe_eval_fast) */
static bool sDbgBilin SAVESTATE_EXCLUDE, sDbgDBilin SAVESTATE_EXCLUDE; static float sDbgScr SAVESTATE_EXCLUDE, sDbgUvc SAVESTATE_EXCLUDE; /* see OOT_PICK */

/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
/* OOT_OVERDRAW=1 (desktop census): per frame, the 3D pixels shaded (passed the depth test and
 * written) against the distinct pixels they land on, and how many were blends -- how much a
 * visible-only 3D could save. Logged every 60 frames. */
#ifndef HANDHELD
static uint8_t sOvd[320 * 240];
static int sOvdOn = -1;
static uint64_t sOvdShaded, sOvdBlend, sOvdDistinct, sOvdMulti, sOvdRej, sOvdRejBil;
static uint32_t sOvdFrames;
#define OVD_REJ(bil) do { if (sOvdOn > 0) { sOvdRej++; sOvdRejBil += (bil); } } while (0)
#define CEN_KEY(key) do { if (sOvdOn > 0) { sCenCur = cen_row(key); sCen[sCenCur].tris++; } } while (0)
#define CEN_SPAN(px) do { if (sOvdOn > 0) sCen[sCenCur].span += (px); } while (0)
#define OVD_NOTE(i, blend) do { if (sOvdOn > 0 && (unsigned)(i) < sizeof(sOvd)) { if (sOvd[i] < 255) sOvd[i]++; sOvdShaded++; sOvdBlend += (blend); sCen[sCenCur].shaded++; if ((int)(i) == sOvdPix) ovd_pix_note(); } } while (0)
/* ...and per kind of draw (the key below: texture size and format, detail layer, fog, blend, depth test/write,
 * flat, bilinear, alpha kind): triangles, span pixels (before the depth test) and pixels shaded, the top 16 logged
 * with the totals (round 69, the basement water room: who owns the pixels) */
typedef struct { uint32_t key; uint64_t tris, span, shaded; } CenRow;
static int sOvdPix = -1; /* OOT_OVDPIX=x,y: every shade of that pixel, frame and kind (the census's row key) */
static void ovd_pix_note(void);
static CenRow sCen[256];
static int sCenN = 1, sCenCur; /* row 0: anything past 255 kinds */
static void ovd_pix_note(void) {
    extern void port_log_always(const char *fmt, ...);
    const uint32_t k = sCen[sCenCur].key;
    port_log_always("OVDPIX f%u tex %dx%d det%d fog%d blend%d zw%d flat%d\n", sOvdFrames, (k >> 27) & 1 ? 4 << ((k >> 20) & 7) : 0,
                    (k >> 27) & 1 ? 4 << ((k >> 23) & 7) : 0, k & 1, (k >> 1) & 1, (k >> 2) & 1, (k >> 4) & 1, (k >> 5) & 1);
}
static int cen_row(uint32_t key) {
    int i;
    for (i = 1; i < sCenN; i++) if (sCen[i].key == key) return i;
    if (sCenN == 256) return 0;
    sCen[sCenN].key = key;
    return sCenN++;
}
static void ovd_frame(void) {
    if (sOvdOn < 0) {
        const char *e = getenv("OOT_OVERDRAW"); sOvdOn = e && e[0] == '1';
        if ((e = getenv("OOT_OVDPIX")) != NULL) { int x = 0, y = 0; if (sscanf(e, "%d,%d", &x, &y) == 2) sOvdPix = y * 240 + x; }
    }
    if (sOvdOn <= 0) return;
    for (size_t i = 0; i < sizeof(sOvd); i++) { if (sOvd[i]) { sOvdDistinct++; sOvdMulti += sOvd[i] > 1; } }
    { /* OOT_OVDMAP=<dir>: this frame's per-pixel shade counts as a PGM, every 60 frames (a heat map) */
        const char *dir = getenv("OOT_OVDMAP");
        if (dir && (sOvdFrames + 1) % 60 == 0) {
            char path[512]; FILE *f;
            snprintf(path, sizeof(path), "%s/ovd_%05u.pgm", dir, sOvdFrames + 1);
            if ((f = fopen(path, "wb")) != NULL) { fprintf(f, "P5\n%d %d\n255\n", sW, sH); for (int y = 0; y < sH; y++) fwrite(&sOvd[y * sW], 1, sW, f); fclose(f); }
        }
    }
    memset(sOvd, 0, sizeof(sOvd));
    if (++sOvdFrames % 60 == 0) {
        extern void port_log_always(const char *fmt, ...);
        port_log_always("OVD 60 frames: shaded %.1fk/frame, distinct %.1fk (%.2fx), blends %.1fk, pixels written 2+ times %.1fk\n",
                        sOvdShaded / 60e3, sOvdDistinct / 60e3, sOvdDistinct ? (double)sOvdShaded / sOvdDistinct : 0.0,
                        sOvdBlend / 60e3, sOvdMulti / 60e3);
        port_log_always("OVD alpha-rejected after shading: %.1fk/frame (%.1fk textured)\n", sOvdRej / 60e3, sOvdRejBil / 60e3);
        {
            int i, j, order[256];
            for (i = 0; i < sCenN; i++) order[i] = i;
            for (i = 0; i < sCenN; i++) for (j = i + 1; j < sCenN; j++) if (sCen[order[j]].shaded > sCen[order[i]].shaded) { int t = order[i]; order[i] = order[j]; order[j] = t; }
            for (i = 0; i < sCenN && i < 16; i++) {
                const CenRow *r = &sCen[order[i]];
                const uint32_t k = r->key;
                port_log_always("CEN %6.1fk shaded %6.1fk span %6.1f tris | tex %dx%d%s det%d fog%d blend%d zt%d zw%d flat%d bil%d\n",
                                r->shaded / 60e3, r->span / 60e3, r->tris / 60.0, (k >> 27) & 1 ? 4 << ((k >> 20) & 7) : 0,
                                (k >> 27) & 1 ? 4 << ((k >> 23) & 7) : 0, (k >> 26) & 1 ? " 16-bit" : "", k & 1, (k >> 1) & 1,
                                (k >> 2) & 1, (k >> 3) & 1, (k >> 4) & 1, (k >> 5) & 1, (k >> 6) & 1);
            }
            memset(sCen, 0, sizeof(sCen));
            sCenN = 1;
        }
        sOvdShaded = sOvdBlend = sOvdDistinct = sOvdMulti = sOvdRej = sOvdRejBil = 0;
    }
}
#else
#define OVD_NOTE(i, blend) ((void)0)
#define OVD_REJ(bil) ((void)0)
#define CEN_KEY(key) ((void)0)
#define CEN_SPAN(px) ((void)0)
static inline void ovd_frame(void) {}
#endif

/* round 69 (the Deku Tree basement water): gfx_pc.c flags a triangle the display list draws twice in a row (a
 * G_TRI2 whose halves are the same triangle; OOT_BLENDTWICE=0 there: both drawn); the triangle loop asks
 * raster_tri to do both draws in one pass, and raster_tri answers whether it did. */
const uint8_t *gfx_soft_tri_twice; /* per triangle of the batch being drawn, or NULL */
#if defined(HH_CM_NEON)
/* OOT_FASTROWS (2026-10-07, from SM64's SM64_FASTROWS; default 1, OOT_FASTROWS=0: raster_tri's own loop): the rows of a
 * triangle whose rows all go to a NEON row function (cm_rows) in a small function of its own. raster_tri keeps ~60
 * values live across its row loop and reloaded or spilled most of them every row (~20% of raster_tri's instructions
 * under qemu went there). Here, row by row, TRI_ROW_SETUP's text with the same float operations in the same order: the
 * edge bases; the edge solve with each edge's kind (w_i_dx > 0, < 0, == 0) taken once a triangle (a row outside an
 * edge with w_i_dx == 0 is skipped, as the solve's xs = xe; break did); the snap's three tests b_i + o * w_i_dx >= 0 in
 * three NEON lanes (vmla.f32: the product rounded, then the sum, as the VFP's multiply then add; -ffp-contract=off keeps
 * the VFP side unfused too); the row's start values (depth, shade, u/w, v/w, 1/w) as NEON lanes, w0 * x0 + w1 * x1 +
 * w2 * x2 as a multiply and two multiply-adds, each rounded as the VFP rounds them (flat: the vertex colour, as
 * TRI_ROW_SETUP); then the triangle's row function, picked once. NEON flushes denormals to zero where the VFP keeps
 * them: no value here comes near one (screen positions, edge values and their steps, depth, colours and colour/w,
 * u/w, v/w and 1/w are each 0 or far above 1e-30). Not for OOT_FILLRULE's owned-edge rows or OOT_PLD's prefetch
 * (raster_tri's loop keeps them). Same pixels. */
typedef void (*CmRowFn)(const CmTri *T, uint32_t *fb, uint16_t *zp, int n,
                        float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw);
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's { b, g, r, a } and { u/w, v/w, 1/w, z } */
    float32x4_t fa;                           /* flat: { b0, g0, r0, a0 } */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    int minx, maxx, sw, ek0, ek1, ek2;        /* ek: each edge's w_i_dx > 0 (0), < 0 (1), == 0 (2) */
    bool flat, persp;
    uint8_t *cov; uint32_t *fbuf; uint16_t *zb;
    CmRowFn fn; const CmTri *t;
} TriRows;
static uint32_t __attribute__((noinline)) tri_rows(const TriRows *R, int y, const int maxy) {
    const float e0c = R->e0c, e0x = R->e0x, e0y = R->e0y, e1c = R->e1c, e1x = R->e1x, e1y = R->e1y;
    const float e2c = R->e2c, e2x = R->e2x, e2y = R->e2y, inv_area = R->inv_area;
    const float w0dx = R->w0dx, w1dx = R->w1dx, w2dx = R->w2dx;
    const int minx = R->minx, maxx = R->maxx, sW = R->sw, ek0 = R->ek0, ek1 = R->ek1, ek2 = R->ek2;
    const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };
    uint32_t px = 0;
    for (; y < maxy; y++) {
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
#define TRI_EDGE(EK, BS, DS)                                                                            \
        if (EK == 0) {                                                                                  \
            if (BS < 0.0f) {                                                                            \
                float q = -(BS) / (DS) - 1e-5f; if (q > 1e6f) q = 1e6f;                                 \
                const int a = minx + iceilf(q);                                                         \
                if (a > xs) xs = a;                                                                     \
            }                                                                                           \
        } else if (EK == 1) {                                                                           \
            float q = -(BS) / (DS) + 1e-5f; if (q > 1e6f) q = 1e6f;                                     \
            const int a = minx + 1 + ifloorf(q);                                                        \
            if (a < xe) xe = a;                                                                         \
        } else if (BS < 0.0f) continue; /* the row outside this edge (TRI_ROW_SETUP: xs = xe) */
        TRI_EDGE(ek0, b0, w0dx)
        TRI_EDGE(ek1, b1, w1dx)
        TRI_EDGE(ek2, b2, w2dx)
#undef TRI_EDGE
        if (xs >= xe) continue;
        const float32x4_t B3 = { b0, b1, b2, 0.0f };
        float32x4_t W;
        for (;;) { /* the snap: the first pixel with b_i + o * w_i_dx >= 0 for all three */
            W = vmlaq_n_f32(B3, D3, (float)(xs - minx));
            const uint32x4_t ge = vcgeq_f32(W, vdupq_n_f32(0.0f));
            const uint32x2_t g2 = vpmin_u32(vget_low_u32(ge), vget_high_u32(ge));
            if (vget_lane_u32(vpmin_u32(g2, g2), 0)) break;
            if (++xs >= xe) break;
        }
        if (xs >= xe) continue;
        const int n = xe - xs, idx = y * sW + xs;
        px += (uint32_t)n; /* fill/overdraw proxy (before z-reject), as TRI_ROW_SETUP */
        if (R->cov) memset(R->cov + idx, 1, (size_t)n);
        /* w0 * x0 + w1 * x1 + w2 * x2 for all eight (W: w0, w1, w2 at xs, as TRI_ROW_SETUP makes them) */
        const float32x2_t wl = vget_low_f32(W), wh = vget_high_f32(W);
        float32x4_t A = vmulq_lane_f32(R->va0, wl, 0), B = vmulq_lane_f32(R->vb0, wl, 0);
        A = vmlaq_lane_f32(A, R->va1, wl, 1); B = vmlaq_lane_f32(B, R->vb1, wl, 1);
        A = vmlaq_lane_f32(A, R->va2, wh, 0); B = vmlaq_lane_f32(B, R->vb2, wh, 0);
        if (R->flat) A = R->fa;
        const float sw = vgetq_lane_f32(B, 2);
        const float rw = R->persp ? ((sw > 1e-12f) ? 1.0f / sw : 0.0f) : 0.0f;
        R->fn(R->t, R->fbuf + idx, R->zb + idx, n, vgetq_lane_f32(B, 3), vgetq_lane_f32(A, 2), vgetq_lane_f32(A, 1),
              vgetq_lane_f32(A, 0), vgetq_lane_f32(A, 3), vgetq_lane_f32(B, 0), vgetq_lane_f32(B, 1), sw, rw);
    }
    return px;
}
#endif
static int sTwiceReq SAVESTATE_EXCLUDE, sTwiceDone SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* The second core's share of raster_tri (see split_row): raster_rows is raster_tri's own row loop
 * (gfx_soft_rows.inc.c) over a copy of the locals it reads, so both shares run the same text over the same values.
 * Exact by construction: TRI_ROW_SETUP derives every row's edge bases and interpolants from y alone (no row-to-row
 * accumulation; per row the same operations in the same order), the shares write disjoint rows of colour, depth,
 * the crack-fill coverage and the deferred sky's mask, everything they read is fixed before the handoff, and the
 * per-triangle pixel count is summed after it. The edge notes (OOT_EDGETINT's ordered list) of the worker's share go
 * to their own list, appended in row order after the wait (edge_note). UI triangles (the HUD's small quads, the box
 * filter's spans) are not split. OOT_THREADS=1 (debug.cfg threads=1): one core, for A/B. */
typedef struct {
    float a0, a1, a2, bl0, bl1, bl2, r0, r1, r2, g0, g1, g2, z0, z1, z2, iw0, iw1, iw2, uw0, uw1, uw2, vw0, vw1,
        vw2, cadx, cbdx, cgdx, crdx, e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, swdx, uwdx, vwdx,
        w0dx, w1dx, w2dx, zdx;
    int32_t adq_t, bdq_t, gdq_t, rdq_t, zdq;
    int amin, ablend, fga, fgb, fgg, fgr, fsa, fsb, fsg, fsr, lkb, lkg, lkr, maxx, minx, th, tw, ws, wt,
        tmask_u, tmask_v, zbias;
    uint32_t clr, cmask;
    bool a_in, a_tex, cm_mod, cm_rows, dbilin, dmask, enote, flat, fogtri, own, persp, pshade, rgb_in, rgb_tex,
        rgb_texa, t16, texadd, texlerp, texlerpv, use_bilin, ztest;
    const SoftTex *tex;
    const SoftTex *dtex;
    const void *texpx;
    uint32_t *fbuf;
    uint16_t *zb;
    uint8_t *cov;
#if defined(HH_CM_NEON)
    CmTri cmt;
#endif
    OwnTri oe;
} RasterRows;
static uint32_t __attribute__((noinline)) raster_rows(const RasterRows *RR, int miny, int maxy) {
    const float a0 = RR->a0, a1 = RR->a1, a2 = RR->a2, bl0 = RR->bl0, bl1 = RR->bl1, bl2 = RR->bl2, r0 = RR->r0,
        r1 = RR->r1, r2 = RR->r2, g0 = RR->g0, g1 = RR->g1, g2 = RR->g2, z0 = RR->z0, z1 = RR->z1, z2 = RR->z2,
        iw0 = RR->iw0, iw1 = RR->iw1, iw2 = RR->iw2, uw0 = RR->uw0, uw1 = RR->uw1, uw2 = RR->uw2, vw0 = RR->vw0,
        vw1 = RR->vw1, vw2 = RR->vw2, cadx = RR->cadx, cbdx = RR->cbdx, cgdx = RR->cgdx, crdx = RR->crdx,
        e0c = RR->e0c, e0x = RR->e0x, e0y = RR->e0y, e1c = RR->e1c, e1x = RR->e1x, e1y = RR->e1y, e2c = RR->e2c,
        e2x = RR->e2x, e2y = RR->e2y, inv_area = RR->inv_area, swdx = RR->swdx, uwdx = RR->uwdx,
        vwdx = RR->vwdx, w0dx = RR->w0dx, w1dx = RR->w1dx, w2dx = RR->w2dx, zdx = RR->zdx;
    const int32_t adq_t = RR->adq_t, bdq_t = RR->bdq_t, gdq_t = RR->gdq_t, rdq_t = RR->rdq_t, zdq = RR->zdq;
    const int amin = RR->amin, ablend = RR->ablend, fga = RR->fga, fgb = RR->fgb, fgg = RR->fgg, fgr = RR->fgr,
        fsa = RR->fsa, fsb = RR->fsb, fsg = RR->fsg, fsr = RR->fsr, lkb = RR->lkb, lkg = RR->lkg, lkr = RR->lkr,
        maxx = RR->maxx, minx = RR->minx, th = RR->th, tw = RR->tw, ws = RR->ws, wt = RR->wt,
        tmask_u = RR->tmask_u, tmask_v = RR->tmask_v, zbias = RR->zbias;
    const uint32_t clr = RR->clr, cmask = RR->cmask;
    const bool a_in = RR->a_in, a_tex = RR->a_tex, cm_mod = RR->cm_mod, cm_rows = RR->cm_rows,
        dbilin = RR->dbilin, dmask = RR->dmask, enote = RR->enote, flat = RR->flat, fogtri = RR->fogtri,
        own = RR->own, persp = RR->persp, pshade = RR->pshade, rgb_in = RR->rgb_in, rgb_tex = RR->rgb_tex,
        rgb_texa = RR->rgb_texa, t16 = RR->t16, texadd = RR->texadd, texlerp = RR->texlerp,
        texlerpv = RR->texlerpv, use_bilin = RR->use_bilin, ztest = RR->ztest;
    const SoftTex * const tex = RR->tex;
    const SoftTex * const dtex = RR->dtex;
    const void * const texpx = RR->texpx;
    uint32_t *const fbuf = RR->fbuf;
    uint16_t *const zb = RR->zb;
    uint8_t *const cov = RR->cov;
#if defined(HH_CM_NEON)
    CmTri cmt = RR->cmt; /* not const: OOT_ZSKIP's miss counter (speed only) counts in each share's copy */
#endif
    const OwnTri oe = RR->oe;
    uint32_t tri_px = 0;
#include "gfx_soft_rows.inc.c"
    return tri_px;
}
#define RASTER_ROWS_PACK(RR) do { \
        (RR).a0 = a0; (RR).a1 = a1; (RR).a2 = a2; (RR).bl0 = bl0; (RR).bl1 = bl1; (RR).bl2 = bl2; \
        (RR).r0 = r0; (RR).r1 = r1; (RR).r2 = r2; (RR).g0 = g0; (RR).g1 = g1; (RR).g2 = g2; (RR).z0 = z0; \
        (RR).z1 = z1; (RR).z2 = z2; (RR).iw0 = iw0; (RR).iw1 = iw1; (RR).iw2 = iw2; (RR).uw0 = uw0; \
        (RR).uw1 = uw1; (RR).uw2 = uw2; (RR).vw0 = vw0; (RR).vw1 = vw1; (RR).vw2 = vw2; (RR).cadx = cadx; \
        (RR).cbdx = cbdx; (RR).cgdx = cgdx; (RR).crdx = crdx; (RR).e0c = e0c; (RR).e0x = e0x; (RR).e0y = e0y; \
        (RR).e1c = e1c; (RR).e1x = e1x; (RR).e1y = e1y; (RR).e2c = e2c; (RR).e2x = e2x; (RR).e2y = e2y; \
        (RR).inv_area = inv_area; (RR).swdx = swdx; (RR).uwdx = uwdx; (RR).vwdx = vwdx; (RR).w0dx = w0dx; \
        (RR).w1dx = w1dx; (RR).w2dx = w2dx; (RR).zdx = zdx; (RR).adq_t = adq_t; (RR).bdq_t = bdq_t; \
        (RR).gdq_t = gdq_t; (RR).rdq_t = rdq_t; (RR).zdq = zdq; (RR).amin = amin; (RR).ablend = ablend; \
        (RR).fga = fga; (RR).fgb = fgb; (RR).fgg = fgg; (RR).fgr = fgr; (RR).fsa = fsa; (RR).fsb = fsb; \
        (RR).fsg = fsg; (RR).fsr = fsr; (RR).lkb = lkb; (RR).lkg = lkg; (RR).lkr = lkr; (RR).maxx = maxx; \
        (RR).minx = minx; (RR).th = th; (RR).tw = tw; (RR).ws = ws; (RR).wt = wt; (RR).tmask_u = tmask_u; \
        (RR).tmask_v = tmask_v; (RR).zbias = zbias; (RR).clr = clr; (RR).cmask = cmask; (RR).a_in = a_in; \
        (RR).a_tex = a_tex; (RR).cm_mod = cm_mod; (RR).cm_rows = cm_rows; (RR).dbilin = dbilin; \
        (RR).dmask = dmask; (RR).enote = enote; (RR).flat = flat; (RR).fogtri = fogtri; (RR).own = own; \
        (RR).persp = persp; (RR).pshade = pshade; (RR).rgb_in = rgb_in; (RR).rgb_tex = rgb_tex; \
        (RR).rgb_texa = rgb_texa; (RR).t16 = t16; (RR).texadd = texadd; (RR).texlerp = texlerp; \
        (RR).texlerpv = texlerpv; (RR).use_bilin = use_bilin; (RR).ztest = ztest; (RR).tex = tex; \
        (RR).dtex = dtex; (RR).texpx = texpx; (RR).fbuf = fbuf; (RR).zb = zb; (RR).cov = cov; (RR).oe = oe; \
        RASTER_ROWS_PACK_CMT(RR) \
    } while (0)
#if defined(HH_CM_NEON)
#define RASTER_ROWS_PACK_CMT(RR) (RR).cmt = cmt;
#else
#define RASTER_ROWS_PACK_CMT(RR)
#endif
typedef struct { const RasterRows *rr; int y0, y1; uint32_t px; } RowJob;
static void row_job(void *arg) {
    RowJob *j = (RowJob *)arg;
    if (j->rr->enote) { sEnWk.n = 0; tEnShare = &sEnWk; } /* its edge notes apart (edge_note), appended after the wait */
    j->px = raster_rows(j->rr, j->y0, j->y1);
    tEnShare = NULL;
}
static uint32_t rows_split(const RasterRows *R, int miny, int maxy) {
    const int ym = sUiMode ? -1
                 : split_row(R->e0c, R->e0x, R->e0y, R->e1c, R->e1x, R->e1y, R->e2c, R->e2x, R->e2y, R->inv_area,
                             R->w0dx, R->w1dx, R->w2dx, R->minx, R->maxx, miny, maxy);
    if (ym < 0) return raster_rows(R, miny, maxy);
    RowJob j = { R, ym, maxy, 0 };
    miyoo_worker_run(row_job, &j);
    const uint32_t px = raster_rows(R, miny, ym);
    miyoo_worker_wait();
    if (R->enote) /* the lower rows' notes after the upper rows' (which this thread noted in place), up to EN_MAX */
        for (int i = 0; i < sEnWk.n && sEnN < EN_MAX; i++) { sEnIdx[sEnN] = sEnWk.idx[i]; sEnZ[sEnN] = sEnWk.z[i]; sEnA[sEnN++] = sEnWk.a[i]; }
    gfx_soft_split_px += px + j.px;
    return px + j.px;
}
#if defined(HH_CM_NEON)
/* OOT_FASTROWS's tri_rows in the same two shares (merged from the handheld main, v1.0.2): tri_rows also derives every row
 * from y alone, so a share starting mid-triangle draws the rows the whole call would. The worker's share gets its own
 * copy of the CmTri, whose OOT_ZSKIP miss counter only decides when to test (never a pixel). */
typedef struct { TriRows R; CmTri t; int y0, y1; uint32_t px; } TriJob;
static void tri_job(void *arg) {
    TriJob *j = (TriJob *)arg;
    if (j->t.enote) { sEnWk.n = 0; tEnShare = &sEnWk; }
    j->R.t = &j->t;
    j->px = tri_rows(&j->R, j->y0, j->y1);
    tEnShare = NULL;
}
static uint32_t tri_rows_split(const TriRows *R, int miny, int maxy) {
    const int ym = sUiMode ? -1
                 : split_row(R->e0c, R->e0x, R->e0y, R->e1c, R->e1x, R->e1y, R->e2c, R->e2x, R->e2y, R->inv_area,
                             R->w0dx, R->w1dx, R->w2dx, R->minx, R->maxx, miny, maxy);
    if (ym < 0) return tri_rows(R, miny, maxy);
    TriJob j;
    j.R = *R; j.t = *R->t; j.y0 = ym; j.y1 = maxy; j.px = 0;
    miyoo_worker_run(tri_job, &j);
    const uint32_t px = tri_rows(R, miny, ym);
    miyoo_worker_wait();
    if (R->t->enote)
        for (int i = 0; i < sEnWk.n && sEnN < EN_MAX; i++) { sEnIdx[sEnN] = sEnWk.idx[i]; sEnZ[sEnN] = sEnWk.z[i]; sEnA[sEnN++] = sEnWk.a[i]; }
    gfx_soft_split_px += px + j.px;
    return px + j.px;
}
#endif
#endif /* MIYOO */
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
    /* Barycentrics = edge/area with SIGNED area: inside is all-w >= 0 for
     * either winding, no separate sign handling needed. (Divide sits after
     * the bbox reject so off-screen triangles don't pay for it.) */
    const float inv_area = 1.0f / area;

    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) {
        /* A textured draw without a texture comes out as a flat block of the combiner's colour (the
         * v35 sunset bar). Said on the console, a few times a session, so a device run catches it. */
        static int sUntexLogs SAVESTATE_EXCLUDE;
        if (sUntexLogs < 4) {
            sUntexLogs++;
            port_log("gfx: untextured draw: tex %u not valid (bound %u/%u, arena %d%%)\n", (unsigned)sBoundTex[0],
                     (unsigned)sBoundTex[0], (unsigned)sBoundTex[1], (int)(sArenaUsed * 100 / TEXARENA_SZ));
        }
        tex = NULL;
    }
    const int tmask_u = tex ? pow2mask(tex->w) : -1, tmask_v = tex ? pow2mask(tex->h) : -1;
    const bool tex_edge = sUseAlpha && sCurShader && sCurShader->cc.opt_texture_edge;
    const bool ztest = use_z && sDepthTest;
    /* (Off by default since v26: gfx_soft_magbilin = 0, every textured pixel bilinear -- the
     * per-triangle switch made floors flash.) Magnified-only bilinear (device round 12, fixed round 13): the 4-tap
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
    if (sHudTri < 0) { const char *e = getenv("OOT_HUDTRI"); sHudTri = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : 0; }
    bool use_bilin = gfx_soft_bilinear && tex && (!sUiMode || sHudTri == 1 || (sHudTri == 2 && tex->linear)); /* HUD/menus: not bilinear -- F-Zero X's rule, not MK64's (MK64 smooths HUD triangles since the user's A/B); OoT keeps it until its own A/B */
    /* The detail layer (sDetail): tile 1 per pixel, bilinear by its own texel size (the
     * lead: when a tile-1 texel covers about a pixel or more, nearest once clearly minified). */
    const SoftTex *dtex = (sDetail.id != 0 && tex) ? &sTex[sDetail.id] : NULL;
    /* OOT_WATERGRAIN=0 (the user's "try it, show me", round 59): the Deku Tree's water surface without its
     * detail layer (the grain) -- the 32x32 layer drawn fogged at constant alpha 127, a quarter of the
     * basement water room's pixels. Default on = the game's look. */
    /* OOT_FASTWATER (debug.cfg fastwater=1 turns it on; round 73, the user's "do it and show me"; ON by default from
     * the release calls, 2026-09-30, OFF again since 2026-10-01: the user picked the exact water + the lean rows on the
     * device, "this wins, runs and looks great"): the
     * basement's water is two layers each drawn twice (the grain layer, then the plain one); after those four
     * half blends the plain layer is ~75% of a pixel, the grain layer ~19%, the floor ~6%. Here the grain layer is
     * skipped and the plain layer blended four times: the floor shows as much, the grain layer's colour is gone,
     * and the room's dearest pixels are not drawn. Only the doubled water (sTwiceReq) of the Deku Tree. */
    const bool fastw = sTwiceReq && gfx_soft_fast_water() && gHhSceneId == 0 && tex && tex->w == 32 && tex->h == 32 &&
                       sFogPx && sFogA == 127;
    if (fastw && dtex) { sTwiceDone = 1; return -1; }
    const bool fastw_top = fastw;
    if (dtex && !gfx_soft_water_grain() && gHhSceneId == 0 && tex->w == 32 && tex->h == 32 && sFogPx && sFogA == 127) dtex = NULL;
    if (dtex && (!dtex->valid || pow2mask(dtex->w) < 0 || pow2mask(dtex->h) < 0)) dtex = NULL;
    bool dbilin = use_bilin && dtex != NULL;
    sDbgScr = sDbgUvc = -1.0f;
    if (use_bilin && gfx_soft_magbilin) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
        dbilin = dbilin && scr * 2.0f >= uvc * sDetail.area;
        sDbgScr = scr; sDbgUvc = uvc;
    }
    sDbgBilin = use_bilin; sDbgDBilin = dbilin;
    const int amin = tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
    /* 0: past amin, a TEX_EDGE pixel over nothing yet (its depth still clear, its colour still the frame's
     * clear colour -- not the sky, which writes no depth) takes the texel's own
     * colour. Everywhere else the old blend stays: the N64's VI anti-aliases coverage edges and the port has
     * no VI AA, so the blend is the better stand-in over drawn geometry (z64 at the same instant: Kokiri
     * shop, graveyard, Ganondorf's room closer blended; PM64 found the same). Over the clear it isn't: the
     * rim takes the fog-coloured clear, and the wall drawn later fails the depth test (the tunnel roots). */
    const int ablend = !(tex_edge && sEdgeOpaque && ztest);
    /* OOT_EDGETINT: such a pixel is written unblended over any pixel no depth was written to yet -- the sky's as well as
     * the clear's (the colour test masked off) -- and noted for edge_vi */
    const bool enote = !ablend && sEdgeTint && sEnMark != NULL && sEnMarkSz >= sW * sH;
    const uint32_t cmask = enote ? 0u : 0xFFFFFFu;
    const uint32_t clr = enote ? 0u : (sUnified ? sNatFill : 0u) & 0xFFFFFFu; /* the internal target clears to 0 */
    if (enote && sEnFB != sFB) { sEnN = 0; sEnFB = sFB; } /* (one target at a time; a switch drops the notes) */
    const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
    const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
    const bool rgb_texa = sCurShader && sCurShader->rgb_texa;
    const void *texpx = tex ? tex->px : NULL; const bool t16 = tex && tex->is16;
    /* Flat shade (one vertex colour) skips the per-pixel colour stepping. */
    /* Round 74 (the user: fog "flashing" like GoldenEye's Surface 2 snow): where the shade alpha carries the RSP fog
     * factor (sFogPx), the factor comes from each vertex's depth unclamped, z_ndc * mul + offset (gfx_pc.c passes
     * the two), and the shade interpolation plus the per-pixel 0..255 saturation make it per-pixel fog from depth.
     * z/w is linear on screen, so this is the true per-pixel value; the N64 clamps per vertex first, and on big
     * triangles whose vertices overshoot 0..255 that bends the fog inside them, and it shifts as the camera moves.
     * OOT_FOGZ=1 turns it on (off by default: it did not change the user's flashing floor; BUILDLOG round 74). */
    float fa0 = v0->a, fa1 = v1->a, fa2 = v2->a;
    if (sFogPx && sFogZ) {
        fa0 = v0->z * sFogZA + sFogZB; fa1 = v1->z * sFogZA + sFogZB; fa2 = v2->z * sFogZA + sFogZB;
        fa0 = fa0 < -16000.0f ? -16000.0f : fa0 > 16000.0f ? 16000.0f : fa0;
        fa1 = fa1 < -16000.0f ? -16000.0f : fa1 > 16000.0f ? 16000.0f : fa1;
        fa2 = fa2 < -16000.0f ? -16000.0f : fa2 > 16000.0f ? 16000.0f : fa2;
    }
    const bool flat = v0->r == v1->r && v1->r == v2->r && v0->g == v1->g && v1->g == v2->g &&
                      v0->b == v1->b && v1->b == v2->b && fa0 == fa1 && fa1 == fa2;

    /* Vertex fields as locals (device round 13): the previous row's framebuffer
     * stores are an aliasing barrier under -fno-strict-aliasing, so GCC was
     * reloading -- and ARM->VFP transferring, ~10 stall cycles each -- all 18 of
     * these on EVERY scanline (12 ldrb + 12 vmov + 12 vcvt in the y-loop).
     * uint8->float and float copies are exact, so every use is bit-identical. */
    /* OOT_PSHADE (X49, from GoldenEye f5fa168; the user's Deku floor "changing shape" as the view turns):
     * the RDP steps the vertex shade linearly on screen, so a big near floor triangle's baked shading slides
     * as the view turns and as the clipper recuts it. Textured depth-tested triangles here step colour/w
     * and 1/w like their u/v: exact at every 16-pixel span end, linear between. Not the RDP's rule (angrylion
     * keeps the screen-linear shade); the shading stays put on the surface. The no-depth sky and the 2D keep
     * the RDP's. OOT_PSHADE=0: the RDP's rule. */
    static int sPShade SAVESTATE_EXCLUDE = -1;
    if (sPShade < 0) { const char *e = getenv("OOT_PSHADE"); sPShade = !(e && e[0] == '0'); }
    const bool pshade = sPShade && tex && ztest && !sUiMode && !flat &&
                        v0->invw > 0.0f && v1->invw > 0.0f && v2->invw > 0.0f;
    const float pm0 = pshade ? v0->invw : 1.0f, pm1 = pshade ? v1->invw : 1.0f, pm2 = pshade ? v2->invw : 1.0f;
    const float z0 = v0->z, z1 = v1->z, z2 = v2->z;
    const float r0 = v0->r * pm0, r1 = v1->r * pm1, r2 = v2->r * pm2, g0 = v0->g * pm0, g1 = v1->g * pm1, g2 = v2->g * pm2;
    const float bl0 = v0->b * pm0, bl1 = v1->b * pm1, bl2 = v2->b * pm2, a0 = fa0 * pm0, a1 = fa1 * pm1, a2 = fa2 * pm2;
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
    const int32_t zdq = span_zdq(zdx); /* +-1 a pixel, +-6 with OOT_NEARW */
    const int32_t rdq_t = sat_i32(crdx * 65536.0f), gdq_t = sat_i32(cgdx * 65536.0f), bdq_t = sat_i32(cbdx * 65536.0f), adq_t = sat_i32(cadx * 65536.0f);

    /* Flat triangles: the shade colour the pixel loop would clamp every pixel
     * (rq = (int32_t)(cr * 65536.0f) with cr == r0 on every row and span, since
     * every colour step is 0), clamped once. (MK64 perf round 24.) */
    const int fsr = clamp8_sh16((int32_t)(r0 * 65536.0f)), fsg = clamp8_sh16((int32_t)(g0 * 65536.0f));
    const int fsb = clamp8_sh16((int32_t)(bl0 * 65536.0f)), fsa = clamp8_sh16((int32_t)(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    /* ZMODE_DEC: the RDP passes a decal pixel when |z - stored z| <= 8 x the larger per-pixel z slope
     * (|dz/dx| + |dz/dy|, the triangle's or the one stored with the pixel) rounded down to a power of
     * two, in its 18-bit z (angrylion's z_compare: deltaz_comparator_lut, dznew <<= 3). A flat 4 lost
     * decals on slopes seen edge-on: the dirt path over Kokiri Forest's grass slopes clipped in and out
     * in Navi's flight (the user, v25). Here: the triangle's slope, our 16-bit z being a quarter of the
     * RDP's, never below the old 4. OOT_DECALDZ=0: the flat 4 (A/B). */
    int zbias = 0;
    if (sZDecal) {
        zbias = 4;
        if (sDecalDz < 0) { const char *e = getenv("OOT_DECALDZ"); sDecalDz = !(e && e[0] == '0'); }
        if (sDecalDz) {
            const float zdy = ((v1->x - v2->x) * z0 + (v2->x - v0->x) * z1 + (v0->x - v1->x) * z2) * inv_area;
            const float dz18 = ((zdx < 0.0f ? -zdx : zdx) + (zdy < 0.0f ? -zdy : zdy)) * (65535.0f * 4.0f);
            int p2 = 1;
            while (p2 < 0x10000 && (float)(p2 * 2) <= dz18) p2 *= 2;
            if (p2 * 2 > zbias) zbias = p2 * 2; /* 8 x p2 in 18-bit z = 2 x p2 in ours */
        }
    }
    const bool dmask = sDepthMask, texadd = sTexAdd;
    const bool texlerp = sTexLerp && !sTexLerpV; const int lkr = sLerpR, lkg = sLerpG, lkb = sLerpB;
    const bool texlerpv = sTexLerp && sTexLerpV; /* OOT_SHADELERP: the scalar loops only (no NEON row takes it) */
    const bool cm_mod = rgb_tex && rgb_in && !texlerp && !texlerpv && !texadd && !rgb_texa; /* texture x shade: see SPAN_LOOP_CM */
    /* fogged per pixel: only triangles with some fog take the fog loops (CM 3 / 4; fog sits far off),
     * and every triangle whose constant combiner alpha the factor displaced (fga): elsewhere the
     * pixel alpha would be the vertex alpha, the factor, 0 near the camera -- invisible (a gap in
     * MK64 7d23507's reference: its constant-alpha class never ran there) */
    const bool fogtri = sFogPx && (sFogA >= 0 || (v0->a | v1->a | v2->a) != 0);
    const int fgr = sFogR, fgg = sFogG, fgb = sFogB, fga = sFogA;
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
#if defined(HH_CM_NEON)
    /* The NEON rows (cm_row_neon): z-tested texture x shade, and texture-only
     * opaque spans without z (the sky), wherever the HUD box filter cannot
     * apply (that is for nearest UI spans only). */
    const bool texonly = rgb_tex && !rgb_in && !rgb_texa; /* rgb from the texel alone: the sky */
    const bool dneon_rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0;
    /* OOT_DETCLAMP (2026-10-04): the detail layer over a tile 0 clamped on an axis or odd-sized (Zora's Domain's walls
     * and floor: 64x32, repeating in s, clamped in t -- 45% of that view's frame in the scalar loops), bilinear on both
     * tiles, in cm_row_leana's rows: tile 0 by wrapf's taps (CM_LBILING, as its rows without the layer), tile 1 as every
     * lean row takes it. Not a doubled triangle's blend-twice draw nor fastwater's (cmt.extra / tinta: cm_row_leana
     * takes neither). OOT_DETCLAMP=0: the scalar loops, as before. */
    const bool dneon_g = !dneon_rr && sDetClamp && sAlphaLean && use_bilin && dbilin && !(sTwiceReq && sUseAlpha && !dmask) && !fastw_top;
    const bool dneon = dtex && ztest && cm_mod && !t16 && !dtex->is16 && (dneon_rr || dneon_g);
    /* A fogged triangle whose constant combiner alpha the factor displaced (fga >= 0) takes the rows
     * too, with that alpha (amode 3): the Deku Tree basement's water, detail layer and all, drew
     * every pixel in the scalar loops (61% of the frame under qemu). OOT_CMFGA=0: scalar, as before. */
    static int sCmFga SAVESTATE_EXCLUDE = -1;
    if (sCmFga < 0) { const char *e = getenv("OOT_CMFGA"); sCmFga = !(e && e[0] == '0'); }
    /* OOT_LERPLEAN: the z-tested bilinear (A - K) * TEXEL0 + K rows (the scalar loops' texlerp branch, fogged or not) in
     * cm_row_leanl; not a doubled triangle's blend-twice draw (cmt.extra below: the scalar loops draw the copy) */
    /* OOT_DETLERP (2026-10-04): with the detail layer too, on a 32-bit tile 0, tile 1 bilinear (the Song of Storms'
     * cloud cover in Kakariko and the field: ~33,000 pixels a frame of a two-tile tint, blended, in the scalar loops) */
    const bool lerp_rows = sLerpLean && sCmNeon && tex && ztest && use_bilin &&
                           (!dtex || (sDetLerp && dbilin && !t16 && !dtex->is16)) && texlerp && rgb_tex && rgb_in &&
                           !rgb_texa && !(sTwiceReq && sUseAlpha && !dmask) &&
                           (t16 || sBil64 != 0 || !(ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0));
    /* OOT_NOZLEAN (2026-10-04): blended texture x shade without a z test, bilinear, not fogged, no detail layer
     * (cm_row_leanz); a blend-twice copy (cmt.extra) never comes without z */
    const bool noz_rows = sNoZLean && sAlphaLean && sCmNeon && tex && !ztest && cm_mod && sUseAlpha && use_bilin && !dtex && !fogtri &&
                          (t16 || sBil64 != 0 || !(ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0));
    const bool cm_rows = lerp_rows || noz_rows || (sCmNeon && tex && (!fogtri || (ztest && cm_mod && (fga < 0 || sCmFga))) &&
                         (use_bilin || !sUiMode || gfx_soft_hud_box <= 0) &&
                         (dtex ? dneon : ((ztest && cm_mod) || (!ztest && texonly && !sUseAlpha))));
    CmTri cmt;
    if (cm_rows) {
        const int32_t d[4] = { bdq_t, gdq_t, rdq_t, adq_t };
        cmt.px = texpx; cmt.w = tw; cmt.h = th; cmt.ws = ws; cmt.wt = wt; cmt.mu = tmask_u; cmt.mv = tmask_v; cmt.t16 = t16;
        cmt.swdx = swdx; cmt.uwdx = uwdx; cmt.vwdx = vwdx; cmt.zdx = zdx;
        cmt.crdx = crdx; cmt.cgdx = cgdx; cmt.cbdx = cbdx; cmt.cadx = cadx;
        cmt.ftw = (float)tw; cmt.fth = (float)th;
        cmt.dacc = vld1q_s32(d); cmt.zdq = zdq; cmt.zbias = zbias; cmt.pshade = pshade;
        cmt.zskip = sZSkip && ztest && !noz_rows ? 4 : 0; /* OOT_ZSKIP: the rows with a depth test */
        cmt.amode = !a_tex ? 2 : !a_in ? 1 : 0; /* fa = !a_tex ? sa : !a_in ? ta : div255(ta*sa) */
        cmt.fgac = fga;
        if (fogtri && fga >= 0) cmt.amode = a_tex ? 4 : 3; /* fa = fga, times the texel alpha if a_tex (SPAN_LOOP_CM, CM >= 3) */
        cmt.cmask = cmask; cmt.enote = enote;
        cmt.amin = amin; cmt.ablend = ablend; cmt.clr = clr; cmt.persp = persp; cmt.dmask = dmask; cmt.alpha = sUseAlpha; cmt.bilin = use_bilin;
        cmt.texonly = !ztest; /* cm_rows without z: the texture-only case */
        /* round 69: a blended triangle without z writes, drawn twice in a row (the Deku Tree basement's water:
         * each G_TRI2 holds the same triangle twice) -- the second draw passes the same z and alpha tests and
         * shades the same source, so only its blend differs: done in the same pixel step (sTwiceDone tells
         * the caller to skip the copy). Anything else draws the copy as before. */
        /* OOT_FWBLENDS (debug.cfg fwblends=, 2..4): fastwater's blends of the top layer; each covers about half of
         * what is under it, so 4 leaves ~6% of the floor seen through the water, 3 ~12%, 2 ~25% (the user: "we
         * just need to see through it a bit"). */
        static int sFwBlends SAVESTATE_EXCLUDE = -1;
        if (sFwBlends < 0) { const char *e = getenv("OOT_FWBLENDS"); sFwBlends = e ? atoi(e) : 2; if (sFwBlends < 2) sFwBlends = 2; if (sFwBlends > 4) sFwBlends = 4; }
        cmt.extra = (sTwiceReq && sUseAlpha && ztest && !dmask) ? (fastw_top ? sFwBlends - 1 : 1) : 0;
        if (cmt.extra) sTwiceDone = 1;
        /* The fastwater tint (round 80, the user: the water "almost invisible", "a simple water transparency
         * colour could sell the effect"): with the grain layer skipped, the plain layer shows the floor through
         * its light streaks; each pixel is then pulled toward the plain texture's mean colour, shaded and fogged
         * like the pixel, by OOT_FWTINT/255 (debug.cfg fwtint=; 0 = off). One multiply per water pixel. */
        cmt.tinta = 0;
        if (fastw_top && cmt.extra && !t16) {
            static int sFwTint SAVESTATE_EXCLUDE = -1;
            if (sFwTint < 0) { const char *e = getenv("OOT_FWTINT"); sFwTint = e ? atoi(e) : 64; if (sFwTint < 0) sFwTint = 0; if (sFwTint > 255) sFwTint = 255; }
            if (sFwTint) {
                static const void *sAvgPx SAVESTATE_EXCLUDE; static uint16x4_t sAvg SAVESTATE_EXCLUDE;
                if (sAvgPx != texpx) {
                    uint32_t sb = 0, sg = 0, sr = 0, sa = 0; const int n = tw * th;
                    for (int i = 0; i < n; i++) { const uint32_t c = ((const uint32_t *)texpx)[i];
                        sb += c & 0xFF; sg += (c >> 8) & 0xFF; sr += (c >> 16) & 0xFF; sa += c >> 24; }
                    sAvg = (uint16x4_t){ (uint16_t)(sb / n), (uint16_t)(sg / n), (uint16_t)(sr / n), (uint16_t)(sa / n) };
                    sAvgPx = texpx;
                }
                /* OOT_FWGLASS (debug.cfg fwglass=, default 1; the user: "can the tint be transparent"): a teal glass
                 * -- the pixel multiplied by the texture's mean colour scaled so its brightest channel is 255, so the
                 * floor and the ripples stay seen through it. fwglass=0: the solid tint above. */
                static int sGlass SAVESTATE_EXCLUDE = -1;
                if (sGlass < 0) { const char *e = getenv("OOT_FWGLASS"); sGlass = !(e && e[0] == '0'); }
                cmt.tinta = sFwTint; cmt.tavg = sAvg; cmt.tglass = sGlass != 0;
                if (sGlass) {
                    uint16_t mx = sAvg[0]; if (sAvg[1] > mx) mx = sAvg[1]; if (sAvg[2] > mx) mx = sAvg[2]; if (!mx) mx = 1;
                    cmt.tavg = (uint16x4_t){ (uint16_t)(sAvg[0] * 255 / mx), (uint16_t)(sAvg[1] * 255 / mx), (uint16_t)(sAvg[2] * 255 / mx), 255 };
                }
            }
        }
        cmt.detail = dtex != NULL;
        cmt.fog = fogtri;
        { const uint16_t fv[4] = { (uint16_t)fgb, (uint16_t)fgg, (uint16_t)fgr, 0 }; cmt.fogv = vld1_u16(fv); }
        if (dtex) {
            const uint16_t winv[4] = { (uint16_t)(256 - sDetail.w[0]), (uint16_t)(256 - sDetail.w[1]),
                                       (uint16_t)(256 - sDetail.w[2]), (uint16_t)(256 - sDetail.w[3]) };
            cmt.dpx = dtex->px; cmt.dw = dtex->w; cmt.dmu = dtex->w - 1; cmt.dmv = dtex->h - 1;
            cmt.dpmu = ((uint32_t)dtex->w << 16) - 1u; cmt.dpmv = ((uint32_t)dtex->h << 16) - 1u;
            cmt.dsls = sDetail.sls; cmt.dsrs = sDetail.srs; cmt.dslt = sDetail.slt; cmt.dsrt = sDetail.srt;
            cmt.doffs = sDetail.offs; cmt.dofft = sDetail.offt;
            cmt.dwt = vld1_u16(sDetail.w); cmt.dwinv = vld1_u16(winv); cmt.dbilin = dbilin;
        }
        /* OOT_WATERLEAN: the folded water rows (cm_row_neon's round-61 cases) in cm_row_lean, where every test they
         * make per pixel is decided here: the blend always runs (fga in [amin, 254], ablend), no z write, no decal
         * bias, no fastwater tint, at most one extra blend; tile 0 (and tile 1) 32-bit, repeating, bilinear */
        cmt.lean = sWaterLean && sCmSpec && sBil64 && !cmt.texonly && cmt.alpha && cmt.bilin && cmt.fog && cmt.amode == 3 &&
                   ablend && !dmask && zbias == 0 && fga >= amin && fga < 255 && (cmt.extra == 0 || cmt.extra == 1) &&
                   cmt.tinta == 0 && !t16 && ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0 &&
                   (!dtex || dbilin);
        /* OOT_OPAQUELEAN: cm_row_neon's opaque z-tested bilinear rows on textures repeating on powers of two (32-bit
         * taps, or 16-bit LUT taps without a detail layer; a detail layer bilinear) in cm_row_lean too */
        cmt.lean = cmt.lean || (sOpaqueLean && !cmt.texonly && !cmt.alpha && cmt.bilin && ws == WRAP_REPEAT &&
                                wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0 && (t16 ? !dtex : sBil64 != 0) &&
                                (!dtex || dbilin));
        /* OOT_ALPHALEAN: the blended / alpha-tested rows (any alpha source) and the opaque rows on clamped or odd-sized
         * textures, bilinear, in cm_row_leana; the detail layer as cm_row_lean takes it (tile 1 bilinear) */
        cmt.lean2 = 0;
        if (!cmt.lean && sAlphaLean && !cmt.texonly && cmt.bilin && cmt.extra == 0 && cmt.tinta == 0 &&
            (!dtex || dbilin) && (t16 || sBil64 != 0 || !(ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0)) &&
            (cmt.alpha || !(ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0)))
            cmt.lean2 = 1;
        /* OOT_SKYLEAN: the sky's texture-only rows, bilinear */
        if (!cmt.lean && sSkyLean && cmt.texonly && cmt.bilin && (t16 || sBil64 != 0 || !(ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0)))
            cmt.lean2 = 2;
        /* OOT_LERPLEAN: the tint rows (the conditions above, lerp_rows) */
        if (noz_rows) { cmt.lean = false; cmt.lean2 = 4; }
        if (lerp_rows) { cmt.lean = false; cmt.lean2 = 3; cmt.lk = (uint16x4_t){ (uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0 }; }
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
    OwnTri oe; /* OOT_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    if (own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&oe.e[0], v1, v2, s); own_edge(&oe.e[1], v2, v0, s); own_edge(&oe.e[2], v0, v1, s);
    }
#if defined(HH_CM_NEON)
    HFX_TRI_HOOK /* half-resolution water (gfx_soft_halfwater.inc.c; off: one test) */
#endif
#ifndef HANDHELD
    if (sOvdPix >= 0 && sOvdOn > 0) { /* OOT_OVDPIX: the triangles whose box holds that pixel */
        const int px = sOvdPix % 240, py = sOvdPix / 240;
        if (px >= minx && px < maxx && py >= miny && py < maxy) {
            extern void port_log_always(const char *fmt, ...);
            extern int gfx_dl_depth_hint(void);
            port_log_always("OVDTRI f%u (%.2f,%.2f,%.5f) (%.2f,%.2f,%.5f) (%.2f,%.2f,%.5f) det%d blend%d zw%d\n", sOvdFrames,
                            v0->x, v0->y, v0->z, v1->x, v1->y, v1->z, v2->x, v2->y, v2->z, dtex != NULL, sUseAlpha, dmask);
        }
    }
    { /* OOT_OVERDRAW's per-kind census (desktop only) */
        int lw = 0, lh = 0;
        while (tex && lw < 7 && (4 << lw) < tw) lw++;
        while (tex && lh < 7 && (4 << lh) < th) lh++;
        CEN_KEY((dtex != NULL) | (fogtri << 1) | (sUseAlpha << 2) | (ztest << 3) | (dmask << 4) | (flat << 5) |
                (use_bilin << 6) | ((uint32_t)lw << 20) | ((uint32_t)lh << 23) | ((uint32_t)(tex && t16) << 26) |
                ((uint32_t)(tex != NULL) << 27));
    }
#endif

#if defined(HH_CM_NEON)
    { /* OOT_SKYDEFER: keep the sky's rows; anything but a marking row draws the kept sky first */
        const bool marking = cm_rows && cmt.lean && !cmt.alpha && sLeanZ && zbias == 0 && dmask && fbuf == sDsFB;
        const bool keep = sSkyDefer && sDsTag && cm_rows && cmt.lean2 == 2 && !sSkyPass && sDsAllow;
        if (sDsN && !marking && !keep) sky_flush();
        if (sDsN && marking) sDsMarked = true;
        if (keep) {
            if (sDsN && (sDsMarked || sDsN == DS_MAX || fbuf != sDsFB)) sky_flush();
            const int sz = sW * sH;
            if (!sDsN) {
                if (sDsMaskSz < sz) { free(sDsMask); sDsMask = (uint8_t *)malloc((size_t)sz); sDsMaskSz = sDsMask ? sz : 0; }
                if (sDsMask) memset(sDsMask, 0, (size_t)sz);
            }
            if (sDsMask) {
                const DsRows R = { e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx,
                                   z0, z1, z2, r0, r1, r2, g0, g1, g2, bl0, bl1, bl2, a0, a1, a2, uw0, uw1, uw2,
                                   vw0, vw1, vw2, iw0, iw1, iw2, minx, maxx, miny, maxy, sW, flat, persp, sDsP3D, zb };
                sDsTri[sDsN].cmt = cmt; sDsTri[sDsN].R = R; sDsN++;
                sDsFB = fbuf; sDsMarking = true; sDsMarked = false;
                return 0; /* its span pixels are counted when it is drawn */
            }
        }
    }
#endif
    /* OOT_CRACKFILL's coverage: every draw that can leave a pixel of its span with colour but no depth -- the
     * alpha-tested cut-outs (their transparent texels are holes on purpose), the blends without a z write, anything
     * drawn without a z test -- but the sky; not the opaque z-tested spans (they write depth or fail against one).
     * So a pixel the fill takes showed nothing but the clear colour or the sky. */
    uint8_t *const cov = (sCov && fbuf == sCovTarget && sCovSz >= sW * sH && !sSkyTag && !sSkyPass && (!ztest || sUseAlpha))
                         ? sCov : NULL; /* (either phase: a prerendered room's ortho triangles come before the 3D) */
#if defined(HH_CM_NEON)
    if (cm_rows && sFastRows && !own && !sPld) { /* OOT_FASTROWS: the rows in tri_rows, the same row functions */
        TriRows R;
        R.va0 = (float32x4_t){ bl0, g0, r0, a0 }; R.vb0 = (float32x4_t){ uw0, vw0, iw0, z0 };
        R.va1 = (float32x4_t){ bl1, g1, r1, a1 }; R.vb1 = (float32x4_t){ uw1, vw1, iw1, z1 };
        R.va2 = (float32x4_t){ bl2, g2, r2, a2 }; R.vb2 = (float32x4_t){ uw2, vw2, iw2, z2 };
        R.fa = (float32x4_t){ bl0, g0, r0, a0 };
        R.e0c = e0c; R.e0x = e0x; R.e0y = e0y; R.e1c = e1c; R.e1x = e1x; R.e1y = e1y;
        R.e2c = e2c; R.e2x = e2x; R.e2y = e2y; R.inv_area = inv_area; R.w0dx = w0dx; R.w1dx = w1dx; R.w2dx = w2dx;
        R.minx = minx; R.maxx = maxx; R.sw = sW; R.flat = flat; R.persp = persp;
        R.ek0 = w0dx > 0.0f ? 0 : w0dx < 0.0f ? 1 : 2;
        R.ek1 = w1dx > 0.0f ? 0 : w1dx < 0.0f ? 1 : 2;
        R.ek2 = w2dx > 0.0f ? 0 : w2dx < 0.0f ? 1 : 2;
        R.cov = cov; R.fbuf = fbuf; R.zb = zb; R.t = &cmt;
        R.fn = cmt.lean ? cm_row_lean : cmt.lean2 == 1 ? cm_row_leana : cmt.lean2 == 2 ? cm_row_leans
             : cmt.lean2 == 3 ? cm_row_leanl : cmt.lean2 == 4 ? cm_row_leanz : cm_row_neon; /* as the loop below */
#if defined(MIYOO)
        tri_px = tri_rows_split(&R, miny, maxy); /* on both cores when the triangle is big */
#else
        tri_px = tri_rows(&R, miny, maxy);
#endif
        CEN_SPAN(tri_px);
        return (int)tri_px;
    }
#endif
#if defined(MIYOO)
    { /* the same loop in raster_rows, in two shares on the two cores when the triangle is big (rows_split) */
        RasterRows RR;
        RASTER_ROWS_PACK(RR);
        tri_px = rows_split(&RR, miny, maxy);
    }
#else
#include "gfx_soft_rows.inc.c"
#endif
    CEN_SPAN(tri_px);
    return (int)tri_px;
}

static bool sPhase3D SAVESTATE_EXCLUDE, sIntDirty SAVESTATE_EXCLUDE, sNatDirty SAVESTATE_EXCLUDE; /* see the frame section */
static bool sHad3D SAVESTATE_EXCLUDE;         /* any 3D batch this frame */
static int sCrackFill SAVESTATE_EXCLUDE = 1;  /* OOT_CRACKFILL, see crack_fill */
static bool sCrackDirty SAVESTATE_EXCLUDE;    /* a depth-writing 3D draw since the last crack fill */
static bool sIntClean SAVESTATE_EXCLUDE;      /* internal colour is all-zero (cleared by the last composite) */
/* Lazy native clear (MK64 perf round 24): OoT opens every frame with a
 * full-screen fill, so clearing native up front was 230 KB of stores for
 * nothing. The colour is cleared just before its first write unless that
 * write covers the whole target. */
static bool sNatStale SAVESTATE_EXCLUDE;
/* The colour a stale native target stands for: 0 (the frame's clear) or the
 * last full-screen opaque solid rect. OoT's Gfx_SetupFrame fills the frame
 * black and then with the scene's clear colour -- two 57.6k-pixel fills, the
 * first never seen. Now neither is drawn: the latest one lands when something
 * first writes or reads native (nat_ready), and not at all when the full sky
 * covers the frame. */
static void nat_ready(void);
static void target_ready(void);
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */

/* OOT_PICK=x,y (debug): log every 3D triangle that changes that pixel of the
 * current target, inline in the OOT_TRACE_AT trace (so the display-list command
 * above it is the one that drew it), and dump the texture it sampled as
 * pick_tex_<id>.ppm into $OOT_DUMP when set. */
static int sPickX SAVESTATE_EXCLUDE = -2, sPickY SAVESTATE_EXCLUDE = -1;
/* (sDbgBilin/Scr/Uvc: raster_tri's last filter decision, for the PICK line) */
static void pick_dump_texture(uint32_t id) {
    static uint8_t dumped[MAX_TEXTURES];
    if (id == 0 || id >= MAX_TEXTURES || dumped[id] || !sTex[id].valid) return;
    dumped[id] = 1;
    const SoftTex *t = &sTex[id];
    const char *dir = getenv("OOT_DUMP"); /* only with a dump dir: never into the working tree */
    char path[512];
    if (!dir) return;
    snprintf(path, sizeof(path), "%s/pick_tex_%u.ppm", dir, (unsigned)id);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    for (int i = 0; i < t->w * t->h; i++) {
        const uint32_t p = texel_at(t->px, t->is16, i);
        const uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    PROF_BEGIN();
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    target_ready();
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
    if (sPhase3D && sDepthMask && !sSkyPass) sCrackDirty = true; /* OOT_CRACKFILL */
    if (sPickX == -2) {
        const char *e = getenv("OOT_PICK");
        if (!e || sscanf(e, "%d,%d", &sPickX, &sPickY) != 2) sPickX = -1;
    }
#if defined(HH_CM_NEON)
    sDsAllow = sPickX < 0; sDsP3D = sPhase3D; /* OOT_SKYDEFER: PICK reads the colour around each triangle */
    if (!sDsAllow) sky_settle();
#endif
    for (size_t i = 0; i < num_tris; i++) {
        SoftVtx h[3];
        if (sSkyPass) { /* screen coordinates to the half-resolution sky target */
            for (int k = 0; k < 3; k++) { h[k] = v[i*3+k]; h[k].x *= 0.5f; h[k].y *= 0.5f; }
        }
        const bool pick = sPickX >= 0 && sPickX < sW && sPickY >= 0 && sPickY < sH;
        const uint32_t pick_c0 = pick ? sFB[sPickY * sW + sPickX] : 0;
        const uint16_t pick_z0 = pick ? sZB[sPickY * sW + sPickX] : 0;
        const bool twice = gfx_soft_tri_twice != NULL && gfx_soft_tri_twice[i]; /* gfx_pc: G_TRI2 with both halves this */
        sTwiceReq = twice && !sSkyPass && !pick;
        sTwiceDone = 0;
        const int px = sSkyPass ? raster_tri(&h[0], &h[1], &h[2], true) : raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
        sTwiceReq = 0;
        if (twice && !sTwiceDone) { /* not foldable here: the copy, as the display list has it */
            if (sSkyPass) raster_tri(&h[0], &h[1], &h[2], true); else raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
        }
        if (pick && (sFB[sPickY * sW + sPickX] != pick_c0 || sZB[sPickY * sW + sPickX] != pick_z0)) {
            const SoftTex *t = &sTex[sBoundTex[0]];
            port_log("  PICK (%d,%d) %08X -> %08X  tex %u %dx%d%s wrap %d/%d bilin %d (scr %.1f uvc %.1f) detail %u/%d  uv (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f)\n",
                     sPickX, sPickY, (unsigned)pick_c0, (unsigned)sFB[sPickY * sW + sPickX], (unsigned)sBoundTex[0],
                     t->w, t->h, t->is16 ? " 16b" : "", t->wrap_s, t->wrap_t, (int)sDbgBilin, sDbgScr, sDbgUvc,
                     (unsigned)sDetail.id, (int)sDbgDBilin,
                     v[i*3].u, v[i*3].v, v[i*3+1].u, v[i*3+1].v, v[i*3+2].u, v[i*3+2].v);
            pick_dump_texture(sBoundTex[0]);
        }
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
 * rect in a VertexColor {u16 u,v; RGBA; u16 x,y,z}: screen pixels + TEXEL UVs
 * (tile-origin already subtracted), combiner colour pre-folded by
 * gfx_eval_vertex_color. GU takes the sprite colour from the second vertex. */
typedef struct __attribute__((aligned(16))) {
    int16_t u, v; /* signed: left of / above the tile origin is negative (SSB64 4843c2c, DKR) */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* z = 5 on a sprite's corners: u/v are S10.5 (1/32 texel), the rectangle is magnified over 2x and
 * the game filters it bilinearly (gfx_pc's magbil: the songs' full-screen cloud layers). One
 * bilinear texel at (u, v) texels, texel centres at +0.5, wrapped like the nearest path. */
static inline uint32_t texel_bilin2d(const void *px, bool t16, float u, float v, int tw, int th, int ws, int wt, int mu,
                                     int mv) {
    u -= 0.5f; v -= 0.5f;
    const int iu = ifloorf(u), iv = ifloorf(v);
    const int fu = (int)((u - (float)iu) * 256.0f), fv = (int)((v - (float)iv) * 256.0f);
    const int c0 = wrapf(iu, tw, ws, mu), c1 = wrapf(iu + 1, tw, ws, mu);
    const int r0 = wrapf(iv, th, wt, mv) * tw, r1 = wrapf(iv + 1, th, wt, mv) * tw;
    const uint32_t a = texel_at(px, t16, r0 + c0), b = texel_at(px, t16, r0 + c1);
    const uint32_t c = texel_at(px, t16, r1 + c0), d = texel_at(px, t16, r1 + c1);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const int ca = (int)((a >> sh) & 0xff), cb = (int)((b >> sh) & 0xff);
        const int cc = (int)((c >> sh) & 0xff), cd = (int)((d >> sh) & 0xff);
        const int top = ca * 256 + (cb - ca) * fu, bot = cc * 256 + (cd - cc) * fu;
        out |= (uint32_t)((top * 256 + (bot - top) * fv + 32768) >> 16) << sh;
    }
    return out;
}

#if defined(HH_2D_NEON)
/* texel_bilin2d with the four channels' lerps in NEON lanes (OOT_MAGBIL2D): the same coordinates, wraps and taps (the
 * scalar code above, statement for statement), then per lane top = a * 256 + (b - a) * fu, bot likewise, and
 * (top * 256 + (bot - top) * fv + 32768) >> 16 in 32-bit lanes: integers below 2^31 (top <= 65280; top * 256 +
 * (bot - top) * fv = top (256 - fv) + bot fv >= 0), so the same values as the scalar ints. */
static inline uint32_t texel_bilin2d_n(const void *px, bool t16, float u, float v, int tw, int th, int ws, int wt, int mu,
                                       int mv) {
    u -= 0.5f; v -= 0.5f;
    const int iu = ifloorf(u), iv = ifloorf(v);
    const int fu = (int)((u - (float)iu) * 256.0f), fv = (int)((v - (float)iv) * 256.0f);
    const int c0 = wrapf(iu, tw, ws, mu), c1 = wrapf(iu + 1, tw, ws, mu);
    const int r0 = wrapf(iv, th, wt, mv) * tw, r1 = wrapf(iv + 1, th, wt, mv) * tw;
    const uint32_t a = texel_at(px, t16, r0 + c0), b = texel_at(px, t16, r0 + c1);
    const uint32_t c = texel_at(px, t16, r1 + c0), d = texel_at(px, t16, r1 + c1);
    const uint16x8_t ab = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)b << 32) | a)));
    const uint16x8_t cd = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | c)));
    const int32x4_t A = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(ab))), B = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(ab)));
    const int32x4_t C = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(cd))), D = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(cd)));
    const int32x4_t top = vmlaq_n_s32(vshlq_n_s32(A, 8), vsubq_s32(B, A), fu);
    const int32x4_t bot = vmlaq_n_s32(vshlq_n_s32(C, 8), vsubq_s32(D, C), fu);
    const int32x4_t o = vshrq_n_s32(vaddq_s32(vmlaq_n_s32(vshlq_n_s32(top, 8), vsubq_s32(bot, top), fv), vdupq_n_s32(32768)), 16);
    const uint16x4_t o16 = vmovn_u32(vreinterpretq_u32_s32(o));
    return vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(o16, o16))), 0);
}
/* A row of n texel_bilin2d_n values (OOT_MAGBIL2D): u_i accumulated from ug by dudx as the general loop steps ub (also
 * kept in ubv, ubv[n] the u after the last), then four pixels at a time: ifloorf's trunc-and-compare, the fraction
 * and the wraps in NEON lanes (REPEAT on a power of two: the mask; CLAMP: min / max -- wrapf's two cases), the taps
 * as texel_at reads them, and the lerps in 16-bit lanes: ca * 256 + (cb - ca) * fu = ca * 256 + cb * fu - ca * fu is
 * in [0, 65280], so modulo 2^16 it is the same integer; (top * 256 + (bot - top) * fv + 32768) >> 16 is
 * (top * (256 - fv) + bot * fv + 32768) >> 16, below 2^24 in 32-bit lanes. The float steps (u - 0.5f, the
 * subtraction, the scale by 256) are the scalar ones, IEEE single in both units: u - 0.5f is done here in VFP, and no
 * value NEON sees is denormal (u - 0.5f is 0 or of magnitude >= 2^-25, the fraction likewise). An odd-sized REPEAT
 * axis in u (wrapf's modulo) and the last n % 4 pixels take texel_bilin2d_n. */
static void bilin2d_row_n(uint32_t *out, float *ubv, int n, const void *px, bool t16, float ug, float dudx, float v,
                          int tw, int th, int ws, int wt, int mu, int mv) {
    float uu[256];
    for (int i = 0; i < n; i++, ug += dudx) { ubv[i] = ug; uu[i] = ug - 0.5f; }
    ubv[n] = ug;
    int i = 0;
    if (ws != WRAP_REPEAT || mu >= 0) {
        const float vh = v - 0.5f;
        const int iv = ifloorf(vh), fv = (int)((vh - (float)iv) * 256.0f);
        const int r0 = wrapf(iv, th, wt, mv) * tw, r1 = wrapf(iv + 1, th, wt, mv) * tw;
        const bool rep = ws == WRAP_REPEAT;
        const int32x4_t vmask = vdupq_n_s32(mu), vmax = vdupq_n_s32(tw - 1), vzero = vdupq_n_s32(0), vone = vdupq_n_s32(1);
        const uint16x4_t wfv = vdup_n_u16((uint16_t)fv), wifv = vdup_n_u16((uint16_t)(256 - fv));
        const uint32x4_t half = vdupq_n_u32(32768);
        for (; i + 4 <= n; i += 4) {
            const float32x4_t x = vld1q_f32(uu + i);
            const int32x4_t t = vcvtq_s32_f32(x);
            const int32x4_t iu = vaddq_s32(t, vreinterpretq_s32_u32(vcltq_f32(x, vcvtq_f32_s32(t)))); /* t - (x < t) */
            const int32x4_t fu = vcvtq_s32_f32(vmulq_n_f32(vsubq_f32(x, vcvtq_f32_s32(iu)), 256.0f));
            const int32x4_t iu1 = vaddq_s32(iu, vone);
            const int32x4_t c0 = rep ? vandq_s32(iu, vmask) : vminq_s32(vmaxq_s32(iu, vzero), vmax);
            const int32x4_t c1 = rep ? vandq_s32(iu1, vmask) : vminq_s32(vmaxq_s32(iu1, vzero), vmax);
            int32_t k0[4], k1[4], f[4];
            vst1q_s32(k0, c0); vst1q_s32(k1, c1); vst1q_s32(f, fu);
            for (int j = 0; j < 4; j += 2) {
                const uint32_t a0 = texel_at(px, t16, r0 + k0[j]), b0 = texel_at(px, t16, r0 + k1[j]);
                const uint32_t c0_ = texel_at(px, t16, r1 + k0[j]), d0 = texel_at(px, t16, r1 + k1[j]);
                const uint32_t a1 = texel_at(px, t16, r0 + k0[j + 1]), b1 = texel_at(px, t16, r0 + k1[j + 1]);
                const uint32_t c1_ = texel_at(px, t16, r1 + k0[j + 1]), d1 = texel_at(px, t16, r1 + k1[j + 1]);
                const uint16x8_t A = vmovl_u8(vcreate_u8(((uint64_t)a1 << 32) | a0)), B = vmovl_u8(vcreate_u8(((uint64_t)b1 << 32) | b0));
                const uint16x8_t C = vmovl_u8(vcreate_u8(((uint64_t)c1_ << 32) | c0_)), D = vmovl_u8(vcreate_u8(((uint64_t)d1 << 32) | d0));
                const uint16x8_t F = vcombine_u16(vdup_n_u16((uint16_t)f[j]), vdup_n_u16((uint16_t)f[j + 1]));
                const uint16x8_t top = vmlsq_u16(vmlaq_u16(vshlq_n_u16(A, 8), B, F), A, F);
                const uint16x8_t bot = vmlsq_u16(vmlaq_u16(vshlq_n_u16(C, 8), D, F), C, F);
                const uint32x4_t lo = vaddq_u32(vmlal_u16(vmull_u16(vget_low_u16(top), wifv), vget_low_u16(bot), wfv), half);
                const uint32x4_t hi = vaddq_u32(vmlal_u16(vmull_u16(vget_high_u16(top), wifv), vget_high_u16(bot), wfv), half);
                const uint8x8_t o = vmovn_u16(vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi, 16)));
                vst1_u32(out + i + j, vreinterpret_u32_u8(o));
            }
        }
    }
    for (; i < n; i++) out[i] = texel_bilin2d_n(px, t16, ubv[i], v, tw, th, ws, wt, mu, mv);
}
static int sMagBil2D SAVESTATE_EXCLUDE = 1; /* magnified bilinear sprites in the NEON combines (2026-10-04); OOT_MAGBIL2D=0: off */
#endif
static int s2dGen SAVESTATE_EXCLUDE = 1; /* the 2D general loop on locals (2026-10-01); OOT_2DGEN=0: the old loop */
/* OOT_CRACKFILL: a rectangle of the current target marked covered when the target is the one the 3D draws to (in
 * either phase: a prerendered room's picture goes to it before the 3D) */
static void cov_rect(int x0, int y0, int x1, int y1) {
    if (!sCov || x1 <= x0 || y1 <= y0 || sFB != sCovTarget || sCovSz < sW * sH) return;
    for (int y = y0; y < y1; y++) memset(sCov + y * sW + x0, 1, (size_t)(x1 - x0));
}
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    sky_settle(); /* OOT_SKYDEFER: the kept sky first (see raster_tri) */
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) tex = NULL;
    const bool modulate = !sCurShader || sCurShader->cc.num_inputs > 0;
    sNatDirty = true; /* gfx_pc switches to the 2D phase before calling us */
    PROF_BEGIN();
#if defined(HH_2D_NEON)
    if (sNeon2D < 0) { const char *e = getenv("OOT_NEON2D"); sNeon2D = !(e && e[0] == '0'); }
    const bool neon2d = sNeon2D != 0;
#endif

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
        const bool magbil = tex && a->z == 5; /* S10.5 corners, bilinear (texel_bilin2d) */
        if (magbil) { u0 /= 32.0f; u1 /= 32.0f; v0 /= 32.0f; v1 /= 32.0f; }
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
        if (!tex && (!use_alpha || ca >= 255) && cx0 == 0 && cy0 == 0 && cx1 == sW && cy1 == sH && sFB == sNatFB) {
            /* covers the whole target: deferred (see sNatFill) */
            sNatStale = true;
            sNatFill = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255);
            continue;
        }
        target_ready();
        cov_rect(cx0, cy0, cx1, cy1); /* OOT_CRACKFILL: 2D drawn into the 3D's target counts as covered */
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa;
        /* A sprite whose alpha takes the constant (ca, alone or x the texel: <= ca) with ca already
         * under the reject writes no pixel: skip it whole. The Deku Tree basement's 96x85 map sprite
         * is drawn every frame at prim alpha 0 -- 8,160 pixels computed and dropped. Exact.
         * OOT_2DSKIP=0: every pixel walked, as before. */
        static int s2dSkip SAVESTATE_EXCLUDE = -1;
        if (s2dSkip < 0) { const char *e = getenv("OOT_2DSKIP"); s2dSkip = !(e && e[0] == '0'); }
        if (s2dSkip && use_alpha && (!a_tex || a_in) && ca < amin) continue; /* fa = !a_tex ? ca : a_in ? ta*ca/255 : ta */
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (gfx_soft_hud_box < 0) { const char *e = getenv("OOT_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
        /* shrunk 1..3x along S: area-average (see texel_box) */
        const bool boxf = tex && gfx_soft_hud_box && dudx > 1.02f && dudx <= 3.0f;
        const bool texel_is_pixel = tex && rgb_tex && a_tex && !rgb_texa &&
                           ((!rgb_in && !a_in) || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
        const bool plain = !boxf && !t16 && texel_is_pixel && !magbil;
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        const int32_t uf_row = tex ? to_fix16(u0 + (cx0 - x0 + 0.5f) * dudx) : 0; /* every row starts here */
        /* area filter a row at a time, through the cross-frame row cache */
        const bool boxc = boxf && cx1 > cx0 && cx1 - cx0 <= BOX_MAX;
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
                    fill32(row + cx0, argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255), cx1 - cx0);
                } else if (ca >= amin) {
                    int x = cx0;
#if defined(HH_2D_NEON)
                    if (neon2d) x += blend_solid_n2d(row + cx0, cx1 - cx0, cr, cg, cb2, ca);
#endif
                    for (; x < cx1; x++) {
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
            int32_t uf = uf_row;
            uint32_t boxtmp[BOX_MAX];
            const uint32_t *boxrow = boxc ? box_row_cached(boxtmp, cx1 - cx0, sBoundTex[0], texpx, t16, trow_base, uf_row,
                                                           duf, tw, ws2, mu, use_alpha)
                                          : NULL;
            if (boxc && texel_is_pixel) {
                /* the averaged texel is the pixel: as the general loop below
                 * with no combiner work (white opaque input changes nothing) */
                int x0n = cx0;
#if defined(HH_2D_NEON)
                if (neon2d && use_alpha) x0n += blend_texel_n2d(row + cx0, boxrow, cx1 - cx0, amin);
#endif
                for (int x = x0n; x < cx1; x++) {
                    const uint32_t t = boxrow[x - cx0];
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
            if (plain) {
                /* 1:1 copy inside the texture: straight run, no wrap per texel. */
                const int iu0 = uf >> 16;
                if (duf == 65536 && iu0 >= 0 && iu0 + (cx1 - cx0) <= tw) {
                    const uint32_t *src = (const uint32_t *)texpx + trow_base + iu0;
                    if (!use_alpha) {
                        for (int x = cx0; x < cx1; x++) row[x] = src[x - cx0] | 0xFF000000u;
                    } else {
                        int x0n = cx0;
#if defined(HH_2D_NEON)
                        if (neon2d) x0n += blend_texel_n2d(row + cx0, src, cx1 - cx0, amin);
#endif
                        for (int x = x0n; x < cx1; x++) {
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
            int xg = cx0;
#if defined(HH_2D_NEON)
            const bool nmod = rgb_tex && rgb_in && !sTexLerp && !sTexAdd && !rgb_texa;
            const bool nlerp = s2dLerp && rgb_tex && rgb_in && sTexLerp && !rgb_texa; /* the tint wins over add, as below */
            /* OOT_MAGBIL2D (2026-10-04): a magnified bilinear sprite (the wasteland's sandstorm, the songs' cloud layers:
             * full-screen S10.5 rectangles that took the scalar loop below with a float bilinear per pixel -- 54% of the
             * wasteland view's frame) through the same NEON combines: its texels gathered as that loop reads them (u
             * accumulated by dudx from the same start, texel_bilin2d's values), then the rest of the row continues from
             * the same u. */
            const bool mbn = magbil && sMagBil2D && gfx_soft_bilinear;
            /* ...and its colour from the texel alone (the sandstorm, the storm's clouds: rgb_in off, so the loop takes
             * fr = tr whatever the tint and add flags): combine_mod_n2d with a white colour, div255(t * 255) = t */
            const bool ntex = mbn && rgb_tex && !rgb_in && !rgb_texa;
            float ubn = 0.0f; /* with mbn: u at the first pixel the NEON combine left */
            if (neon2d && (nmod || nlerp || ntex) && (!magbil || mbn) && cx1 - cx0 >= 8) {
                /* texture x colour or the tint: the texels gathered as the loop below would read them */
                uint32_t gath[256];
                float ubv[257];
                const uint32_t *tv = boxrow;
                const int n = cx1 - cx0 <= 256 ? cx1 - cx0 : 256;
                if (mbn) {
                    bilin2d_row_n(gath, ubv, n, texpx, t16, u0 + (cx0 - x0 + 0.5f) * dudx, dudx, v, tw, th, ws2, wt2, mu, mv);
                    tv = gath;
                } else if (!boxc) {
                    int32_t ug = uf;
                    for (int i = 0; i < n; i++, ug += duf)
                        gath[i] = boxf ? texel_box(texpx, t16, trow_base, ug, duf, tw, ws2, mu, use_alpha)
                                       : texel_at(texpx, t16, trow_base + wrapf(ug >> 16, tw, ws2, mu));
                    tv = gath;
                }
                const int done = ntex ? combine_mod_n2d(row + cx0, tv, n, 255, 255, 255, ca, !a_tex ? 0 : !a_in ? 1 : 2,
                                                        use_alpha, amin)
                                 : nmod ? combine_mod_n2d(row + cx0, tv, n, cr, cg, cb2, ca, !a_tex ? 0 : !a_in ? 1 : 2,
                                                        use_alpha, amin)
                                      : combine_lerp_n2d(row + cx0, tv, n, cr, cg, cb2, ca, !a_tex ? 0 : !a_in ? 1 : 2,
                                                         use_alpha, amin, sLerpR, sLerpG, sLerpB);
                xg += done;
                uf += done * duf;
                if (mbn) ubn = ubv[done];
            }
            float ub = mbn ? (xg == cx0 ? u0 + (cx0 - x0 + 0.5f) * dudx : ubn)
                           : u0 + (xg - x0 + 0.5f) * dudx; /* magbil: this pixel's u, in texels */
#else
            float ub = u0 + (xg - x0 + 0.5f) * dudx; /* magbil: this pixel's u, in texels */
#endif
/* The general 2D loop. FETCH: the texel source; TL/TA/LR/LG/LB/BIL: the tint and add flags, the tint constant and the
 * bilinear switch -- the globals (the old loop: -fno-strict-aliasing reloads each one after every row[] store) or
 * locals of the same values. */
#define GEN2D_LOOP(FETCH, TL, TA, LR, LG, LB, BIL)                                                      \
            for (int x = xg; x < cx1; x++, uf += duf, ub += dudx) {                                     \
                const uint32_t t = FETCH(BIL);                                                          \
                const int ta = (t>>24)&0xff;                                                            \
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff; \
                int fr, fg, fb, fa;                                                                     \
                if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }                                             \
                else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }                                              \
                else if (TL) { fr=LR+((cr-LR)*tr)/255; fg=LG+((cg-LG)*tg)/255; fb=LB+((cb2-LB)*tb)/255; } \
                else if (TA) { fr=tr+cr; fg=tg+cg; fb=tb+cb2; if (fr>255)fr=255; if (fg>255)fg=255; if (fb>255)fb=255; } \
                else              { fr=(tr*cr)/255; fg=(tg*cg)/255; fb=(tb*cb2)/255; }                  \
                fa = !a_tex ? ca : (!a_in ? ta : (ta*ca)/255);                                          \
                if (use_alpha) {                                                                        \
                    if (fa < amin) continue;                                                            \
                    if (fa < 255) {                                                                     \
                        const uint32_t d = row[x];                                                      \
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;                           \
                        fr = (fr*fa + dr*(255-fa))/255;                                                 \
                        fg = (fg*fa + dg*(255-fa))/255;                                                 \
                        fb = (fb*fa + db*(255-fa))/255;                                                 \
                    }                                                                                   \
                }                                                                                       \
                row[x] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);                                 \
            }
#define F2D_ANY(BIL) (magbil ? (BIL ? texel_bilin2d(texpx, t16, ub, v, tw, th, ws2, wt2, mu, mv)                 \
                                    : texel_at(texpx, t16, wrapf(ifloorf(v), th, wt2, mv) * tw + wrapf(ifloorf(ub), tw, ws2, mu))) \
                      : boxc ? boxrow[x - cx0]                                                          \
                      : boxf ? texel_box(texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha)        \
                             : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu)))
#define F2D_BOXC(BIL) boxrow[x - cx0]
#define F2D_BOXF(BIL) texel_box(texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha)
#define F2D_NEAR(BIL) texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu))
            if (s2dGen && !magbil) { /* OOT_2DGEN (2026-10-01): locals, and the texel source chosen once a sprite row */
                const bool tl = sTexLerp, tad = sTexAdd; const int lr = sLerpR, lg = sLerpG, lb = sLerpB;
                if (boxc)      { GEN2D_LOOP(F2D_BOXC, tl, tad, lr, lg, lb, 0) }
                else if (boxf) { GEN2D_LOOP(F2D_BOXF, tl, tad, lr, lg, lb, 0) }
                else           { GEN2D_LOOP(F2D_NEAR, tl, tad, lr, lg, lb, 0) }
            } else {
                GEN2D_LOOP(F2D_ANY, sTexLerp, sTexAdd, sLerpR, sLerpG, sLerpB, gfx_soft_bilinear)
            }
#undef GEN2D_LOOP
#undef F2D_ANY
#undef F2D_BOXC
#undef F2D_BOXF
#undef F2D_NEAR
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
static void nat_ready(void) {
    if (sNatStale && sNatFB) {
        if (sNatFill == 0) memset(sNatFB, 0, (size_t)sNatW * sNatH * 4);
        else fill32(sNatFB, sNatFill, sNatW * sNatH);
        sNatStale = false;
    }
}
/* Before writing the current target (only native is cleared lazily). */
static void target_ready(void) { if (sFB == sNatFB) nat_ready(); }
/* Presented image: always the native target (composited at end_frame). */
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { sky_settle(); if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }

static void alloc_target(uint32_t **fb, uint16_t **zb, int *w, int *h, int nw, int nh) {
    if (*fb && *w == nw && *h == nh) return;
    free(*fb); free(*zb);
    *w = nw; *h = nh;
    *fb = (uint32_t *)malloc((size_t)nw * nh * 4);
    *zb = (uint16_t *)malloc((size_t)nw * nh * 2);
}

static void select_target(bool is3d) {
    sky_settle();
    extern struct GfxDimensions gfx_current_dimensions;
    sPhase3D = is3d;
    if (is3d && !sUnified) { sFB = sIntFB; sZB = sIntZB; sW = sIntW; sH = sIntH; }
    else                   { sFB = sNatFB; sZB = sNatZB; sW = sNatW; sH = sNatH; }
    sCovMark = (is3d && sCov && sCovSz >= sW * sH) ? sCov : NULL; sCovFB = sFB; /* OOT_CRACKFILL */
    sCovTarget = sUnified ? sNatFB : sIntFB;
    sScX0 = 0; sScY0 = 0; sScX1 = sW; sScY1 = sH;
    sVpX = 0; sVpY = 0; sVpW = sW; sVpH = sH;
    gfx_current_dimensions.width = (uint32_t)sW;
    gfx_current_dimensions.height = (uint32_t)sH;
    gfx_current_dimensions.aspect_ratio = sH ? (float)sW / (float)sH : 1.0f;
}

/* Nearest-upscale the internal 3D image into native where the 3D wrote
 * (alpha == 255), then clear it for any later 3D batch this frame. */
static void composite_internal(void) {
    sky_settle();
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    PROF_BEGIN();
    nat_ready();
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
    sky_settle();
    PROF_BEGIN();
    nat_ready();
    sIntClean = false;
    if (sCov) memset(sCov, 1, (size_t)sCovSz); /* OOT_CRACKFILL: the 2D under the 3D counts as covered everywhere */
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

/* OOT_EDGETINT (2026-10-02, the user's flowers on the path to Hyrule Castle: "a 1-px blue outline"; "a few things
 * still have outlines like the kokiri vines did"): the castle path's flowers and tall grass are G_RM_AA_ZB_TEX_EDGE
 * draws (CVG_X_ALPHA + ALPHA_CVG_SEL, no FORCE_BL). The room's cullable entries are drawn nearest first, so a patch
 * near the camera often comes before the ground behind it, when its pixels hold only the sky (drawn first, no
 * depth). Round 49 wrote such an edge pixel unblended only over the clear colour; over the sky it still blended by
 * the texel's alpha, so the rim took the sky's blue, and the ground drawn next failed the depth test there and
 * left it (OOT_PICK on the castle path: sky 738CFF, grass edge -> 5176A2, nothing after). The RDP blends neither:
 * the memory's coverage is full (the sky's, the clear's), the sum overflows, blend_en stays off, so the pixel is
 * written as its own colour with the coverage alpha gives it ((a * 8 + 4) >> 3 >> 5 eighths), and the VI's filter
 * then pulls each partly covered pixel toward its fully covered neighbours (video_filter: up-left, up-right, two
 * left, two right, down-left, down-right; the penultimate min and max of those and the pixel; the pixel moved by
 * (pmin + pmax - 2c) x (8 - cvg) / 8). Now such a pixel is written unblended over any pixel without depth (the sky
 * or the clear) and noted; at the end of the 3D (crack_fill's hooks) each noted pixel still holding its own depth
 * gets that filter, on the finished picture: its neighbours are whatever ended up there (the ground; or the sky,
 * where nothing came behind it, as before). Clamped where the VI wraps. Pixels over drawn geometry keep the blend
 * (round 49: the port's stand-in for the VI there). OOT_EDGETINT=0: round 49's rule. */
static inline void vi_penult(const int *v, int n, int *pmin, int *pmax) { /* angrylion's video_max_optimized */
    int posmax = 0, posmin = 0, cmax = v[0], cmin = v[0];
    for (int i = 1; i < n; i++) {
        if (v[i] > v[posmax]) { cmax = v[posmax]; posmax = i; }
        else if (v[i] < v[posmin]) { cmin = v[posmin]; posmin = i; }
    }
    if (cmax != v[posmax]) for (int i = posmax + 1; i < n; i++) if (v[i] > cmax) cmax = v[i];
    if (cmin != v[posmin]) for (int i = posmin + 1; i < n; i++) if (v[i] < cmin) cmin = v[i];
    *pmin = cmin; *pmax = cmax;
}
static void edge_vi(void) {
    const int n = sEnN;
    sEnN = 0;
    if (!n || sEnFB != sFB || !sEnMark || sEnMarkSz < sW * sH) return;
    uint32_t *const fb = sFB; const uint16_t *const zb = sZB;
    const int w = sW, h = sH;
    static uint32_t sOut[EN_MAX] SAVESTATE_EXCLUDE;
    for (int k = 0; k < n; k++) if (zb[sEnIdx[k]] == sEnZ[k]) sEnMark[sEnIdx[k]] = 1; /* still its own: partly covered */
    target_ready();
    for (int k = 0; k < n; k++) {
        const int i = sEnIdx[k];
        sOut[k] = fb[i];
        if (!sEnMark[i] || zb[i] != sEnZ[k]) continue;
        const int x = i % w, y = i / w, cvg = sEnA[k] >> 5; /* ((a * 8 + 4) >> 3) >> 5, a < 255 */
        static const int8_t ox[6] = { -1, 1, -2, 2, -1, 1 }, oy[6] = { -1, -1, 0, 0, 1, 1 };
        int r[7], g[7], b[7], m = 1;
        r[0] = (fb[i] >> 16) & 0xFF; g[0] = (fb[i] >> 8) & 0xFF; b[0] = fb[i] & 0xFF;
        for (int j = 0; j < 6; j++) {
            const int X = x + ox[j], Y = y + oy[j];
            if (X < 0 || X >= w || Y < 0 || Y >= h || sEnMark[Y * w + X]) continue;
            const uint32_t q = fb[Y * w + X];
            r[m] = (q >> 16) & 0xFF; g[m] = (q >> 8) & 0xFF; b[m++] = q & 0xFF;
        }
        int c[3], lo, hi;
        const int *ch[3] = { r, g, b };
        for (int t = 0; t < 3; t++) {
            vi_penult(ch[t], m, &lo, &hi);
            int v = ch[t][0] + (((lo + hi - 2 * ch[t][0]) * (8 - cvg) + 4) >> 3);
            c[t] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
        sOut[k] = 0xFF000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
    }
    for (int k = 0; k < n; k++) { fb[sEnIdx[k]] = sOut[k]; sEnMark[sEnIdx[k]] = 0; }
    gfx_soft_edge_vi += (uint32_t)n;
}

/* OOT_CRACKFILL (2026-10-02, the user's dashed line along a floor edge out of Kokiri; X40): the N64 has no
 * T-junction cracks. Where two pieces of the game's geometry meet with a sub-pixel gap (a vertex on its
 * neighbour's edge, rounded off it), the RDP gives the pixel to both with partial coverage -- the second blends
 * with the first, the VI's anti-aliasing smooths the rest -- and the clear colour never shows (angrylion). Here a
 * pixel is in or out by its centre, so a gap of a few tenths of a pixel leaves pixel centres covered by neither:
 * a dashed line of the clear colour (or the sky) along the edge, a run on each row where the edge steps, longer
 * the shallower the edge. At the end of the 3D (the switch to 2D, the frame's end) a pixel that no draw covered (z
 * still 0xFFFF, inside no span of an alpha-tested, blended or depth-less draw but the sky's, nor of a 2D picture
 * drawn into the 3D: it shows the clear colour or the sky; a cut-out's transparent texels stay open), between
 * covered pixels above and below -- or left and right -- each with another covered pixel beyond it, whose depths
 * agree as one surface (apart by at most twice their outer steps, plus 8: not a near object against a far one),
 * and not a notch in a silhouette (the diagonal pixels past both ends open on one side), takes their mean colour
 * and depth. A gap that wide in that direction is under a pixel, which the N64 would never show; wider gaps are
 * left as they are (there the N64 shows the background too). Pixels with any depth or coverage are never touched,
 * and every decision reads the state before the fill. The other cracks
 * the port could leave -- a pixel centre exactly on a shared edge that rounding puts outside both triangles (the
 * guard-band fan's corner diagonals: dl_test seam) -- are the same kind of pixel and filled the same way.
 * OOT_CRACKFILL=0: off. */
uint32_t gfx_soft_crack_fills SAVESTATE_EXCLUDE; /* pixels filled (dl_test's seam count) */
static inline int crack_pair(const uint16_t *zb, const uint8_t *cv, int i, int d, int lo, int hi) {
    /* pixel i uncovered, i - d and i + d covered with depth, and the pixels beyond them too (within lo..hi), their
     * depths one surface: apart by at most twice the two outer steps, plus 8 */
    if (cv[i] || zb[i] != 0xFFFF || i - 2 * d < lo || i + 2 * d >= hi) return 0;
    const int a = zb[i - d], b = zb[i + d], a2 = zb[i - 2 * d], b2 = zb[i + 2 * d];
    if (a == 0xFFFF || b == 0xFFFF || a2 == 0xFFFF || b2 == 0xFFFF) return 0;
    const int sa = a > a2 ? a - a2 : a2 - a, sb = b > b2 ? b - b2 : b2 - b, dz = a > b ? a - b : b - a;
    return dz <= 2 * (sa + sb) + 8;
}
/* closed across: not a notch in a silhouette, where the pixels diagonally beyond both ends of the gap on one side are
 * open too (o: the step across) */
static inline int crack_closed(const uint16_t *zb, const uint8_t *cv, int i, int d, int o) {
#define CF_OPEN(j) (zb[j] == 0xFFFF && !cv[j]) /* no depth, and no alpha draw's span */
    return !((CF_OPEN(i - d - o) && CF_OPEN(i + d - o)) || (CF_OPEN(i - d + o) && CF_OPEN(i + d + o)));
#undef CF_OPEN
}
/* OOT_CRACKSCAN (2026-10-03, the final exact speed check): crack_fill's scan with a prefilter, eight pixels at a time.
 * The old scan spent ~24 instructions a pixel wherever the depth is clear -- the sky (Hyrule Field by day: 1.36M
 * guest instructions a frame under qemu, 9% of the rasterizer; the device Bench priced the edge fixes at ~3 ms
 * there), the depth-less backdrops -- since every such pixel ran crack_pair's tests in full. A pixel can only be
 * filled when it has no depth, no coverage, and depth on both sides in one direction: above and below, or left and
 * right (crack_pair's own first tests). The prefilter computes exactly that for eight pixels at once (NEON lanes;
 * elsewhere the same test pixel by pixel, crack_cand) and hands only those pixels, in the same order, to the
 * unchanged per-pixel tests (CF_TEST); a pixel it drops would fail them. So the same pixels are filled in the same
 * order, under the same cap (CF_MAX). Columns past the last whole group of eight take the old per-pixel test.
 * tools/handheld/crack_test.sh: the fill, colour and depth, identical with the switch off and on. OOT_CRACKSCAN=0: the
 * old scan. */
static int sCrackScan SAVESTATE_EXCLUDE = 1;
#if !(defined(__ARM_NEON) || defined(__ARM_NEON__))
static inline int crack_cand(const uint16_t *zb, const uint8_t *cv, int i, int w) { /* the prefilter, one pixel */
    return zb[i] == 0xFFFF && !cv[i] && ((zb[i - w] != 0xFFFF && zb[i + w] != 0xFFFF) ||
                                         (zb[i - 1] != 0xFFFF && zb[i + 1] != 0xFFFF));
}
#endif
static void crack_fill(void) {
    sCrackDirty = false;
    if (!sCrackFill || !sFB || !sZB || sW < 8 || sH < 8 || !sCovMark || sCovFB != sFB) return;
    uint32_t *const fb = sFB; uint16_t *const zb = sZB; const uint8_t *const cv = sCovMark;
    const int w = sW, h = sH, n = w * h;
    enum { CF_MAX = 2048 };
    static int32_t sIdx[CF_MAX] SAVESTATE_EXCLUDE, sStep[CF_MAX] SAVESTATE_EXCLUDE;
    int nf = 0;
#define CF_TEST(i)                                                                                                  \
    if (crack_pair(zb, cv, i, w, 0, n) && crack_closed(zb, cv, i, w, 1)) { sIdx[nf] = i; sStep[nf++] = w; }         \
    else if (crack_pair(zb, cv, i, 1, lo, hi) && crack_closed(zb, cv, i, 1, w)) { sIdx[nf] = i; sStep[nf++] = 1; }
    if (sCrackScan) {
        const int xg = 1 + ((w - 2) / 8) * 8; /* columns 1 .. xg - 1 in groups of eight, xg .. w - 2 one by one */
        for (int y = 1; y < h - 1 && nf < CF_MAX; y++) {
            const int lo = y * w, hi = lo + w;
            int x = 1;
            for (; x < xg && nf < CF_MAX; x += 8) {
                const int i0 = lo + x;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                const uint16x8_t ff = vdupq_n_u16(0xFFFF);
                const uint16x8_t cz = vceqq_u16(vld1q_u16(zb + i0), ff);                 /* no depth */
                const uint32x2_t cz2 = vreinterpret_u32_u16(vorr_u16(vget_low_u16(cz), vget_high_u16(cz)));
                if (!(vget_lane_u32(cz2, 0) | vget_lane_u32(cz2, 1))) continue;       /* all eight have depth */
                const uint16x8_t cu = vceqq_u16(vld1q_u16(zb + i0 - w), ff), cd = vceqq_u16(vld1q_u16(zb + i0 + w), ff);
                const uint16x8_t cl = vceqq_u16(vld1q_u16(zb + i0 - 1), ff), cr = vceqq_u16(vld1q_u16(zb + i0 + 1), ff);
                /* depth above and below, or left and right: not (cu or cd) or not (cl or cr) */
                const uint16x8_t both = vmvnq_u16(vandq_u16(vorrq_u16(cu, cd), vorrq_u16(cl, cr)));
                const uint8x8_t m8 = vand_u8(vmovn_u16(vandq_u16(cz, both)), vceq_u8(vld1_u8(cv + i0), vdup_n_u8(0)));
                uint64_t m = vget_lane_u64(vreinterpret_u64_u8(m8), 0);
                for (int k = 0; m && nf < CF_MAX; k++, m >>= 8) {
                    if (!(m & 0xFF)) continue;
                    const int i = i0 + k;
                    CF_TEST(i)
                }
#else
                for (int k = 0; k < 8 && nf < CF_MAX; k++) {
                    const int i = i0 + k;
                    if (!crack_cand(zb, cv, i, w)) continue;
                    CF_TEST(i)
                }
#endif
            }
            for (; x < w - 1 && nf < CF_MAX; x++) {
                const int i = lo + x;
                if (zb[i] != 0xFFFF || cv[i]) continue;
                CF_TEST(i)
            }
        }
        goto scanned;
    }
    for (int y = 1; y < h - 1 && nf < CF_MAX; y++) {
        const uint16_t *zr = zb + y * w;
        const int lo = y * w, hi = lo + w;
        for (int x = 1; x < w - 1 && nf < CF_MAX; x++) {
            if ((x & 3) == 0 && x + 4 <= w) { /* four depths, none clear: skip them (SWAR: a zero lane of ~v) */
                uint64_t v; memcpy(&v, zr + x, 8); v = ~v;
                if (!((v - 0x0001000100010001ull) & ~v & 0x8000800080008000ull)) { x += 3; continue; }
            }
            if (zr[x] != 0xFFFF || cv[lo + x]) continue;
            const int i = lo + x;
            if (crack_pair(zb, cv, i, w, 0, n) && crack_closed(zb, cv, i, w, 1)) { sIdx[nf] = i; sStep[nf++] = w; }
            else if (crack_pair(zb, cv, i, 1, lo, hi) && crack_closed(zb, cv, i, 1, w)) { sIdx[nf] = i; sStep[nf++] = 1; }
        }
    }
#undef CF_TEST
scanned:
    if (!nf) return;
    target_ready();
    for (int k = 0; k < nf; k++) { /* from the covered pixels only (never a filled one) */
        const int i = sIdx[k], d = sStep[k];
        const uint32_t p = fb[i - d], q = fb[i + d];
        fb[i] = (((p & 0xFEFEFEu) >> 1) + ((q & 0xFEFEFEu) >> 1) + (p & q & 0x010101u)) | 0xFF000000u; /* the mean, down */
        zb[i] = (uint16_t)(((int)zb[i - d] + (int)zb[i + d]) >> 1);
    }
    gfx_soft_crack_fills += (uint32_t)nf;
}

/* gfx_pc direct-call: switch the draw target for the next batch. */
void gfx_scegu_set_sky_pass(bool on);
void gfx_scegu_set_phase(int is3d) {
    sky_settle();
    if ((bool)is3d == sPhase3D) return;
    if (!is3d && sPhase3D && !sSkyPass) edge_vi();
    if (!is3d && sCrackDirty && !sSkyPass) crack_fill();
    if (sSkyPass) { sPhase3D = (bool)is3d; return; } /* unified: one target, stay on the sky buffer */
    if (!sUnified) { /* unified: everything is already on native, nothing to move */
        if (!is3d && sIntDirty) composite_internal();
        if (is3d && sNatDirty) seed_internal_from_native();
    }
    select_target((bool)is3d);
}

static void ensure_targets(void) {
    sky_settle();
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

/* debug.cfg cmneon=alt: hh_renderer.c flips the NEON rows at the end of each PERF window, so one
 * device session times both on the same scenes (F-Zero X's device bench found its NEON path
 * slower than C; OoT's qemu counts can't say). The rows and the C loops draw the same pixels
 * (qsweep), so nothing on screen changes. Returns the state the window just measured used
 * (1 NEON, 0 C) and flips it; -1 when not alternating. */
/* debug.cfg bil64=alt: the same for the one-load bilinear taps (the lead's rule: time every
 * speed change on the device). Same pixels either way (qsweep 39/39). */
int gfx_soft_bil64_alt_step(void) {
#if defined(HH_CM_NEON)
    if (sBil64Alt) {
        const int used = sBil64;
        sBil64 = !sBil64;
        return used;
    }
#endif
    return -1;
}

int gfx_soft_cmneon_alt_step(void) {
#if defined(HH_CM_NEON)
    if (sCmNeonAlt) {
        const int used = sCmNeon;
        sCmNeon = !sCmNeon;
        return used;
    }
#endif
    return -1;
}

static void gfx_soft_init(void) {
    unpack_lut_init();
#if defined(HH_CM_NEON)
    { const char *e = getenv("OOT_CMNEON"); if (e && e[0] == '0') sCmNeon = 0; if (e && e[0] == 'a') sCmNeonAlt = 1; }
    { const char *e = getenv("OOT_BIL64"); if (e && e[0] == '0') sBil64 = 0; if (e && e[0] == 'a') sBil64Alt = 1; }
    { const char *e = getenv("OOT_CMSPEC"); if (e && e[0] == '0') sCmSpec = 0; }
    { const char *e = getenv("OOT_WATERLEAN"); if (e && e[0] == '0') sWaterLean = 0; }   /* default on (the user, 2026-10-01) */
    { const char *e = getenv("OOT_OPAQUELEAN"); if (e && e[0] == '0') sOpaqueLean = 0; } /* default on (the user, 2026-10-01) */
    { const char *e = getenv("OOT_ALPHALEAN"); if (e && e[0] == '0') sAlphaLean = 0; }
    { const char *e = getenv("OOT_SKYLEAN"); if (e && e[0] == '0') sSkyLean = 0; }
    { const char *e = getenv("OOT_LEANZ"); if (e && e[0] == '0') sLeanZ = 0; }
    { const char *e = getenv("OOT_LERPLEAN"); if (e && e[0] == '0') sLerpLean = 0; }
    { const char *e = getenv("OOT_DETCLAMP"); if (e && e[0] == '0') sDetClamp = 0; }
    { const char *e = getenv("OOT_DETLERP"); if (e && e[0] == '0') sDetLerp = 0; }
    { const char *e = getenv("OOT_NOZLEAN"); if (e && e[0] == '0') sNoZLean = 0; }
    { const char *e = getenv("OOT_CLAMPTAP"); if (e && e[0] == '0') sClampTap = 0; }
    { const char *e = getenv("OOT_ALPHASKIP"); if (e && e[0] == '0') sAlphaSkip = 0; }
    { const char *e = getenv("OOT_ZSKIP"); if (e && e[0] == '0') sZSkip = 0; }
    { const char *e = getenv("OOT_FASTROWS"); if (e && e[0] == '0') sFastRows = 0; }
#endif
    { const char *e = getenv("OOT_UILOOP"); if (e && e[0] == '0') sUiLoop = 0; }
    { const char *e = getenv("OOT_PSVCVT"); if (e && e[0] == '0') sPsVcvt = 0; }
    { const char *e = getenv("OOT_2DGEN"); if (e && e[0] == '0') s2dGen = 0; }
    { const char *e = getenv("OOT_2DLERP"); if (e && e[0] == '0') s2dLerp = 0; }
#if defined(HH_2D_NEON)
    { const char *e = getenv("OOT_MAGBIL2D"); if (e && e[0] == '0') sMagBil2D = 0; }
#endif
    { const char *e = getenv("OOT_NEARIN"); if (e && e[0] == '0') sNearIn = 0; }
    { const char *e = getenv("OOT_UINEON"); if (e && e[0] == '0') sUiNeon = 0; }
    { const char *e = getenv("OOT_PLD"); sPld = e && e[0] == '1'; }
    { const char *e = getenv("OOT_TPEFAST"); if (e && e[0] == '0') sTpeFast = 0; }
    { const char *e = getenv("OOT_TPEROWS"); if (e && e[0] == '0') sTpeRows = 0; }
    { const char *e = getenv("OOT_BLENDNEON"); if (e && e[0] == '0') sBlendNeon = 0; }
    { const char *e = getenv("OOT_CRACKFILL"); if (e && e[0] == '0') sCrackFill = 0; }
    { const char *e = getenv("OOT_CRACKSCAN"); if (e && e[0] == '0') sCrackScan = 0; }
    { const char *e = getenv("OOT_EDGETINT"); if (e && e[0] == '0') sEdgeTint = 0; }
#if defined(HH_CM_NEON)
    { const char *e = getenv("OOT_SKYDEFER"); if (e) sSkyDefer = e[0] != '0'; }
#endif
    if (!sTexArena) {
        sTexArena = aligned_alloc(16, TEXARENA_SZ);
        if (!sTexArena) { fprintf(stderr, "gfx_soft: cannot allocate the %u MB texture arena\n", (unsigned) (TEXARENA_SZ >> 20)); abort(); }
    }
    ensure_targets(); select_target(false);
}
static void gfx_soft_on_resize(void) { ensure_targets(); select_target(sPhase3D); }
u32 gfx_soft_frames_rendered SAVESTATE_EXCLUDE; /* frames that reached end_frame (frameskip excluded) */
/* Pause background (oot-PSP contract: z_kaleido_scope_call.c asks for it, then
 * the game stops drawing the world while the menu is up): the last presented
 * frame is kept and every frame starts from it instead of black.
 * The kept frame and its flags are game state (hh_game.h): on the N64 the pause background
 * lives in the game's own heap (PreRender), and a save state taken with the pause menu up
 * must show it again after a load in a fresh process. A static buffer, not malloc. */
#define PAUSE_MAX_PX (320 * 240)
static uint32_t sPauseFB[PAUSE_MAX_PX] HH_GS_BSS;
static int sPauseW HH_GS_BSS, sPauseH HH_GS_BSS;
static bool sPauseActive HH_GS_BSS, sPauseValid HH_GS_BSS;
void gfx_soft_capture_pause_background(void) {
    sky_settle();
    if (!sNatFB || sNatW <= 0 || sNatH <= 0 || sNatW * sNatH > PAUSE_MAX_PX) return;
    nat_ready();
    memcpy(sPauseFB, sNatFB, (size_t)sNatW * sNatH * 4);
    sPauseW = sNatW; sPauseH = sNatH;
    sPauseValid = true;
}
void gfx_soft_set_pause_background_active(bool on) {
    sPauseActive = on;
    if (!on) sPauseValid = false;
}

static void gfx_soft_start_frame(void) {
    sky_settle();
    ovd_frame();
    ensure_targets();
    PROF_BEGIN();
    if (sPauseActive && sPauseValid && sPauseW == sNatW && sPauseH == sNatH) {
        memcpy(sNatFB, sPauseFB, (size_t)sNatW * sNatH * 4);
        sNatStale = false;
    } else {
        sNatStale = true; /* cleared lazily, or never if the frame's first draw covers it */
        sNatFill = 0;
    }
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    const int cz = (sNatW * sNatH > sIntW * sIntH) ? sNatW * sNatH : sIntW * sIntH;
    if (sCrackFill) { /* OOT_CRACKFILL: no 3D coverage yet */
        if (sCovSz < cz) { free(sCov); sCov = (uint8_t *)malloc((size_t)cz); sCovSz = sCov ? cz : 0; }
        if (sCov) memset(sCov, 0, (size_t)sCovSz);
    }
    sEnN = 0; /* OOT_EDGETINT: no notes yet (the marks are cleared as edge_vi uses them) */
    if (sEdgeTint && sEnMarkSz < cz) { free(sEnMark); sEnMark = (uint8_t *)calloc((size_t)cz, 1); sEnMarkSz = sEnMark ? cz : 0; }
    PROF_END(PROF_CLR);
    sIntDirty = false; sNatDirty = false; sHad3D = false; sCrackDirty = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
static void gfx_soft_end_frame(void) {
    sky_settle();
    if (sSkyPass) gfx_scegu_set_sky_pass(false);
    if (sPhase3D) edge_vi();
    if (sCrackDirty && sPhase3D) crack_fill();
    nat_ready(); /* nothing reached native this frame: present it cleared */
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
void gfx_soft_set_fog(bool on, int r, int g, int b, int a) { sFogPx = on; sFogR = r; sFogG = g; sFogB = b; sFogA = a; }
/* round 74: the fog factor from depth (see raster_tri): fog = z * a + b in the SoftVtx z's mapping */
void gfx_soft_set_fogz(bool on, float a, float b) { sFogZ = on; sFogZA = a; sFogZB = b; }
/* Texture edges without FORCE_BL (G_RM_AA_ZB_TEX_EDGE; gfx_pc, OOT_EDGEOPAQUE): the RDP takes coverage
 * from the texel's alpha and never blends such a pixel with memory -- it is written as the texel's own
 * colour, or not at all (the VI's AA softens the edge later). Blending by alpha mixed each root strand's
 * rim with whatever was behind at draw time: the fog-coloured clear, before the wall it hides (Deku
 * tunnel roots, the user's IMG_0162 / IMG_0170: light outlines up close). */
void gfx_soft_set_edge_opaque(bool on) { sEdgeOpaque = on; }
/* OOT_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }
/* gfx_pc's fog plan: the current combine as raster_tri runs it (texture presence aside) */
void gfx_soft_combine_flags(bool *rgb_in, bool *a_tex, bool *a_in) {
    *rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
    *a_tex = sCurShader && sCurShader->a_tex;
    *a_in = !sCurShader || sCurShader->a_in || sForceIn;
}
void gfx_scegu_set_ui_mode(bool on) {
    sUiMode = on;
    if (gfx_soft_hud_box < 0) { const char *e = getenv("OOT_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
}

/* S2DEX background (gfx_pc gfx_s2dex_bg): nearest blit of a 16-bit RGBA5551
 * image in host order (fmt 0: the CPU-decoded JPEG rooms) or a CI8 image with
 * a big-endian RGBA16 TLUT (fmt 1) into the current target. No depth. */
void gfx_soft_draw_bg(const void *img, int img_w, int img_h, int fmt, const uint8_t *tlut, int x0, int y0, int x1,
                      int y1, float u0, float v0, float dudx, float dvdy) {
    sky_settle();
    if (!img || img_w <= 0 || img_h <= 0) return;
    if (fmt == 1 && !tlut) return;
    if (x0 < sScX0) x0 = sScX0; if (y0 < sScY0) y0 = sScY0;
    if (x1 > sScX1) x1 = sScX1; if (y1 > sScY1) y1 = sScY1;
    target_ready();
    cov_rect(x0, y0, x1, y1); /* OOT_CRACKFILL: a prerendered room's picture is covered */
    PROF_BEGIN();
    for (int y = y0; y < y1; y++) {
        int iv = (int)(v0 + (y - y0 + 0.5f) * dvdy);
        if (iv < 0) iv = 0; if (iv >= img_h) iv = img_h - 1;
        uint32_t *row = &sFB[y * sW];
        float u = u0 + 0.5f * dudx;
        for (int x = x0; x < x1; x++, u += dudx) {
            int iu = (int)u;
            if (iu < 0) iu = 0; if (iu >= img_w) iu = img_w - 1;
            uint16_t p;
            if (fmt == 1) {
                const uint8_t idx = ((const uint8_t *)img)[iv * img_w + iu];
                p = (uint16_t)((tlut[idx * 2] << 8) | tlut[idx * 2 + 1]);
            } else {
                p = ((const uint16_t *)img)[iv * img_w + iu];
            }
            const uint32_t r5 = p >> 11, g5 = (p >> 6) & 31, b5 = (p >> 1) & 31;
            row[x] = 0xFF000000u | (((r5 << 3) | (r5 >> 2)) << 16) | (((g5 << 3) | (g5 >> 2)) << 8) | ((b5 << 3) | (b5 >> 2));
        }
    }
    sNatDirty = true;
    PROF_END(PROF_R2D);
}
void gfx_scegu_set_tex_lerp(bool on, uint8_t r, uint8_t g, uint8_t b) { sTexLerp = on; sLerpR = r; sLerpG = g; sLerpB = b; }
/* OOT_SHADELERP (gfx_pc gfx_update_tex_lerp): the lerp runs from the vertex colour (the shade) toward K */
void gfx_scegu_set_tex_lerpv(bool on) { sTexLerpV = on; }

/* Half-resolution sky. OoT draws the skybox first, over the whole screen, and
 * the world then covers most of it: ~57k of the frame's ~190k 3D pixels in
 * Hyrule Field for a soft gradient. Between the game's SKY tags the triangles
 * go to a half-size buffer (a quarter of the pixels) that is then bilinearly
 * doubled into the frame where the sky drew. Unified (100%) targets only;
 * otherwise the tags are ignored. */
static uint32_t *sSkyFB; static uint16_t *sSkyZB; static int sSkyW, sSkyH;
static uint32_t *sSkySaveFB; static uint16_t *sSkySaveZB; static int sSkySaveW, sSkySaveH;
static int sSkySaveVp[4], sSkySaveSc[4];
static int sSkyNeon SAVESTATE_EXCLUDE = 1; /* OOT_SKYNEON=0: the scalar rows (A/B) */

/* One output row of the 4-tap upscale, with the coverage rules: skip where the
 * sky did not draw, nearest where a tap is uncovered (the horizon edge). */
static void sky_row_4tap(uint32_t *d, int W, int y, int sw, int sh) {
    /* output row y samples source row (y - 0.5) / 2: rows j0/j1 weighted 3:1 or 1:3 */
    const int j = y >> 1;
    const int ja = (y & 1) ? j : (j > 0 ? j - 1 : 0), jb = (y & 1) ? (j + 1 < sh ? j + 1 : j) : j;
    const int wya = (y & 1) ? 3 : 1, wyb = 4 - wya;
    const uint32_t *ra = sSkyFB + (ja < sh ? ja : sh - 1) * sw, *rb = sSkyFB + (jb < sh ? jb : sh - 1) * sw;
    const uint32_t *rn = sSkyFB + (j < sh ? j : sh - 1) * sw;
    for (int x = 0; x < W; x++) {
        const int i = x >> 1;
        const int ic = i < sw ? i : sw - 1;
        if (!(rn[ic] >> 24)) continue; /* the sky did not draw here */
        const int ia = (x & 1) ? ic : (ic > 0 ? ic - 1 : 0), ib = (x & 1) ? (ic + 1 < sw ? ic + 1 : ic) : ic;
        const int wxa = (x & 1) ? 3 : 1, wxb = 4 - wxa;
        const uint32_t p00 = ra[ia], p01 = ra[ib], p10 = rb[ia], p11 = rb[ib];
        if (!((p00 & p01 & p10 & p11) >> 24)) { d[x] = rn[ic] | 0xFF000000u; continue; } /* edge: nearest */
        const int w00 = wya * wxa, w01 = wya * wxb, w10 = wyb * wxa, w11 = wyb * wxb; /* sum 16 */
        /* red+blue and green as packed lanes: each lane * 16 still fits its gap */
        const uint32_t rb_ = (((p00 & 0xFF00FFu) * w00 + (p01 & 0xFF00FFu) * w01 + (p10 & 0xFF00FFu) * w10 + (p11 & 0xFF00FFu) * w11) >> 4) & 0xFF00FFu;
        const uint32_t g_ = (((p00 & 0x00FF00u) * w00 + (p01 & 0x00FF00u) * w01 + (p10 & 0x00FF00u) * w10 + (p11 & 0x00FF00u) * w11) >> 4) & 0x00FF00u;
        d[x] = 0xFF000000u | rb_ | g_;
    }
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* One separable row of sky_upscale (every tap covered) in NEON u16 lanes, B G R
 * A per column: vertical wya*a + wyb*b (<= 1020), then (l + 3c) >> 4 and
 * (3c + r) >> 4 per channel (<= 4080 before the shift) -- the same sums as the
 * packed scalar loops, whose lanes never carry into each other, so the bytes
 * are identical. Columns -1 and sw are the clamped copies of 0 and sw-1. */
static void sky_row_sep_neon(uint32_t *d, int W, const uint32_t *ra, const uint32_t *rb, int sw,
                             uint32_t wya, uint32_t wyb) {
    static uint16_t V[(MAP_MAX + 4) * 4] __attribute__((aligned(16)));
    uint16_t *const v = V + 4; /* column i at v[4 * i], i = -1 .. sw */
    const uint8x8_t ka = vdup_n_u8((uint8_t)wya), kb = vdup_n_u8((uint8_t)wyb);
    int i = 0;
    for (; i + 2 <= sw; i += 2) {
        const uint8x8_t a = vld1_u8((const uint8_t *)(ra + i)), b = vld1_u8((const uint8_t *)(rb + i));
        vst1q_u16(v + 4 * i, vmlal_u8(vmull_u8(a, ka), b, kb));
    }
    for (; i < sw; i++) {
        for (int c = 0; c < 4; c++) {
            v[4 * i + c] = (uint16_t)(wya * ((ra[i] >> (8 * c)) & 0xFF) + wyb * ((rb[i] >> (8 * c)) & 0xFF));
        }
    }
    memcpy(v - 4, v, 4 * sizeof(uint16_t));
    memcpy(v + 4 * sw, v + 4 * (sw - 1), 4 * sizeof(uint16_t));
    const uint32x4_t opaque = vdupq_n_u32(0xFF000000u);
    const int pairs = W >> 1;
    i = 0;
    for (; i + 2 <= pairs; i += 2) { /* columns i, i+1 -> pixels 2i .. 2i+3 */
        const uint16x8_t L = vld1q_u16(v + 4 * (i - 1)), C = vld1q_u16(v + 4 * i), R = vld1q_u16(v + 4 * (i + 1));
        const uint16x8_t C3 = vaddq_u16(C, vshlq_n_u16(C, 1));
        const uint8x8_t ev = vshrn_n_u16(vaddq_u16(L, C3), 4), od = vshrn_n_u16(vaddq_u16(C3, R), 4);
        const uint32x2x2_t z = vzip_u32(vreinterpret_u32_u8(ev), vreinterpret_u32_u8(od));
        vst1q_u32(d + 2 * i, vorrq_u32(vcombine_u32(z.val[0], z.val[1]), opaque));
    }
    for (; i < pairs; i++) {
        uint32_t e = 0xFF000000u, o = 0xFF000000u;
        for (int c = 0; c < 3; c++) {
            const uint32_t l = v[4 * (i - 1) + c], m = 3u * v[4 * i + c], r = v[4 * (i + 1) + c];
            e |= ((l + m) >> 4) << (8 * c);
            o |= ((m + r) >> 4) << (8 * c);
        }
        d[2 * i] = e; d[2 * i + 1] = o;
    }
    if (W & 1) { /* odd width: the last pixel is an even x = 2*pairs */
        uint32_t e = 0xFF000000u;
        for (int c = 0; c < 3; c++) e |= ((v[4 * (pairs - 1) + c] + 3u * v[4 * pairs + c]) >> 4) << (8 * c);
        d[2 * pairs] = e;
    }
}
#endif

static void sky_upscale(void) {
    const int W = sSkySaveW, H = sSkySaveH, sw = sSkyW, sh = sSkyH;
    static int sInit;
    if (!sInit) { const char *e = getenv("OOT_SKYNEON"); sInit = 1; if (e && e[0] == '0') sSkyNeon = 0; }
    uint32_t *dst = sSkySaveFB;
    /* rowand[j]: AND of source row j. The sky writes alpha 255 where it draws
     * (0 elsewhere), so a row pair whose ANDs keep alpha has every tap of every
     * pixel covered: the 4-tap rules reduce to plain bilinear there, which is
     * separable and exact -- sum(wy*wx*p) == sum(wx * (wya*pa + wyb*pb)) in
     * integers -- so a vertical pass per source column and a horizontal one
     * per output PAIR give the same bytes. Only rows touching an undrawn
     * source pixel (the horizon; the whole frame when the sky is partial) take
     * the 4-tap path. (It used to run for the whole frame as soon as ONE
     * source pixel was undrawn -- every Hyrule Field frame.) */
    static uint32_t rowand[MAP_MAX], rowor[MAP_MAX];
    static uint32_t vrb[MAP_MAX + 2], vg[MAP_MAX + 2]; /* [1..sw], edges clamped into [0] and [sw+1] */
    if (sw > MAP_MAX || sh > MAP_MAX || sw < 1 || sh < 1 || W > 2 * sw) {
        if (dst == sNatFB) nat_ready();
        for (int y = 0; y < H; y++) sky_row_4tap(dst + y * W, W, y, sw, sh);
        return;
    }
    bool full = true;
    for (int j = 0; j < sh; j++) {
        const uint32_t *r = sSkyFB + j * sw;
        uint32_t acc = 0xFFFFFFFFu, any = 0;
        for (int i = 0; i < sw; i++) { acc &= r[i]; any |= r[i]; }
        rowand[j] = acc; rowor[j] = any;
        full &= (acc >> 24) != 0;
    }
    /* full: every native pixel is written below, so native needs no clear */
    if (full && dst == sNatFB && W == sNatW && H == sNatH) sNatStale = false; else if (dst == sNatFB) nat_ready();
    for (int y = 0; y < H; y++) {
        const int j = y >> 1;
        const int ja = (y & 1) ? j : (j > 0 ? j - 1 : 0), jb = (y & 1) ? (j + 1 < sh ? j + 1 : j) : j;
        const int ca = ja < sh ? ja : sh - 1, cb = jb < sh ? jb : sh - 1;
        uint32_t *d = dst + y * W;
        /* the sky drew nothing on this row's centre source row: sky_row_4tap
         * would skip every pixel (rows under the horizon) */
        if (!(rowor[j < sh ? j : sh - 1] >> 24)) continue;
        if (!((rowand[ca] & rowand[cb]) >> 24)) { sky_row_4tap(d, W, y, sw, sh); continue; }
        const uint32_t wya = (y & 1) ? 3 : 1, wyb = 4 - wya;
        const uint32_t *ra = sSkyFB + ca * sw, *rb = sSkyFB + cb * sw;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (sSkyNeon) { sky_row_sep_neon(d, W, ra, rb, sw, wya, wyb); continue; }
#endif
        for (int i = 0; i < sw; i++) {
            vrb[i + 1] = wya * (ra[i] & 0xFF00FFu) + wyb * (rb[i] & 0xFF00FFu);
            vg[i + 1] = wya * (ra[i] & 0x00FF00u) + wyb * (rb[i] & 0x00FF00u);
        }
        vrb[0] = vrb[1]; vg[0] = vg[1];                /* x = 0 takes column 0 twice */
        vrb[sw + 1] = vrb[sw]; vg[sw + 1] = vg[sw];     /* the last odd x takes column sw-1 twice */
        /* x = 2i: columns i-1, i weighted 1:3;  x = 2i+1: columns i, i+1 weighted 3:1 */
        const int pairs = W >> 1;
        for (int i = 0; i < pairs; i++) {
            const uint32_t l_rb = vrb[i], c_rb = vrb[i + 1], r_rb = vrb[i + 2];
            const uint32_t l_g = vg[i], c_g = vg[i + 1], r_g = vg[i + 2];
            const uint32_t c3_rb = c_rb * 3, c3_g = c_g * 3;
            d[2 * i] = 0xFF000000u | (((l_rb + c3_rb) >> 4) & 0xFF00FFu) | (((l_g + c3_g) >> 4) & 0x00FF00u);
            d[2 * i + 1] = 0xFF000000u | (((c3_rb + r_rb) >> 4) & 0xFF00FFu) | (((c3_g + r_g) >> 4) & 0x00FF00u);
        }
        if (W & 1) { /* odd width: the last pixel is an even x = 2*pairs */
            const int i = pairs;
            d[2 * i] = 0xFF000000u | (((vrb[i] + vrb[i + 1] * 3) >> 4) & 0xFF00FFu) | (((vg[i] + vg[i + 1] * 3) >> 4) & 0x00FF00u);
        }
    }
}

void gfx_scegu_set_sky_pass(bool on) {
    sSkyTag = on; /* OOT_CRACKFILL: the sky is no coverage */
#if defined(HH_CM_NEON)
    sDsTag = on; /* OOT_SKYDEFER keeps the full-resolution sky's rows (the tags change no target or pixel then) */
#endif
    if (gfx_trace_frames > 0) port_log("  soft: sky pass %d (unified %d nat %d)\n", (int) on, (int) sUnified, (int) (sFB == sNatFB));
    if (on == sSkyPass) return;
    if (on) {
        /* The half-resolution sky (a speed trick, ~3 ms desktop) is opt-in since round 78: the user's rule
         * (2026-09-30, all ports) is correct pixels first -- "i'd rather slower but correct visuals". The sky
         * draws at full resolution unless debug.cfg fullsky=0 (OOT_FULLSKY=0) asks for the half-res pass. */
        static int sHalfSky SAVESTATE_EXCLUDE = -1;
        if (sHalfSky < 0) { const char *e = getenv("OOT_FULLSKY"); sHalfSky = e != NULL && e[0] == '0'; }
        if (!sUnified || sFB != sNatFB || !sHalfSky) return;
        sky_settle(); /* the half-res pass switches the target */
        const int w = (sNatW + 1) / 2, h = (sNatH + 1) / 2;
        if (sSkyW != w || sSkyH != h) {
            free(sSkyFB); free(sSkyZB);
            sSkyFB = (uint32_t *)malloc((size_t)w * h * 4);
            sSkyZB = (uint16_t *)malloc((size_t)w * h * 2);
            if (!sSkyFB || !sSkyZB) { sSkyW = sSkyH = 0; return; }
            sSkyW = w; sSkyH = h;
        }
        memset(sSkyFB, 0, (size_t)sSkyW * sSkyH * 4);
        memset(sSkyZB, 0xFF, (size_t)sSkyW * sSkyH * 2);
        sSkySaveFB = sFB; sSkySaveZB = sZB; sSkySaveW = sW; sSkySaveH = sH;
        sSkySaveVp[0] = sVpX; sSkySaveVp[1] = sVpY; sSkySaveVp[2] = sVpW; sSkySaveVp[3] = sVpH;
        sSkySaveSc[0] = sScX0; sSkySaveSc[1] = sScY0; sSkySaveSc[2] = sScX1; sSkySaveSc[3] = sScY1;
        sSkyFull[0] = sVpX; sSkyFull[1] = sVpY; sSkyFull[2] = sVpW; sSkyFull[3] = sVpH;
        sSkyFull[4] = sScX0; sSkyFull[5] = sScY0; sSkyFull[6] = sScX1 - sScX0; sSkyFull[7] = sScY1 - sScY0;
        sFB = sSkyFB; sZB = sSkyZB; sW = sSkyW; sH = sSkyH;
        sSkyPass = true;
        gfx_soft_set_viewport(sSkyFull[0], sSkyFull[1], sSkyFull[2], sSkyFull[3]);
        gfx_soft_set_scissor(sSkyFull[4], sSkyFull[5], sSkyFull[6], sSkyFull[7]);
    } else {
        sky_settle();
        PROF_BEGIN();
        sSkyPass = false;
        sFB = sSkySaveFB; sZB = sSkySaveZB; sW = sSkySaveW; sH = sSkySaveH;
        sky_upscale();
        /* the full-resolution state, including any viewport/scissor set during the pass */
        sVpX = sSkyFull[0]; sVpY = sSkyFull[1]; sVpW = sSkyFull[2]; sVpH = sSkyFull[3];
        gfx_soft_set_scissor(sSkyFull[4], sSkyFull[5], sSkyFull[6], sSkyFull[7]);
        (void)sSkySaveVp; (void)sSkySaveSc;
        PROF_END(PROF_R3D);
    }
}

/* Depth reads for the game (sun lens flare, light glows: z_kankyo.c /
 * z_lights.c). x, y are 320x240 game coordinates; the buffer holds the last
 * rendered 3D frame (the game asks during its update, before this frame is
 * drawn, as on the N64). Returns false off-buffer. */
static bool hh_read_depth(int32_t x, int32_t y, uint16_t *depth) {
    const bool uni = sUnified || sIntZB == NULL;
    const uint16_t *zb = uni ? sNatZB : sIntZB;
    const int w = uni ? sNatW : sIntW, h = uni ? sNatH : sIntH;
    if (zb == NULL || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= 320 || y >= 240) return false;
    extern int gfx_depth_read_x(int x, int w, int h); /* gfx_pc.c: where the 3D drew N64 x (OOT_FLARE) */
    const int bx = gfx_depth_read_x(x, w, h);
    if (bx < 0) return false;
    *depth = zb[(y * h / 240) * w + bx];
    return true;
}
bool gfx_soft_depth_is_clear(int32_t x, int32_t y) {
    uint16_t d;
    return hh_read_depth(x, y, &d) && d == 0xFFFF;
}
/* projectedZ is NDC z/w; the rasterizer stores clamp(z, 0, 1) * 65535 of what
 * gfx_pc emits -- z/w * 0.5 + 0.5 (OOT_NEARCLIP, the full depth range) or z/w
 * itself (the kit's) -- so compare in the same terms. */
bool gfx_depth_full_range(void);
bool gfx_soft_depth_test(int32_t x, int32_t y, float projectedZ) {
    uint16_t d;
    float z = gfx_depth_full_range() ? projectedZ * 0.5f + 0.5f : projectedZ;
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
static int sBlendFast SAVESTATE_EXCLUDE = -1; /* OOT_BLENDFAST=0: the per-texel loop, for A/B */
/* A tile 1 COARSER than tile 0 (shift 1..10: one tile-1 texel spans 2^shift output texels)
 * is read bilinearly when the draw filters, as the RDP filters it at its own scale: the
 * nearest read baked each tile-1 texel into a flat 2^shift square, which the rasterizer's
 * bilinear only softened at the edges -- Lake Hylia's bed and shore came out in big squares
 * where the console shows smooth blobs (the user's v32 photo, "looks really pixelated").
 * The tile-1 knots land on output texels, so the rasterizer's bilinear over the pre-blend
 * then gives the console's two-scale result. Returns the tap pair and weight (0..256). */
static inline void hh_tile_taps(int c, int f0, int shift, int corner, int f1, int size, bool filt,
                                int *i0, int *i1, int *w) {
    if (filt && shift >= 1 && shift <= 8) {
        /* In 1/256 of a tile-1 texel, with both corners' quarter texels (f0: tile 0's, f1:
         * tile 1's): the game scrolls these layers a quarter texel at a time, and whole-texel
         * corners made a 4x-coarser layer stand, then jump 4 texels (the user, v33: "the
         * texture moves"). */
        const int pos = ((((c << 2) + f0) << 6) >> shift) - ((corner << 8) + (f1 << 6));
        *w = pos & 255;
        *i0 = hh_wrap(pos >> 8, size);
        *i1 = hh_wrap((pos >> 8) + 1, size);
        return;
    }
    *i0 = *i1 = hh_wrap(hh_tile_coord(c, shift) - corner, size);
    *w = 0;
}
/* a + (b - a) * w / 256 on all four ARGB bytes, two lanes a multiply (w 0..256; w 0 = a exactly). */
static inline uint32_t hh_lerp_argb(uint32_t a, uint32_t b, int w) {
    const uint32_t iw = 256u - (uint32_t)w, uw = (uint32_t)w;
    const uint32_t rb = (((a & 0xff00ffu) * iw + (b & 0xff00ffu) * uw) >> 8) & 0xff00ffu;
    const uint32_t ag = ((a >> 8) & 0xff00ffu) * iw + ((b >> 8) & 0xff00ffu) * uw;
    return rb | (ag & 0xff00ff00u);
}
static int sBlendBil SAVESTATE_EXCLUDE = -1; /* OOT_BLENDBIL=0: the nearest tile-1 read, for A/B */
/* OOT_BLENDNEON (2026-10-04, the final sweep): gfx_soft_blend_textures's per-texel selections and hh_lerp8s in NEON
 * lanes (B G R A), as tpe_eval_fast's ARM loop evaluates cycle 1: each lane is P + ((Q - P) * K) / 255 toward zero,
 * P the lane's first source (tile 0's, or tile 1's when the mode swaps), Q the other, K the lane's factor (the constant,
 * tile 0's or tile 1's alpha; 0 where the mode does not lerp: P + 0 = P), each picked by VTBL from a 24-byte table
 * (bytes 0-3 tile 0's texel, 4-7 tile 1's, 8-11 the constants B G R A, 12 a zero); x / 255 toward zero as
 * sign * ((|x| * 0x8081) >> 23) (exact for |x| <= 65025: dl_test's vc check), P + it in [0, 255]. Tile 0's row is read
 * once a row (texel_at: the same words). The lava (Volvagia's room, the Fire Temple), the lakes' and fountains' water,
 * scrolling two-texture layers re-blended every frame: ~100 instructions a texel in the scalar loops. tools/handheld/
 * tpe_test.sh compares it with them (blend calls). OOT_BLENDNEON=0: the scalar loops. */
static uint32_t sBRow0[BLEND_MAXW] SAVESTATE_EXCLUDE, sBRow1[BLEND_MAXW] SAVESTATE_EXCLUDE; /* its rows */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
typedef struct { uint8x8_t via, vic; uint8x8x3_t T; } BlendN;
static void blend_n_setup(BlendN *b, bool lerp_rgb, bool swap_rgb, bool lerp_a, bool swap_a, int krgb_src,
                          const uint8_t krgb[3], int ka_src, uint8_t ka) {
    uint8_t tab[24] = { 0 }, ia[8], ic[8];
    tab[8] = krgb[2]; tab[9] = krgb[1]; tab[10] = krgb[0]; tab[11] = ka; /* lanes B G R A; tab[12] = 0 */
    for (int lane = 0; lane < 4; lane++) {
        uint8_t P, Q, K;
        if (lane < 3) {
            P = (uint8_t)(swap_rgb ? 4 + lane : lane); Q = (uint8_t)(swap_rgb ? lane : 4 + lane);
            K = !lerp_rgb ? 12 : !krgb_src ? (uint8_t)(8 + lane) : krgb_src == 1 ? 3 : 7;
        } else {
            P = swap_a ? 7 : 3; Q = swap_a ? 3 : 7;
            K = !lerp_a ? 12 : ka_src == 1 ? 3 : ka_src == 2 ? 7 : 11;
        }
        ia[lane] = Q; ia[4 + lane] = P; ic[lane] = K; ic[4 + lane] = P; /* tpe's a, b / c, d: (a - b) * c / 255 + d */
    }
    b->via = vld1_u8(ia); b->vic = vld1_u8(ic);
    b->T.val[1] = vld1_u8(tab + 8); b->T.val[2] = vld1_u8(tab + 16);
}
static inline uint32_t blend_n_texel(BlendN *b, uint32_t p0, uint32_t p1) {
    b->T.val[0] = vreinterpret_u8_u64(vcreate_u64(((uint64_t)p1 << 32) | p0));
    const int16x8_t ab = vreinterpretq_s16_u16(vmovl_u8(vtbl3_u8(b->T, b->via))); /* Q lanes 0-3, P 4-7 */
    const int16x8_t cd = vreinterpretq_s16_u16(vmovl_u8(vtbl3_u8(b->T, b->vic))); /* K, P */
    const int32x4_t pr = vmull_s16(vsub_s16(vget_low_s16(ab), vget_high_s16(ab)), vget_low_s16(cd));
    const int32x4_t q = vreinterpretq_s32_u32(vshrq_n_u32(vmulq_u32(vreinterpretq_u32_s32(vabsq_s32(pr)), vdupq_n_u32(0x8081)), 23));
    const int32x4_t v = vaddw_s16(vbslq_s32(vcgtq_s32(vdupq_n_s32(0), pr), vnegq_s32(q), q), vget_high_s16(cd));
    const uint16x4_t n = vqmovun_s32(v);
    return vget_lane_u32(vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(n, n))), 0);
}
#endif
bool gfx_soft_blend_textures(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                             int uls1, int ult1, int shs1, int sht1, int rgb_mode, int a_mode,
                             int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka, bool filt, uint32_t qfrac) {
    if (dst == 0 || dst >= MAX_TEXTURES || id0 >= MAX_TEXTURES || id1 >= MAX_TEXTURES) return false;
    const SoftTex *t0 = &sTex[id0], *t1 = &sTex[id1];
    if (!t0->valid || !t1->valid || W <= 0 || H <= 0 || W * H > 256 * 256) return false;
    uint32_t *out;
    sTexGen[dst]++; /* rewritten, possibly in place: box row cache key */
    if (sTex[dst].valid && !sTex[dst].is16 && sTex[dst].w == W && sTex[dst].h == H) {
        out = (uint32_t *)sTex[dst].px;
        sky_texels_change(out); /* rewritten in place: a kept sky triangle may read them */
    } else {
        const size_t bytes = ((size_t)W * H * 4 + 15u) & ~(size_t)15;
        if (sArenaUsed + bytes > TEXARENA_SZ) return false;
        out = (uint32_t *)&sTexArena[sArenaUsed];
        sArenaUsed += bytes;
        sTex[dst].px = out; sTex[dst].is16 = false; sTex[dst].w = W; sTex[dst].h = H; sTex[dst].valid = true;
    }
    if (sBlendFast < 0) { const char *e = getenv("OOT_BLENDFAST"); sBlendFast = !(e && e[0] == '0'); }
    if (sBlendBil < 0) { const char *e = getenv("OOT_BLENDBIL"); sBlendBil = !(e && e[0] == '0'); }
    if (!sBlendBil) filt = false;
    if (filt && W <= BLEND_MAXW && ((shs1 >= 1 && shs1 <= 8) || (sht1 >= 1 && sht1 <= 8))) {
        /* The fast loop's selections and lerps, with tile 1 read through hh_tile_taps. */
        static int sC0[BLEND_MAXW] SAVESTATE_EXCLUDE, sCa[BLEND_MAXW] SAVESTATE_EXCLUDE,
                   sCb[BLEND_MAXW] SAVESTATE_EXCLUDE, sCw[BLEND_MAXW] SAVESTATE_EXCLUDE;
        static uint32_t sRow1[BLEND_MAXW] SAVESTATE_EXCLUDE; /* tile 1's row, lerped between its two rows */
        if (t1->w > BLEND_MAXW) return false;
        const bool lerp_rgb = rgb_mode >= 2, swap_rgb = rgb_mode == 1 || rgb_mode == 3;
        const bool lerp_a = a_mode >= 2, swap_a = a_mode == 1 || a_mode == 3;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const bool bn_on = sBlendNeon && t0->w <= BLEND_MAXW; BlendN bn;
        if (bn_on) blend_n_setup(&bn, lerp_rgb, swap_rgb, lerp_a, swap_a, krgb_src, krgb, ka_src, ka);
#endif
        for (int x = 0; x < W; x++) {
            sC0[x] = x % t0->w;
            hh_tile_taps(base_s + x, qfrac & 3, shs1, uls1, (qfrac >> 4) & 3, t1->w, true, &sCa[x], &sCb[x], &sCw[x]);
        }
        for (int y = 0; y < H; y++) {
            int ra, rb, wt;
            const int r0 = (y % t0->h) * t0->w;
            hh_tile_taps(base_t + y, (qfrac >> 2) & 3, sht1, ult1, (qfrac >> 6) & 3, t1->h, true, &ra, &rb, &wt);
            ra *= t1->w;
            rb *= t1->w;
            for (int i = 0; i < t1->w; i++) {
                sRow1[i] = hh_lerp_argb(texel_at(t1->px, t1->is16, ra + i), texel_at(t1->px, t1->is16, rb + i), wt);
            }
            uint32_t *const orow = out + y * W;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            if (bn_on) { /* OOT_BLENDNEON */
                const int n0 = t0->w < W ? t0->w : W; /* sC0[x] = x % w < n0 */
                for (int i = 0; i < n0; i++) sBRow0[i] = texel_at(t0->px, t0->is16, r0 + i);
                for (int x = 0; x < W; x++)
                    orow[x] = blend_n_texel(&bn, sBRow0[sC0[x]], hh_lerp_argb(sRow1[sCa[x]], sRow1[sCb[x]], sCw[x]));
                continue;
            }
#endif
            for (int x = 0; x < W; x++) {
                const uint32_t p0 = texel_at(t0->px, t0->is16, r0 + sC0[x]);
                const uint32_t p1 = hh_lerp_argb(sRow1[sCa[x]], sRow1[sCb[x]], sCw[x]);
                const int a0 = p0 >> 24, a1 = p1 >> 24;
                const uint32_t p = swap_rgb ? p1 : p0, q = swap_rgb ? p0 : p1;
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
    if (W <= BLEND_MAXW && sBlendFast) {
        /* The loop below with its per-texel work hoisted: the column wraps
         * (two divides a texel) come from a table, the mode tests pick a
         * source and a lerp once. Same selections, same hh_lerp8: identical
         * texels. (Kokiri's scrolling water re-blends 64x64 every frame at
         * ~190 instructions a texel under qemu.) OOT_BLENDFAST=0: the old loop. */
        static int sCol0[BLEND_MAXW] SAVESTATE_EXCLUDE, sCol1[BLEND_MAXW] SAVESTATE_EXCLUDE;
        const bool lerp_rgb = rgb_mode >= 2, swap_rgb = rgb_mode == 1 || rgb_mode == 3;
        const bool lerp_a = a_mode >= 2, swap_a = a_mode == 1 || a_mode == 3;
        for (int x = 0; x < W; x++) {
            sCol0[x] = x % t0->w;
            sCol1[x] = hh_wrap(hh_tile_coord(base_s + x, shs1) - uls1, t1->w);
        }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const bool bn_on = sBlendNeon && t0->w <= BLEND_MAXW && t1->w <= BLEND_MAXW; BlendN bn;
        if (bn_on) blend_n_setup(&bn, lerp_rgb, swap_rgb, lerp_a, swap_a, krgb_src, krgb, ka_src, ka);
#endif
        for (int y = 0; y < H; y++) {
            const int r0 = (y % t0->h) * t0->w;
            const int r1 = hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w;
            uint32_t *const orow = out + y * W;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            if (bn_on) { /* OOT_BLENDNEON: both rows read once (sCol0[x] < min(w0, W), sCol1[x] < w1) */
                const int n0 = t0->w < W ? t0->w : W, n1 = t1->w;
                for (int i = 0; i < n0; i++) sBRow0[i] = texel_at(t0->px, t0->is16, r0 + i);
                for (int i = 0; i < n1; i++) sBRow1[i] = texel_at(t1->px, t1->is16, r1 + i);
                for (int x = 0; x < W; x++) orow[x] = blend_n_texel(&bn, sBRow0[sCol0[x]], sBRow1[sCol1[x]]);
                continue;
            }
#endif
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
/* Cycle 1 of the colour combiner evaluated per texel, for the forms the
 * cc_id model cannot hold (an alpha or PRIM_LOD_FRAC input in C, a tile mix
 * gfx_soft_blend_textures has no mode for) when cycle 1 reads only the two
 * tiles and constants: torch flames' (T1 - PRIM) * PRIM_LOD_FRAC + T0,
 * get-item models' (T0 - PRIM) * PRIM_LOD_FRAC + T0, ... gfx_pc then draws
 * the result as TEXEL0. m[8] = the cycle-1 muxes (rgb a b c d, alpha a b c d);
 * k[] = prim r g b a, env r g b a, prim_lod_frac. keep_alpha: the alpha
 * combiner stays in the cc_id and reads tile 0's alpha, copied through. Same
 * texel domain and tile-1 addressing as gfx_soft_blend_textures, and the
 * renderer's (a - b) * c / 255 + d. id1 = 0: no tile 1. */
static inline int tpe_rgb(uint8_t mux, int slot, uint32_t p0, uint32_t p1, const uint8_t *k, int ch) {
    const int sh = 16 - ch * 8;
    switch (mux) {
        case G_CCMUX_TEXEL0: return (p0 >> sh) & 0xff;
        case G_CCMUX_TEXEL1: return (p1 >> sh) & 0xff;
        case G_CCMUX_PRIMITIVE: return k[ch];
        case G_CCMUX_ENVIRONMENT: return k[4 + ch];
    }
    if (slot == 2) {
        switch (mux) {
            case G_CCMUX_TEXEL0_ALPHA: return p0 >> 24;
            case G_CCMUX_TEXEL1_ALPHA: return p1 >> 24;
            case G_CCMUX_PRIMITIVE_ALPHA: return k[3];
            case G_CCMUX_ENV_ALPHA: return k[7];
            case G_CCMUX_PRIM_LOD_FRAC: return k[8];
        }
        return 0;
    }
    return ((slot == 0 || slot == 3) && mux == G_CCMUX_1) ? 255 : 0;
}
static inline int tpe_a(uint8_t mux, int slot, uint32_t p0, uint32_t p1, const uint8_t *k) {
    switch (mux) {
        case G_ACMUX_TEXEL0: return p0 >> 24;
        case G_ACMUX_TEXEL1: return p1 >> 24;
        case G_ACMUX_PRIMITIVE: return k[3];
        case G_ACMUX_ENVIRONMENT: return k[7];
        case G_ACMUX_1: return slot == 2 ? k[8] : 255; /* 6 in C = PRIM_LOD_FRAC */
    }
    return 0;
}
static inline int tpe_abcd(int a, int b, int c, int d) {
    const int v = ((a - b) * c) / 255 + d;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}
/* OOT_TPEFAST (2026-10-02, FPS round 2): gfx_soft_eval_texels with each operand's source chosen once a call. The torch
 * flames scroll, so their cycle 1 is evaluated again every frame (the basement view's profile: ~1% of the frame here),
 * and the loop walked tpe_rgb / tpe_a's switches for 16 operands a texel, reloading the muxes and constants after every
 * texel store (-fno-strict-aliasing), and took x % w (a divide) and the 16-bit test for each tile-0 read. Here: per
 * operand a source index into the texel's values {t0, t1, t0 alpha, t1 alpha} or 4 + the slot (the constant, or the
 * 0 / 255 literal, that tpe_rgb / tpe_a return for that mux and slot), the same (a - b) * c / 255 + d and clamp, the
 * tile-0 column stepped and wrapped (x % w for x >= 0), the texels read in the same order before each texel's store.
 * tools/handheld/tpe_test.sh compares it with the old loop on every mux of every slot. OOT_TPEFAST=0: the old loop
 * (sTpeFast, with the switches above raster_tri). */
static inline int tpe_src_rgb(uint8_t mux, int slot, const uint8_t *k, int ch, int *kv) {
    switch (mux) {
        case G_CCMUX_TEXEL0: return 0;
        case G_CCMUX_TEXEL1: return 1;
        case G_CCMUX_PRIMITIVE: *kv = k[ch]; return 4 + slot;
        case G_CCMUX_ENVIRONMENT: *kv = k[4 + ch]; return 4 + slot;
    }
    if (slot == 2) {
        switch (mux) {
            case G_CCMUX_TEXEL0_ALPHA: return 2;
            case G_CCMUX_TEXEL1_ALPHA: return 3;
            case G_CCMUX_PRIMITIVE_ALPHA: *kv = k[3]; return 4 + slot;
            case G_CCMUX_ENV_ALPHA: *kv = k[7]; return 4 + slot;
            case G_CCMUX_PRIM_LOD_FRAC: *kv = k[8]; return 4 + slot;
        }
        *kv = 0; return 4 + slot;
    }
    *kv = ((slot == 0 || slot == 3) && mux == G_CCMUX_1) ? 255 : 0;
    return 4 + slot;
}
static inline int tpe_src_a(uint8_t mux, int slot, const uint8_t *k, int *kv) {
    switch (mux) {
        case G_ACMUX_TEXEL0: return 2;
        case G_ACMUX_TEXEL1: return 3;
        case G_ACMUX_PRIMITIVE: *kv = k[3]; return 4 + slot;
        case G_ACMUX_ENVIRONMENT: *kv = k[7]; return 4 + slot;
        case G_ACMUX_1: *kv = slot == 2 ? k[8] : 255; return 4 + slot;
    }
    *kv = 0; return 4 + slot;
}
static void tpe_eval_fast(uint32_t *out, const SoftTex *t0, const SoftTex *t1, int W, int H, int base_t, int ult1,
                          int sht1, const int *col1, const uint8_t m[8], const uint8_t k[9], bool keep_alpha) {
    int sel[4][4], val[4][8]; /* [channel: r g b a][slot]: the source; [channel][4 + slot]: its constant */
    for (int ch = 0; ch < 4; ch++)
        for (int sl = 0; sl < 4; sl++) {
            int kv = 0;
            sel[ch][sl] = ch < 3 ? tpe_src_rgb(m[sl], sl, k, ch, &kv) : tpe_src_a(m[4 + sl], sl, k, &kv);
            val[ch][4 + sl] = kv;
        }
    const void *const px0 = t0->px; const bool is16_0 = t0->is16; const int w0 = t0->w, h0 = t0->h;
    const void *const px1 = t1 != NULL ? t1->px : NULL; const bool is16_1 = t1 != NULL && t1->is16;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* ARM: the four channels in lanes (B G R A, the texel's byte order), each operand by VTBL from a 24-byte table:
     * bytes 0-3 tile 0's texel, 4-7 tile 1's, 8 + 4 * slot + lane the slot's constant (gfx_vc_eval's way, round 1);
     * keep_alpha as alpha = (0 - 0) * 0 / 255 + tile 0's alpha. (a - b) * c in int32 as the C (|x| <= 65025), x / 255
     * toward zero as sign * ((|x| * 0x8081) >> 23) (exact there: dl_test vc checks all 130,051 products), + d, the
     * clamp as two saturating narrows (< 0 -> 0, > 255 -> 255). */
    {
        static const uint8_t kLaneCh[4] = { 2, 1, 0, 3 }; /* lane B G R A <- channel r g b a's index */
        uint8_t tab[24] = { 0 }, ia[8], ic[8];
        for (int lane = 0; lane < 4; lane++) {
            const int ch = kLaneCh[lane];
            for (int sl = 0; sl < 4; sl++) {
                int sx = sel[ch][sl];
                if (ch == 3 && keep_alpha) { sx = sl == 3 ? 0 : 4 + sl; val[3][4 + sl] = 0; }
                const uint8_t idx = sx == 0 ? (uint8_t)lane : sx == 1 ? (uint8_t)(4 + lane) : sx == 2 ? 3 : sx == 3 ? 7
                                  : (uint8_t)(8 + 4 * sl + lane);
                if (sx >= 4) tab[8 + 4 * sl + lane] = (uint8_t)val[ch][4 + sl];
                if (sl < 2) ia[sl * 4 + lane] = idx; else ic[(sl - 2) * 4 + lane] = idx;
            }
        }
        const uint8x8_t via = vld1_u8(ia), vic = vld1_u8(ic);
        uint8x8x3_t T;
        T.val[1] = vld1_u8(tab + 8); T.val[2] = vld1_u8(tab + 16);
        const uint32x4_t m8081 = vdupq_n_u32(0x8081);
/* one texel from p0, p1 into orow[x] */
#define TPE_TEXEL_N                                                                                                      \
                T.val[0] = vcreate_u8(((uint64_t)p1 << 32) | p0);                                                        \
                const int16x8_t ab = vreinterpretq_s16_u16(vmovl_u8(vtbl3_u8(T, via))); /* a lanes 0-3, b 4-7 */         \
                const int16x8_t cd = vreinterpretq_s16_u16(vmovl_u8(vtbl3_u8(T, vic))); /* c, d */                       \
                const int32x4_t pr = vmull_s16(vsub_s16(vget_low_s16(ab), vget_high_s16(ab)), vget_low_s16(cd));        \
                const int32x4_t q = vreinterpretq_s32_u32(vshrq_n_u32(vmulq_u32(vreinterpretq_u32_s32(vabsq_s32(pr)), m8081), 23)); \
                const int32x4_t v = vaddw_s16(vbslq_s32(vcltq_s32(pr, vdupq_n_s32(0)), vnegq_s32(q), q), vget_high_s16(cd)); \
                const uint8x8_t o = vqmovn_u16(vcombine_u16(vqmovun_s32(v), vdup_n_u16(0)));                            \
                vst1_lane_u32(orow + x, vreinterpret_u32_u8(o), 0);
        /* OOT_TPEROWS (2026-10-04, the flame overlays): each row's texels read into 32-bit rows first -- tile 0's
         * min(W, w0) columns (all x % w0 reads), tile 1's whole row (col1[x] < t1->w) -- then the texel loop reads
         * those words: the same texels (texel_at's unpack, or the word itself), without the per-texel 16-bit tests
         * and LUT unpacks (tile 1's 32 columns read twice over a 64-texel row). The flames' cycle 1 (gEffFire1DL: an
         * I8 tile 0 mirrored to 64x128, an I4 tile 1) is evaluated again for every scroll: ~5 a frame in Volvagia's
         * bursts (EffectSsKFire), 64x128 each. Taken when both rows fit BLEND_MAXW; OOT_TPEROWS=0: per texel. */
        static uint32_t sTpeRow0[BLEND_MAXW] SAVESTATE_EXCLUDE, sTpeRow1[BLEND_MAXW] SAVESTATE_EXCLUDE;
        const int n0 = W < w0 ? W : w0;
        const bool rows = sTpeRows && n0 <= BLEND_MAXW && (t1 == NULL || t1->w <= BLEND_MAXW);
        for (int y = 0; y < H; y++) {
            const int r0 = (y % h0) * w0;
            const int r1 = t1 != NULL ? hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w : 0;
            uint32_t *const orow = out + y * W;
            int x0 = 0; /* x % w0 */
            if (rows) {
                const uint32_t *q0 = (const uint32_t *)px0 + r0, *q1 = t1 != NULL && !is16_1 ? (const uint32_t *)px1 + r1 : sTpeRow1;
                if (is16_0) { for (int i = 0; i < n0; i++) sTpeRow0[i] = texel_at(px0, true, r0 + i); q0 = sTpeRow0; }
                if (t1 != NULL && is16_1) { const int w1 = t1->w; for (int i = 0; i < w1; i++) sTpeRow1[i] = texel_at(px1, true, r1 + i); }
                if (t1 == NULL) sTpeRow1[0] = 0;
                const int *const c1 = t1 != NULL ? col1 : NULL;
                for (int x = 0; x < W; x++) {
                    const uint32_t p0 = q0[x0], p1 = q1[c1 != NULL ? c1[x] : 0];
                    if (++x0 >= w0) x0 = 0; /* (x0 < w0: the same as == w0) */
                    TPE_TEXEL_N
                }
                continue;
            }
            for (int x = 0; x < W; x++) {
                const uint32_t p0 = is16_0 ? texel_at(px0, true, r0 + x0) : ((const uint32_t *)px0)[r0 + x0];
                const uint32_t p1 = t1 != NULL ? (is16_1 ? texel_at(px1, true, r1 + col1[x]) : ((const uint32_t *)px1)[r1 + col1[x]]) : 0;
                if (++x0 == w0) x0 = 0;
                TPE_TEXEL_N
            }
        }
#undef TPE_TEXEL_N
        return;
    }
#endif
    for (int y = 0; y < H; y++) {
        const int r0 = (y % h0) * w0;
        const int r1 = t1 != NULL ? hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w : 0;
        uint32_t *const orow = out + y * W;
        int x0 = 0; /* x % w0 */
        for (int x = 0; x < W; x++) {
            const uint32_t p0 = texel_at(px0, is16_0, r0 + x0);
            const uint32_t p1 = t1 != NULL ? texel_at(px1, is16_1, r1 + col1[x]) : 0;
            x0 = x0 + 1 == w0 ? 0 : x0 + 1;
            int c[4];
            for (int ch = 0; ch < 4; ch++) {
                if (ch == 3 && keep_alpha) { c[3] = (int)(p0 >> 24); break; }
                int *const v = val[ch];
                const int sh = ch < 3 ? 16 - ch * 8 : 24;
                v[0] = (p0 >> sh) & 0xff; v[1] = (p1 >> sh) & 0xff; v[2] = (int)(p0 >> 24); v[3] = (int)(p1 >> 24);
                c[ch] = tpe_abcd(v[sel[ch][0]], v[sel[ch][1]], v[sel[ch][2]], v[sel[ch][3]]);
            }
            orow[x] = argb((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], (uint8_t)c[3]);
        }
    }
}
bool gfx_soft_eval_texels(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t, int uls1,
                          int ult1, int shs1, int sht1, const uint8_t m[8], const uint8_t k[9], bool keep_alpha) {
    if (dst == 0 || dst >= MAX_TEXTURES || id0 == 0 || id0 >= MAX_TEXTURES || id1 >= MAX_TEXTURES) return false;
    const SoftTex *t0 = &sTex[id0], *t1 = id1 != 0 ? &sTex[id1] : NULL;
    if (!t0->valid || (t1 != NULL && !t1->valid) || W <= 0 || H <= 0 || W * H > 256 * 256 || W > BLEND_MAXW) {
        return false;
    }
    uint32_t *out;
    sTexGen[dst]++; /* rewritten, possibly in place: box row cache key */
    if (sTex[dst].valid && !sTex[dst].is16 && sTex[dst].w == W && sTex[dst].h == H) {
        out = (uint32_t *)sTex[dst].px;
        sky_texels_change(out); /* rewritten in place: a kept sky triangle may read them */
    } else {
        const size_t bytes = ((size_t)W * H * 4 + 15u) & ~(size_t)15;
        if (sArenaUsed + bytes > TEXARENA_SZ) return false;
        out = (uint32_t *)&sTexArena[sArenaUsed];
        sArenaUsed += bytes;
        sTex[dst].px = out; sTex[dst].is16 = false; sTex[dst].w = W; sTex[dst].h = H; sTex[dst].valid = true;
    }
    static int sCol1[BLEND_MAXW] SAVESTATE_EXCLUDE;
    if (t1 != NULL) {
        for (int x = 0; x < W; x++) sCol1[x] = hh_wrap(hh_tile_coord(base_s + x, shs1) - uls1, t1->w);
    }
    if (sTpeFast) { tpe_eval_fast(out, t0, t1, W, H, base_t, ult1, sht1, sCol1, m, k, keep_alpha); return true; }
    for (int y = 0; y < H; y++) {
        const int r0 = (y % t0->h) * t0->w;
        const int r1 = t1 != NULL ? hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w : 0;
        uint32_t *const orow = out + y * W;
        for (int x = 0; x < W; x++) {
            const uint32_t p0 = texel_at(t0->px, t0->is16, r0 + x % t0->w);
            const uint32_t p1 = t1 != NULL ? texel_at(t1->px, t1->is16, r1 + sCol1[x]) : 0;
            int c[4];
            for (int ch = 0; ch < 3; ch++) {
                c[ch] = tpe_abcd(tpe_rgb(m[0], 0, p0, p1, k, ch), tpe_rgb(m[1], 1, p0, p1, k, ch),
                                 tpe_rgb(m[2], 2, p0, p1, k, ch), tpe_rgb(m[3], 3, p0, p1, k, ch));
            }
            c[3] = keep_alpha ? (int)(p0 >> 24)
                              : tpe_abcd(tpe_a(m[4], 0, p0, p1, k), tpe_a(m[5], 1, p0, p1, k),
                                         tpe_a(m[6], 2, p0, p1, k), tpe_a(m[7], 3, p0, p1, k));
            orow[x] = argb((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], (uint8_t)c[3]);
        }
    }
    return true;
}
void gfx_scegu_sync_pending(void) { }
void gfx_scegu_upload_texture(const uint8_t *buf, int w, int h, unsigned int type) { gfx_soft_upload_texture(buf, w, h, type); }
int  gfx_vram_space_available(void) { return 1; }

int  texman_usage_percent(void) { return (int)(sArenaUsed * 100 / TEXARENA_SZ); }
int  texman_can_hold(unsigned int num, unsigned int bytes) {
    (void)num;
    /* TEX32: an expanded 16-bit upload takes twice its source size (bytes is
     * sized by the source format, 2 bytes a texel; 32-bit sources are
     * over-counted, so it flushes early). */
    return sArenaUsed + (size_t) bytes * (tex32_expand(bytes / 2u) ? 2u : 1u) <= TEXARENA_SZ;
}
void texman_bind_tex(unsigned int num) { sBoundTex[sTexUploadTile] = num; }
void texman_clear(void) { sky_settle(); sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; box_cache_clear(); sDetail.id = 0; }

/* Framebuffer readback (copy_framebuffer): the race-start wipe and the
 * stadium TV screens ask for small patches of the PREVIOUS frame at N64
 * 320x240 coordinates, stored as big-endian N64 RGBA16 tiles that re-enter
 * through the normal texture import next frame. The soft renderer owns its
 * framebuffer, so unlike the PSP's deferred GPU readback this is a direct
 * nearest-neighbour copy from sFB (which still holds the last rendered frame
 * while the game builds the next display list). The stub here was why the
 * transition wipe drew see-through and the Jumbotrons were blank. */
void port_fb_copy_request(int x, int y, int w, int h, uint16_t *target) {
    sky_settle();
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

/* Debug: dump the framebuffer as a binary PPM (for headless verification). */
void gfx_soft_dump_ppm(const char *path) {
    sky_settle();
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
