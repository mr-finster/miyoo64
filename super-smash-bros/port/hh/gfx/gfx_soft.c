/* super-smash-bros: tracks ocarina-of-time's copy of this file (last synced: OoT 2b629ab).
 * Update by 3-way merge and keep the Smash-specific changes listed in
 * the port's notes ("Smash-specific renderer choices"); verify with tools/sweep.sh. */
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
#define TEXARENA_SZ  (16 * 1024 * 1024) /* 16-bit textures are stored unpacked (4 B/texel) */

typedef struct {
    const void *px;     /* is16 ? uint16 RGBA5551 : uint32 ARGB8888, w*h */
    int w, h;
    int wrap_s, wrap_t;
    bool valid;
    bool is16;          /* device round 10: 5551 stored 16-bit, not expanded */
} SoftTex;
static uint32_t sTexGen[MAX_TEXTURES] SAVESTATE_EXCLUDE; /* bumped whenever a texture's texels are (re)written: box row cache key */

/* Texture arena + table excluded from savestates: 12 MB would bloat every
 * state, and the cache is flushed on load so textures re-import from the
 * restored course data anyway. Byte-addressed: the N64's RGBA16/CI textures
 * (the road/kart/scenery fill that dominates the memory-bound raster) are
 * stored 16-bit rather than expanded to 32-bit, halving their cache
 * footprint; IA/I textures stay 32-bit ARGB. */
static SoftTex sTex[MAX_TEXTURES] SAVESTATE_EXCLUDE;
/* PERF2 (main.c): uploads refused because the arena is full or no texture id
 * was left, and full texture-cache resets. port_log never reaches the
 * device's console.log, so these are counted and printed with PERF. */
uint32_t gfx_soft_upload_drops SAVESTATE_EXCLUDE, gfx_soft_upload_noid SAVESTATE_EXCLUDE, gfx_soft_tex_resets SAVESTATE_EXCLUDE;
/* Heap, not a static array: a named section is PROGBITS even when zero-
 * initialised, so the arena was 12 MB of zeros in the ELF (M6: the shipped
 * game is code only and should be ~1 MB). Allocated once in gfx_soft_init. */
static uint8_t *sTexArena SAVESTATE_EXCLUDE;
static size_t sArenaUsed SAVESTATE_EXCLUDE; /* bytes */
size_t gfx_soft_arena_peak SAVESTATE_EXCLUDE; /* high-water mark since the last PERF line */

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
 * 16-bit). SSB64_TEX32=0/1/2 (or debug.cfg tex32=) overrides PORT_TEX32. */
#ifndef PORT_TEX32
#define PORT_TEX32 1 /* Smash: every 16-bit texture (diddy-kong-racing a0ee8b5; 16 MB arena) */
#endif
#define TEX32_SMALL 4096
int gfx_soft_tex32_mode(void) {
    static int mode SAVESTATE_EXCLUDE = -1;
    if (mode < 0) { const char *e = getenv("SSB64_TEX32"); mode = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : PORT_TEX32; }
    return mode;
}
static bool tex32_expand(size_t texels) {
    const int m = gfx_soft_tex32_mode();
    return m == 1 || (m == 2 && texels <= TEX32_SMALL);
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
    if (sNextTexId >= MAX_TEXTURES) { gfx_soft_upload_noid++; return 0; }
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
        gfx_soft_upload_drops++;
        return;
    }
    void *dst = &sTexArena[sArenaUsed];
    sArenaUsed += bytes;
    if (sArenaUsed > gfx_soft_arena_peak) gfx_soft_arena_peak = sArenaUsed;
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
    { /* MK64_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("SSB64_TEXDUMP"); }
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

/* HUD/menu sprites are laid out for the N64's 320-wide screen and land on 240
 * columns on the handheld: 4 texels to 3 pixels. Nearest drops every fourth
 * texel column (strokes vanish or double: jaggy, low-res text and digits), so a
 * shrunk sprite takes the area average of exactly the texels each pixel covers -
 * every column counts, edges antialias. Alpha is premultiplied so transparent
 * texels do not darken the average. SSB64_HUDFILTER=0 restores nearest (A/B).
 * (MK64 hud-area-filter cd479d8, from f-zero-x ca8ef33.) */
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

/* Smash squeezes 2D uniformly (0.75 in both axes), so rows drop like columns:
 * blend the S-averaged rows each pixel covers along T too (up to 4 rows,
 * 8.8 coverage weights, premultiplied like texel_box). */
static inline uint32_t texel_box2(const void *px, bool t16, const int *rowbase, const uint32_t *rw, int nr, int32_t uf,
                                  int32_t duf, int tw, int ws, int mu, bool premul) {
    if (nr == 1) return texel_box(px, t16, rowbase[0], uf, duf, tw, ws, mu, premul);
    uint32_t sr = 0, sg = 0, sb = 0, sa = 0, sw = 0;
    for (int i = 0; i < nr; i++) {
        const uint32_t t = texel_box(px, t16, rowbase[i], uf, duf, tw, ws, mu, premul);
        const uint32_t a = t >> 24, w = rw[i], k = premul ? a * w : 255u * w;
        sr += ((t >> 16) & 0xff) * k; sg += ((t >> 8) & 0xff) * k; sb += (t & 0xff) * k;
        sa += a * w; sw += w;
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
 * (then all but this frame's, then all). SSB64_BOXCACHE=0 walks every row. */
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
        const char *e = getenv("SSB64_BOXCACHE");
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

/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}
/* float -> int32 as the device's VFP converts (vcvt saturates, NaN gives 0), so the x86 proxy
 * agrees with it where a plain cast is undefined: a near-degenerate triangle's colour step can
 * reach ~1e10 in 16.16, and x86's cvttss2si gives INT_MIN there (MK64 b2dff40; make PLAT=desktop
 * UBSAN=1 checks float casts). */
static inline int32_t sat_i32(float f) {
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
}

/* 3D bilinear filtering (menu "3D FILTER: SMOOTH"; off by default). uf/vf are
 * 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* 1 = bilinear only when magnified; 0 = always bilinear (the default since round 15, the
 * lead's P17). The gate chose bilinear or nearest per triangle, so a surface drew sharp
 * beyond a border along triangle edges that moved with the camera: DKR's and GoldenEye's
 * floors flashed on the device. The N64 filters every texel. SSB64_MAGBILIN=1: the gate. */
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
/* A magnified 2D draw bilerps every pixel (round 17), and bilerp's u-lerp of a texture row
 * depends only on that row and the pixel's column: the same for every screen row of the
 * draw. So a draw builds each texture row's u-lerped lanes once (the two last used are
 * kept) and per pixel only the v-lerp is left: bilerp's operations in bilerp's order,
 * bit-identical. Dream Land's backdrop (~48k px a frame) had cost +49% ARM instructions
 * on the stage (v19 device: r2d 3-5 -> 9-10 ms/frame). SSB64_MAGROW=0: per pixel. */
#define MAGROW_MAX BOX_MAX /* the row goes through boxtmp[BOX_MAX] */
static uint32_t sMagE[2][MAGROW_MAX] SAVESTATE_EXCLUDE, sMagO[2][MAGROW_MAX] SAVESTATE_EXCLUDE;
static int sMagKey[2] SAVESTATE_EXCLUDE;
/* the columns are the same for every row of a draw: wrapped texel pair and fraction per pixel */
static int16_t sMagU0[MAGROW_MAX] SAVESTATE_EXCLUDE, sMagU1[MAGROW_MAX] SAVESTATE_EXCLUDE;
static uint8_t sMagFu[MAGROW_MAX] SAVESTATE_EXCLUDE;
static void mag_cols(int w, int ws, int mu, int32_t uf, int32_t duf, int n) {
    for (int x = 0; x < n; x++, uf += duf) {
        const int32_t su = uf - 0x8000;
        sMagU0[x] = (int16_t)wrapf(su >> 16, w, ws, mu); sMagU1[x] = (int16_t)wrapf((su >> 16) + 1, w, ws, mu);
        sMagFu[x] = (uint8_t)((su >> 8) & 0xff);
    }
}
static int sMagRowOn SAVESTATE_EXCLUDE = -1;
static void mag_row_build(int slot, const void *px, bool i16, int w, int ws, int mu, int r, int32_t uf, int32_t duf, int n) {
    const int b = r * w;
    uint32_t *const E = sMagE[slot], *const O = sMagO[slot];
    (void)ws; (void)mu; (void)uf; (void)duf;
    for (int x = 0; x < n; x++) {
        const int fu = sMagFu[x];
        const uint32_t a = texel_at(px, i16, b + sMagU0[x]), c = texel_at(px, i16, b + sMagU1[x]);
        E[x] = lerp2(a & 0x00FF00FFu, c & 0x00FF00FFu, fu);
        O[x] = lerp2((a >> 8) & 0x00FF00FFu, (c >> 8) & 0x00FF00FFu, fu);
    }
    sMagKey[slot] = r;
}
/* the slot holding texture row r (built if missing), never evicting slot `keep` */
static int mag_row_slot(int r, int keep, const void *px, bool i16, int w, int ws, int mu, int32_t uf, int32_t duf, int n) {
    if (sMagKey[0] == r) return 0;
    if (sMagKey[1] == r) return 1;
    const int s = keep == 0 ? 1 : 0;
    mag_row_build(s, px, i16, w, ws, mu, r, uf, duf, n);
    return s;
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
static int sLerpR, sLerpG, sLerpB;
static bool sForceIn;      /* 2-cycle: the vertex colour carries cycle 2, always modulate */
/* Per-pixel fog (gfx_pc tri_state.fog_px, round 15): after the combine each pixel goes to
 * lerp(colour, sFogR/G/B, sFogA), as the RDP's cycle-1 blend of G_RM_FOG_PRIM_A does (the fog
 * colour's own alpha is the factor: Smash's fighter tints). */
static bool sFogPx;
static int sFogR, sFogG, sFogB, sFogA;
static bool sUiMode;       /* HUD/menu triangles (game UI tags): never bilinear; area-filtered when squeezed */

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
/* SSB64_FILLRULE (2026-10-03; ocarina-of-time a7c4909, the Deku Tree's vision: a one-pixel bright diagonal across
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
 * so it is bit-identical (proved on ARM by tools/qsweep.sh). In raster_tri
 * the span setup spilled to the stack, paid an FPSCR transfer per clamp and
 * re-converted the texture size every span, and each span paid a 76-instruction
 * call; flat triangles come in with zero colour steps. SSB64_CMNEON=0 keeps the
 * scalar loops. */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16; /* texture */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx;  /* triangle x-steps */
    float ftw, fth;                                       /* (float)tw, (float)th */
    int32x4_t dacc;                                       /* B G R A 16.16 x-steps */
    int32_t zdq; int zbias, amode, amin, ablend; /* ablend 0: TEX_EDGE without FORCE_BL, unblended over the clear */
    uint32_t clr;                                /* ...the frame's clear colour (sNatFill), rgb */
    bool persp, dmask, alpha, bilin;
    bool texonly; /* MODE 1: texture only, no z, opaque (the sky) */
    bool noz;     /* MODE 2: texture x shade without z (X23) */
    bool tint;    /* MODE 0/2 with K + ((shade - K) * texel) / 255 for rgb (sTexLerp; SSB64_LERPROWS) */
    uint16x4_t kl; /* ...K in lanes B G R, 0 in A */
    float32x4_t da, db; /* SSB64_SPANVEC: { zdx, cbdx, cgdx, crdx }, { cadx, uwdx, vwdx, swdx } */
    int zskip;          /* SSB64_ZSKIP: the span test's budget (4 after a skip, -1 a miss; 0: off for the triangle) */
} CmTri;
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* The pixel loop of one span: Tx (the texel, B G R A in u16 lanes) from the fetch (...).
 * MODE 0: z-tested texture x shade (SPAN_LOOP_CM); MODE 1: texture only (the
 * combiner's rgb input off), no z test or write, opaque -- SPAN_LOOP(1, 0, 0, *)
 * with rgb_in clear: the sky. MODE 2: MODE 0 without the z test or write (X23). */
#define CM_PIXELS(MODE, ALPHA, TINT, ...)                                                                \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        int uz = 0;                                                                                     \
        if (MODE == 0) { uz = clamp16_sh8(zq); if (uz > *pz + zbias) continue; }                        \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        uint16x4_t M = Tx;                                                                              \
        if (MODE != 1) {                                                                                \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        if (TINT) /* k + ((s - k) * t) / 255 toward zero = k + div255(sat(s - k) t) - div255(sat(k - s) t) */ \
            M = vsub_u16(vadd_u16(kl, cm_div255(vmul_u16(vqsub_u16(S, kl), Tx))),                       \
                         cm_div255(vmul_u16(vqsub_u16(kl, S), Tx)));                                    \
        else M = cm_div255(vmul_u16(Tx, S));                                                            \
        if (ALPHA) { /* fa: modulated (0), texel (1) or shade (2) alpha -- the scalar fa */               \
            const int fa = amode == 0 ? vget_lane_u16(M, 3) : amode == 1 ? vget_lane_u16(Tx, 3)          \
                                                                           : vget_lane_u16(S, 3);           \
            if (fa < amin) continue;                                                                    \
            if (fa < 255 && (ablend || *pz != 0xFFFF || (*pf & 0xFFFFFFu) != clr)) { /* div255(f*fa + d*(255-fa)) */ \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));      \
                M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));         \
            }                                                                                           \
        }                                                                                               \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (MODE == 0 && dmask) *pz = (uint16_t)uz;                                                     \
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
#define CM_ROW(MODE, ALPHA, TINT, ...)                                                                   \
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
        const float32x4_t cq = { cb, cg, cr, ca };                                                      \
        int32x4_t acc = vcvtq_n_s32_f32(cq, 16);                                                        \
        uint32_t *const end = fb + span;                                                                \
        uint16_t *pz = zp;                                                                              \
        CM_PIXELS(MODE, ALPHA, TINT, __VA_ARGS__)                                                       \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }
static int sCmNeon SAVESTATE_EXCLUDE = 1; /* SSB64_CMNEON=0 (debug.cfg cmneon=0): the scalar loops, for A/B */
#endif
/* The texel-lerp rows (2026-10-03): the bilinear (A - K) * TEXEL0 + K triangles (sTexLerp: the largest 3D pixel
 * group Smash left on the scalar loops, round 16f) through cm_row_neon, z-tested or not. SSB64_LERPROWS=0: the
 * scalar loops, as before (the bench's A/B flips it at run time). */
int gfx_soft_lerp_rows SAVESTATE_EXCLUDE = 1;
/* SSB64_FASTROWS (2026-10-07, branch speed-2026-10-07, from super-mario-64's speed round; default 1): raster_tri's row
 * loop, for the triangles whose rows go to cm_row_neon, in a small function of its own (tri_rows): the same row bases,
 * edge solve and snap (or owned-edge ends), the edge kinds (w_i_dx > 0, < 0, == 0) decided once a triangle, the snap's
 * three tests in NEON lanes, and the row's start values (depth, shade, u/w, v/w, 1/w: 24 multiplies, 16 adds) as NEON
 * lanes, multiply then add, each rounded as the VFP rounds them (-ffp-contract=off; NEON's vmla is not fused). raster_tri
 * keeps ~45 values live across its row loop and spilled most of them every row. 0: raster_tri's own loop. Same pixels. */
int gfx_soft_fastrows SAVESTATE_EXCLUDE = 1;
/* SSB64_SPANVEC (2026-10-07, default 1): cm_row_neon_v, cm_row_neon with the row's float state as two NEON vectors,
 * A = { z, b, g, r } and B = { a, u/w, v/w, 1/w }: the next span's start is A + dA * span and B + dB * span (two vmla,
 * the product rounded and then the sum, as the VFP's multiply and add), the perspective ends and the 1/16 step as
 * two-lane multiplies and a subtract (the divides stay scalar VFP divides), the shade's 16.16 start {b, g, r, a} one
 * vcvt of the two vectors. In Smash the rows' own count hardly moves (their per-pixel loop is most of it); the gain
 * is at the call, where SSB64_FASTROWS hands the two vectors over as they are instead of nine floats. NEON flushes
 * denormals to zero where the VFP keeps them: no value here comes near one (screen and texture coordinates, 1/w,
 * colours 0..255, depth 0..1 and their per-pixel steps are each 0 or far above 1e-30), as super-mario-64 argues. Same
 * pixels. SSB64_ZSKIP lives in these rows: SSB64_SPANVEC=0 turns it off too. */
int gfx_soft_spanvec SAVESTATE_EXCLUDE = 1;
/* SSB64_ZSKIP (2026-10-07, default 1; in SSB64_SPANVEC's rows): a depth-tested span whose nearest depth (z steps by a
 * constant along the span and the clamp is monotonic: one of its ends) is further than the farthest depth stored under
 * it plus the decal window fails the depth test at every pixel, so no pixel of it is written: it is skipped whole,
 * before its texture setup. A sufficient test, never a guess. A triangle stops testing after four misses running
 * (speed only). Same pixels. */
int gfx_soft_zskip SAVESTATE_EXCLUDE = 1;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
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
    const uint32_t clr = T->clr;
    const uint16x4_t kl = T->kl;
    const bool persp = T->persp, dmask = T->dmask;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    (void)th; (void)ws; (void)wt; (void)px32;
#define CM_VARIANTS(KIND, MODE, ALPHA, TINT)                                                            \
    if (rr) { if (T->t16) { CM_ROW(MODE, ALPHA, TINT, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }      \
              else        { CM_ROW(MODE, ALPHA, TINT, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }    \
    else    { if (T->t16) { CM_ROW(MODE, ALPHA, TINT, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }        \
              else        { CM_ROW(MODE, ALPHA, TINT, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
#define CM_DISPATCH                                                                                     \
    if (T->texonly) { if (T->bilin) { CM_VARIANTS(CM_BILIN, 1, 0, 0) } else { CM_VARIANTS(CM_NEAR, 1, 0, 0) } } \
    else if (T->tint) { /* bilinear only (cm_rows) */                                                   \
        if (T->noz) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 2, 1, 1) } else { CM_VARIANTS(CM_BILIN, 2, 0, 1) } } \
        else        { if (T->alpha) { CM_VARIANTS(CM_BILIN, 0, 1, 1) } else { CM_VARIANTS(CM_BILIN, 0, 0, 1) } } \
    }                                                                                                   \
    else if (T->noz) {                                                                                  \
        if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 2, 1, 0) } else { CM_VARIANTS(CM_BILIN, 2, 0, 0) } } \
        else          { if (T->alpha) { CM_VARIANTS(CM_NEAR, 2, 1, 0) }  else { CM_VARIANTS(CM_NEAR, 2, 0, 0) } } \
    }                                                                                                   \
    else if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 0, 1, 0) } else { CM_VARIANTS(CM_BILIN, 0, 0, 0) } } \
    else               { if (T->alpha) { CM_VARIANTS(CM_NEAR, 0, 1, 0) }  else { CM_VARIANTS(CM_NEAR, 0, 0, 0) } }
    CM_DISPATCH
}
/* SSB64_SPANVEC (above): CM_ROW with the float state in A = { z, b, g, r }, B = { a, u/w, v/w, 1/w } (the same
 * operations in the same order, lane by lane), and SSB64_ZSKIP's test before a z-tested span's setup. */
#define CM_ROWV(MODE, ALPHA, TINT, ...)                                                                  \
    while (n > 0) {                                                                                     \
        const int span = n > PERSP_SPAN ? PERSP_SPAN : n;                                               \
        const float fspan = (float)span;                                                                \
        const float32x4_t fsv = vdupq_n_f32(fspan);                                                     \
        const float32x4_t An = vmlaq_f32(A, DA, fsv), Bn = vmlaq_f32(B, DB, fsv); /* the next span's start */ \
        const float sw_b = vgetq_lane_f32(Bn, 3);                                                       \
        float rw_b = 0.0f;                                                                              \
        if (persp) rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;                                         \
        const float32x2_t zc = vmin_f32(vmax_f32(vdup_lane_f32(vget_low_f32(A), 0), vdup_n_f32(-1.0f)), vdup_n_f32(2.0f)); \
        int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);                  \
        if (MODE == 0 && T->zskip > 0 && span >= 4) { /* SSB64_ZSKIP: wholly behind the stored depths? */ \
            const int uz0 = clamp16_sh8(zq), uz1 = clamp16_sh8(zq + (span - 1) * zdq);                  \
            uint16x4_t m4;                                                                              \
            if (span >= 8) { const uint16x8_t m8 = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8));  \
                             m4 = vmax_u16(vget_low_u16(m8), vget_high_u16(m8)); }                      \
            else m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));                                   \
            m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);                                             \
            if ((uz0 < uz1 ? uz0 : uz1) > (int)vget_lane_u16(m4, 0) + zbias) {                          \
                T->zskip = 4;                                                                           \
                A = An; B = Bn; rw = rw_b;                                                              \
                fb += span; zp += span; n -= span;                                                      \
                continue;                                                                               \
            }                                                                                           \
            T->zskip--;                                                                                 \
        }                                                                                               \
        const float32x2_t uv = vget_low_f32(vextq_f32(B, B, 1)); /* { u/w, v/w } */                     \
        float32x2_t ua, du; /* { u_a, v_a }, { du, dv } */                                              \
        if (persp) {                                                                                    \
            ua = vmul_n_f32(uv, rw);                                                                    \
            const float32x2_t nuv = vsub_f32(vmul_n_f32(vget_low_f32(vextq_f32(Bn, Bn, 1)), rw_b), ua); \
            if (span == PERSP_SPAN) du = vmul_n_f32(nuv, 1.0f / PERSP_SPAN);                             \
            else du = vset_lane_f32(vget_lane_f32(nuv, 1) / fspan, vdup_n_f32(vget_lane_f32(nuv, 0) / fspan), 1); \
        } else { ua = uv; du = uvd; }                                                                   \
        const float32x2x2_t uz2 = vzip_f32(ua, du);                                                     \
        const float32x4_t fq = vcombine_f32(uz2.val[0], uz2.val[1]); /* { u_a, du, v_a, dv } */         \
        const int32x4_t tq = vcvtq_n_s32_f32(vminq_f32(vmaxq_f32(vmulq_f32(fq, tsz), vdupq_n_f32(-16000.0f)), \
                                                       vdupq_n_f32(16000.0f)), 16);                     \
        int32_t uf = vgetq_lane_s32(tq, 0), duf = vgetq_lane_s32(tq, 1);                                \
        int32_t vf = vgetq_lane_s32(tq, 2), dvf = vgetq_lane_s32(tq, 3);                                \
        int32x4_t acc = vcvtq_n_s32_f32(vextq_f32(A, B, 1), 16); /* { b, g, r, a } */                   \
        uint32_t *const end = fb + span;                                                                \
        uint16_t *pz = zp;                                                                              \
        CM_PIXELS(MODE, ALPHA, TINT, __VA_ARGS__)                                                       \
        A = An; B = Bn; rw = rw_b;                                                                      \
        fb = end; zp += span; n -= span;                                                                \
    }
static void __attribute__((noinline))
cm_row_neon_v(CmTri *T, uint32_t *fb, uint16_t *zp, int n, float32x4_t A, float32x4_t B, float rw) {
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float32x4_t DA = T->da, DB = T->db;
    const float32x2_t uvd = vget_low_f32(vextq_f32(DB, DB, 1)); /* { uwdx, vwdx } */
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc = T->dacc;
    const int32_t zdq = T->zdq;
    const int zbias = T->zbias, amode = T->amode, amin = T->amin, ablend = T->ablend;
    const uint32_t clr = T->clr;
    const uint16x4_t kl = T->kl;
    const bool persp = T->persp, dmask = T->dmask;
    const bool rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    (void)th; (void)ws; (void)wt; (void)px32;
#undef CM_ROW
#define CM_ROW CM_ROWV
    CM_DISPATCH
}
#undef CM_DISPATCH
#undef CM_VARIANTS
#undef CM_ROWV
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

static bool sDbgBilin SAVESTATE_EXCLUDE; static float sDbgScr SAVESTATE_EXCLUDE, sDbgUvc SAVESTATE_EXCLUDE; /* see SSB64_PICK */

/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
static void int_rows_touch(int y0, int y1); /* internal target: seed/dirty rows (see the frame section) */
static uint32_t *sIntFB SAVESTATE_EXCLUDE; /* defined with the targets below */
/* A textured draw without a valid texture comes out as a flat block of the combiner's colour (OoT's
 * red-sun bar after a long session). Said on the console a few times a session, so a device run
 * catches it; the desktop prints the total at exit (the texture-stress sweeps, round 16d). */
static unsigned long sUntexDraws SAVESTATE_EXCLUDE;
static void note_untextured(const char *what, unsigned id) {
    extern void port_log_always(const char *fmt, ...);
    if (sUntexDraws++ < 4)
        port_log_always("gfx: untextured draw (%s): tex %u not valid (bound %u/%u, arena %d%%)\n", what, id,
                        (unsigned)sBoundTex[0], (unsigned)sBoundTex[1], (int)(sArenaUsed * 100 / TEXARENA_SZ));
}
#if !defined(HANDHELD)
__attribute__((destructor)) static void untex_report(void) {
    if (getenv("SSB64_TEXSTRESS") || sUntexDraws) fprintf(stderr, "UNTEX draws %lu\n", sUntexDraws);
}
#endif

static bool sEdgeOpaque SAVESTATE_EXCLUDE; /* gfx_soft_set_edge_opaque */
static bool sOwnTri SAVESTATE_EXCLUDE;     /* gfx_soft_set_own_edges (SSB64_FILLRULE) */
static uint32_t sNatFill SAVESTATE_EXCLUDE; /* the frame's clear colour (defined with the frame section below) */
#if defined(HH_CM_NEON)
/* SSB64_FASTROWS (see gfx_soft_fastrows): a triangle's row constants and the NEON row it draws with. */
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's { z, b, g, r } and { a, u/w, v/w, 1/w } */
    float32x4_t fa, fb;                       /* flat: { -, b0, g0, r0 }, { a0, -, -, - } in place of the sums */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    int minx, maxx, sw, persp, vec;
    int ek[3];                                /* each edge's w_i_dx: 0 > 0, 1 < 0, 2 == 0 (or NaN) */
    const OwnTri *own;                        /* SSB64_FILLRULE's owned-edge test, or NULL */
    uint32_t *fbuf; uint16_t *zb;
    CmTri *cmt;
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
/* Rows y..maxy-1 of the triangle, each exactly as raster_tri's loop draws it with cm_rows: the same bases, edge solve,
 * snap (or owned-edge ends) and start values; then the row's NEON function. Returns the span pixels. */
static inline __attribute__((always_inline)) uint32_t
tri_rows_t(const TriRows *T, int y, const int maxy, const int FLAT TR_STEP_PARAM) {
    const float e0c = T->e0c, e0x = T->e0x, e0y = T->e0y, e1c = T->e1c, e1x = T->e1x, e1y = T->e1y;
    const float e2c = T->e2c, e2x = T->e2x, e2y = T->e2y, inv_area = T->inv_area;
    const float w0dx = T->w0dx, w1dx = T->w1dx, w2dx = T->w2dx;
    const int minx = T->minx, maxx = T->maxx, ek0 = T->ek[0], ek1 = T->ek[1], ek2 = T->ek[2];
    const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };
    const OwnTri *const own = T->own;
    uint32_t px = 0;
    for (; y < maxy; TR_YNEXT) {
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
        /* raster_tri's solve, as it is there, its ds tests made once a triangle */
#define TRI_EDGE(EK, BS, DS)                                                                                     \
        if (EK == 0) {                                                                                          \
            if ((BS) < 0.0f) {                                                                                  \
                float q = -(BS) / (DS) - 1e-5f; if (q > 1e6f) q = 1e6f;                                          \
                const int a = minx + iceilf(q);                                                                  \
                if (a > xs) xs = a;                                                                              \
            }                                                                                                    \
        } else if (EK == 1) {                                                                                   \
            float q = -(BS) / (DS) + 1e-5f; if (q > 1e6f) q = 1e6f;                                              \
            const int a = minx + 1 + ifloorf(q);                                                                 \
            if (a < xe) xe = a;                                                                                  \
        } else if ((BS) < 0.0f) continue; /* the row outside this edge: nothing (xs = xe there) */
        TRI_EDGE(ek0, b0, w0dx)
        TRI_EDGE(ek1, b1, w1dx)
        TRI_EDGE(ek2, b2, w2dx)
#undef TRI_EDGE
        if (xs >= xe) continue;
        const float32x4_t B3 = { b0, b1, b2, 0.0f };
        float32x4_t W;
        if (own) { /* SSB64_FILLRULE: the row's ends by the owned-edge test, one pixel either side */
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;
            while (xs < xe && !own_px(own, xs, py)) xs++;
            while (xe > xs && !own_px(own, xe - 1, py)) xe--;
            if (xs >= xe) continue;
            W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx))); /* w_i = b_i + o * w_i_dx */
        } else {
            /* the snap: b_i + (xs - minx) * w_i_dx >= 0 for all three, in three lanes (vmla is not fused: the VFP's
             * sums); the lanes it ends on are the row's w_i */
            for (;;) {
                W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
                const uint32x4_t ge = vcgeq_f32(W, vdupq_n_f32(0.0f));
                const uint32x2_t g2 = vand_u32(vget_low_u32(ge), vget_high_u32(ge)); /* lane 3: 0 >= 0 */
                if (vget_lane_u32(g2, 0) & vget_lane_u32(g2, 1)) break;
                if (++xs >= xe) break;
            }
            if (xs >= xe) continue;
        }
        const int n = xe - xs;
        px += (uint32_t)n;
        /* w0*z0 + w1*z1 + w2*z2 for all eight, as raster_tri's sums */
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
        if (T->vec) cm_row_neon_v(T->cmt, T->fbuf + idx, T->zb + idx, n, A, B, rw);
        else cm_row_neon(T->cmt, T->fbuf + idx, T->zb + idx, n, vgetq_lane_f32(A, 0), vgetq_lane_f32(A, 3),
                         vgetq_lane_f32(A, 2), vgetq_lane_f32(A, 1), vgetq_lane_f32(B, 0), vgetq_lane_f32(B, 1),
                         vgetq_lane_f32(B, 2), sw, rw);
    }
    return px;
}
static uint32_t __attribute__((noinline)) tri_rows_f(const TriRows *T, int y, int maxy TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, 1 TR_STEP_ARG(ystep)); }
static uint32_t __attribute__((noinline)) tri_rows_s(const TriRows *T, int y, int maxy TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, 0 TR_STEP_ARG(ystep)); }
#if defined(MIYOO)
/* The Mini's second core (kit miyoo_worker.c, the shared Miyoo kit KM2; as diddy-kong-racing's Miyoo split): a 3D triangle
 * whose rows all go to the NEON rows (SSB64_FASTROWS' tri_rows) and that is big enough is drawn by both cores at once,
 * this one its even rows from miny, the worker the odd ones. Exact: tri_rows computes each row from the triangle's
 * constants and its own y alone (the bases, the edge solve, the snap or the owned-edge ends and the row's start values
 * are recomputed per row; nothing steps from one row to the next), so a row comes out the same whichever core draws
 * it; the shares write disjoint rows of colour and depth; and the triangle is finished (miyoo_worker_wait) before
 * the next one starts. All of the setup (int_rows_touch, the texture, the CmTri, the owned edges) is done first, by
 * this core, and the worker only reads it; the one thing the rows write besides pixels is SSB64_ZSKIP's run of misses
 * (CmTri.zskip, carried row to row), which only decides which spans are tested (a tested span is skipped only when
 * it draws nothing), so the worker has its own copy of the CmTri. Each share's pixel count is added after the wait.
 * SSB64_THREADS=1 (or threads=1 in bench.cfg): one core, as before. SSB64_SPLIT=<px>: the smallest triangle split,
 * in pixels of area (default TR_SPLIT_PX). The worker starts at the first triangle big enough, from the game
 * thread, after the audio and present threads exist (they keep both cores; a thread started after the pinning
 * would inherit core 0). */
#include "miyoo_worker.h"
#define TR_SPLIT_PX   64 /* below this, the handoff costs about what it saves */
#define TR_SPLIT_ROWS 4
typedef struct { TriRows trr; CmTri cmt; int flat, y, maxy; uint32_t px; } TrJob;
static void tr_job(void *arg) {
    TrJob *const j = (TrJob *)arg;
    j->px = j->flat ? tri_rows_f(&j->trr, j->y, j->maxy, 2) : tri_rows_s(&j->trr, j->y, j->maxy, 2);
}
static int sTrMinPx SAVESTATE_EXCLUDE = -1, sTrWorker SAVESTATE_EXCLUDE = -1;
static uint32_t sTrTris SAVESTATE_EXCLUDE, sTrSplits SAVESTATE_EXCLUDE; /* the PERF line's split counts */
static uint64_t sTrPx SAVESTATE_EXCLUDE, sTrSplitPx SAVESTATE_EXCLUDE;
/* main.c's PERF line: this window's split triangles, the split share of the NEON rows' pixels, the worker's jobs and
 * the waits that slept */
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
static uint32_t tri_rows_mt(const TriRows *trr, const CmTri *cmt, int flat, int miny, int maxy, float area) {
    uint32_t px;
    if (sTrMinPx < 0) { const char *e = getenv("SSB64_SPLIT"); sTrMinPx = e && e[0] ? atoi(e) : TR_SPLIT_PX; }
    if (maxy - miny >= TR_SPLIT_ROWS && (area < 0.0f ? -area : area) >= 2.0f * (float)sTrMinPx) {
        if (sTrWorker < 0) sTrWorker = miyoo_worker_start();
    }
    if (sTrWorker == 1 && maxy - miny >= TR_SPLIT_ROWS && (area < 0.0f ? -area : area) >= 2.0f * (float)sTrMinPx) {
        TrJob job; /* the worker's share: the odd rows, with its own CmTri */
        job.trr = *trr; job.cmt = *cmt; job.trr.cmt = &job.cmt;
        job.flat = flat; job.y = miny + 1; job.maxy = maxy; job.px = 0;
        miyoo_worker_run(tr_job, &job);
        px = flat ? tri_rows_f(trr, miny, maxy, 2) : tri_rows_s(trr, miny, maxy, 2);
        miyoo_worker_wait();
        px += job.px;
        sTrSplits++; sTrSplitPx += px;
    } else {
        px = flat ? tri_rows_f(trr, miny, maxy, 1) : tri_rows_s(trr, miny, maxy, 1);
    }
    sTrTris++; sTrPx += px;
    return px;
}
#endif
#endif
/* O2 (fullcmp 2026-10-09): a 1/2-cycle draw's alpha-compare threshold (G_AC_THRESHOLD: the RDP keeps a pixel when
 * its alpha is at least the blend colour's), set by gfx_pc.c (SSB64_ACTHRESH); 0 = the near-zero reject alone. */
static int sAlphaRef SAVESTATE_EXCLUDE;
void gfx_soft_set_alpha_ref(int a) { sAlphaRef = a; }
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

    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { note_untextured("3d", (unsigned)sBoundTex[0]); tex = NULL; }
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
    bool use_bilin = gfx_soft_bilinear && tex && !sUiMode; /* HUD/menus: never bilinear (MK64 rule) */
    sDbgScr = sDbgUvc = -1.0f;
    if (use_bilin && gfx_soft_magbilin) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
        sDbgScr = scr; sDbgUvc = uvc;
    }
    sDbgBilin = use_bilin;
    const int amin = imax(tex_edge ? 0x55 : 8, sAlphaRef); /* alpha reject: texture-edge or near-zero (only under sUseAlpha), or the
                                                         * draw's alpha-compare threshold (sAlphaRef, O2) */
    /* 0: a cutout edge over nothing yet (depth and colour still the frame's clear) takes the texel's own colour
     * (gfx_soft_set_edge_opaque; OoT 05dd742's narrowed rule) */
    const int ablend = !(tex_edge && sEdgeOpaque);
    const uint32_t eclr = sNatFill & 0xFFFFFFu;
    const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
    const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
    const bool rgb_texa = sCurShader && sCurShader->rgb_texa;
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
    /* A flat quad at exactly 1:1 (one texel a pixel along x on one texture axis, none on the other):
     * the texel itself, as the N64 samples its fonts (G_TF_POINT). Bilinear, half a texel off, spreads
     * every 1-texel stroke over two pixels at half strength (super-mario-64 eb0a164, the lead's X27).
     * Smash: its HUD and menus are texrects, squeezed 0.75 in x (area-filtered, never 1:1), and its
     * rects sit on whole pixels already; round 20 counted one such triangle in the census (results).
     * SSB64_PIXEL11=0: as before. */
    static int sPix11 SAVESTATE_EXCLUDE = -1;
    if (sPix11 < 0) { const char *e = getenv("SSB64_PIXEL11"); sPix11 = !(e && e[0] == '0'); }
    if (sPix11 && use_bilin && iw0 == iw1 && iw1 == iw2) {
        const float k = persp ? 1.0f / iw0 : 1.0f;
        const float su = fabsf(uwdx * k * (float)tex->w), sv = fabsf(vwdx * k * (float)tex->h);
        if ((fabsf(su - 1.0f) <= 0.02f && sv < 1.0f / 64.0f) || (fabsf(sv - 1.0f) <= 0.02f && su < 1.0f / 64.0f)) use_bilin = false;
    }
    if (flat) { crdx = cgdx = cbdx = cadx = 0.0f; } /* flat: no per-pixel colour step */
    /* Fixed-point deltas are invariant too (were rebuilt every 16-px span). */
    const int32_t zdq = (int32_t)((zdx < -1.0f ? -1.0f : zdx > 1.0f ? 1.0f : zdx) * (65535.0f * 256.0f));
    const int32_t rdq = sat_i32(crdx * 65536.0f), gdq = sat_i32(cgdx * 65536.0f), bdq = sat_i32(cbdx * 65536.0f), adq = sat_i32(cadx * 65536.0f);

    /* Flat triangles: the shade colour the pixel loop would clamp every pixel
     * (rq = (int32_t)(cr * 65536.0f) with cr == r0 on every row and span, since
     * every colour step is 0), clamped once. (MK64 perf round 24.) */
    const int fsr = clamp8_sh16((int32_t)(r0 * 65536.0f)), fsg = clamp8_sh16((int32_t)(g0 * 65536.0f));
    const int fsb = clamp8_sh16((int32_t)(bl0 * 65536.0f)), fsa = clamp8_sh16((int32_t)(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const int zbias = sZDecal ? 4 : 0; const bool dmask = sDepthMask, texadd = sTexAdd;
    const bool texlerp = sTexLerp; const int lkr = sLerpR, lkg = sLerpG, lkb = sLerpB;
    const bool cm_mod = rgb_tex && rgb_in && !texlerp && !texadd && !rgb_texa; /* texture x shade: see SPAN_LOOP_CM */
    /* fogged per pixel (sFogPx) with a factor: the general loops with the fog step (CM=2) */
    const bool fogtri = sFogPx && sFogA > 0;
    const int fgr = sFogR, fgg = sFogG, fgb = sFogB, ffa = sFogA;
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
#if defined(HH_CM_NEON)
    /* The NEON rows (cm_row_neon): z-tested texture x shade, and texture-only
     * opaque spans without z (the sky), wherever the HUD box filter cannot
     * apply (that is for nearest UI spans only). */
    const bool texonly = rgb_tex && !rgb_in && !rgb_texa; /* rgb from the texel alone: the sky */
    /* ...and texture x shade without z (the lead's X23, goldeneye-007 287aa70, paper-mario aa3ffff): the
     * general scalar loop computes the same div255(t*s) per channel. SSB64_CMNOZ=0: those stay scalar. */
    static int sCmNoZ SAVESTATE_EXCLUDE = -1;
    if (sCmNoZ < 0) { const char *e = getenv("SSB64_CMNOZ"); sCmNoZ = !(e && e[0] == '0'); }
    /* the texel lerp: the scalar loops' texlerp branch (rgb_tex, rgb_in, not rgb_texa: texlerp comes before texadd) */
    const bool cm_tint = gfx_soft_lerp_rows && use_bilin && rgb_tex && rgb_in && texlerp && !rgb_texa;
    const bool cm_rows = sCmNeon && tex && !fogtri && (use_bilin || !sUiMode || gfx_soft_hud_box <= 0) &&
                         ((ztest && cm_mod) || (!ztest && texonly && !sUseAlpha) || (!ztest && cm_mod && sCmNoZ) ||
                          (cm_tint && (ztest || sCmNoZ)));
    CmTri cmt;
    if (cm_rows) {
        const int32_t d[4] = { bdq, gdq, rdq, adq };
        cmt.px = texpx; cmt.w = tw; cmt.h = th; cmt.ws = ws; cmt.wt = wt; cmt.mu = tmask_u; cmt.mv = tmask_v; cmt.t16 = t16;
        cmt.swdx = swdx; cmt.uwdx = uwdx; cmt.vwdx = vwdx; cmt.zdx = zdx;
        cmt.crdx = crdx; cmt.cgdx = cgdx; cmt.cbdx = cbdx; cmt.cadx = cadx;
        cmt.ftw = (float)tw; cmt.fth = (float)th;
        cmt.dacc = vld1q_s32(d); cmt.zdq = zdq; cmt.zbias = zbias;
        cmt.amode = !a_tex ? 2 : !a_in ? 1 : 0; /* fa = !a_tex ? sa : !a_in ? ta : div255(ta*sa) */
        cmt.amin = amin; cmt.ablend = ablend; cmt.clr = eclr; cmt.persp = persp; cmt.dmask = dmask; cmt.alpha = sUseAlpha; cmt.bilin = use_bilin;
        cmt.texonly = !ztest && !cm_mod && !cm_tint; /* cm_rows without z: the texture-only case */
        cmt.noz = !ztest && (cm_mod || cm_tint);
        cmt.tint = cm_tint;
        { const uint16_t k[4] = { (uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0 }; cmt.kl = vld1_u16(k); }
        cmt.da = (float32x4_t){ zdx, cbdx, cgdx, crdx }; cmt.db = (float32x4_t){ cadx, uwdx, vwdx, swdx };
        cmt.zskip = gfx_soft_spanvec && gfx_soft_zskip ? 4 : 0;
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
    OwnTri oe; /* SSB64_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    if (own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&oe.e[0], v1, v2, s); own_edge(&oe.e[1], v2, v0, s); own_edge(&oe.e[2], v0, v1, s);
    }
#if defined(HH_CM_NEON)
    if (cm_rows && gfx_soft_fastrows) { /* SSB64_FASTROWS: every row in tri_rows */
        TriRows trr;
        trr.va0 = (float32x4_t){ z0, bl0, g0, r0 }; trr.vb0 = (float32x4_t){ a0, uw0, vw0, iw0 };
        trr.va1 = (float32x4_t){ z1, bl1, g1, r1 }; trr.vb1 = (float32x4_t){ a1, uw1, vw1, iw1 };
        trr.va2 = (float32x4_t){ z2, bl2, g2, r2 }; trr.vb2 = (float32x4_t){ a2, uw2, vw2, iw2 };
        trr.fa = (float32x4_t){ 0.0f, bl0, g0, r0 }; trr.fb = (float32x4_t){ a0, 0.0f, 0.0f, 0.0f };
        trr.e0c = e0c; trr.e0x = e0x; trr.e0y = e0y; trr.e1c = e1c; trr.e1x = e1x; trr.e1y = e1y;
        trr.e2c = e2c; trr.e2x = e2x; trr.e2y = e2y; trr.inv_area = inv_area;
        trr.w0dx = w0dx; trr.w1dx = w1dx; trr.w2dx = w2dx;
        trr.minx = minx; trr.maxx = maxx; trr.sw = sW; trr.persp = persp; trr.vec = gfx_soft_spanvec != 0;
        trr.ek[0] = w0dx > 0.0f ? 0 : w0dx < 0.0f ? 1 : 2;
        trr.ek[1] = w1dx > 0.0f ? 0 : w1dx < 0.0f ? 1 : 2;
        trr.ek[2] = w2dx > 0.0f ? 0 : w2dx < 0.0f ? 1 : 2;
        trr.own = own ? &oe : NULL; trr.fbuf = fbuf; trr.zb = zb; trr.cmt = &cmt;
#if defined(MIYOO)
        return (int)tri_rows_mt(&trr, &cmt, flat, miny, maxy, area);
#else
        return (int)(flat ? tri_rows_f(&trr, miny, maxy) : tri_rows_s(&trr, miny, maxy));
#endif
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
        if (own) { /* SSB64_FILLRULE: the row's ends by the owned-edge test, one pixel either side */
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
            if (gfx_soft_spanvec) /* SSB64_SPANVEC */
                cm_row_neon_v(&cmt, fbuf + idx, zb + idx, xe - xs, (float32x4_t){ zf, cb, cg, cr }, (float32x4_t){ ca, uw, vw, sw }, rw);
            else cm_row_neon(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
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
            int32_t rq = (int32_t)(cr * 65536.0f), gq = (int32_t)(cg * 65536.0f), bq = (int32_t)(cb * 65536.0f), aq = (int32_t)(ca * 65536.0f);

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
/* UI quads squeezed 4:3 -> 1:1 like the 2D sprites: area average along S. */
#define FETCH_BOX                                                                                      \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_box(texpx, t16, iv * tw, uf, duf, tw, ws, tmask_u, sUseAlpha);
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once per
 * triangle above) -- the four colour accumulators and their steps drop out of
 * the pixel loop. Same values: flat => every step is 0.
 * CM=1: the combiner is plain texture x shade (cm_mod), so the per-pixel walk
 * of the combine flags drops out. (Both MK64 perf round 24, bit-exact.)
 * CM=2: the general combine, then the per-pixel fog (fogtri; round 15). */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
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
                    else if (texlerp)   { fr=lkr+((sr-lkr)*tr)/255; fg=lkg+((sg-lkg)*tg)/255; fb=lkb+((sb-lkb)*tb)/255; } \
                    else if (texadd)    { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = !a_tex ? sa : (!a_in ? ta : div255(ta*sa));                                    \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (CM == 2) { fr=div255(fr*(255-ffa) + fgr*ffa); fg=div255(fg*(255-ffa) + fgg*ffa);     \
                               fb=div255(fb*(255-ffa) + fgb*ffa); }                                     \
                if (ALPHA) {                                                                            \
                    if (fa < amin) continue;                                                            \
                    if (fa < 255 && (ablend || zb[idx] != 0xFFFF || (fbuf[idx] & 0xFFFFFFu) != eclr)) { \
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
            const bool boxspan = tex && !use_bilin && sUiMode && gfx_soft_hud_box > 0 && duf > 66846 && duf <= 196608;
            if (fogtri) {
                /* fogged by the fog colour's alpha (the fighters' colour animations) */
                if (tex && use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 2) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 2) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, BILINEAR, 0, 2) else SPAN_LOOP_CM(1, 0, 0, BILINEAR, 0, 2) }
                } else if (tex) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 2) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(1, 0, 0, NEAREST, 0, 2) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(0, 1, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(0, 1, 0, NEAREST, 0, 2) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(0, 0, 1, NEAREST, 0, 2) else SPAN_LOOP_CM(0, 0, 0, NEAREST, 0, 2) }
                }
            } else if (tex && ztest && cm_mod && !boxspan) {
                /* the hot path: z-tested texture x shade */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                }
            } else if (tex) {
                if (boxspan) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BOX) else SPAN_LOOP(1, 1, 0, BOX) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BOX) else SPAN_LOOP(1, 0, 0, BOX) }
                } else if (flat && ztest) {
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
                /* Constant-colour run: with no z, no alpha and zero steps every
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
#undef FETCH_NEAREST
#undef FETCH_BILINEAR
#undef FETCH_BOX
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
static uint32_t sNatFill SAVESTATE_EXCLUDE;
static void nat_ready(void);
static void target_ready(void);
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */

/* SSB64_PICK=x,y (debug): log every 3D triangle that changes that pixel of the
 * current target, inline in the SSB64_TRACE_AT trace (so the display-list command
 * above it is the one that drew it), and dump the texture it sampled as
 * pick_tex_<id>.ppm into $SSB64_DUMP when set. */
static int sPickX SAVESTATE_EXCLUDE = -2, sPickY SAVESTATE_EXCLUDE = -1;
/* (sDbgBilin/Scr/Uvc: raster_tri's last filter decision, for the PICK line) */
static void pick_dump_texture(uint32_t id) {
    static uint8_t dumped[MAX_TEXTURES];
    if (id == 0 || id >= MAX_TEXTURES || dumped[id] || !sTex[id].valid) return;
    dumped[id] = 1;
    const SoftTex *t = &sTex[id];
    const char *dir = getenv("SSB64_DUMP"); /* only with a dump dir: never into the working tree */
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

static int sZImg SAVESTATE_EXCLUDE = -1; /* B4: see gfx_soft_set_zimage */
static bool sZClean SAVESTATE_EXCLUDE;
static void zimg_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, uint16_t z);
static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    if (sZImg >= 0) { /* B4: into the depth buffer (see zimg_tri) */
        for (size_t i = 0; i < num_tris; i++) zimg_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], (uint16_t)sZImg);
        return;
    }
    if (sDepthMask) sZClean = false;
    PROF_BEGIN();
    target_ready();
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
    if (sPickX == -2) {
        const char *e = getenv("SSB64_PICK");
        if (!e || sscanf(e, "%d,%d", &sPickX, &sPickY) != 2) sPickX = -1;
    }
    for (size_t i = 0; i < num_tris; i++) {
        SoftVtx h[3];
        if (sSkyPass) { /* screen coordinates to the half-resolution sky target */
            for (int k = 0; k < 3; k++) { h[k] = v[i*3+k]; h[k].x *= 0.5f; h[k].y *= 0.5f; }
        }
        const bool pick = sPickX >= 0 && sPickX < sW && sPickY >= 0 && sPickY < sH;
        const uint32_t pick_c0 = pick ? sFB[sPickY * sW + sPickX] : 0;
        const uint16_t pick_z0 = pick ? sZB[sPickY * sW + sPickX] : 0;
        const int px = sSkyPass ? raster_tri(&h[0], &h[1], &h[2], true) : raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
        if (pick && (sFB[sPickY * sW + sPickX] != pick_c0 || sZB[sPickY * sW + sPickX] != pick_z0)) {
            const SoftTex *t = &sTex[sBoundTex[0]];
            port_log("  PICK (%d,%d) %08X -> %08X  tex %u %dx%d%s wrap %d/%d bilin %d (scr %.1f uvc %.1f)  uv (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f)\n",
                     sPickX, sPickY, (unsigned)pick_c0, (unsigned)sFB[sPickY * sW + sPickX], (unsigned)sBoundTex[0],
                     t->w, t->h, t->is16 ? " 16b" : "", t->wrap_s, t->wrap_t, (int)sDbgBilin, sDbgScr, sDbgUvc,
                     v[i*3].u, v[i*3].v, v[i*3+1].u, v[i*3+1].v, v[i*3+2].u, v[i*3+2].v);
            port_log("  PICKX vtx %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X usealpha %d shader %08X rgbtex %d atex %d ain %d texa %d\n",
                     v[i*3].r, v[i*3].g, v[i*3].b, v[i*3].a, v[i*3+1].r, v[i*3+1].g, v[i*3+1].b, v[i*3+1].a, v[i*3+2].r, v[i*3+2].g, v[i*3+2].b, v[i*3+2].a,
                     (int)sUseAlpha, sCurShader ? (unsigned)sCurShader->shader_id : 0u, sCurShader ? sCurShader->rgb_tex : -1, sCurShader ? sCurShader->a_tex : -1, sCurShader ? sCurShader->a_in : -1,
                     t->valid ? (int)(texel_at(t->px, t->is16, 0) >> 24) : -1);
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

/* B4 (fullcmp 2026-10-09): draws aimed at the depth buffer (gfx_pc.c, SSB64_ZIMAGE). With the colour image
 * set to the Z buffer the RDP writes each pixel's colour into depth: the opening's desk-to-stage burst fills
 * the depth with black (nearest) and then draws its burst shape in white (farthest), so the stage drawn next
 * passes the depth test only inside the shape. sZImg >= 0 is the depth such draws write (gfx_pc converts the
 * colour); the colour buffer is left alone. Triangles and rectangles keep the scissor and viewport. The 3D
 * target's depth takes them (a rectangle is in native pixels: mapped onto the internal target when it is
 * apart). sZClean: the 3D depth still holds the frame's own clear, so a clear to the far value is a no-op
 * (the game clears depth at the start of every camera; skipping the redundant ones keeps the fights' cost). */
void gfx_soft_set_zimage(int z) { sZImg = z; }
static void zimg_span_rect(uint16_t *zb, int w, int h, int x0, int y0, int x1, int y1, uint16_t z) {
    if (zb == NULL) return;
    x0 = imax(x0, 0); y0 = imax(y0, 0); x1 = imin(x1, w); y1 = imin(y1, h);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) zb[y * w + x] = z;
}
static void zimg_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, uint16_t z) {
    const float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) return;
    const bool vp_ok = sVpW > 0 && sVpH > 0;
    const int vx0 = vp_ok ? imax(sVpX, 0) : 0, vy0 = vp_ok ? imax(sVpY, 0) : 0;
    const int vx1 = vp_ok ? imin(sVpX + sVpW, sW) : sW, vy1 = vp_ok ? imin(sVpY + sVpH, sH) : sH;
    const int minx = imax(imax(imax((int)fmin3(v0->x, v1->x, v2->x), sScX0), vx0), 0);
    const int maxx = imin(imin(imin(iceilf(fmax3(v0->x, v1->x, v2->x)), sScX1), vx1), sW);
    const int miny = imax(imax(imax((int)fmin3(v0->y, v1->y, v2->y), sScY0), vy0), 0);
    const int maxy = imin(imin(imin(iceilf(fmax3(v0->y, v1->y, v2->y)), sScY1), vy1), sH);
    const float sg = area > 0.0f ? 1.0f : -1.0f;
    for (int y = miny; y < maxy; y++) {
        const float py = y + 0.5f;
        for (int x = minx; x < maxx; x++) {
            const float px = x + 0.5f;
            if (edge(v1, v2, px, py) * sg >= 0.0f && edge(v2, v0, px, py) * sg >= 0.0f && edge(v0, v1, px, py) * sg >= 0.0f)
                sZB[y * sW + x] = z;
        }
    }
    sZClean = false;
}
static void zimg_rect(int x0, int y0, int x1, int y1, uint16_t z); /* below, with the targets */

/* B4: the colour kept from the last frame (GfxSetWidescreenFramebufferPersistence, gfx_pc.c): while on, a
 * frame starts from the previous picture, as the N64's colour buffer does when no camera clears it (the
 * burst opens over the frozen desk). */
static bool sPersist SAVESTATE_EXCLUDE;
void gfx_soft_set_persist(bool on) { sPersist = on; }

/* 2D path (HUD, menus, logo): gfx_pc.c emits PSP GU_SPRITES — TWO corners per
 * rect in a VertexColor {u16 u,v; RGBA; u16 x,y,z}: screen pixels + TEXEL UVs
 * (tile-origin already subtracted), combiner colour pre-folded by
 * gfx_eval_vertex_color. GU takes the sprite colour from the second vertex. */
typedef struct __attribute__((aligned(16))) {
    int16_t u, v; /* signed, as gfx_pc.c's VertexColor since 4843c2c (DKR's Vtx2D half, the lead's queue item
                   * from round 11b). A texrect that starts above or left of its tile origin writes -1: read
                   * unsigned, 65535 made a junk dvdy and the fight-start lights panel (97x33, t = -1) drew
                   * solid black where the real game shows its grey metal frame (round 26).
                   * SSB64_VTX2DSIGNED=0: read unsigned, as before. */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* Magnified 2D sprites (round 17): the N64 filters texture rectangles like triangles, so a sprite
 * drawn bigger than its texture comes out smooth. The port sampled them nearest: Smash's explosions,
 * dust and effect sprites (3D-scaled texrects, e.g. DK's entrance barrel blast, a 32x32 fire drawn
 * 87 px wide) came out in big square blocks. gfx_pc sets gfx_soft_2d_bilin from the draw's
 * G_TF_BILERP per sprite; a sprite magnified along either axis then takes the 3D path's bilerp at the
 * pixel centre. Shrunk sprites (the HUD at 240) keep the area filter, 1:1 ones the copy.
 * SSB64_2DBILIN=0: nearest, as before. */
bool gfx_soft_2d_bilin SAVESTATE_EXCLUDE;
static int sBilin2DOn SAVESTATE_EXCLUDE = -1;
static bool sNoDefer SAVESTATE_EXCLUDE; /* O2: a depth-tested rectangle is drawn now, never as the deferred fill */
static void draw_2d_sprites(const Vtx2D *q, size_t num_sprites);
static void draw_2d_sprites(const Vtx2D *q, size_t num_sprites) {
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { note_untextured("2d", (unsigned)sBoundTex[0]); tex = NULL; }
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
        static int sSigned SAVESTATE_EXCLUDE = -1;
        if (sSigned < 0) { const char *e = getenv("SSB64_VTX2DSIGNED"); sSigned = !(e && e[0] == '0'); }
        float u0 = sSigned ? a->u : (uint16_t) a->u, u1 = sSigned ? b->u : (uint16_t) b->u;
        float v0 = sSigned ? a->v : (uint16_t) a->v, v1 = sSigned ? b->v : (uint16_t) b->v;
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; float tu = u0; u0 = u1; u1 = tu; }
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; float tv = v0; v0 = v1; v1 = tv; }
        if (x1 <= x0 || y1 <= y0) continue;
        const float dudx = (u1 - u0) / (float)(x1 - x0);
        const float dvdy = (v1 - v0) / (float)(y1 - y0);
        int cx0 = imax(imax(x0, sScX0), 0), cx1 = imin(imin(x1, sScX1), sW);
        int cy0 = imax(imax(y0, sScY0), 0), cy1 = imin(imin(y1, sScY1), sH);
        if (sFB == sIntFB && cy0 < cy1) int_rows_touch(cy0, cy1);
        const int cr = b->r, cg = b->g, cb2 = b->b, ca = b->a; /* GU: colour of 2nd vertex */
        /* Per-sprite constants hoisted out of the pixel loop. amin folds the
         * texture-edge (0x55) and near-zero (8) alpha rejects. "plain" = the
         * texel is the final colour (no combiner input, or a white opaque
         * prim, which is what nearly every HUD sprite and menu tile uses). */
        const bool use_alpha = sUseAlpha;
        const int amin = use_alpha ? imax((sCurShader && sCurShader->cc.opt_texture_edge) ? 0x55 : 8, sAlphaRef) : 0;
        if (!tex && (!use_alpha || ca >= 255) && cx0 == 0 && cy0 == 0 && cx1 == sW && cy1 == sH && sFB == sNatFB && !sNoDefer) {
            /* covers the whole target: deferred (see sNatFill) */
            sNatStale = true;
            sNatFill = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255);
            continue;
        }
        target_ready();
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (gfx_soft_hud_box < 0) { const char *e = getenv("SSB64_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
        /* shrunk 1..3x along S: area-average (see texel_box) */
        const bool boxf = tex && gfx_soft_hud_box && dudx > 1.02f && dudx <= 3.0f;
        /* With blending off the alpha is never read, so only RGB has to be the
         * texel's (round 16: Dream Land's backdrop, texel RGB with a constant
         * alpha, ~48k px a frame took the general loop for nothing). */
        const bool texel_is_pixel = tex && rgb_tex && !rgb_texa &&
                           ((a_tex && ((!rgb_in && !a_in) || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255 && ca == 255))) ||
                            (!use_alpha && (!rgb_in || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255))));
        if (sBilin2DOn < 0) { const char *e = getenv("SSB64_2DBILIN"); sBilin2DOn = !(e && e[0] == '0'); }
        const bool mag2 = tex && sBilin2DOn && gfx_soft_2d_bilin && !boxf && (dudx < 0.999f || dvdy < 0.999f);
        const bool plain = !boxf && !mag2 && !t16 && texel_is_pixel;
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        const int32_t uf_row = tex ? to_fix16(u0 + (cx0 - x0 + 0.5f) * dudx) : 0; /* every row starts here */
        /* area filter a row at a time, through the cross-frame row cache */
        const bool boxc = boxf && cx1 > cx0 && cx1 - cx0 <= BOX_MAX;
        if (sMagRowOn < 0) { const char *e = getenv("SSB64_MAGROW"); sMagRowOn = !(e && e[0] == '0'); }
        const bool magrow = mag2 && sMagRowOn && cx1 > cx0 && cx1 - cx0 <= MAGROW_MAX;
        if (magrow) { sMagKey[0] = sMagKey[1] = -1; mag_cols(tw, ws2, mu, uf_row, duf, cx1 - cx0); }
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
                    fill32(row + cx0, argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255), cx1 - cx0);
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
            const int32_t vf_row = mag2 ? to_fix16(v) : 0; /* bilerp: texel coordinate of the pixel centre */
            const int iv = wrapf(ifloorf(v), th, wt2, mv);
            const int trow_base = iv * tw; /* texel index of this scanline's row start */
            int brow[4] = { trow_base }; uint32_t bw[4] = { 256 }; int nbr = 1;
            if (boxf && dvdy > 1.02f && dvdy <= 3.0f) {
                const float vt = v - 0.5f * dvdy, vb = vt + dvdy;
                nbr = 0;
                for (int r = ifloorf(vt); r < vb && nbr < 4; r++) {
                    const float lo = r > vt ? (float) r : vt, hi = (r + 1) < vb ? (float) (r + 1) : vb;
                    brow[nbr] = wrapf(r, th, wt2, mv) * tw;
                    bw[nbr++] = (uint32_t) ((hi - lo) * 256.0f + 0.5f);
                }
                if (nbr == 0) { brow[0] = trow_base; bw[0] = 256; nbr = 1; }
            }
            int32_t uf = uf_row;
            uint32_t boxtmp[BOX_MAX];
            const uint32_t *boxrow = NULL; /* the row's texels: area-averaged (boxc) or bilerped (magrow) */
            if (boxc && nbr == 1) {
                boxrow = box_row_cached(boxtmp, cx1 - cx0, sBoundTex[0], texpx, t16, brow[0], uf_row, duf, tw, ws2, mu,
                                        use_alpha);
            } else if (boxc) {
                /* Shrunk along T too: blend the cached S-averaged rows by
                 * coverage, exactly as texel_box2 does per pixel. Each row is
                 * accumulated before the next lookup (a lookup may compact
                 * the cache and move earlier rows). */
                const int n = cx1 - cx0;
                uint32_t ar[BOX_MAX], ag[BOX_MAX], ab[BOX_MAX], aa[BOX_MAX], aw = 0;
                memset(ar, 0, n * 4); memset(ag, 0, n * 4); memset(ab, 0, n * 4); memset(aa, 0, n * 4);
                for (int i = 0; i < nbr; i++) {
                    const uint32_t *r = box_row_cached(boxtmp, n, sBoundTex[0], texpx, t16, brow[i], uf_row, duf, tw, ws2, mu,
                                                       use_alpha);
                    const uint32_t w = bw[i];
                    for (int x = 0; x < n; x++) {
                        const uint32_t t = r[x], a = t >> 24, k = use_alpha ? a * w : 255u * w;
                        ar[x] += ((t >> 16) & 0xff) * k; ag[x] += ((t >> 8) & 0xff) * k; ab[x] += (t & 0xff) * k;
                        aa[x] += a * w;
                    }
                    aw += w;
                }
                for (int x = 0; x < n; x++) {
                    const uint32_t kd = use_alpha ? aa[x] : 255u * aw;
                    boxtmp[x] = (aw == 0 || kd == 0) ? 0
                              : ((aa[x] / aw) << 24) | ((ar[x] / kd) << 16) | ((ag[x] / kd) << 8) | (ab[x] / kd);
                }
                boxrow = boxtmp;
            }
            if (magrow) { /* bilerp of the whole row (see mag_row_build) */
                const int32_t sv = vf_row - 0x8000;
                const int iv0 = wrapf(sv >> 16, th, wt2, mv), iv1 = wrapf((sv >> 16) + 1, th, wt2, mv);
                const int fv = (sv >> 8) & 0xff, n = cx1 - cx0;
                const int s0 = mag_row_slot(iv0, -1, texpx, t16, tw, ws2, mu, uf_row, duf, n);
                const int s1 = mag_row_slot(iv1, s0, texpx, t16, tw, ws2, mu, uf_row, duf, n);
                const uint32_t *E0 = sMagE[s0], *O0 = sMagO[s0], *E1 = sMagE[s1], *O1 = sMagO[s1];
                int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                { /* lerp2 four pixels at a time: the same u32 lane arithmetic (no carries, see lerp2) */
                    const uint32x4_t m = vdupq_n_u32(0x00FF00FFu);
                    const uint32_t g = (uint32_t)(256 - fv), f = (uint32_t)fv;
                    for (; x + 4 <= n; x += 4) {
                        const uint32x4_t e = vandq_u32(vshrq_n_u32(vmlaq_n_u32(vmulq_n_u32(vld1q_u32(E0 + x), g), vld1q_u32(E1 + x), f), 8), m);
                        const uint32x4_t o = vandq_u32(vshrq_n_u32(vmlaq_n_u32(vmulq_n_u32(vld1q_u32(O0 + x), g), vld1q_u32(O1 + x), f), 8), m);
                        vst1q_u32(boxtmp + x, vorrq_u32(e, vshlq_n_u32(o, 8)));
                    }
                }
#endif
                for (; x < n; x++) boxtmp[x] = lerp2(E0[x], E1[x], fv) | (lerp2(O0[x], O1[x], fv) << 8);
                boxrow = boxtmp;
            }
            if ((boxc || magrow) && texel_is_pixel) {
                /* the averaged texel is the pixel: as the general loop below
                 * with no combiner work (white opaque input changes nothing) */
                for (int x = cx0; x < cx1; x++) {
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
                /* the whole run inside the texture: no wrap per texel (any wrap
                 * mode leaves an in-range coordinate as it is) */
                const int64_t ul = (int64_t)uf + (int64_t)(cx1 - cx0 - 1) * duf;
                if (!use_alpha && uf >= 0 && ul >= 0 && (uf >> 16) < tw && (int)(ul >> 16) < tw) {
                    const uint32_t *src = (const uint32_t *)texpx + trow_base;
                    for (int x = cx0; x < cx1; x++, uf += duf) row[x] = src[uf >> 16] | 0xFF000000u;
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
            /* RGB is the input colour, alpha the texel's (times the input's):
             * HUD glyphs and icons tinted by their prim (MK64 28cc007, the
             * minimap). The general loop below with its dead branches gone. */
            if (!rgb_tex && a_tex && use_alpha) {
                for (int x = cx0; x < cx1; x++, uf += duf) {
                    const uint32_t t = (boxc || magrow) ? boxrow[x - cx0]
                                     : boxf ? texel_box2(texpx, t16, brow, bw, nbr, uf, duf, tw, ws2, mu, use_alpha)
                                     : mag2 ? bilerp(texpx, t16, tw, th, ws2, wt2, uf, vf_row, mu, mv)
                                            : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                    const int ta = (int)(t >> 24), fa = !a_in ? ta : (ta * ca) / 255;
                    if (fa < amin) continue;
                    if (fa >= 255) { row[x] = argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255); continue; }
                    const uint32_t d = row[x];
                    const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                    row[x] = argb((uint8_t)((cr*fa + dr*(255-fa))/255), (uint8_t)((cg*fa + dg*(255-fa))/255), (uint8_t)((cb2*fa + db*(255-fa))/255), 255);
                }
                continue;
            }
            /* General: per-channel combiner (RGB and alpha each from the
             * texel, the input colour, or their product). */
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = (boxc || magrow) ? boxrow[x - cx0]
                                 : boxf ? texel_box2(texpx, t16, brow, bw, nbr, uf, duf, tw, ws2, mu, use_alpha)
                                 : mag2 ? bilerp(texpx, t16, tw, th, ws2, wt2, uf, vf_row, mu, mv)
                                        : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                const int ta = (t>>24)&0xff;
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff;
                int fr, fg, fb, fa;
                if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }
                else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }
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
static uint16_t *sIntZB SAVESTATE_EXCLUDE;
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
/* B4: a depth-image rectangle (gfx_scegu_draw_triangles_2d), native pixels, the native scissor */
static void zimg_rect(int x0, int y0, int x1, int y1, uint16_t z) { /* native pixels, the native scissor */
    if (z == 0xFFFF && sZClean) return;
    x0 = imax(x0, sScX0); y0 = imax(y0, sScY0); x1 = imin(x1, sScX1); y1 = imin(y1, sScY1);
    if (x0 >= x1 || y0 >= y1) return;
    zimg_span_rect(sNatZB, sNatW, sNatH, x0, y0, x1, y1, z);
    if (!sUnified && sNatW > 0 && sNatH > 0)
        zimg_span_rect(sIntZB, sIntW, sIntH, x0 * sIntW / sNatW, y0 * sIntH / sNatH,
                       (x1 * sIntW + sNatW - 1) / sNatW, (y1 * sIntH + sNatH - 1) / sNatH, z);
    if (z != 0xFFFF) sZClean = false;
}

/* O2 (fullcmp 2026-10-09): rectangles with G_ZS_PRIM depth-test against the 3D depth at the primitive depth
 * (gfx_pc.c, SSB64_PRIMDEPTH): Smash's particles (lbparticle.c sets each one's screen z) and the opening's
 * wallpaper behind the burst. sRectZ >= 0 is that depth in this renderer's units, sRectZUpd = Z_UPD. Each
 * such rectangle is drawn alone: the pixels where the depth fails keep their colour (saved before, put back
 * after), and with Z_UPD the drawn ones take the depth. While the depth holds only the frame's clear every
 * pixel passes, so nothing extra runs (a wallpaper drawn first each frame). */
static int sRectZ SAVESTATE_EXCLUDE = -1;
static bool sRectZUpd SAVESTATE_EXCLUDE;
void gfx_soft_set_rect_z(int z, bool upd) { sRectZ = z; sRectZUpd = upd; }
static uint16_t *rect_zb(int x, int y) { /* the 3D depth under native pixel (x, y) */
    if (sUnified || sIntZB == NULL) return &sNatZB[y * sNatW + x];
    return &sIntZB[sN2I_y[y] * sIntW + sN2I_x[x]];
}
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    if (sZImg >= 0) { /* B4: into the depth buffer (see zimg_rect) */
        for (size_t s = 0; s < num_sprites; s++) {
            const Vtx2D *a = &q[s*2+0], *b = &q[s*2+1];
            zimg_rect(imin(a->x, b->x), imin(a->y, b->y), imax(a->x, b->x), imax(a->y, b->y), (uint16_t)sZImg);
        }
        return;
    }
    if (sRectZ < 0 || (sZClean && !sRectZUpd) || sFB != sNatFB) { draw_2d_sprites(q, num_sprites); return; }
    static uint32_t save[320 * 240] SAVESTATE_EXCLUDE; /* the rectangle's colour before it drew (a bigger one draws untested) */
    const uint16_t z = (uint16_t)sRectZ;
    for (size_t s = 0; s < num_sprites; s++) {
        const Vtx2D *a = &q[s*2+0], *b = &q[s*2+1];
        const int cx0 = imax(imax(imin(a->x, b->x), sScX0), 0), cx1 = imin(imin(imax(a->x, b->x), sScX1), sW);
        const int cy0 = imax(imax(imin(a->y, b->y), sScY0), 0), cy1 = imin(imin(imax(a->y, b->y), sScY1), sH);
        const int w = cx1 - cx0;
        if (w <= 0 || cy1 <= cy0 || (size_t)w * (size_t)(cy1 - cy0) > sizeof(save) / sizeof(save[0])) { draw_2d_sprites(a, 1); continue; }
        nat_ready();
        for (int y = cy0; y < cy1; y++) memcpy(&save[(y - cy0) * w], &sFB[y * sW + cx0], (size_t)w * 4);
        sNoDefer = true;
        draw_2d_sprites(a, 1);
        sNoDefer = false;
        for (int y = cy0; y < cy1; y++) {
            for (int x = cx0; x < cx1; x++) {
                uint16_t *pz = rect_zb(x, y);
                uint32_t *pc = &sFB[y * sW + x];
                if (z > *pz) *pc = save[(y - cy0) * w + (x - cx0)];       /* behind: the old pixel */
                else if (sRectZUpd && *pc != save[(y - cy0) * w + (x - cx0)]) *pz = z;
            }
        }
        if (sRectZUpd) sZClean = false;
    }
}
/* Presented image: always the native target (composited at end_frame). */
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }

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

/* Row bookkeeping for the internal target (diddy-kong-racing a0ee8b5: a full-screen
 * seed + composite + clear per 2D<->3D switch; Smash switches 3-6 times a
 * frame). The seed is lazy -- a row is copied from native the first time a
 * triangle, rect or background touches it -- and the composite and clear
 * cover only the rows seeded or drawn since the last composite. The output
 * is identical: untouched rows hold no drawn (alpha > 0) pixel. */
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

/* Nearest-upscale the internal 3D image into native where the 3D wrote
 * (alpha == 255), then clear it for any later 3D batch this frame. */
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
    nat_ready();
    sIntClean = false;
    if (sIntW == sNatW && sIntH == sNatH) {
        for (int i = 0; i < sIntW * sIntH; i++) sIntFB[i] = sNatFB[i] & 0x00FFFFFFu;
        return;
    }
    /* lazy: rows are copied as draws reach them (int_rows_touch) */
    sSeedPending = true;
    if (++sSeedGen == 0) { memset(sRowGen, 0, sizeof(sRowGen)); sSeedGen = 1; }
}

/* gfx_pc direct-call: switch the draw target for the next batch. */
void gfx_scegu_set_sky_pass(bool on);
uint32_t gfx_soft_phase_switches SAVESTATE_EXCLUDE; /* 2D<->3D switches (PERF "sw N/f") */
void gfx_scegu_set_phase(int is3d) {
    if ((bool)is3d == sPhase3D) return;
    gfx_soft_phase_switches++;
    if (sSkyPass) { sPhase3D = (bool)is3d; return; } /* unified: one target, stay on the sky buffer */
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
    unpack_lut_init();
#if defined(HH_CM_NEON)
    { const char *e = getenv("SSB64_CMNEON"); if (e && e[0] == '0') sCmNeon = 0; }
    { extern int hh_neon_enabled(void); if (!hh_neon_enabled()) sCmNeon = 0; } /* bench.cfg neon=0 */
#endif
    { extern int hh_cfg_switch(const char *env, const char *key, int dflt); /* SSB64_LERPROWS, bench.cfg lerprows= */
      if (hh_cfg_switch("SSB64_LERPROWS", "lerprows", 1) == 0) gfx_soft_lerp_rows = 0;
      /* the 2026-10-07 speed round's switches (bench.cfg fastrows= spanvec= zskip=); the bench's A/B flips them */
      if (hh_cfg_switch("SSB64_FASTROWS", "fastrows", 1) == 0) gfx_soft_fastrows = 0;
      if (hh_cfg_switch("SSB64_SPANVEC", "spanvec", 1) == 0) gfx_soft_spanvec = 0;
      if (hh_cfg_switch("SSB64_ZSKIP", "zskip", 1) == 0) gfx_soft_zskip = 0; }
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
 * frame is kept and every frame starts from it instead of black. */
static uint32_t *sPauseFB SAVESTATE_EXCLUDE;
static int sPauseW, sPauseH;
static bool sPauseActive, sPauseValid;
void gfx_soft_capture_pause_background(void) {
    if (!sNatFB || sNatW <= 0 || sNatH <= 0) return;
    nat_ready();
    if (sPauseW != sNatW || sPauseH != sNatH) {
        free(sPauseFB);
        sPauseFB = (uint32_t *)malloc((size_t)sNatW * sNatH * 4);
        sPauseW = sPauseFB ? sNatW : 0; sPauseH = sPauseFB ? sNatH : 0;
    }
    if (sPauseFB) { memcpy(sPauseFB, sNatFB, (size_t)sNatW * sNatH * 4); sPauseValid = true; }
}
void gfx_soft_set_pause_background_active(bool on) {
    sPauseActive = on;
    if (!on) sPauseValid = false;
}

static void gfx_soft_start_frame(void) {
    ensure_targets();
    PROF_BEGIN();
    if (sPauseActive && sPauseValid && sPauseW == sNatW && sPauseH == sNatH) {
        memcpy(sNatFB, sPauseFB, (size_t)sNatW * sNatH * 4);
        sNatStale = false;
    } else if (sPersist) {
        nat_ready(); /* B4: the last picture stays (a clear still pending from it lands first) */
    } else {
        sNatStale = true; /* cleared lazily, or never if the frame's first draw covers it */
        sNatFill = 0;
    }
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    sZClean = true;
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    PROF_END(PROF_CLR);
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
/* The opening's paper shot (round 16, mvopeningroom.c under PORT): the N64
 * frames the Nintendo/HAL sheet to fill its picture exactly, so the logo screen
 * seems to become the paper. The square screen's extra rows showed the room
 * past the sheet's edges. For those ticks the rows outside the N64 picture's own
 * band (its scissor, N64 rows 10..230) are black, as the real game's are, and
 * they open as the camera pulls back. GfxSetOpeningBars(closed): 1 = the N64
 * band only, 0 = off; read once, by the next frame that reaches end_frame.
 * No-op at 4:3. SSB64_OPENBARS=0: off (A/B). */
static float sBars SAVESTATE_EXCLUDE;
void GfxSetOpeningBars(float closed) { sBars = closed < 0.0f ? 0.0f : (closed > 1.0f ? 1.0f : closed); }
static void apply_bars(void) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("SSB64_OPENBARS"); on = !(e && e[0] == '0'); }
    const float k = sBars; sBars = 0.0f;
    if (!on || k <= 0.0f || !sNatFB) return;
    const float sc = (float)sNatW / 320.0f, top = 0.5f * ((float)sNatH - 240.0f * sc);
    if (top < 1.0f) return;
    const int y0 = (int)ceilf(top + 10.0f * sc - 0.5f), y1 = (int)ceilf(top + 230.0f * sc - 0.5f); /* R23 centres */
    const int t = (int)(y0 * k + 0.5f), b = sNatH - (int)((sNatH - y1) * k + 0.5f);
    if (t > 0) memset(sNatFB, 0, (size_t)t * sNatW * 4);
    if (b < sNatH) memset(sNatFB + (size_t)b * sNatW, 0, (size_t)(sNatH - b) * sNatW * 4);
}
static void gfx_soft_end_frame(void) {
    if (sSkyPass) gfx_scegu_set_sky_pass(false);
    nat_ready(); /* nothing reached native this frame: present it cleared */
    if (!sUnified && sIntDirty) composite_internal();
    apply_bars();
    gfx_soft_last_frame_had_3d = sHad3D;
    gfx_soft_frames_rendered++;
    select_target(false); /* leave native current for present/menu/dumps */
}
static void gfx_soft_finish_render(void) { }

/* -------------------------------------------------------------- shims --- */
/* gfx_pc.c calls these sceGu/texman helpers directly; software equivalents. */
void gfx_scegu_set_cull_mode(uint32_t cull) { (void)cull; /* GE-only API; winding cull lives in gfx_sp_tri1 */ }
void gfx_scegu_set_texfunc_add(bool on) { sTexAdd = on; }
/* Texture edges without FORCE_BL (G_RM_AA_ZB_TEX_EDGE; gfx_pc, SSB64_EDGEOPAQUE): the RDP takes coverage from
 * the texel's alpha and never blends such a pixel with memory -- it is written as the texel's own colour, or
 * not at all (OoT 7120396: blending by alpha mixed each cutout's rim with whatever was behind at draw time). */
void gfx_soft_set_edge_opaque(bool on) { sEdgeOpaque = on; }
/* SSB64_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; sFogPx = false; }
void gfx_soft_set_fog(bool on, int r, int g, int b, int a) { sFogPx = on; sFogR = r; sFogG = g; sFogB = b; sFogA = a; }
void gfx_scegu_set_force_in(bool on) { sForceIn = on; }
void gfx_scegu_set_ui_mode(bool on) {
    sUiMode = on;
    if (gfx_soft_hud_box < 0) { const char *e = getenv("SSB64_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
}

/* S2DEX background (gfx_pc gfx_s2dex_bg): nearest blit of a 16-bit RGBA5551
 * image in host order (fmt 0: the CPU-decoded JPEG rooms) or a CI8 image with
 * a big-endian RGBA16 TLUT (fmt 1) into the current target. No depth. */
void gfx_soft_draw_bg(const void *img, int img_w, int img_h, int fmt, const uint8_t *tlut, int x0, int y0, int x1,
                      int y1, float u0, float v0, float dudx, float dvdy) {
    if (!img || img_w <= 0 || img_h <= 0) return;
    if (fmt == 1 && !tlut) return;
    if (x0 < sScX0) x0 = sScX0; if (y0 < sScY0) y0 = sScY0;
    if (x1 > sScX1) x1 = sScX1; if (y1 > sScY1) y1 = sScY1;
    target_ready();
    if (sFB == sIntFB && y0 < y1) int_rows_touch(y0, y1);
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

/* Half-resolution sky. OoT draws the skybox first, over the whole screen, and
 * the world then covers most of it: ~57k of the frame's ~190k 3D pixels in
 * Hyrule Field for a soft gradient. Between the game's SKY tags the triangles
 * go to a half-size buffer (a quarter of the pixels) that is then bilinearly
 * doubled into the frame where the sky drew. Unified (100%) targets only;
 * otherwise the tags are ignored. */
static uint32_t *sSkyFB; static uint16_t *sSkyZB; static int sSkyW, sSkyH;
static uint32_t *sSkySaveFB; static uint16_t *sSkySaveZB; static int sSkySaveW, sSkySaveH;
static int sSkySaveVp[4], sSkySaveSc[4];
static int sSkyNeon SAVESTATE_EXCLUDE = 1; /* SSB64_SKYNEON=0: the scalar rows (A/B) */

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
    if (!sInit) { const char *e = getenv("SSB64_SKYNEON"); sInit = 1; if (e && e[0] == '0') sSkyNeon = 0; }
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
    if (gfx_trace_frames > 0) port_log("  soft: sky pass %d (unified %d nat %d)\n", (int) on, (int) sUnified, (int) (sFB == sNatFB));
    if (on == sSkyPass) return;
    if (on) {
        if (!sUnified || sFB != sNatFB || getenv("SSB64_FULLSKY") != NULL) return;
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
static int sBlendFast SAVESTATE_EXCLUDE = -1; /* SSB64_BLENDFAST=0: the per-texel loop, for A/B */
bool gfx_soft_blend_textures(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                             int uls1, int ult1, int shs1, int sht1, int rgb_mode, int a_mode,
                             int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka) {
    if (dst == 0 || dst >= MAX_TEXTURES || id0 >= MAX_TEXTURES || id1 >= MAX_TEXTURES) return false;
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
        sTex[dst].px = out; sTex[dst].is16 = false; sTex[dst].w = W; sTex[dst].h = H; sTex[dst].valid = true;
    }
    if (sBlendFast < 0) { const char *e = getenv("SSB64_BLENDFAST"); sBlendFast = !(e && e[0] == '0'); }
    if (W <= BLEND_MAXW && sBlendFast) {
        /* The loop below with its per-texel work hoisted: the column wraps
         * (two divides a texel) come from a table, the mode tests pick a
         * source and a lerp once. Same selections, same hh_lerp8: identical
         * texels. (Kokiri's scrolling water re-blends 64x64 every frame at
         * ~190 instructions a texel under qemu.) SSB64_BLENDFAST=0: the old loop. */
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

int  texman_peak_percent(void) { int p = (int)(gfx_soft_arena_peak * 100 / TEXARENA_SZ); gfx_soft_arena_peak = sArenaUsed; return p; }
int  texman_usage_percent(void) { return (int)(sArenaUsed * 100 / TEXARENA_SZ); }
int  texman_can_hold(unsigned int num, unsigned int bytes) {
    (void)num;
    /* TEX32: an expanded 16-bit upload takes twice its source size (bytes is
     * sized by the source format, 2 bytes a texel; 32-bit sources are
     * over-counted, so it flushes early). */
    return sArenaUsed + (size_t) bytes * (tex32_expand(bytes / 2u) ? 2u : 1u) <= TEXARENA_SZ;
}
void texman_bind_tex(unsigned int num) { sBoundTex[sTexUploadTile] = num; }
void texman_clear(void) { sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; box_cache_clear(); gfx_soft_tex_resets++; }

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

/* Debug: dump the framebuffer as a binary PPM (for headless verification). */
/* The screen filled by a stretch (gfx_pc SSB64_FILLBORDER=4 nearest, the default, or 1 bilinear; the user's
 * pick): the game's TV-safe frame as drawn on the native target, x 7..232 and y 10..230 on a square output (N64
 * 10..310 x 10..230, R23's pixel centres), over the whole screen as it goes out (the present's colour
 * conversion and the dumps), so the rendering itself is unchanged. Taps stay inside the frame. */
#define STR_MAX 1024
static uint16_t sStrX0[STR_MAX] SAVESTATE_EXCLUDE, sStrX1[STR_MAX] SAVESTATE_EXCLUDE, sStrY0[STR_MAX] SAVESTATE_EXCLUDE, sStrY1[STR_MAX] SAVESTATE_EXCLUDE;
static uint8_t sStrXF[STR_MAX] SAVESTATE_EXCLUDE, sStrYF[STR_MAX] SAVESTATE_EXCLUDE;
static int sStrW SAVESTATE_EXCLUDE = -1, sStrH SAVESTATE_EXCLUDE = -1, sStrLo SAVESTATE_EXCLUDE, sStrHi SAVESTATE_EXCLUDE;
static uint16_t sStrNX[STR_MAX] SAVESTATE_EXCLUDE, sStrNY[STR_MAX] SAVESTATE_EXCLUDE; /* nearest (fill mode 4) */
static int sStrMode SAVESTATE_EXCLUDE;
static void str_map(uint16_t *m0, uint16_t *m1, uint8_t *mf, int n, float a, float b, int *plo, int *phi) {
    const int lo = (int) ceilf(a - 0.5f), hi = (int) floorf(b - 0.5f);
    for (int i = 0; i < n && i < STR_MAX; i++) {
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
/* true when the native frame goes out stretched (square output, the fill mode 1) */
bool gfx_soft_stretch_on(void) {
    extern int gfx_fill_mode_get(void);
    extern int gfx_movie_frame_on(void);
    const int mode = gfx_movie_frame_on() ? 0 : gfx_fill_mode_get(); /* the movie maps its own crop */
#if defined(MIYOO)
    /* The Miyoo Mini's 320x240 is 4:3 already, but its N64 frame is still 300x220 inside a black border (the game's
     * own sprite code clips the stage wallpapers to it, so widening the frame, mode 2, left black strips beside the
     * stage): the same stretch as the handheld's (mode 4, the user's sharp pick for Smash) fills the screen with it. */
    if (!sNatFB || sNatW > STR_MAX || sNatH > STR_MAX || (mode != 1 && mode != 4)) return false;
#else
    if (!sNatFB || sNatW > STR_MAX || sNatH > STR_MAX || sNatW * 3 >= sNatH * 4 - 1 || (mode != 1 && mode != 4)) return false;
#endif
    sStrMode = mode;
    if (sStrW != sNatW || sStrH != sNatH) {
        const float kx = (float) sNatW / 320.0f, ky = (float) sNatH / 240.0f;
        /* the pixels the frame's scissor keeps (R23: centres inside), [a, b) */
        str_map(sStrX0, sStrX1, sStrXF, sNatW, ceilf(10.0f * kx - 0.5f), ceilf(310.0f * kx - 0.5f), &sStrLo, &sStrHi);
        str_map(sStrY0, sStrY1, sStrYF, sNatH, ceilf(10.0f * ky - 0.5f), ceilf(230.0f * ky - 0.5f), NULL, NULL);
        for (int i = 0; i < sNatW; i++) { sStrX0[i] -= (uint16_t) sStrLo; sStrX1[i] -= (uint16_t) sStrLo; } /* into the lerped row */
        { /* nearest: the source pixel under each output pixel's centre */
            const float ax = ceilf(10.0f * kx - 0.5f), bx = ceilf(310.0f * kx - 0.5f), ay = ceilf(10.0f * ky - 0.5f), by = ceilf(230.0f * ky - 0.5f);
            for (int i = 0; i < sNatW; i++) sStrNX[i] = (uint16_t) (ax + floorf(((float) i + 0.5f) * (bx - ax) / (float) sNatW));
            for (int i = 0; i < sNatH; i++) sStrNY[i] = (uint16_t) (ay + floorf(((float) i + 0.5f) * (by - ay) / (float) sNatH));
        }
        sStrW = sNatW; sStrH = sNatH;
    }
    return true;
}
/* output row y of the stretched frame, ARGB (gfx_soft_stretch_on first) */
void gfx_soft_stretch_row(uint32_t *dst, int y) {
    if (sStrMode == 4) {
        const uint32_t *r = sNatFB + (size_t) sStrNY[y] * sNatW;
        for (int x = 0; x < sNatW; x++) dst[x] = r[sStrNX[x]];
        return;
    }
    uint32_t v[STR_MAX];
    const uint32_t *r0 = sNatFB + (size_t) sStrY0[y] * sNatW, *r1 = sNatFB + (size_t) sStrY1[y] * sNatW;
    const int fy = sStrYF[y], lo = sStrLo, n = sStrHi - sStrLo + 1;
    int k = 0;
    /* the two source rows lerped along y (lerp2's lanes: B,R and G,A), the columns the frame covers */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    {
        const uint32x4_t m = vdupq_n_u32(0x00FF00FFu);
        const uint32_t g = (uint32_t) (256 - fy), f = (uint32_t) fy;
        for (; k + 4 <= n; k += 4) {
            const uint32x4_t a = vld1q_u32(r0 + lo + k), b = vld1q_u32(r1 + lo + k);
            const uint32x4_t e = vandq_u32(vshrq_n_u32(vmlaq_n_u32(vmulq_n_u32(vandq_u32(a, m), g), vandq_u32(b, m), f), 8), m);
            const uint32x4_t o = vandq_u32(vshrq_n_u32(vmlaq_n_u32(vmulq_n_u32(vandq_u32(vshrq_n_u32(a, 8), m), g), vandq_u32(vshrq_n_u32(b, 8), m), f), 8), m);
            vst1q_u32(v + k, vorrq_u32(e, vshlq_n_u32(o, 8)));
        }
    }
#endif
    for (; k < n; k++) {
        const uint32_t a = r0[lo + k], b = r1[lo + k];
        v[k] = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, fy) | (lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, fy) << 8);
    }
    /* then along x */
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    { /* four columns a pass (f-zero-x ff35c9e): the taps loaded lane by lane, each column's weight
       * repeated over its four bytes; lerp2's (a * (256 - f) + b * f) >> 8 as a * 256 - a * f + b * f in
       * u16 lanes (the true value is 0..65280, so the wrap-around arithmetic lands on it exactly) */
        static const uint8_t kRep[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 };
        const uint8x8_t ilo = vld1_u8(kRep), ihi = vld1_u8(kRep + 8);
        for (; x + 4 <= sNatW; x += 4) {
            uint32x4_t A = vld1q_dup_u32(v + sStrX0[x]), B = vld1q_dup_u32(v + sStrX1[x]);
            A = vld1q_lane_u32(v + sStrX0[x + 1], A, 1); B = vld1q_lane_u32(v + sStrX1[x + 1], B, 1);
            A = vld1q_lane_u32(v + sStrX0[x + 2], A, 2); B = vld1q_lane_u32(v + sStrX1[x + 2], B, 2);
            A = vld1q_lane_u32(v + sStrX0[x + 3], A, 3); B = vld1q_lane_u32(v + sStrX1[x + 3], B, 3);
            uint32_t f4;
            memcpy(&f4, sStrXF + x, 4);
            const uint8x8_t F = vreinterpret_u8_u32(vdup_n_u32(f4));
            const uint8x8_t Flo = vtbl1_u8(F, ilo), Fhi = vtbl1_u8(F, ihi);
            const uint8x16_t a8 = vreinterpretq_u8_u32(A), b8 = vreinterpretq_u8_u32(B);
            uint16x8_t l16 = vshll_n_u8(vget_low_u8(a8), 8), h16 = vshll_n_u8(vget_high_u8(a8), 8);
            l16 = vmlal_u8(vmlsl_u8(l16, vget_low_u8(a8), Flo), vget_low_u8(b8), Flo);
            h16 = vmlal_u8(vmlsl_u8(h16, vget_high_u8(a8), Fhi), vget_high_u8(b8), Fhi);
            vst1q_u8((uint8_t *)(dst + x), vcombine_u8(vshrn_n_u16(l16, 8), vshrn_n_u16(h16, 8)));
        }
    }
#endif
    for (; x < sNatW; x++) {
        const uint32_t a = v[sStrX0[x]], b = v[sStrX1[x]];
        const int fx = sStrXF[x];
        dst[x] = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, fx) | (lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, fx) << 8);
    }
}

void gfx_soft_dump_ppm(const char *path) {
    if (!sNatFB) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sNatW, sNatH);
    const bool st = gfx_soft_stretch_on(); /* what goes to the screen */
    uint32_t row[STR_MAX];
    for (int y = 0; y < sNatH; y++) {
        const uint32_t *src = sNatFB + (size_t) y * sNatW;
        if (st) { gfx_soft_stretch_row(row, y); src = row; }
        for (int x = 0; x < sNatW; x++) {
            const uint32_t p = src[x];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, f);
        }
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
