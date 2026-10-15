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
typedef struct { float u, v; uint8_t r, g, b, a; float x, y, z; float invw; uint8_t kr, kg, kb, ka; } SoftVtx;
/* kr..ka (GoldenEye, PORT_GE_RAWCC): the combiner's value at texel 0. The pixel is then
 * K + (vertex - K) * texel (vertex = the value at texel 255): fogged and ENV-lerped
 * surfaces. All zero = the plain texture x shade paths. */

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
    uint8_t nlev;       /* mip levels built at upload (GoldenEye): lev[0] = px, each half the last */
    uint16_t mw, mh;    /* lev[0]'s size: w x h, or the 2^n part of a 2^n + 1 LOD texture */
    const void *lev[7];
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
#define TEX32_SMALL 4096
int gfx_soft_tex32_mode(void) {
    static int mode SAVESTATE_EXCLUDE = -1;
    if (mode < 0) { const char *e = getenv("GE_TEX32"); mode = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : PORT_TEX32; }
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
/* Mipmaps (GoldenEye). Minified 3D surfaces used to take the nearest texel of the
 * full-size image, so a distant floor sparkled; the N64 mipmaps GE's floors (its combiner
 * lerps the next level by LOD_FRACTION). Every texture gets box-filtered levels at upload
 * while both sides halve, colour weighted by alpha so cut-out edges don't darken, in the
 * texture's own storage format; raster_tri picks a level per 16-pixel span from the UV
 * derivatives and blends two levels by the fraction where it matters -- trilinear, as the
 * RDP does for GE's floors (GE_MIPS=0: off, GE_MIPBIAS: level bias, + = blurrier). */
static inline uint32_t lerp_argb(uint32_t a, uint32_t b, uint32_t f) { /* f in 0..256 */
    const uint32_t rb = ((((a & 0xFF00FFu) * (256u - f)) + ((b & 0xFF00FFu) * f)) >> 8) & 0xFF00FFu;
    const uint32_t ag = ((((a >> 8) & 0xFF00FFu) * (256u - f)) + (((b >> 8) & 0xFF00FFu) * f)) & 0xFF00FF00u;
    return rb | ag;
}
int gfx_soft_mips SAVESTATE_EXCLUDE = -1;
/* Full trilinear by default (round 43, the user's correct-over-fast audit: +1.0% on the Dam): the two
 * levels blended across the whole step, as the RDP's LOD_FRACTION does. GE_TRILIN=0: the old "brilinear"
 * (level L alone for the first and last quarter of each step). */
static int sTrilin SAVESTATE_EXCLUDE;
/* Perspective-correct shade (round 43, the user: the floor's baked shadow "changing shape" as the view
 * turns; on by default after the before/after, GE_PSHADE=0: the RDP's rule). The RDP steps the vertex colours linearly on screen, so on a big near floor triangle the shading
 * slides with the viewing angle -- and more so as the clipper recuts it. Here textured
 * perspective triangles step colour/w and 1/w like their u/v: exact at every 16-pixel span end, linear
 * between (as the texture). Not the RDP's rule: the shading stays put on the surface. Depth-tested
 * scenery only: the no-depth sky keeps the RDP's screen gradient (its clouds changed brightness). */
static int sPShade SAVESTATE_EXCLUDE = -1;
static float sMipScale SAVESTATE_EXCLUDE = 1.0f; /* rho^2 scale: GE_MIPBIAS b multiplies it by 4^b */
static float sMipLin SAVESTATE_EXCLUDE = 1.0f;   /* the same as a rho scale, 2^b (GE_MIPLOD's level choice) */
/* The RDP's TRILERP, (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0, with the combiner's rounding (angrylion
 * color_combiner_equation: ((a - b) * c + (d << 8) + 0x80) >> 8) per channel; f 0..255. GE_MIPLOD's level blend. */
static inline uint32_t rdp_trilerp(uint32_t a, uint32_t b, uint32_t f) {
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const int x = (int)((a >> sh) & 0xffu), y = (int)((b >> sh) & 0xffu);
        r |= (uint32_t)(((y - x) * (int)f + (x << 8) + 0x80) >> 8) << sh; /* 0..255: x * 256 + (y - x) * f >= x */
    }
    return r;
}
static inline uint16_t pack5551(uint32_t c) {
    return (uint16_t)(((c >> 19) & 0x1f) | (((c >> 11) & 0x1f) << 5) | (((c >> 3) & 0x1f) << 10) | ((c >> 31) << 15));
}
/* One mip level: w x h (both even) box-filtered into w/2 x h/2 (build_mips). */
static void mip_down(const void *src, int w, int h, bool is16, void *dst) {
    const int nw = w / 2, nh = h / 2;
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            const int i0 = (2 * y) * w + 2 * x, i1 = i0 + w;
            const uint32_t q[4] = { texel_at(src, is16, i0), texel_at(src, is16, i0 + 1),
                                    texel_at(src, is16, i1), texel_at(src, is16, i1 + 1) };
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0, ur = 0, ug = 0, ub = 0;
            for (int k = 0; k < 4; k++) {
                const uint32_t a = q[k] >> 24, r = (q[k] >> 16) & 0xff, g = (q[k] >> 8) & 0xff, b = q[k] & 0xff;
                sa += a; sr += r * a; sg += g * a; sb += b * a; ur += r; ug += g; ub += b;
            }
            uint32_t r, g, b;
            if (sa) { r = (sr + sa / 2) / sa; g = (sg + sa / 2) / sa; b = (sb + sa / 2) / sa; }
            else    { r = (ur + 2) / 4; g = (ug + 2) / 4; b = (ub + 2) / 4; }
            const uint32_t c = argb((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)((sa + 2) / 4));
            if (is16) ((uint16_t *)dst)[y * nw + x] = pack5551(c);
            else      ((uint32_t *)dst)[y * nw + x] = c;
        }
    }
}
static void build_mips(SoftTex *t) {
    if (gfx_soft_mips < 0) {
        const char *e = getenv("GE_MIPS");
        gfx_soft_mips = !(e && e[0] == '0');
        { const char *t = getenv("GE_TRILIN"); sTrilin = !(t && t[0] == '0'); }
        e = getenv("GE_MIPBIAS");
        /* in quarter levels (round 22, the user: "could be faster" -- a positive bias takes the smaller
         * levels sooner in the distance, cheaper and blurrier; off by default): 4^b from whole steps and
         * the exact square roots (the handheld's powf/exp2f are off, and would differ from the desktop's) */
        if (e) {
            int q = (int)((float)atof(e) * 4.0f + ((float)atof(e) < 0.0f ? -0.5f : 0.5f)); /* quarters */
            const float r2 = sqrtf(2.0f);
            while (q >= 4) { sMipScale *= 4.0f; q -= 4; }
            while (q <= -4) { sMipScale *= 0.25f; q += 4; }
            /* 4^(q/4) = 2^(q/2): q = 1 sqrt 2, 2 = 2, 3 = 2 sqrt 2 */
            if (q > 0) sMipScale *= q == 1 ? r2 : q == 2 ? 2.0f : 2.0f * r2;
            if (q < 0) sMipScale /= q == -1 ? r2 : q == -2 ? 2.0f : 2.0f * r2;
        }
        sMipLin = sqrtf(sMipScale); /* exact for the whole and half steps (powers of two, sqrt 2 x them) */
    }
    t->mw = (uint16_t)t->w; t->mh = (uint16_t)t->h;
    if (!gfx_soft_mips) return;
    int w = t->w, h = t->h;
    const void *src = t->px;
    /* GoldenEye's LOD textures carry one border texel, 2^n + 1 (the file select's 65x65 folder
     * crest, its own levels 33, 17, ... clamped at the last): the chain is mipped from the
     * 2^n part, a cropped copy that becomes level 0. raster_tri keeps the texel scale of the
     * full image (su/sv), so only the border column/row is lost, where the RDP clamps. */
    const int cw = (w > 2 && (w & 1) && ((w - 1) & (w - 2)) == 0) ? w - 1 : w;
    const int ch = (h > 2 && (h & 1) && ((h - 1) & (h - 2)) == 0) ? h - 1 : h;
    if ((cw != w || ch != h) && !(cw & 1) && !(ch & 1)) {
        const size_t bpp = t->is16 ? 2 : 4;
        const size_t bytes = (((size_t)cw * ch * bpp) + 15u) & ~(size_t)15;
        if (sArenaUsed + bytes > TEXARENA_SZ) return;
        uint8_t *dst = &sTexArena[sArenaUsed];
        sArenaUsed += bytes;
        for (int y = 0; y < ch; y++)
            memcpy(dst + (size_t)y * cw * bpp, (const uint8_t *)src + (size_t)y * w * bpp, (size_t)cw * bpp);
        t->lev[0] = dst; t->mw = (uint16_t)cw; t->mh = (uint16_t)ch;
        src = dst; w = cw; h = ch;
    }
    while (t->nlev < 7 && w >= 2 && h >= 2 && !(w & 1) && !(h & 1)) {
        const int nw = w / 2, nh = h / 2;
        const size_t bytes = (((size_t)nw * nh * (t->is16 ? 2 : 4)) + 15u) & ~(size_t)15;
        if (sArenaUsed + bytes > TEXARENA_SZ) break;
        void *dst = &sTexArena[sArenaUsed];
        sArenaUsed += bytes;
        mip_down(src, w, h, t->is16, dst);
        t->lev[t->nlev++] = dst;
        src = dst; w = nw; h = nh;
    }
}
/* build_mips again into the levels a texture already has (the same size): a water pre-blend
 * rewritten in place every frame (gfx_soft_blend_textures) keeps its arena space. */
static void rebuild_mips(SoftTex *t) {
    int w = t->w, h = t->h;
    const void *src = t->px;
    if (t->mw != w || t->mh != h) { /* the cropped 2^n copy (build_mips) */
        const size_t bpp = t->is16 ? 2 : 4;
        for (int y = 0; y < t->mh; y++)
            memcpy((uint8_t *)t->lev[0] + (size_t)y * t->mw * bpp, (const uint8_t *)src + (size_t)y * w * bpp, (size_t)t->mw * bpp);
        src = t->lev[0]; w = t->mw; h = t->mh;
    }
    for (int l = 1; l < t->nlev; l++) {
        mip_down(src, w, h, t->is16, (void *)t->lev[l]);
        src = t->lev[l]; w /= 2; h /= 2;
    }
}

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
    sTex[id].nlev = 1; sTex[id].lev[0] = dst;
    build_mips(&sTex[id]);
    sTexGen[id]++; /* box row cache key */
    { /* MK64_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("GE_TEXDUMP"); }
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
        /* in range: no divide (GoldenEye's 56x56 explosion and smoke sprites repeat, round 13) */
        if ((unsigned)c < (unsigned)size) return c;
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
 * (MK64 hud-area-filter cd479d8, from the F-Zero X port ca8ef33.) */
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
        const char *e = getenv("GE_BOXCACHE");
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

/* n pixels under a solid colour at alpha ca (the 2D solid rectangle: the death's red fade, the hit flashes, the pause
 * dim): each channel (c * ca + d * (255 - ca)) / 255, alpha 255. GE_2DFILL (2026-10-02, default on): eight pixels at a
 * time in NEON lanes -- the sum is at most 255 * 255, so the u16 lane holds it and floor(x / 255) is
 * (x + (x >> 8) + 1) >> 8 (exact for 0 <= x <= 65,535; rast_test ops checks every c, d and ca). =0: the scalar loop. */
static int s2dFill SAVESTATE_EXCLUDE = -1;
static void blend_solid_row(uint32_t *d, int n, int cr, int cg, int cb, int ca) {
    int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (s2dFill > 0) {
        const uint8x8_t inv = vdup_n_u8((uint8_t)(255 - ca));
        const uint16x8_t kr = vdupq_n_u16((uint16_t)(cr * ca)), kg = vdupq_n_u16((uint16_t)(cg * ca));
        const uint16x8_t kb = vdupq_n_u16((uint16_t)(cb * ca)), one = vdupq_n_u16(1);
        for (; i + 8 <= n; i += 8) {
            uint8x8x4_t px = vld4_u8((const uint8_t *)(d + i)); /* B G R A planes */
            const uint16x8_t xb = vmlal_u8(kb, px.val[0], inv), xg = vmlal_u8(kg, px.val[1], inv);
            const uint16x8_t xr = vmlal_u8(kr, px.val[2], inv);
            px.val[0] = vshrn_n_u16(vaddq_u16(vsraq_n_u16(xb, xb, 8), one), 8);
            px.val[1] = vshrn_n_u16(vaddq_u16(vsraq_n_u16(xg, xg, 8), one), 8);
            px.val[2] = vshrn_n_u16(vaddq_u16(vsraq_n_u16(xr, xr, 8), one), 8);
            px.val[3] = vdup_n_u8(0xFF);
            vst4_u8((uint8_t *)(d + i), px);
        }
    }
#endif
    for (; i < n; i++) {
        const uint32_t p = d[i];
        const int dr = (p >> 16) & 0xff, dg = (p >> 8) & 0xff, db = p & 0xff;
        d[i] = argb((uint8_t)((cr * ca + dr * (255 - ca)) / 255), (uint8_t)((cg * ca + dg * (255 - ca)) / 255),
                    (uint8_t)((cb * ca + db * (255 - ca)) / 255), 255);
    }
}

/* R31 (MK64 b2dff40; the lead's round 2): colour steps and starts convert to 16.16 as the ARM
 * does -- saturating, NaN to 0 -- so the desktop proxy's frames match the device's. Where a
 * near-degenerate triangle's step overflows, x86's cvttss2si gives INT_MIN (the proxy's blocky
 * sky rows, round 9) and the ARM saturates. GE_SATI32=0: the plain cast. */
static int sSatI32 SAVESTATE_EXCLUDE = -1;
static inline int32_t sat_i32(float f) {
#if defined(__arm__) && defined(__ARM_FP)
    /* VFP's vcvt.s32.f32 is this function: toward zero, saturating, NaN -> 0. One instruction
     * for the knob load and three compares, several times a span (11% of raster_tri's
     * instructions on the Cradle, round 17); the plain cast is the same on the ARM. */
    float t;
    int32_t r;
    __asm__("vcvt.s32.f32 %0, %1" : "=t"(t) : "t"(f));
    memcpy(&r, &t, sizeof r);
    return r;
#endif
    if (sSatI32 < 0) { const char *e = getenv("GE_SATI32"); sSatI32 = !(e && e[0] == '0'); }
    if (!sSatI32) return (int32_t)f;
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
}

/* GE_NEARW (default on; 0 = the old W_MIN 2 clip and depth path): GoldenEye's F3D clips only at
 * w >= 0, so gfx_pc.c's clipToNearW now keeps everything from w = 1/64 on (the user's wall clip
 * at the Dam's first tower: pressed against the block by the outer stairs, the eye came within
 * 2 units of its faces and the old cut opened a hole to the valley). Those faces reach raster_tri
 * with depths far outside 0..1 -- z/w = -128 * near at w = 1/64 -- and the span setup lost them:
 * each 16-pixel span started from its depth clamped to [-1, 2] and stepped by a slope clamped to
 * +-1 a pixel, so a span starting at z = -3 and rising got the depth of a face metres away a few
 * pixels on (also the old path, wherever w < near / 2). Now a span that starts below -1 and
 * rises (or above 2 and falls) starts instead at the first pixel inside [-1, 2], from that
 * pixel's exact depth, and steps back to the start; the slope clamp is +-6. Every pixel then
 * clamps to what its exact depth clamps to (the RDP's 0..max): below -1 and above 2 both
 * directions agree, and past a step of 6 the depth crosses the whole range in one pixel either
 * way. The values stay within 98 units (16 steps of 6 from 2), inside the 24.8 int32 range. A
 * span already starting inside [-1, 2] is set up exactly as before. */
static int sNearW SAVESTATE_EXCLUDE = -1;
/* GE_PSHADE as gfx_pc's fog split needs it (the colour its cut corners take; raster_tri's pshade rule). */
int gfx_soft_pshade_on(void) {
    if (sPShade < 0) { const char *e = getenv("GE_PSHADE"); sPShade = !(e && e[0] == '0'); }
    return sPShade;
}
int gfx_soft_nearw_on(void) {
    if (sNearW < 0) { const char *e = getenv("GE_NEARW"); sNearW = !(e && e[0] == '0'); }
    return sNearW;
}
#define ZQ_SCALE (65535.0f * 256.0f)
/* the depth slope as 24.8 a pixel: clamped to +-1 (old) or +-6 (GE_NEARW) */
static inline int32_t span_zdq(float zdx) {
    const float m = gfx_soft_nearw_on() ? 6.0f : 1.0f;
    return (int32_t)((zdx < -m ? -m : zdx > m ? m : zdx) * ZQ_SCALE);
}
/* A span's first depth, zf outside [-1, 2] with GE_NEARW on (above). span: its pixels (<= 16). */
static int32_t __attribute__((noinline)) span_zq_far(float zf, float zdx, int span, int32_t zdq) {
    float k;
    if (zf < -1.0f && zdx > 0.0f) k = (-1.0f - zf) / zdx;
    else if (zf > 2.0f && zdx < 0.0f) k = (zf - 2.0f) / -zdx;
    else return (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * ZQ_SCALE); /* never enters: as before */
    int kc = span - 1;
    if (k < (float)kc) { kc = (int)k; if ((float)kc < k) kc++; } /* the first pixel inside (ceil; k >= 0) */
    /* in double: zf and k * zdx cancel there (e.g. -4366 + 7 * 623.86), and float kept only ~1e-4 of the sum */
    double zr = (double)zf + (double)kc * (double)zdx;
    zr = zr < -1.0 ? -1.0 : zr > 2.0 ? 2.0 : zr;
    return (int32_t)(zr * (double)ZQ_SCALE) - kc * zdq;
}
/* true: span_zq_far sets this span up (zf outside [-1, 2], not NaN, GE_NEARW on) */
static inline bool span_z_out(float zf) {
    return (zf < -1.0f || zf > 2.0f) && gfx_soft_nearw_on();
}

/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}

/* A span start past to_fix16's +-16000 texels on a repeating texture, brought back by whole
 * periods first (the texels repeat, so they are the same ones). The per-triangle UV drop
 * (gfx_pc.c) keeps coordinates near the vertex with the smallest u; for a perspective triangle
 * that reaches the horizon that is the far vertex, where u/w explodes. GoldenEye's Frigate sea
 * seen from the boat (round 14; the user: "water glitches as it moves"): whenever the bobbing
 * boat took the horizon corner's W under ~1, its near pixels started 38,000 texels out, were
 * clamped to 16000 and drew the band of sea 15-90 rows under the horizon wrong for that frame.
 * Only spans that were clamped change. GE_SPANWRAP=0: off. */
static int sSpanWrap SAVESTATE_EXCLUDE = -1;
static inline float wrap_far(float x, int period, int mode) {
    if (x <= 16000.0f && x >= -16000.0f) return x;
    if (sSpanWrap < 0) { const char *e = getenv("GE_SPANWRAP"); sSpanWrap = !(e && e[0] == '0'); }
    if (!sSpanWrap || mode != WRAP_REPEAT || period <= 0) return x;
    return x - (float)period * floorf(x / (float)period);
}

/* 3D bilinear filtering (menu "3D FILTER: SMOOTH"; off by default). uf/vf are
 * 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* 0 = every textured pixel bilinear, as the N64 filters (default since round 12: P17, DKR
 * 467ad1e -- the magnified-only gate drew floors sharp beyond a border along triangle edges
 * that jumped as the camera moved, half of the user's floor flash); 1 = bilinear only when
 * magnified (GE_MAGBILIN=1) */
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
    const int fu = (su >> 8) & 0xff, fv = (sv >> 8) & 0xff;
    uint32_t a, b, c, d;
    if (ws == WRAP_REPEAT && wt == WRAP_REPEAT && (mu | mv) >= 0 && !i16) {
        /* Repeat on power-of-two sizes, 32-bit texels (the common case: the Cradle's cloud
         * layers): wrapf is c & mask, four ANDs for the four mode/mask tests, and texel_at's
         * format test drops out. Same texels, bit-exact (round 18). */
        const uint32_t *p = (const uint32_t *)px;
        const int u0 = su >> 16, v0 = sv >> 16;
        const int iu0 = u0 & mu, iu1 = (u0 + 1) & mu;
        const int b0 = (v0 & mv) * w, b1 = ((v0 + 1) & mv) * w;
        a = p[b0 + iu0]; b = p[b0 + iu1]; c = p[b1 + iu0]; d = p[b1 + iu1];
    } else {
        const int iu0 = wrapf(su >> 16, w, ws, mu), iu1 = wrapf((su >> 16) + 1, w, ws, mu);
        const int iv0 = wrapf(sv >> 16, h, wt, mv), iv1 = wrapf((sv >> 16) + 1, h, wt, mv);
        const int b0 = iv0 * w, b1 = iv1 * w;
        a = texel_at(px, i16, b0 + iu0); b = texel_at(px, i16, b0 + iu1);
        c = texel_at(px, i16, b1 + iu0); d = texel_at(px, i16, b1 + iu1);
    }
    /* even lanes = B,R ; odd lanes = G,A (shifted down 8) */
    const uint32_t te = lerp2(a & 0x00FF00FFu, b & 0x00FF00FFu, fu), to = lerp2((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, fu);
    const uint32_t ue = lerp2(c & 0x00FF00FFu, d & 0x00FF00FFu, fu), uo = lerp2((c >> 8) & 0x00FF00FFu, (d >> 8) & 0x00FF00FFu, fu);
    return lerp2(te, ue, fv) | (lerp2(to, uo, fv) << 8);
}
/* GE_MIPLOD's texel between two levels: level L and L+1 each filtered as level 0 is (bilerp; the RDP filters
 * every tile it reads), then the RDP's TRILERP by the LOD fraction. uf, vf are level L's pixel centre (16.16);
 * level L+1's is half of it. Level L+1 is (w / 2) x (h / 2), its masks halved with it. */
static inline __attribute__((always_inline)) uint32_t
tribil_texel(const void *px, const void *px2, bool i16, int w, int h, int ws, int wt, int mu, int mv, int32_t uf,
             int32_t vf, uint32_t f) {
    const int w2 = w >> 1, h2 = h >> 1;
    const uint32_t a = bilerp(px, i16, w, h, ws, wt, uf, vf, mu, mv);
    const uint32_t b = bilerp(px2, i16, w2, h2, ws, wt, uf >> 1, vf >> 1, mu >= 0 ? w2 - 1 : -1, mv >= 0 ? h2 - 1 : -1);
    return rdp_trilerp(a, b, f);
}

/* -------------------------------------------------------------- shaders --- */
/* Per-channel texel use, derived from the combiner (device round 6b): the
 * cloud sprites are I4 textures used ONLY for alpha -- RGB = SHADE (white).
 * Multiplying the texel's grey intensity into RGB painted the soft edge grey. */
struct ShaderProgram { uint32_t shader_id; struct CCFeatures cc; bool rgb_tex, rgb_in, a_tex, a_in, rgb_texa; };
#define MAX_SHADERS 64
static struct ShaderProgram sShaders[MAX_SHADERS];
static int sNumShaders;
static struct ShaderProgram *sCurShader SAVESTATE_EXCLUDE;

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
/* Off, as gfx_pc's cached rendering_state starts (and as gfx_overlay_state_dirty leaves it):
 * gfx_pc sends only changes, so a backend that started on kept testing and writing depth until
 * the first depth-on draw. The intro never draws with depth: the gun barrel's backdrop wrote
 * z 0.57 and Bond (z 0.995, no Z_CMP) failed against it -- missing from the barrel (round 17). */
static bool sDepthTest = false, sDepthMask = false, sZDecal, sUseAlpha;
static int sVpX, sVpY, sVpW, sVpH;
static int sScX0, sScY0, sScX1, sScY1;
static bool sTexAdd;       /* GE ADD: texel + shade */
static bool sTexLerp;      /* out = K + (vertex - K) * texel: OoT's (A - K) * TEXEL0 + K tints */
static int sLerpR, sLerpG, sLerpB;
static bool sForceIn;      /* 2-cycle: the vertex colour carries cycle 2, always modulate */
static bool sALerp SAVESTATE_EXCLUDE; static int sALerpK SAVESTATE_EXCLUDE; /* 2D: alpha = K + (colour alpha - K) x texel alpha (gfx_pc cyc2k) */
static bool sForceAIn SAVESTATE_EXCLUDE; /* 2D, alpha only: a 2-cycle x ENV_ALPHA carried in the colour's alpha (gfx_pc cyc2a) */
static bool sUiMode;       /* HUD/menu triangles (game UI tags): never bilinear; area-filtered when squeezed */
/* The N64's own filter (gfx_scegu_set_n64_filter): bilinear however far the texture is minified,
 * and mipmaps only where the game turns the RDP's LOD on (sTexLod) -- for screens of 2D-like
 * images on 3D models (GoldenEye's legal page and folder screens), where the world's mipmaps
 * thicken thin strokes and the magnified-only bilinear breaks them into dots. */
static bool sN64Filter, sTexLod;
static int sMaxLevel; /* gSPTexture's level (mip levels - 1) of a G_TL_LOD draw, 0 otherwise (gfx_scegu_set_lod_level) */

/* GE_MIPLOD (2026-10-03, the reference sweep's Runway shutter and Depot containers): which mip level a span reads.
 * The old pick (0) took log2 of the longer texel-step vector a 16-pixel span and read the NEAREST texel of each level
 * (only a span at level 0 alone was bilinear); a wall's fine stripes, 1.5 - 4 texels a pixel there, broke into
 * ragged dashes that crawled as the view turned. The RDP (angrylion tex.c tclod_2cycle, lodfrac_lodtile_signals):
 * lod = the largest of |ds/dx|, |dt/dx|, |ds/dy|, |dt/dy| in 10.5 texels; tile L = floor(log2(lod)), LOD_FRACTION =
 * lod / 2^L - 1 (linear across the step, not log), magnified (< 1 texel) = level 0, past the game's last level
 * (gSPTexture's level) = that level; both tiles bilinear and lerped by the combiner's TRILERP.
 *   1: that rule (the x step a span's average, the y step the mean of the span's ends), and only G_TL_LOD draws
 *      mipmapped, as on the N64: the closest to angrylion, and as soft.
 *   2 (default): the same on the G_TL_LOD draws (walls, floors), half a level finer (lod x 2^-0.5): no shimmer in
 *      the turning tests, about the crispness of a 4x supersampled render; the other draws (the sky, characters,
 *      the gun) keep the old pick.
 *   3: as 2 at the RDP's level, but from the SMALLER of the x and y steps (an oblique wall keeps its finer level).
 * GE_MIPBIAS (quarter levels, - = sharper) scales lod in every mode; GE_TRILIN=0 blends two levels only across the
 * middle half of each step in every mode (cheaper: the Bench's "brilinear"). */
static int sMipLod SAVESTATE_EXCLUDE = -1;
/* GE_MIPFAR=n (2026-10-05, experimental, default off): with GE_MIPLOD 1-3, a G_TL_LOD span whose level is n or more
 * reads that level's nearest texel alone (no bilinear, no blend into the next level), as the old pick did; levels
 * below n keep the full filter. Far levels cover a few pixels at 240. Off (unset or 0): 99, no level is far. */
static int sMipFar SAVESTATE_EXCLUDE = 99;
static inline void mip_lod_init(void) {
    if (sMipLod < 0) {
        const char *e = getenv("GE_MIPLOD"), *f = getenv("GE_MIPFAR");
        sMipLod = e && e[0] >= '0' && e[0] <= '3' ? e[0] - '0' : 2;
        sMipFar = f && f[0] >= '1' && f[0] <= '9' ? f[0] - '0' : 99;
    }
}
/* angrylion-rdp-plus lodfrac_lodtile_signals and tclod_2cycle's tile pair, no sharpen or detail: lod is
 * tclod_4x17_to_15's 15 bits (0x4000 = past 512 texels), min level 0. t1, t2: the two levels TRILERP reads. */
static inline void n64_lod_tiles(int32_t lod, int max_level, int *t1, int *t2, uint32_t *lf) {
    if (lod & 0x4000) lod = 0x7fff;
    const int mag = lod < 32, il = (lod >> 5) & 0xff;
    int ltil = il ? 31 - __builtin_clz((unsigned)il) : 0; /* log2table[] */
    const int dis = (lod & 0x6000) || ltil >= max_level;
    uint32_t f = (uint32_t)((lod << 3) >> ltil) & 0xffu;
    if (dis) f = 0xff; else if (mag) f = 0;
    if (dis) ltil = max_level;
    *t1 = ltil;
    *t2 = (dis || mag) ? ltil : ltil + 1;
    *lf = f;
}
/* A span's level (*lvl), LOD fraction and whether it blends two levels, from its texel steps (level-0 texels):
 * du, dv the span's average x step in u, v; the y step at its two ends from the 1/w-space y gradients. */
static __attribute__((noinline)) void mip_pick_n64(const SoftTex *tex, int tw_l0, int th_l0, float du, float dv,
                                                               float u_a, float v_a, float uwdy, float vwdy, float swdy,
                                                               float rw, float rw_b, float fspan, int *lvl,
                                                               uint32_t *fr, bool *tri) {
    const float dux = du * (float)tw_l0, dvx = dv * (float)th_l0;
    const float u_b = u_a + du * fspan, v_b = v_a + dv * fspan;
    const float duy = ((uwdy - u_a * swdy) * rw + (uwdy - u_b * swdy) * rw_b) * 0.5f * (float)tw_l0;
    const float dvy = ((vwdy - v_a * swdy) * rw + (vwdy - v_b * swdy) * rw_b) * 0.5f * (float)th_l0;
    const float aux = dux < 0.0f ? -dux : dux, avx = dvx < 0.0f ? -dvx : dvx;
    const float auy = duy < 0.0f ? -duy : duy, avy = dvy < 0.0f ? -dvy : dvy;
    const float ax = aux > avx ? aux : avx, ay = auy > avy ? auy : avy;
    const float rho = (sMipLod == 3 ? (ax < ay ? ax : ay) : (ax > ay ? ax : ay)) * sMipLin *
                      (sMipLod == 2 ? 0.70710678f : 1.0f); /* 2: half a level finer, 2^-0.5 */
    const int32_t lod = rho < 512.0f ? (int32_t)(rho * 32.0f) : 0x7fff; /* NaN: the last level */
    const int top = tex->nlev - 1;
    int t1, t2;
    n64_lod_tiles(lod, sMaxLevel, &t1, &t2, fr);
    if (t1 > top) t1 = top;
    if (t2 > top) t2 = top;
    if (!sTrilin && t2 != t1) { /* GE_TRILIN=0 ("brilinear"): one level for the outer quarters of the step, cheaper */
        if (*fr < 64u) t2 = t1;
        else if (*fr >= 192u) t1 = t2;
        else *fr = (*fr - 64u) * 2u;
    }
    *lvl = t1;
    *tri = t2 != t1 && *fr != 0u && t1 < sMipFar;
}
/* GE_LODFAST (2026-10-04, the device Bench: the new walls cost +3.6 ms on the Dam): mip_pick_n64 inline, with the
 * same float steps and every comparison on the core. mip_pick_n64 was a call a span (ten floats, three pointers,
 * the results through memory) whose |x| and max/min were nine VFP compares, each a VMRS that waits for the VFP
 * pipeline on the A7; in the Dam's ~70-px triangles it was most of the new path's extra instructions. Here:
 *   - dux, dvx, duy, dvy: the same operations in the same order ((float)tw_l0 is the caller's, the same conversion);
 *   - |x| as the bits & 0x7fffffff, and the largest (or, mode 3, the smaller of the two largest) as an unsigned
 *     compare of those bits: for floats that are not NaN, the order of magnitudes is the order of the bit patterns.
 *     x < 0 ? -x : x keeps -0 as -0 where the mask gives +0, and a tie between equal magnitudes picks the other
 *     operand: either way the same magnitude, and the sign of a zero rho gives the same lod (0);
 *   - any NaN among the four (a magnitude above 0x7f800000; the largest is checked) goes to mip_pick_n64 itself,
 *     whose compares drop or keep a NaN by position;
 *   - rho = m * sMipLin (* 2^-0.5 in mode 2), the same multiplies (mode 1 and 3's * 1.0f is the identity); rho is
 *     +0 or positive and not NaN, so rho < 512.0f is its bits < 0x44000000 (+inf above), then (int32_t)(rho * 32.0f)
 *     as before; n64_lod_tiles and the clamps unchanged.
 * Checked against mip_pick_n64 over random and edge inputs (rast_test lod) and by every R_LOD run. */
static int sLodFast SAVESTATE_EXCLUDE = -1;
static int sTbilPair SAVESTATE_EXCLUDE = -1; /* GE_TBILPAIR (see LEAN_TBIL32P) */
static int sBilPair SAVESTATE_EXCLUDE = -1;  /* GE_BILPAIR (see LEAN_BIL32P) */
static inline __attribute__((always_inline)) uint32_t lod_fbits(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }
static inline __attribute__((always_inline)) void mip_pick_fast(const SoftTex *tex, int tw_l0, int th_l0, float ftw,
                                                                float fth, float du, float dv, float u_a, float v_a,
                                                                float uwdy, float vwdy, float swdy, float rw, float rw_b,
                                                                float fspan, int *lvl, uint32_t *fr, bool *tri) {
    const float dux = du * ftw, dvx = dv * fth;
    const float u_b = u_a + du * fspan, v_b = v_a + dv * fspan;
    const float duy = ((uwdy - u_a * swdy) * rw + (uwdy - u_b * swdy) * rw_b) * 0.5f * ftw;
    const float dvy = ((vwdy - v_a * swdy) * rw + (vwdy - v_b * swdy) * rw_b) * 0.5f * fth;
    const uint32_t aux = lod_fbits(dux) & 0x7fffffffu, avx = lod_fbits(dvx) & 0x7fffffffu;
    const uint32_t auy = lod_fbits(duy) & 0x7fffffffu, avy = lod_fbits(dvy) & 0x7fffffffu;
    const uint32_t ax = aux > avx ? aux : avx, ay = auy > avy ? auy : avy;
    const uint32_t mx = ax > ay ? ax : ay;
    if (__builtin_expect(mx > 0x7f800000u, 0)) { /* a NaN: the compares' own order decides */
        mip_pick_n64(tex, tw_l0, th_l0, du, dv, u_a, v_a, uwdy, vwdy, swdy, rw, rw_b, fspan, lvl, fr, tri);
        return;
    }
    const int mode = sMipLod;
    union { uint32_t u; float f; } m = { mode == 3 ? (ax < ay ? ax : ay) : mx };
    float rho = m.f * sMipLin;
    if (mode == 2) rho = rho * 0.70710678f;
    const int32_t lod = lod_fbits(rho) < 0x44000000u ? (int32_t)(rho * 32.0f) : 0x7fff;
    const int top = tex->nlev - 1;
    int t1, t2;
    uint32_t f;
    n64_lod_tiles(lod, sMaxLevel, &t1, &t2, &f);
    if (t1 > top) t1 = top;
    if (t2 > top) t2 = top;
    if (!sTrilin && t2 != t1) {
        if (f < 64u) t2 = t1;
        else if (f >= 192u) t1 = t2;
        else f = (f - 64u) * 2u;
    }
    *lvl = t1;
    *fr = f;
    *tri = t2 != t1 && f != 0u && t1 < sMipFar;
}

/* GE_LODLEAN (2026-10-05, GE_MIPLOD=2's price on the Dam: ~95M qemu instructions of the 345M are this pick, once a
 * 16-pixel span, ~4,600 spans a frame): mip_pick_fast with
 *   - the largest magnitude on NEON: the four steps' bits & 0x7fffffff (vbic), then two pairwise unsigned maxima
 *     (vpmax: max(aux, avx), max(auy, avy), then their max; mode 3 the pairwise minimum of the two) -- the same unsigned
 *     compares as mip_pick_fast's, one transfer to the core where it made four transfers and seven compares;
 *   - the switches (GE_MIPLOD's mode, GE_MIPBIAS's scale, the draw's last level, GE_TRILIN, GE_MIPFAR) from the caller's
 *     MipSet, read once a row, not from the globals every span;
 *   - n64_lod_tiles for the lods the pick can make: lod is (int32_t)(rho * 32) with 0 <= rho < 512 (rho's sign bit is
 *     clear, the scales positive), so 0..0x3fff, or 0x7fff. There (lod & 0x6000) != 0 is lod >= 0x2000 (and 0x4000
 *     sets 0x7fff, which is >= 0x2000 too); for 32 <= lod < 0x2000 ((lod >> 5) & 0xff) is lod >> 5, whose log2
 *     31 - clz(lod >> 5) is 26 - clz(lod); for lod < 32 the tile is 0, which 26 - clz(lod | 32) gives for every lod
 *     below 0x2000 (lod | 32 keeps lod's top bit when lod >= 32). dis (past the last level or lod >= 0x2000) before mag
 *     (lod < 32), as there. Checked for every lod 0..0x7fff and last level 0..7 (rast_test lod, n64_lod_tiles_lean),
 *     and the whole pick against mip_pick_n64 over random and edge inputs.
 * GE_LODLEAN=0: mip_pick_fast. */
static int sLodLean SAVESTATE_EXCLUDE = -1;
typedef struct { float lin; int mode, maxlev, trilin, far; } MipSet;
static inline __attribute__((always_inline)) void mip_set_get(MipSet *m) {
    m->lin = sMipLin; m->mode = sMipLod; m->maxlev = sMaxLevel; m->trilin = sTrilin; m->far = sMipFar;
}
/* n64_lod_tiles for 0 <= lod <= 0x3fff and lod = 0x7fff (see above) */
static inline __attribute__((always_inline)) void n64_lod_tiles_lean(int32_t lod, int max_level, int *t1, int *t2,
                                                                     uint32_t *lf) {
    const int ltil = 26 - __builtin_clz((unsigned)lod | 32u);
    if (lod >= 0x2000 || ltil >= max_level) { *t1 = *t2 = max_level; *lf = 0xff; }
    else if (lod < 32) { *t1 = *t2 = 0; *lf = 0; }
    else { *t1 = ltil; *t2 = ltil + 1; *lf = (uint32_t)((lod << 3) >> ltil) & 0xffu; }
}

static void gfx_soft_set_depth_test(bool d) { sDepthTest = d; }
static void gfx_soft_set_depth_mask(bool m) { sDepthMask = m; }
static void gfx_soft_set_zmode_decal(bool d) { sZDecal = d; }
static void gfx_soft_set_use_alpha(bool a) { sUseAlpha = a; }
static bool sOwnTri SAVESTATE_EXCLUDE; /* gfx_soft_set_own_edges (GE_FILLRULE, see own_edge) */
/* GE_FILLRULE: gfx_pc says when the draw is blended or alpha-tested without AA_EN (see own_edge) */
void gfx_soft_set_own_edges(bool on) { sOwnTri = on; }
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

/* The sprite span skip (round 13; the user: "Smoke kills fps"). GoldenEye's explosions and
 * smoke are 56x56 sprites on quads that fill the screen in layers -- 353k pixels a frame in the
 * device's worst second, 10 fps -- and over half of every quad reads only alpha-0 texels: the
 * round puff's corners and fringe. Blended, the alpha compare drops each such pixel, but only
 * after its z test, four taps, the lerps and the combine. A span whose every bilinear tap reads
 * alpha 0 draws nothing, so it is skipped whole. Exact: the texel alpha of such a pixel is 0,
 * so its combined alpha is 0 (texture x shade, or the texture alone) and the compare (amin >= 8)
 * drops it before any colour or depth write; raster_tri takes the skip only there (blended,
 * texture alpha, no per-vertex K, level 0 of a non-mipmapped texture, bilinear).
 * Per texture, each row's first and last texel with alpha != 0, keyed by id and generation
 * (every texel write bumps sTexGen, and texman_clear never resets it). GE_SPANSKIP=0: off. */
/* 64 textures (round 18, MK64's finding): at 16 the Dam's explosions rescanned a sprite on 4.8%
 * of lookups (6,494 of 134,882); at 64, 176. ~66 KB. */
#define AROWS_N 64
typedef struct AlphaRowsTag { uint32_t id, gen; int w, h, full; int16_t lo[256], hi[256]; } AlphaRows;
static AlphaRows sARows[AROWS_N] SAVESTATE_EXCLUDE;
static unsigned sARowsNext SAVESTATE_EXCLUDE;
static int sSpanSkip SAVESTATE_EXCLUDE = -1;
uint32_t gfx_soft_spanskip_spans SAVESTATE_EXCLUDE, gfx_soft_spanskip_px SAVESTATE_EXCLUDE; /* GE_TILECENSUS */
#if defined(MIYOO)
/* The Mini's second core (miyoo_worker.c; notes/miyoo.md "The second core"). raster_tri_body hands the bottom share
 * of a big triangle's rows to the worker, which runs raster_tri_body on the same triangle with tlSplit set: the same
 * setup from the same inputs by the same code (so the same values), then only its own rows. Every row of the loop
 * is computed from its own y (the edge bases and the span-start interpolants, "no row-to-row drift"; the mip
 * level's state is rebuilt each span), so the worker's first row is the one the whole loop would have drawn. The
 * shares write disjoint rows of colour and depth (each pixel store, depth test and blend is its own pixel's), the
 * game thread waits for the worker before the next triangle (draw order kept), and what the setup leaves behind
 * (the untextured-draw count, the PICK line's sDbg*, the alpha_rows cache) is the game thread's alone: the worker
 * takes the game thread's alpha_rows result and counts its skipped spans into the job, added after the wait. */
#include <sched.h>
#include "miyoo_worker.h"
typedef struct {
    const SoftVtx *v0, *v1, *v2;
    bool use_z;
    int ylo;                      /* the worker's rows: ylo up to the triangle's (the batch's) last */
    const AlphaRows *arows;       /* the game thread's alpha_rows result for this triangle (batch) */
    int px;                       /* out: the worker's span pixels (raster_tri_body's return) */
    uint32_t skip_spans, skip_px; /* out: its span-skip counts */
    const SoftVtx *bv;            /* a batch share (batch_split): bn triangles from bv, results into bpx */
    int bn, *bpx;
    bool alpha_off, cvg_mixed;    /* a triangle share: the game thread's raster_tri choices for it */
} SplitJob;
static __thread SplitJob *tlSplit; /* the worker's job while it draws a share; NULL on the game thread */
/* The rows this thread draws (a y scissor, raster_tri_body's box): the whole target, or a batch share's band. */
static __thread int tlRowLo, tlRowHi = 0x7fffffff;
/* raster_tri's GE_CVGALPHA choices for the body, per thread (the batch share's worker makes them too): the
 * alpha off for an opaque triangle, the mixed cut. The game thread's globals sUseAlpha / sCvgMixed are left
 * alone while the body runs. */
static __thread bool tlAlphaOff, tlCvgMixed;
#define SPANSKIP_ADD(n)                                                                                 \
    do {                                                                                                \
        if (tlSplit) { tlSplit->skip_spans++; tlSplit->skip_px += (uint32_t)(n); }                      \
        else { gfx_soft_spanskip_spans++; gfx_soft_spanskip_px += (uint32_t)(n); }                      \
    } while (0)
#endif
static const AlphaRows *alpha_rows(uint32_t id, const SoftTex *t) {
    if (sSpanSkip < 0) { const char *e = getenv("GE_SPANSKIP"); sSpanSkip = !(e && e[0] == '0'); }
    if (!sSpanSkip || id == 0 || id >= MAX_TEXTURES || t->w > 4096 || t->h > 256) return NULL;
    for (int i = 0; i < AROWS_N; i++)
        if (sARows[i].id == id && sARows[i].gen == sTexGen[id] && sARows[i].w == t->w && sARows[i].h == t->h)
            return sARows[i].full ? NULL : &sARows[i]; /* opaque in every texel: no span is ever clear */
    AlphaRows *r = &sARows[sARowsNext++ % AROWS_N];
    r->id = id; r->gen = sTexGen[id]; r->w = t->w; r->h = t->h;
    int full = 1;
    for (int y = 0; y < t->h; y++) {
        int lo = t->w, hi = -1;
        for (int x = 0; x < t->w; x++)
            if (texel_at(t->px, t->is16, y * t->w + x) >> 24) { if (x < lo) lo = x; hi = x; }
            else full = 0;
        r->lo[y] = (int16_t)lo; r->hi[y] = (int16_t)hi;
    }
    r->full = full;
    return full ? NULL : r;
}
/* The texel range [lo, hi] as the sampler wraps it: one or two ranges in out[], 0 = unknown. */
static inline __attribute__((always_inline)) int wrap_range(int lo, int hi, int size, int mode, int out[4]) {
    if (mode == WRAP_CLAMP) {
        out[0] = lo < 0 ? 0 : (lo >= size ? size - 1 : lo);
        out[1] = hi < 0 ? 0 : (hi >= size ? size - 1 : hi);
        return 1;
    }
    if (mode != WRAP_REPEAT) return 0;
    if (lo >= 0 && hi < size) { out[0] = lo; out[1] = hi; return 1; } /* in range: no divide */
    if (hi - lo + 1 >= size) { out[0] = 0; out[1] = size - 1; return 1; }
    const int a = ((lo % size) + size) % size, b = ((hi % size) + size) % size;
    if (a <= b) { out[0] = a; out[1] = b; return 1; }
    out[0] = a; out[1] = size - 1; out[2] = 0; out[3] = b;
    return 2;
}
/* Every bilinear tap of span pixels 0..n-1, at (uf + i duf, vf + i dvf), reads alpha 0? The
 * taps lie in the box of the end pixels' 2x2 neighbourhoods (the coordinates are linear
 * along the span), taken with the int64 end points so a span whose steps wrap int32 is never
 * skipped; a box over more than 64 rows is not tried. */
static bool span_clear(const AlphaRows *r, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n, int ws, int wt) {
    /* 32-bit end points (64-bit math was ~45% of this function on the ARM, round 18): a span
     * whose end point or its -0x8000 would wrap is simply not tried -- the skip is only ever
     * conservative (a span not skipped draws the same pixels) */
    int32_t dus, dvs, ua = uf, ub, va = vf, vb;
    if (__builtin_mul_overflow(duf, n - 1, &dus) || __builtin_add_overflow(uf, dus, &ub) ||
        __builtin_mul_overflow(dvf, n - 1, &dvs) || __builtin_add_overflow(vf, dvs, &vb))
        return false;
    const int32_t lim = INT32_MIN + 0x8000;
    if (ua < lim || ub < lim || va < lim || vb < lim) return false;
    const int ulo = ((ua < ub ? ua : ub) - 0x8000) >> 16, uhi = (((ua < ub ? ub : ua) - 0x8000) >> 16) + 1;
    const int vlo = ((va < vb ? va : vb) - 0x8000) >> 16, vhi = (((va < vb ? vb : va) - 0x8000) >> 16) + 1;
    int cr[4], rr[4];
    const int nc = wrap_range(ulo, uhi, r->w, ws, cr), nr = wrap_range(vlo, vhi, r->h, wt, rr);
    if (nc == 0 || nr == 0) return false;
    if ((rr[1] - rr[0] + 1) + (nr == 2 ? rr[3] - rr[2] + 1 : 0) > 64) return false;
    for (int k = 0; k < nr; k++)
        for (int y = rr[2 * k]; y <= rr[2 * k + 1]; y++) {
            const int lo = r->lo[y], hi = r->hi[y];
            if (hi < lo) continue; /* an all-transparent row */
            for (int j = 0; j < nc; j++)
                if (cr[2 * j] <= hi && cr[2 * j + 1] >= lo) return false;
        }
    return true;
}

/* GE_MIPLOD's spans away from level 0: every bilinear tap of level lvl (and of lvl + 1 when tri, at half the
 * coordinates) mapped to the level-0 texels its box filter covers, checked as span_clear_mip does. Conservative:
 * a level texel built from alpha-0 texels only is alpha 0. */
static bool span_clear_lv(const AlphaRows *r, int64_t ua, int64_t ub, int64_t va, int64_t vb, int ws, int wt, int tw,
                          int th, int sh) {
    const int64_t lo32 = (int64_t)INT32_MIN + 0x8000, hi32 = INT32_MAX;
    if (ua < lo32 || ub < lo32 || va < lo32 || vb < lo32 || ua > hi32 || ub > hi32 || va > hi32 || vb > hi32) return false;
    const int ulo = (int)(((ua < ub ? ua : ub) - 0x8000) >> 16), uhi = (int)(((ua < ub ? ub : ua) - 0x8000) >> 16) + 1;
    const int vlo = (int)(((va < vb ? va : vb) - 0x8000) >> 16), vhi = (int)(((va < vb ? vb : va) - 0x8000) >> 16) + 1;
    int cr[4], rr[4];
    const int nc = wrap_range(ulo, uhi, tw, ws, cr), nr = wrap_range(vlo, vhi, th, wt, rr);
    if (nc == 0 || nr == 0) return false;
    int rows = 0;
    for (int k = 0; k < 2 * nc; k += 2) { cr[k] <<= sh; cr[k + 1] = ((cr[k + 1] + 1) << sh) - 1; }
    if (cr[2 * nc - 1] >= r->w) return false;
    for (int k = 0; k < 2 * nr; k += 2) {
        rr[k] <<= sh; rr[k + 1] = ((rr[k + 1] + 1) << sh) - 1;
        if (rr[k + 1] >= r->h) return false;
        rows += rr[k + 1] - rr[k] + 1;
    }
    if (rows > 64) return false;
    for (int k = 0; k < nr; k++)
        for (int y = rr[2 * k]; y <= rr[2 * k + 1]; y++) {
            const int lo = r->lo[y], hi = r->hi[y];
            if (hi < lo) continue;
            for (int j = 0; j < nc; j++)
                if (cr[2 * j] <= hi && cr[2 * j + 1] >= lo) return false;
        }
    return true;
}
static bool span_clear_mipb(const AlphaRows *r, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n, int ws, int wt,
                            int tw, int th, int lvl, bool tri) {
    const int64_t ua = uf, ub = (int64_t)uf + (int64_t)duf * (n - 1);
    const int64_t va = vf, vb = (int64_t)vf + (int64_t)dvf * (n - 1);
    if (!span_clear_lv(r, ua, ub, va, vb, ws, wt, tw, th, lvl)) return false;
    return !tri || span_clear_lv(r, ua >> 1, ub >> 1, va >> 1, vb >> 1, ws, wt, tw >> 1, th >> 1, lvl + 1);
}
/* span_clear for a mipmapped span (round 16, the user: "cradle is a bit slow" -- its antenna's
 * lattice girders are cut-out textures, mipmapped, a third of the level's pixels drawn through
 * and dropped as alpha 0, and the round-13 skip took only unmipped sprites). Away from level 0
 * raster_tri reads the nearest texel of level L (FETCH_NEAREST), blended with L+1's when
 * between levels (FETCH_TRILIN: the L index halved). A level-L texel box-filters 2^L x 2^L
 * texels of level 0 (mip_down), so it is alpha 0 when they all are: the span's texels at L
 * (L+1 for the blend), mapped to level 0, are checked against the level-0 row extents. Exact
 * as span_clear is; a 2^n + 1 texture (cropped for its levels) is not tried. GE_MIPSKIP=0: off. */
static bool span_clear_mip(const AlphaRows *r, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n, int ws, int wt,
                           int tw, int th, int lvl, bool tri) {
    const int64_t ua = uf, ub = (int64_t)uf + (int64_t)duf * (n - 1);
    const int64_t va = vf, vb = (int64_t)vf + (int64_t)dvf * (n - 1);
    if (ub < INT32_MIN || ub > INT32_MAX || vb < INT32_MIN || vb > INT32_MAX) return false;
    const int ulo = (int)((ua < ub ? ua : ub) >> 16), uhi = (int)((ua < ub ? ub : ua) >> 16);
    const int vlo = (int)((va < vb ? va : vb) >> 16), vhi = (int)((va < vb ? vb : va) >> 16);
    int cr[4], rr[4];
    const int nc = wrap_range(ulo, uhi, tw, ws, cr), nr = wrap_range(vlo, vhi, th, wt, rr);
    if (nc == 0 || nr == 0) return false;
    const int sh = lvl + (tri ? 1 : 0); /* level-0 texels per texel read, as a power of two */
    int rows = 0;
    for (int k = 0; k < 2 * nc; k += 2) {
        const int lo = tri ? cr[k] >> 1 : cr[k], hi = tri ? cr[k + 1] >> 1 : cr[k + 1];
        cr[k] = lo << sh; cr[k + 1] = ((hi + 1) << sh) - 1;
    }
    for (int k = 0; k < 2 * nr; k += 2) {
        const int lo = tri ? rr[k] >> 1 : rr[k], hi = tri ? rr[k + 1] >> 1 : rr[k + 1];
        rr[k] = lo << sh; rr[k + 1] = ((hi + 1) << sh) - 1;
        if (rr[k + 1] >= r->h || cr[2 * nc - 1] >= r->w) return false; /* not the level-0 image's shape */
        rows += rr[k + 1] - rr[k] + 1;
    }
    if (rows > 64) return false;
    for (int k = 0; k < nr; k++)
        for (int y = rr[2 * k]; y <= rr[2 * k + 1]; y++) {
            const int lo = r->lo[y], hi = r->hi[y];
            if (hi < lo) continue; /* an all-transparent row */
            for (int j = 0; j < nc; j++)
                if (cr[2 * j] <= hi && cr[2 * j + 1] >= lo) return false;
        }
    return true;
}
static int sMipSkip SAVESTATE_EXCLUDE = -1;

/* The mip block's level and fraction from rb's bits on the core, as the float code computes them:
 *   l2 = ((float)e + (float)m * 2^-23) * 0.5f;  if (l2 > 0) { lvl = l2 < 7 ? (int)l2 : 7; fr = (l2 - lvl) * 256; }
 * l2 > 0 only for e > 0 or e = 0, m > 0 (e = -1 and below stay negative). Then (e + m/2^23) * 2^23 = e<<23 + m is
 * an integer, the float sum is it rounded to 24 significant bits (to nearest, ties to even), l2 is that over 2^24,
 * and (int)l2, (l2 - lvl) * 256 truncated are its bits. Equal to the float code for all 2^32 inputs (checked
 * exhaustively; the float steps are exact IEEE single operations, the same on the A7's VFP and the desktop's SSE).
 * No FPSCR transfers, no float/int conversions. */
static inline __attribute__((always_inline, unused)) void lean_mip_lf(uint32_t u, int *plvl, uint32_t *pfr) {
    const int e = (int)((u >> 23) & 0xff) - 127;
    const uint32_t m = u & 0x7fffffu;
    if (e < 0 || (e == 0 && m == 0)) { *plvl = 0; *pfr = 0; return; }
    uint32_t v = ((uint32_t)e << 23) + m;
    const int L = 32 - __builtin_clz(v);
    if (L > 24) {
        const int k = L - 24;
        const uint32_t rem = v & ((1u << k) - 1u), half = 1u << (k - 1);
        v >>= k;
        if (rem > half || (rem == half && (v & 1u))) v++;
        v <<= k;
    }
    if (v >= (7u << 24)) { *plvl = 7; *pfr = (v - (7u << 24)) >> 16; return; }
    *plvl = (int)(v >> 24); *pfr = (v >> 16) & 0xffu;
}
static inline float edge(const SoftVtx *a, const SoftVtx *b, float px, float py) {
    return (px - a->x) * (b->y - a->y) - (py - a->y) * (b->x - a->x);
}
/* GE_FILLRULE (2026-10-03, ported from the OoT port's OOT_FILLRULE, the Deku Tree's flame line): the row solve below is
 * biased toward inclusion, and a pixel centre exactly on an edge counts as inside, so a pixel on the edge two
 * triangles share is drawn by both. Opaque, that is invisible; blended, the pixel is blended twice: a one-pixel
 * bright (or dark) line along the shared edge -- a see-through quad's diagonal wherever it runs through pixel
 * centres. GoldenEye: smoke (G_RM_ZB_CLD_SURF) or an explosion's flames (G_RM_PASS + G_RM_ZB_CLD_SURF2) filling the
 * screen are clipped at the guard band into fans whose diagonal runs corner to corner, (-120,360) to (360,-120)
 * on a square output: a line through every pixel centre of x + y = 239 (the Runway's smoke, Bond inside an explosion).
 * The RDP without AA_EN writes a pixel only when one fixed sample of it is covered (angrylion blender.c: cvbit, the
 * mask's bit 7), and the edges split the samples exactly, so exactly one triangle draws each pixel. Here, for such
 * draws (gfx_pc: blended or
 * alpha-tested, no AA_EN), each row's ends are decided per pixel by edge functions taken in a canonical vertex
 * order (the lower y, then the lower x, first), so the two triangles of a shared edge compute the same float there
 * and only its sign differs; a centre exactly on the edge goes to the triangle the edge is a left (or top) edge of.
 * With AA_EN both triangles write the pixel they share on the N64 too (partial coverage each): left as it was.
 * Both row loops take it (raster_tri_body's and hot_tri_lean_body's, GE_LEANTRI), so a quad drawn half by each
 * still draws each pixel once; a row whose first pixel moved starts its interpolation there (+-1 on a few of its
 * pixels). A deferred background is resolved with the rule it was marked with (RastState.own). Other draws keep
 * the inclusive solve, bit-identical. GE_FILLRULE=0: every draw. */
typedef struct { float px, py, ex, ey, sg; bool tie; float sey, sex; uint32_t lo; } OwnEdge;
typedef struct { OwnEdge e[3]; } OwnTri;
static inline void own_edge(OwnEdge *o, const SoftVtx *a, const SoftVtx *b, float s) {
    const bool fwd = a->y < b->y || (a->y == b->y && a->x < b->x);
    const SoftVtx *p = fwd ? a : b, *q = fwd ? b : a;
    o->px = p->x; o->py = p->y; o->ex = q->x - p->x; o->ey = q->y - p->y;
    o->sg = fwd ? s : -s; /* the triangle's inside is where sg * e > 0 */
    o->tie = o->sg * o->ey > 0.0f || (o->ey == 0.0f && o->sg * o->ex < 0.0f); /* left edge, or top (y down) */
    o->sey = o->sg * o->ey; o->sex = o->sg * o->ex; o->lo = o->tie ? 0u : 1u; /* own_row (GE_OWNFAST) */
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
/* GE_OWNFAST (2026-10-04; the device Bench: the rule cost 0.9 ms in the explosions): own_px's ends of a row with no
 * VFP compare. own_px took sg * e > 0 || (e == 0 && tie) per edge -- one or two VMRS each (the A7 waits for the VFP
 * pipeline at every one), up to twelve edges a row -- and the row's (py - y) * ex at every pixel. With sg = +-1:
 *   - sg * e = (fx - px) * (sg * ey) - (py - py0) * (sg * ex): rounding to nearest is symmetric, so negating an
 *     operand negates the product and the difference exactly; only the sign of a zero difference can differ;
 *   - the row's (py - py0) * (sg * ex) once a row (the same product at every pixel of it);
 *   - v = that + 0.0f: +0 for either zero, every other value (NaN, infinities, denormals) as it is;
 *   - sg * e > 0 is v in (+0, +inf], and e == 0 is v == +0: the test is v's bits in [lo, 0x7f800000] with lo = 0
 *     for a tie edge, 1 otherwise -- one unsigned compare on the core (NaN and the negatives lie above).
 * The same pixels decide the same ends, in the same order. */
static int sOwnFast SAVESTATE_EXCLUDE = -1;
static inline __attribute__((always_inline)) bool own_pxf(const OwnTri *t, const float *rb, int x) {
    const float fx = (float)x + 0.5f;
    for (int i = 0; i < 3; i++) {
        const OwnEdge *o = &t->e[i];
        union { float f; uint32_t u; } v = { ((fx - o->px) * o->sey - rb[i]) + 0.0f };
        if (v.u - o->lo > 0x7f800000u - o->lo) return false;
    }
    return true;
}
static inline __attribute__((always_inline)) void own_row(const OwnTri *t, float py, int *pxs, int *pxe) {
    const float rb[3] = { (py - t->e[0].py) * t->e[0].sex, (py - t->e[1].py) * t->e[1].sex, (py - t->e[2].py) * t->e[2].sex };
    int xs = *pxs, xe = *pxe;
    while (xs < xe && !own_pxf(t, rb, xs)) xs++;
    while (xe > xs && !own_pxf(t, rb, xe - 1)) xe--;
    *pxs = xs; *pxe = xe;
}

/* Span rasterizer. Per row: solve the three half-plane constraints for an
 * x-interval (no per-pixel inside tests), then walk the span with
 * incrementally stepped z / shade / (u/w, v/w, 1/w). Perspective division
 * happens every PERSP_SPAN pixels with linear u,v between (the N64-plugin
 * standard; MK64's ground polygons are huge, so the per-pixel fdiv the old
 * loop did dominated the race profile). Row bases are recomputed from the
 * vertex data each row, so precision stays within one mul-add of exact and
 * shared triangle edges stay crack-free. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* The world's hot span on NEON: z-tested, opaque (no alpha test or blend),
 * bilinear texture x shade -- SPAN_LOOP_CM(1, 1, 0, BILINEAR, *, 1) below.
 * Texel addressing and the z test stay scalar and identical to bilerp /
 * the macro; the two lerps, the shade clamp and the modulate run in NEON
 * lanes (B G R A) with the same integer arithmetic:
 *   lerp    (x*(256-f) + y*f) >> 8   in u16 (<= 255*256)
 *   shade   usat(q >> 16, 8)         vshrq 16, vqmovun_s32, vmin 255
 *   modulate div255(t*s) = (x + 1 + (x >> 8)) >> 8, x <= 255*255
 * so it is bit-identical (proved on ARM by tools/handheld/qsweep.sh). A function
 * of its own so the loop gets the registers: inside raster_tri it spilled its
 * colour accumulators every pixel. Flat triangles come in with a zero step. */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16;
} CmTex;
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* ZT 0 (round 27): no depth test, no depth write -- the sky, the Frigate's sea, deferred-sky resolves */
#define CM_SPAN_BODY_ZT(ZT, FETCH)                                                                       \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) {        \
        const int uz = ZT ? clamp16_sh8(zq) : 0;                                                        \
        if (ZT && uz > *zp + zbias) continue;                                                           \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        uint32_t a, b, c, d; FETCH                                                                      \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));       \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));       \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        const uint16x4_t T = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2),            \
                                                      vget_high_u16(H), wts, 3), 8);                     \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        const uint16x4_t M = vorr_u16(cm_div255(vmul_u16(T, S)), a255);                                  \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (ZT && dmask) *zp = (uint16_t)uz;                                                            \
    }
#define CM_FETCH_ADDR(WU, WV)                                                                           \
        const int iu0 = WU(su >> 16), iu1 = WU((su >> 16) + 1);                                         \
        const int b0 = WV(sv >> 16) * t->w, b1 = WV((sv >> 16) + 1) * t->w;
#define CM_WRAP_RRU(x) ((x) & mu)
#define CM_WRAP_RRV(x) ((x) & mv)
#define CM_WRAP_GU(x) wrapf((x), t->w, t->ws, t->mu)
#define CM_WRAP_GV(x) wrapf((x), t->h, t->wt, t->mv)
#define CM_FETCH32(WU, WV) CM_FETCH_ADDR(WU, WV)                                                        \
        a = px32[b0 + iu0]; b = px32[b0 + iu1]; c = px32[b1 + iu0]; d = px32[b1 + iu1];
#define CM_FETCH16(WU, WV) CM_FETCH_ADDR(WU, WV)                                                        \
        a = texel_at(t->px, true, b0 + iu0); b = texel_at(t->px, true, b0 + iu1);                       \
        c = texel_at(t->px, true, b1 + iu0); d = texel_at(t->px, true, b1 + iu1);
static int sCmNeon SAVESTATE_EXCLUDE = 1; /* GE_CMNEON=0 (a GE_CMNEON=0 line in debug.cfg): the scalar loop, for A/B */
static void __attribute__((noinline))
cm_bilin_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                   int32_t zq, int32_t zdq, int zbias, bool dmask, const CmTex *t, int32x4_t acc, int32x4_t dacc) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull); /* lane 3 (A) = 255: argb(..., 255) */
    const uint32_t *const px32 = (const uint32_t *)t->px;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0; /* wrapf = mask */
    if (rr) {
        if (t->t16) { CM_SPAN_BODY_ZT(1, CM_FETCH16(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else        { CM_SPAN_BODY_ZT(1, CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
    } else {
        if (t->t16) { CM_SPAN_BODY_ZT(1, CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else        { CM_SPAN_BODY_ZT(1, CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    }
}
/* The same without a depth test (round 27: the scalar bilinear loop was ~15% of raster_tri in the
 * Frigate -- its sea, the sky, the deferred sky's resolves -- ~8k pixels a frame). GE_CMNOZ=0: scalar. */
static int sCmNoZ SAVESTATE_EXCLUDE = -1;
static void __attribute__((noinline))
cm_bilin_span_neon_noz(uint32_t *fb, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                       const CmTex *t, int32x4_t acc, int32x4_t dacc) {
    uint32_t *const end = fb + span;
    uint16_t *zp = NULL; int32_t zq = 0; const int32_t zdq = 0; const int zbias = 0; const bool dmask = false;
    (void)zp; (void)zbias; (void)dmask;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint32_t *const px32 = (const uint32_t *)t->px;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    if (rr) {
        if (t->t16) { CM_SPAN_BODY_ZT(0, CM_FETCH16(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else        { CM_SPAN_BODY_ZT(0, CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
    } else {
        if (t->t16) { CM_SPAN_BODY_ZT(0, CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else        { CM_SPAN_BODY_ZT(0, CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    }
}
/* The VK combine (SPAN_LOOP_VK, bilinear, opaque): F = K + trunc((S - K) * T / 255) per colour
 * channel, K and S interpolated per pixel, T the bilinear texel (its alpha on all three channels
 * under rgb_texa). One pixel a step with the channels in lanes, like cm_bilin_span_neon. The
 * scalar loop's signed "/ 255" truncates toward zero: |x| <= 65,025 here, so the quotient is
 * div255(|x|) with x's sign put back -- exact (round 18: the Cradle's cloud layers and walls). */
static inline int32x4_t vk_div255_trunc(int32x4_t x) {
    const int32x4_t s = vshrq_n_s32(x, 31);                                  /* 0 or -1 */
    const int32x4_t m = vabsq_s32(x);
    const int32x4_t q = vshrq_n_s32(vaddq_s32(vaddq_s32(m, vshrq_n_s32(m, 8)), vdupq_n_s32(1)), 8);
    return vsubq_s32(veorq_s32(q, s), s);
}
#define VK_SPAN_BODY(ZT, FETCH)                                                                          \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq,                                       \
                     acc = vaddq_s32(acc, dacc), kacc = vaddq_s32(kacc, dkacc)) {                       \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (ZT) { if (uz > *zp + zbias) continue; }                                                     \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        uint32_t a, b, c, d; FETCH                                                                      \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));       \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));       \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        uint16x4_t T = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2),                  \
                                                vget_high_u16(H), wts, 3), 8);                           \
        if (texa) T = vdup_lane_u16(T, 3);                                                              \
        const int16x4_t S = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), c255));    \
        const int16x4_t K = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(kacc, 16)), c255));   \
        const int32x4_t P = vmull_s16(vsub_s16(S, K), vreinterpret_s16_u16(T));                         \
        const uint16x4_t F = vorr_u16(vreinterpret_u16_s16(vmovn_s32(vaddq_s32(vmovl_s16(K),             \
                                                                               vk_div255_trunc(P)))), a255); \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(F, F))), 0);                         \
        if (ZT && dmask) *zp = (uint16_t)uz;                                                            \
    }
/* The same with the alpha test and blend (SPAN_LOOP_VK with ALPHA: fa = K + (S - K) * T / 255 in
 * alpha when the texture supplies alpha, else the shade's; below amin nothing is drawn; under 255
 * div255(F * fa + D * (255 - fa)) per channel, as cm_bilin_blend_span_neon). Round 19: the
 * Jungle's foliage, a third of its raster_tri. Exact. GE_VKANEON=0: the scalar loops. */
#define VKA_SPAN_BODY(ZT, FETCH)                                                                          \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq,                                       \
                     acc = vaddq_s32(acc, dacc), kacc = vaddq_s32(kacc, dkacc)) {                       \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (ZT) { if (uz > *zp + zbias) continue; }                                                     \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        uint32_t a, b, c, d; FETCH                                                                      \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));       \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));       \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        uint16x4_t T = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2),                  \
                                                vget_high_u16(H), wts, 3), 8);                           \
        if (texa) T = vdup_lane_u16(T, 3);                                                              \
        const int16x4_t S = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), c255));    \
        const int16x4_t K = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(kacc, 16)), c255));   \
        const int32x4_t P = vmull_s16(vsub_s16(S, K), vreinterpret_s16_u16(T));                         \
        uint16x4_t F = vreinterpret_u16_s16(vmovn_s32(vaddq_s32(vmovl_s16(K), vk_div255_trunc(P))));   \
        const unsigned fa = a_tex ? vget_lane_u16(F, 3) : (unsigned)vget_lane_s16(S, 3);                \
        if ((int)fa < amin) continue;                                                                   \
        if (fa < 255) {                                                                                 \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(*fb))));           \
            F = cm_div255(vmla_n_u16(vmul_n_u16(F, (uint16_t)fa), D, (uint16_t)(255 - fa)));            \
        }                                                                                               \
        F = vorr_u16(F, a255);                                                                          \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(F, F))), 0);                         \
        if (ZT && dmask) *zp = (uint16_t)uz;                                                            \
    }
/* The blended texture x shade span (SPAN_LOOP_CM with ALPHA: the explosions and smoke, 28% of
 * raster_tri's instructions in the Dam's GE_EXPLODE scene): cm_bilin_span_neon's combine, then
 * the scalar loop's alpha -- a_tex ? (a_in ? T*S : T) : S -- its amin reject and its blend,
 * div255(M * fa + D * (255 - fa)) per channel (each sum <= 65,025: u16 lanes). Exact (round 18). */
#define CMB_SPAN_BODY(FETCH)                                                                             \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) {        \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *zp + zbias) continue;                                                                 \
        const int32_t su = uf - 0x8000, sv = vf - 0x8000;                                               \
        uint32_t a, b, c, d; FETCH                                                                      \
        if (a_tex && !((a | b | c | d) >> 24)) continue; /* four alpha-0 taps: fa 0 < amin */        \
        const uint32_t fu = (uint32_t)(su >> 8) & 0xff, fv = (uint32_t)(sv >> 8) & 0xff;                \
        const uint16x4_t wts = vreinterpret_u16_u32(vcreate_u32(                                         \
            ((uint64_t)((fv << 16) | (256u - fv)) << 32) | ((fu << 16) | (256u - fu))));                  \
        const uint16x8_t X = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)c << 32) | a)));       \
        const uint16x8_t Y = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)d << 32) | b)));       \
        const uint16x8_t H = vshrq_n_u16(vmlaq_lane_u16(vmulq_lane_u16(X, wts, 0), Y, wts, 1), 8);        \
        const uint16x4_t T = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(H), wts, 2),            \
                                                      vget_high_u16(H), wts, 3), 8);                     \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M = cm_div255(vmul_u16(T, S));                                                       \
        const unsigned fa = !a_tex ? vget_lane_u16(S, 3) : (!a_in ? vget_lane_u16(T, 3) : vget_lane_u16(M, 3)); \
        if ((int)fa < amin) continue;                                                                   \
        if (fa < 255) {                                                                                 \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(*fb))));           \
            M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));            \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (dmask) *zp = (uint16_t)uz;                                                                  \
    }
static int sCmbNeon SAVESTATE_EXCLUDE = -1; /* GE_CMBNEON=0: the scalar loop, for A/B */
static void __attribute__((noinline))
cm_bilin_blend_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                         int32_t zq, int32_t zdq, int zbias, bool dmask, const CmTex *t, int32x4_t acc, int32x4_t dacc,
                         bool a_tex, bool a_in, int amin) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint32_t *const px32 = (const uint32_t *)t->px;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    if (rr) {
        if (t->t16) { CMB_SPAN_BODY(CM_FETCH16(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else        { CMB_SPAN_BODY(CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
    } else {
        if (t->t16) { CMB_SPAN_BODY(CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else        { CMB_SPAN_BODY(CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    }
}
#undef CMB_SPAN_BODY
static int sVkNeon SAVESTATE_EXCLUDE = -1; /* GE_VKNEON=0: the scalar loop, for A/B */
static void __attribute__((noinline))
vk_bilin_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                   int32_t zq, int32_t zdq, int zbias, bool dmask, bool ztest, bool texa, const CmTex *t,
                   int32x4_t acc, int32x4_t dacc, int32x4_t kacc, int32x4_t dkacc) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t c255 = vdup_n_u16(255);
    const uint32_t *const px32 = (const uint32_t *)t->px;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0 && !t->t16;
    if (ztest) {
        if (rr) { VK_SPAN_BODY(1, CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else if (t->t16) { VK_SPAN_BODY(1, CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else { VK_SPAN_BODY(1, CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    } else {
        if (rr) { VK_SPAN_BODY(0, CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else if (t->t16) { VK_SPAN_BODY(0, CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else { VK_SPAN_BODY(0, CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    }
}
static int sVkaNeon SAVESTATE_EXCLUDE = -1;
static void __attribute__((noinline))
vk_bilin_blend_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                         int32_t zq, int32_t zdq, int zbias, bool dmask, bool ztest, bool texa, const CmTex *t,
                         int32x4_t acc, int32x4_t dacc, int32x4_t kacc, int32x4_t dkacc, bool a_tex, int amin) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t c255 = vdup_n_u16(255);
    const uint32_t *const px32 = (const uint32_t *)t->px;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0 && !t->t16;
    if (ztest) {
        if (rr) { VKA_SPAN_BODY(1, CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else if (t->t16) { VKA_SPAN_BODY(1, CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else { VKA_SPAN_BODY(1, CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    } else {
        if (rr) { VKA_SPAN_BODY(0, CM_FETCH32(CM_WRAP_RRU, CM_WRAP_RRV)) }
        else if (t->t16) { VKA_SPAN_BODY(0, CM_FETCH16(CM_WRAP_GU, CM_WRAP_GV)) }
        else { VKA_SPAN_BODY(0, CM_FETCH32(CM_WRAP_GU, CM_WRAP_GV)) }
    }
}
/* The mipmapped spans on NEON (round 19): away from level 0 raster_tri reads the nearest texel
 * of level L (FETCH_NEAREST) or lerps it with L+1's (FETCH_TRILIN: the index halved, lerp_argb),
 * and those opaque loops were scalar -- a third of raster_tri on the Dam and the Caverns, the
 * lerp and the wraps most of it. The texel is fetched and lerped exactly as the macros do; the
 * combine is cm_bilin_span_neon's (texture x shade) or vk_bilin_span_neon's (K + (S - K) * T / 255).
 * Plain nearest spans (no mips, not the HUD box) take the same loops. GE_MIPNEON=0: the scalar loops. */
/* GE_TBILNEON (2026-10-04, the heavy-scene scan: the Jungle's blended fronds and the Aztec hangar's walls spend 12-18%
 * in these span functions, most of it in tribil_texel): tribil_texel for 32-bit texels in NEON lanes. The same taps
 * (wrapf on each axis and level, as bilerp's general path; on a repeating power-of-two texture wrapf is bilerp's mask),
 * the same lerps as the lean rows' LEAN_LERP2 (lerp2's (x (256 - f) + y f) >> 8 is (x 256 + (y - x) f) >> 8, the true
 * value <= 65,280 in u16 lanes) and LEAN_TBIL32's TRILERP (rdp_trilerp's ((y - x) f + x 256 + 0x80) >> 8, <= 65,408).
 * The texel comes back packed as tribil_texel's. GE_TBILNEON=0: tribil_texel. */
static int sTbilNeon SAVESTATE_EXCLUDE = -1;
static inline __attribute__((always_inline)) uint16x4_t tbn_level(const uint32_t *p, int w, int h, int ws, int wt, int mu,
                                                                  int mv, int32_t uf, int32_t vf) {
    const int32_t su = uf - 0x8000, sv = vf - 0x8000;
    const int iu0 = wrapf(su >> 16, w, ws, mu), iu1 = wrapf((su >> 16) + 1, w, ws, mu);
    const int b0 = wrapf(sv >> 16, h, wt, mv) * w, b1 = wrapf((sv >> 16) + 1, h, wt, mv) * w;
    const uint8x8_t X8 = vreinterpret_u8_u32(vld1_lane_u32(p + b1 + iu0, vld1_dup_u32(p + b0 + iu0), 1));
    const uint8x8_t Y8 = vreinterpret_u8_u32(vld1_lane_u32(p + b1 + iu1, vld1_dup_u32(p + b0 + iu1), 1));
    const uint16x8_t Hq = vshrq_n_u16(vmlaq_u16(vshll_n_u8(X8, 8), vsubl_u8(Y8, X8), vdupq_n_u16((uint16_t)((su >> 8) & 0xff))), 8);
    return vshr_n_u16(vmla_u16(vshl_n_u16(vget_low_u16(Hq), 8), vsub_u16(vget_high_u16(Hq), vget_low_u16(Hq)),
                               vdup_n_u16((uint16_t)((sv >> 8) & 0xff))), 8);
}
static inline __attribute__((always_inline)) uint32_t tribil_n32(const void *px, const void *px2, int w, int h, int ws, int wt,
                                                                 int mu, int mv, int32_t uf, int32_t vf, uint32_t f) {
    const int w2 = w >> 1, h2 = h >> 1;
    const uint16x4_t T0 = tbn_level((const uint32_t *)px, w, h, ws, wt, mu, mv, uf, vf);
    const uint16x4_t T1 = tbn_level((const uint32_t *)px2, w2, h2, ws, wt, mu >= 0 ? w2 - 1 : -1, mv >= 0 ? h2 - 1 : -1,
                                    uf >> 1, vf >> 1);
    const uint16x4_t T = vshr_n_u16(vmla_u16(vadd_u16(vshl_n_u16(T0, 8), vdup_n_u16(0x80)), vsub_u16(T1, T0),
                                             vdup_n_u16((uint16_t)f)), 8);
    return vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(T, T))), 0);
}
/* bil (GE_MIPLOD, two levels): both levels filtered and TRILERPed, tribil_texel -- as the scalar FETCH_TRILIN */
#define MIP_FETCH_N(WU, WV, TEXEL0, TEXEL1)                                                            \
        uint32_t tx;                                                                                    \
        if (bil) tx = !t->t16 && sTbilNeon > 0                                                          \
                      ? tribil_n32(t->px, px2, t->w, t->h, t->ws, t->wt, t->mu, t->mv, uf, vf, mfrac)   \
                      : tribil_texel(t->px, px2, t->t16, t->w, t->h, t->ws, t->wt, t->mu, t->mv, uf, vf, mfrac); \
        else {                                                                                          \
        const int iu = WU(uf >> 16), iv = WV(vf >> 16);                                                 \
        tx = tri ? lerp_argb(TEXEL0(iv * t->w + iu), TEXEL1((iv >> 1) * tw2 + (iu >> 1)), mfrac)        \
                 : TEXEL0(iv * t->w + iu);                                                              \
        }
#define MIP_T32(i) px32[i]
#define MIP_T32B(i) px32b[i]
#define MIP_T16(i) texel_at(t->px, true, (i))
#define MIP_T16B(i) texel_at(px2, true, (i))
#define MIP_CM_BODY(WU, WV, T0, T1)                                                                             \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) {        \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *zp + zbias) continue;                                                                 \
        MIP_FETCH_N(WU, WV, T0, T1)                                                                     \
        const uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(tx))));                \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        const uint16x4_t M = vorr_u16(cm_div255(vmul_u16(T, S)), a255);                                  \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (dmask) *zp = (uint16_t)uz;                                                                  \
    }
#define MIP_VK_BODY(ZT, WU, WV, T0, T1)                                                                         \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq,                                       \
                     acc = vaddq_s32(acc, dacc), kacc = vaddq_s32(kacc, dkacc)) {                       \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (ZT) { if (uz > *zp + zbias) continue; }                                                     \
        MIP_FETCH_N(WU, WV, T0, T1)                                                                     \
        uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(tx))));                      \
        if (texa) T = vdup_lane_u16(T, 3);                                                              \
        const int16x4_t S = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), c255));    \
        const int16x4_t K = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(kacc, 16)), c255));   \
        const int32x4_t P = vmull_s16(vsub_s16(S, K), vreinterpret_s16_u16(T));                         \
        const uint16x4_t F = vorr_u16(vreinterpret_u16_s16(vmovn_s32(vaddq_s32(vmovl_s16(K),             \
                                                                               vk_div255_trunc(P)))), a255); \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(F, F))), 0);                         \
        if (ZT && dmask) *zp = (uint16_t)uz;                                                            \
    }
#define MIP_VKA_BODY(ZT, WU, WV, T0, T1)                                                                         \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq,                                       \
                     acc = vaddq_s32(acc, dacc), kacc = vaddq_s32(kacc, dkacc)) {                       \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (ZT) { if (uz > *zp + zbias) continue; }                                                     \
        MIP_FETCH_N(WU, WV, T0, T1)                                                                     \
        uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(tx))));                      \
        if (texa) T = vdup_lane_u16(T, 3);                                                              \
        const int16x4_t S = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), c255));    \
        const int16x4_t K = vreinterpret_s16_u16(vmin_u16(vqmovun_s32(vshrq_n_s32(kacc, 16)), c255));   \
        const int32x4_t P = vmull_s16(vsub_s16(S, K), vreinterpret_s16_u16(T));                         \
        uint16x4_t F = vreinterpret_u16_s16(vmovn_s32(vaddq_s32(vmovl_s16(K), vk_div255_trunc(P))));   \
        const unsigned fa = a_tex ? vget_lane_u16(F, 3) : (unsigned)vget_lane_s16(S, 3);                \
        if ((int)fa < amin) continue;                                                                   \
        if (fa < 255) {                                                                                 \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(*fb))));           \
            F = cm_div255(vmla_n_u16(vmul_n_u16(F, (uint16_t)fa), D, (uint16_t)(255 - fa)));            \
        }                                                                                               \
        F = vorr_u16(F, a255);                                                                          \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(F, F))), 0);                         \
        if (ZT && dmask) *zp = (uint16_t)uz;                                                            \
    }
/* blended texture x shade on the mip fetch (round 20: the Cradle's cut-out girders at a distance):
 * cm_bilin_blend_span_neon's alpha and blend; an alpha-0 texel gives fa 0 (< amin) when the texture
 * supplies alpha, so it is dropped before the combine */
#define MIP_CMB_BODY(WU, WV, T0, T1)                                                                     \
    for (; fb < end; fb++, zp++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) {        \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *zp + zbias) continue;                                                                 \
        MIP_FETCH_N(WU, WV, T0, T1)                                                                     \
        if (a_tex && !(tx >> 24)) continue;                                                             \
        const uint16x4_t T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(tx))));                \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M = cm_div255(vmul_u16(T, S));                                                       \
        const unsigned fa = !a_tex ? vget_lane_u16(S, 3) : (!a_in ? vget_lane_u16(T, 3) : vget_lane_u16(M, 3)); \
        if ((int)fa < amin) continue;                                                                   \
        if (fa < 255) {                                                                                 \
            const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(*fb))));           \
            M = cm_div255(vmla_n_u16(vmul_n_u16(M, (uint16_t)fa), D, (uint16_t)(255 - fa)));            \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        vst1_lane_u32(fb, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (dmask) *zp = (uint16_t)uz;                                                                  \
    }
/* the four fetch forms: repeat/pow2 (a mask) or wrapf, 32- or 16-bit storage */
#define MIP_FETCHES(BODY)                                                                                \
    if (rr) {                                                                                           \
        if (t->t16) { BODY(CM_WRAP_RRU, CM_WRAP_RRV, MIP_T16, MIP_T16B) }                 \
        else        { BODY(CM_WRAP_RRU, CM_WRAP_RRV, MIP_T32, MIP_T32B) }                 \
    } else {                                                                                            \
        if (t->t16) { BODY(CM_WRAP_GU, CM_WRAP_GV, MIP_T16, MIP_T16B) }                   \
        else        { BODY(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }                   \
    }
#define MIP_CM_BODY1(WU, WV, T0, T1) MIP_CM_BODY(WU, WV, T0, T1)
#define MIP_CMB_BODY1(WU, WV, T0, T1) MIP_CMB_BODY(WU, WV, T0, T1)
#define MIP_VK_BODY_Z1(WU, WV, T0, T1) MIP_VK_BODY(1, WU, WV, T0, T1)
#define MIP_VK_BODY_Z0(WU, WV, T0, T1) MIP_VK_BODY(0, WU, WV, T0, T1)
#define MIP_VKA_BODY_Z1(WU, WV, T0, T1) MIP_VKA_BODY(1, WU, WV, T0, T1)
#define MIP_VKA_BODY_Z0(WU, WV, T0, T1) MIP_VKA_BODY(0, WU, WV, T0, T1)
static int sMipNeon SAVESTATE_EXCLUDE = -1; /* GE_MIPNEON=0: the scalar loops, for A/B */
static void __attribute__((noinline))
cm_mip_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                 int32_t zq, int32_t zdq, int zbias, bool dmask, const CmTex *t, bool tri, const void *px2, int tw2,
                 uint32_t mfrac, int32x4_t acc, int32x4_t dacc, bool bil) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint32_t *const px32 = (const uint32_t *)t->px, *const px32b = (const uint32_t *)px2;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    if (bil) { const bool tri = true, bil = true; MIP_CM_BODY1(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }
    else if (tri) { const bool tri = true, bil = false; MIP_FETCHES(MIP_CM_BODY1) }
    else     { const bool tri = false, bil = false; MIP_FETCHES(MIP_CM_BODY1) }
}
static void __attribute__((noinline))
cm_mip_blend_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                       int32_t zq, int32_t zdq, int zbias, bool dmask, const CmTex *t, bool tri, const void *px2, int tw2,
                       uint32_t mfrac, int32x4_t acc, int32x4_t dacc, bool a_tex, bool a_in, int amin, bool bil) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint32_t *const px32 = (const uint32_t *)t->px, *const px32b = (const uint32_t *)px2;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    if (bil) { const bool tri = true, bil = true; MIP_CMB_BODY1(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }
    else if (tri) { const bool tri = true, bil = false; MIP_FETCHES(MIP_CMB_BODY1) }
    else     { const bool tri = false, bil = false; MIP_FETCHES(MIP_CMB_BODY1) }
}
static void __attribute__((noinline))
vk_mip_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                 int32_t zq, int32_t zdq, int zbias, bool dmask, bool ztest, bool texa, const CmTex *t, bool tri,
                 const void *px2, int tw2, uint32_t mfrac, int32x4_t acc, int32x4_t dacc, int32x4_t kacc, int32x4_t dkacc, bool bil) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t c255 = vdup_n_u16(255);
    const uint32_t *const px32 = (const uint32_t *)t->px, *const px32b = (const uint32_t *)px2;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    if (ztest) {
        if (bil) { const bool tri = true, bil = true; MIP_VK_BODY_Z1(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }
        else if (tri) { const bool tri = true, bil = false; MIP_FETCHES(MIP_VK_BODY_Z1) }
        else     { const bool tri = false, bil = false; MIP_FETCHES(MIP_VK_BODY_Z1) }
    } else {
        if (bil) { const bool tri = true, bil = true; MIP_VK_BODY_Z0(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }
        else if (tri) { const bool tri = true, bil = false; MIP_FETCHES(MIP_VK_BODY_Z0) }
        else     { const bool tri = false, bil = false; MIP_FETCHES(MIP_VK_BODY_Z0) }
    }
}
static void __attribute__((noinline))
vk_mip_blend_span_neon(uint32_t *fb, uint16_t *zp, int span, int32_t uf, int32_t vf, int32_t duf, int32_t dvf,
                       int32_t zq, int32_t zdq, int zbias, bool dmask, bool ztest, bool texa, const CmTex *t, bool tri,
                       const void *px2, int tw2, uint32_t mfrac, int32x4_t acc, int32x4_t dacc, int32x4_t kacc,
                       int32x4_t dkacc, bool a_tex, int amin, bool bil) {
    uint32_t *const end = fb + span;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull);
    const uint16x4_t c255 = vdup_n_u16(255);
    const uint32_t *const px32 = (const uint32_t *)t->px, *const px32b = (const uint32_t *)px2;
    const int mu = t->mu, mv = t->mv;
    const bool rr = t->ws == WRAP_REPEAT && t->wt == WRAP_REPEAT && mu >= 0 && mv >= 0;
    if (ztest) {
        if (bil) { const bool tri = true, bil = true; MIP_VKA_BODY_Z1(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }
        else if (tri) { const bool tri = true, bil = false; MIP_FETCHES(MIP_VKA_BODY_Z1) }
        else     { const bool tri = false, bil = false; MIP_FETCHES(MIP_VKA_BODY_Z1) }
    } else {
        if (bil) { const bool tri = true, bil = true; MIP_VKA_BODY_Z0(CM_WRAP_GU, CM_WRAP_GV, MIP_T32, MIP_T32B) }
        else if (tri) { const bool tri = true, bil = false; MIP_FETCHES(MIP_VKA_BODY_Z0) }
        else     { const bool tri = false, bil = false; MIP_FETCHES(MIP_VKA_BODY_Z0) }
    }
}
#undef MIP_FETCH_N
#undef MIP_T32
#undef MIP_T32B
#undef MIP_T16
#undef MIP_T16B
#undef MIP_CM_BODY
#undef MIP_VK_BODY
#undef MIP_FETCHES
#undef MIP_CM_BODY1
#undef MIP_CMB_BODY1
#undef MIP_CMB_BODY
#undef MIP_VK_BODY_Z1
#undef MIP_VK_BODY_Z0
#undef MIP_VKA_BODY
#undef MIP_VKA_BODY_Z1
#undef MIP_VKA_BODY_Z0
#undef VKA_SPAN_BODY
#undef VK_SPAN_BODY
#undef CM_SPAN_BODY
#undef CM_FETCH_ADDR
#undef CM_WRAP_RRU
#undef CM_WRAP_RRV
#undef CM_WRAP_GU
#undef CM_WRAP_GV
#undef CM_FETCH32
#undef CM_FETCH16
#define HH_CM_NEON 1
#endif

#define PERSP_SPAN 16
/* Deferred backgrounds (round 24; see def_record below): raster_tri's per-row hook (declared here for the lean
 * triangle rows; see raster_tri_body). */
enum { DEF_OP_NONE, DEF_OP_MARK, DEF_OP_COVER, DEF_OP_DIRECT, DEF_OP_RESOLVE };
static int sDefOp SAVESTATE_EXCLUDE;
static bool def_row(int y, int xs, int xe); /* true: draw nothing on this row */
/* GE_TJFILL (see tj_fill): the pixels of the draws that may leave one unwritten or write it without depth, marked a
 * byte a pixel (sTjMask) by tj_mark_batch -- each triangle's rows by the pixel-centre rule, computed there, nothing in
 * the span loops; sTjRow: the rows to clear. */
static bool sPhase3D SAVESTATE_EXCLUDE; /* (the frame section) */
extern bool gfx_soft_depth_written;     /* (the deferred backgrounds) */
static uint8_t *sTjMask SAVESTATE_EXCLUDE, *sTjRow SAVESTATE_EXCLUDE;
static int sTjMaskW SAVESTATE_EXCLUDE, sTjMaskH SAVESTATE_EXCLUDE;
#define TJ_MARKED(x, y) (sTjMask[(y) * sTjMaskW + (x)])
static bool sDbgBilin SAVESTATE_EXCLUDE; static float sDbgScr SAVESTATE_EXCLUDE, sDbgUvc SAVESTATE_EXCLUDE; /* see OOT_PICK */

#if defined(HH_CM_NEON)
/* A whole row of the world's hot case -- opaque, z-tested texture x shade, no K, not the HUD --
 * in one call (round 19; FZX 1b73772 / the OoT port c935b73's per-row entry): raster_tri's span
 * setup for it, the same float steps in the same order (perspective ends, the mip level, the
 * NEON to_fix16, the fixed-point colours), with the per-triangle constants in one struct. Inside
 * raster_tri that setup spilled around its ~60 live values every span. GE_HOTROW=0: the spans
 * from raster_tri's own loop. */
typedef struct {
    uint32_t *fbuf; uint16_t *zb;
    int zbias; bool dmask, persp, mip, t16, bilin_l0, tbil, nlod;
    bool lodfast, tbilpair, bilpair, lodlean;     /* GE_LODFAST: mip_pick_fast; GE_TBILPAIR; GE_BILPAIR; GE_LODLEAN */
    float ftw_l0, fth_l0;                         /* (float)tw_l0, th_l0 (mip_pick_fast) */
    const SoftTex *tex;
    int ws, wt, tw_l0, th_l0, tmu_l0, tmv_l0;
    const void *texpx; int tw, th, tmask_u, tmask_v; float su, sv; /* without mips */
    float zdx, crdx, cgdx, cbdx, cadx, uwdx, vwdx, swdx, uwdy, vwdy, swdy;
    int32_t zdq, rdq, gdq, bdq, adq;
    bool ztest, texa;                             /* the VK rows (hot_row_vk) */
    bool alpha, a_tex; int amin;                  /* blended VK rows */
    bool a_in; const struct AlphaRowsTag *arows;  /* the lean blended rows (GE_LEANBLEND): the alpha input, span skip */
    float ckrdx, ckgdx, ckbdx, ckadx;
    int32_t kdq_r, kdq_g, kdq_b, kdq_a;
    bool pshade;                                  /* colours are c/w: perspective-correct per span */
    float32x4_t dp, dc, dk;                       /* GE_SPANVEC: { uwdx, vwdx, swdx, zdx }, { cbdx, cgdx, crdx, cadx }, K's */
    int zsk;                                      /* GE_ZSKIP: tests left this triangle (see hot_row_lean_body); 0: none */
} HotTri;
static int sHotRow SAVESTATE_EXCLUDE = -1;
static inline __attribute__((always_inline)) void hot_row_body(const HotTri *h, const bool vk, const bool nl, int idx, int x, int xe,
                                                               float zf, float cr, float cg, float cb, float ca, float uw,
                                                               float vw, float sw, float rw, float ckr, float ckg, float ckb,
                                                               float cka) {
    const int ws = h->ws, wt = h->wt;
    int32x4_t dacc = { h->bdq, h->gdq, h->rdq, h->adq };
    int32x4_t dkacc = { h->kdq_b, h->kdq_g, h->kdq_r, h->kdq_a };
    while (x < xe) {
        int span = xe - x;
        if (span > PERSP_SPAN) span = PERSP_SPAN;
        const float fspan = (float)span;
        const float sw_b = sw + h->swdx * fspan;
        float rw_b = 0.0f;
        float ckr_s = ckr, ckg_s = ckg, ckb_s = ckb, cka_s = cka;
        float u_a, v_a, du, dv;
        if (h->persp) {
            rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
            u_a = uw * rw; v_a = vw * rw;
            const float nu = (uw + h->uwdx * fspan) * rw_b - u_a;
            const float nv = (vw + h->vwdx * fspan) * rw_b - v_a;
            if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); }
            else                    { du = nu / fspan;               dv = nv / fspan; }
        } else {
            u_a = uw; v_a = vw; du = h->uwdx; dv = h->vwdx;
        }
        const void *texpx = h->texpx, *texpx2 = NULL;
        int tw = h->tw, th = h->th, tmask_u = h->tmask_u, tmask_v = h->tmask_v, tw2 = 1;
        float su = h->su, sv = h->sv;
        bool mtri = false, use_bilin = h->bilin_l0;
        uint32_t mfrac = 0;
        if (h->mip) { /* raster_tri's mip block, line for line */
            const SoftTex *const tex = h->tex;
            const int tw_l0 = h->tw_l0, th_l0 = h->th_l0;
            const float dux = du * (float)tw_l0, dvx = dv * (float)th_l0;
            const float duy = (h->uwdy - u_a * h->swdy) * rw * (float)tw_l0, dvy = (h->vwdy - v_a * h->swdy) * rw * (float)th_l0;
            const float rx = dux * dux + dvx * dvx, ry = duy * duy + dvy * dvy;
            union { float f; uint32_t u; } rb;
            rb.f = (rx > ry ? rx : ry) * sMipScale;
            const int e = (int)((rb.u >> 23) & 0xff) - 127;
            const float l2 = ((float)e + (float)(rb.u & 0x7fffffu) * (1.0f / 8388608.0f)) * 0.5f;
            int lvl = 0;
            uint32_t fr = 0;
            bool ntri = false;
            if (nl) {
                if (h->lodfast)
                    mip_pick_fast(tex, tw_l0, th_l0, h->ftw_l0, h->fth_l0, du, dv, u_a, v_a, h->uwdy, h->vwdy, h->swdy, rw, rw_b,
                                  fspan, &lvl, &fr, &ntri);
                else
                    mip_pick_n64(tex, tw_l0, th_l0, du, dv, u_a, v_a, h->uwdy, h->vwdy, h->swdy, rw, rw_b, fspan, &lvl, &fr, &ntri);
            } else {
            if (l2 > 0.0f) { lvl = l2 < 7.0f ? (int)l2 : 7; fr = (uint32_t)((l2 - (float)lvl) * 256.0f); }
            if (lvl >= tex->nlev - 1) { lvl = tex->nlev - 1; fr = 0; }
            if (!sTrilin && fr >= 192u) { lvl++; fr = 0; }
            }
            texpx = tex->lev[lvl];
            tw = tw_l0 >> lvl; th = th_l0 >> lvl;
            {
                union { uint32_t u; float f; } inv = { (uint32_t)(127 - lvl) << 23 };
                su = (float)tex->w * inv.f; sv = (float)tex->h * inv.f;
            }
            tmask_u = h->tmu_l0 >= 0 ? tw - 1 : -1; tmask_v = h->tmv_l0 >= 0 ? th - 1 : -1;
            mtri = nl ? ntri : sTrilin ? fr > 0u : fr > 64u;
            if (mtri) {
                texpx2 = tex->lev[lvl + 1];
                tw2 = tw >> 1;
                mfrac = (nl || sTrilin) ? fr : (fr - 64u) * 2u;
            }
            use_bilin = h->bilin_l0 && (lvl == 0 || (nl && lvl < sMipFar)) && !mtri; /* GE_MIPFAR */
        }
        const float fin[4] = { wrap_far(u_a * su, tw, ws), du * su, wrap_far(v_a * sv, th, wt), dv * sv };
        const int32x4_t fx = vcvtq_s32_f32(vmulq_n_f32(vminq_f32(vmaxq_f32(vld1q_f32(fin), vdupq_n_f32(-16000.0f)),
                                                                 vdupq_n_f32(16000.0f)), 65536.0f));
        const int32_t uf = vgetq_lane_s32(fx, 0), duf = vgetq_lane_s32(fx, 1), vf = vgetq_lane_s32(fx, 2), dvf = vgetq_lane_s32(fx, 3);
        const int32_t zq = span_z_out(zf) ? span_zq_far(zf, h->zdx, span, h->zdq)
                                          : (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
        int32x4_t acc = { sat_i32(cb * 65536.0f), sat_i32(cg * 65536.0f), sat_i32(cr * 65536.0f), sat_i32(ca * 65536.0f) };
        if (h->pshade) {
            const float is = 1.0f / fspan;
            const float ar = cr * rw, ag = cg * rw, ab = cb * rw, aa = ca * rw;
            const float br = (cr + h->crdx * fspan) * rw_b, bg = (cg + h->cgdx * fspan) * rw_b;
            const float bb = (cb + h->cbdx * fspan) * rw_b, ba = (ca + h->cadx * fspan) * rw_b;
            acc = (int32x4_t){ sat_i32(ab * 65536.0f), sat_i32(ag * 65536.0f), sat_i32(ar * 65536.0f), sat_i32(aa * 65536.0f) };
            dacc = (int32x4_t){ sat_i32((bb - ab) * is * 65536.0f), sat_i32((bg - ag) * is * 65536.0f),
                                sat_i32((br - ar) * is * 65536.0f), sat_i32((ba - aa) * is * 65536.0f) };
            if (vk) {
                const float akr = ckr * rw, akg = ckg * rw, akb = ckb * rw, aka = cka * rw;
                const float bkr = (ckr + h->ckrdx * fspan) * rw_b, bkg = (ckg + h->ckgdx * fspan) * rw_b;
                const float bkb = (ckb + h->ckbdx * fspan) * rw_b, bka = (cka + h->ckadx * fspan) * rw_b;
                ckr_s = akr; ckg_s = akg; ckb_s = akb; cka_s = aka;
                dkacc = (int32x4_t){ sat_i32((bkb - akb) * is * 65536.0f), sat_i32((bkg - akg) * is * 65536.0f),
                                     sat_i32((bkr - akr) * is * 65536.0f), sat_i32((bka - aka) * is * 65536.0f) };
            }
        }
        const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, h->t16 };
        if (vk) {
            const int32x4_t kacc = { sat_i32(ckb_s * 65536.0f), sat_i32(ckg_s * 65536.0f), sat_i32(ckr_s * 65536.0f), sat_i32(cka_s * 65536.0f) };
            if (h->alpha) {
                if (use_bilin)
                    vk_bilin_blend_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask,
                                             h->ztest, h->texa, &ct, acc, dacc, kacc, dkacc, h->a_tex, h->amin);
                else
                    vk_mip_blend_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask,
                                           h->ztest, h->texa, &ct, mtri, texpx2, tw2, mfrac, acc, dacc, kacc, dkacc,
                                           h->a_tex, h->amin, (nl && h->tbil) && mtri);
            } else if (use_bilin)
                vk_bilin_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, h->ztest,
                                   h->texa, &ct, acc, dacc, kacc, dkacc);
            else
                vk_mip_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, h->ztest,
                                 h->texa, &ct, mtri, texpx2, tw2, mfrac, acc, dacc, kacc, dkacc, (nl && h->tbil) && mtri);
        } else if (use_bilin)
            cm_bilin_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, &ct, acc, dacc);
        else
            cm_mip_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, &ct,
                             mtri, texpx2, tw2, mfrac, acc, dacc, (nl && h->tbil) && mtri);
        idx += span;
        zf += h->zdx * fspan;
        cr += h->crdx * fspan; cg += h->cgdx * fspan; cb += h->cbdx * fspan; ca += h->cadx * fspan;
        if (vk) { ckr += h->ckrdx * fspan; ckg += h->ckgdx * fspan; ckb += h->ckbdx * fspan; cka += h->ckadx * fspan; }
        uw += h->uwdx * fspan;
        vw += h->vwdx * fspan;
        sw = sw_b; rw = rw_b;
        x += span;
    }
}
static void __attribute__((noinline)) hot_row(const HotTri *h, int idx, int x, int xe, float zf, float cr, float cg,
                                              float cb, float ca, float uw, float vw, float sw, float rw) {
    if (h->nlod) hot_row_body(h, false, true, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0.0f, 0.0f, 0.0f, 0.0f);
    else hot_row_body(h, false, false, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0.0f, 0.0f, 0.0f, 0.0f);
}
/* the VK rows (K + (S - K) * T, opaque, z-tested or not): the same setup plus K's colours */
static void __attribute__((noinline)) hot_row_vk(const HotTri *h, int idx, int x, int xe, float zf, float cr, float cg,
                                                 float cb, float ca, float uw, float vw, float sw, float rw, float ckr,
                                                 float ckg, float ckb, float cka) {
    if (h->nlod) hot_row_body(h, true, true, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
    else hot_row_body(h, true, false, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
}
/* GE_LEANROW (2026-10-01, the FPS hunt; OoT's lean opaque rows, the OoT port fabace3/bfe7b5d, in GoldenEye's shape):
 * hot_row's and hot_row_vk's rows -- opaque texture x shade, and K + (S - K) * T -- with the span setup and the
 * pixel loops in one function, for the same values in the same order (and, below, the same rows without the depth
 * test: GE_LEANNOZ; the blended ones: GE_LEANBLEND; the whole row loop: GE_LEANTRI). hot_row_body hands each 16-pixel span to a
 * span function: ~15 arguments through the stack, the CmTex built on the stack, the colour vectors stored and
 * reloaded, and inside the span the invariants reloaded from the stack every pixel (zbias, dmask, the masks, the row
 * stride, the steps, the end), the trilinear lerp on the core (lerp_argb). Here:
 *   - the pixel loops are inline, specialised for the case that carries the world: both axes repeating on a power of
 *     two, no decal bias (zbias 0), z written when tested; the VK rows opaque, 32-bit, the texel's own colour (no
 *     rgb_texa). Anything else goes to the old span functions, as before;
 *   - the bilinear weights are taken by the one-multiply lerp, floor((x*256 + (y - x)*f) / 256) =
 *     floor((x*(256 - f) + y*f) / 256): the true sum is at most 255*256, so the wrapping 16-bit lanes give it exactly
 *     (every x, y, f checked: port/hh/tools/rast_test.c ops); the weights are vdup'd from the core, not packed;
 *   - the trilinear texel (two levels by mfrac) is that lerp in NEON lanes: lerp_argb per channel, (a*(256 - f) +
 *     b*f) >> 8, the same as above (f is the span's constant); the taps go straight into NEON lanes (vld1 lane);
 *   - the -0x8000 of the bilinear taps is taken once a span (uf - 0x8000 + i*duf, wrapping, = (uf + i*duf) - 0x8000);
 *     the row stride is mask + 1, so row * stride is row * mask + row (one register fewer in the loop);
 *   - the shade clamp is vqshrun #16 (= vqmovun(vshr #16)), x/255 adds x >> 8 by vsra (= cm_div255; all 65,536
 *     inputs checked); the store's bytes come from one vtbl (B G R lanes, A from an all-ones register) in place of
 *     the vorr with 255, the vcombine copy and vmovn (every lane is <= 255, so its low byte is the value);
 *   - the VK combine K + trunc((S - K) * T / 255) in 16-bit lanes, where vk_div255_trunc works on 32-bit ones:
 *     sign(S - K) * div255(|S - K| * T), the product at most 255 * 255 (see LEAN_VK_LOOP);
 *   - the span setup: PSHADE's 1 / fspan from a table (the same floats, no divide); a span's u/w * w and PSHADE
 *     colour/w at its start carried from the last span's end (the same operations on the same values, see the
 *     loop); the PSHADE colours' sat_i32 as NEON vcvt from the same scalar VFP values (vcvt.s32.f32 is sat_i32 on the
 *     ARM: toward zero, saturating, NaN -> 0; checked on every float class), the vectors built in s0..s3 (lean_f4)
 *     instead of through the stack; the depth clamp as vmax/vmin and wrap_far's range test as one vcale (each the
 *     same value for every float; NaN gives 0 after the convert either way); the mip level and fraction from the
 *     bits on the core (lean_mip_lf, equal to the float code for all 2^32 inputs) -- eight FPSCR transfers a span
 *     fewer.
 * GE_LEANROW=0: hot_row and hot_row_vk as before; GE_LEANVK=0: hot_row_vk as before (the lean hot_row stays).
 * Bit-exact: port/hh/tools/rast_test.sh (ARM under qemu against the scalar loops, GE_HOTROW=0 and the old rows; random
 * triangles in every mode; each inline loop caught by a mutation run). */
static int sLeanRow SAVESTATE_EXCLUDE = -1; /* GE_LEANROW=0: hot_row / hot_row_vk as before */
static int sLeanVk SAVESTATE_EXCLUDE = -1;  /* GE_LEANVK=0: the lean rows for hot_row only (hot_row_vk as before) */
static int sLeanNoz SAVESTATE_EXCLUDE = -1; /* GE_LEANNOZ=0: hot_row's case without depth stays in raster_tri's spans */
static int sLeanBlend SAVESTATE_EXCLUDE = -1; /* GE_LEANBLEND=0: the blended texture x shade rows stay in raster_tri's spans */
static inline uint16x4_t lean_div255(uint16x4_t m) { /* cm_div255: (m + (m >> 8) + 1) >> 8 */
    return vshr_n_u16(vadd_u16(vsra_n_u16(m, m, 8), vdup_n_u16(1)), 8);
}
/* {lo, hi} in one d register by one core-to-NEON move (GCC otherwise took some pairs through the stack) */
static inline uint8x8_t lean_pair(uint32_t lo, uint32_t hi) {
    uint32x2_t r;
    __asm__("vmov %P0, %1, %2" : "=w"(r) : "r"(lo), "r"(hi));
    return vreinterpret_u8_u32(r);
}
/* the one-multiply lerp of a bilinear quad: X8 = {a, c}, Y8 = {b, d}; uf, vf biased by -0x8000 */
#define LEAN_LERP2(X8, Y8)                                                                              \
        const uint16x8_t Hq = vshrq_n_u16(vmlaq_u16(vshll_n_u8(X8, 8), vsubl_u8(Y8, X8),                \
                                                    vdupq_n_u16((uint16_t)((uf >> 8) & 0xff))), 8);      \
        T = vshr_n_u16(vmla_u16(vshl_n_u16(vget_low_u16(Hq), 8), vsub_u16(vget_high_u16(Hq), vget_low_u16(Hq)), \
                                vdup_n_u16((uint16_t)((vf >> 8) & 0xff))), 8);
/* the row stride is mu + 1 (a power of two: tw == tmask + 1 whenever the mask is set), so row * stride is
 * row * mu + row -- one register fewer in the loop */
#define LEAN_ROWS                                                                                       \
        const int r0_ = (vf >> 16) & mv, r1_ = ((vf >> 16) + 1) & mv;                                   \
        const int b0 = r0_ * mu + r0_, b1 = r1_ * mu + r1_;
#define LEAN_BIL32                                                                                      \
        const int iu0 = (uf >> 16) & mu, iu1 = ((uf >> 16) + 1) & mu;                                  \
        LEAN_ROWS                                                                                       \
        const uint8x8_t X8 = vreinterpret_u8_u32(vld1_lane_u32(p32 + b1 + iu0, vld1_dup_u32(p32 + b0 + iu0), 1)); \
        const uint8x8_t Y8 = vreinterpret_u8_u32(vld1_lane_u32(p32 + b1 + iu1, vld1_dup_u32(p32 + b0 + iu1), 1)); \
        LEAN_LERP2(X8, Y8)
#define LEAN_BIL16                                                                                      \
        const int iu0 = (uf >> 16) & mu, iu1 = ((uf >> 16) + 1) & mu;                                  \
        LEAN_ROWS                                                                                       \
        const uint32_t a = texel_at(texpx, true, b0 + iu0), b = texel_at(texpx, true, b0 + iu1);        \
        const uint32_t c = texel_at(texpx, true, b1 + iu0), d = texel_at(texpx, true, b1 + iu1);        \
        const uint8x8_t X8 = lean_pair(a, c), Y8 = lean_pair(b, d);                                     \
        LEAN_LERP2(X8, Y8)
#define LEAN_NEAR32                                                                                     \
        const int r_ = (vf >> 16) & mv;                                                                 \
        T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(p32 + r_ * mu + r_ + ((uf >> 16) & mu)))));
#define LEAN_NEAR16                                                                                     \
        const int r_ = (vf >> 16) & mv;                                                                 \
        T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(texel_at(texpx, true, r_ * mu + r_ + ((uf >> 16) & mu))))));
/* two levels' nearest texels, lerped by mfrac (lerp_argb: (a*(256 - f) + b*f) >> 8 per channel) */
#define LEAN_TRI_LERP(L8)                                                                               \
        const uint16x8_t Lq = vmovl_u8(L8);                                                             \
        T = vshr_n_u16(vmla_u16(vshl_n_u16(vget_low_u16(Lq), 8), vsub_u16(vget_high_u16(Lq), vget_low_u16(Lq)), vmf), 8);
#define LEAN_TRI32                                                                                      \
        const int iu = (uf >> 16) & mu, iv = (vf >> 16) & mv;                                           \
        LEAN_TRI_LERP(vreinterpret_u8_u32(vld1_lane_u32(p32b + (iv >> 1) * tw2 + (iu >> 1),             \
                                                        vld1_dup_u32(p32 + iv * mu + iv + iu), 1)))
/* GE_MIPLOD's two levels, each filtered (tribil_texel, as the span functions and the scalar loops): LEAN_TBIL32 the
 * 32-bit masked case in lanes -- level L's quad and level L+1's (half the coordinates, half the masks, stride tw / 2)
 * through LEAN_LERP2's lerps, then the TRILERP (T1 - T0) * f + T0 * 256 + 0x80 >> 8 in u16 lanes (the true value is
 * 0..65,408, so the wrapping subtract and multiply-add land on it); LEAN_TBIL any texture, on the core */
#define LEAN_TBIL32                                                                                     \
        {                                                                                               \
        uint16x4_t T0_, T1_;                                                                            \
        {   const int32_t uf_ = uf - 0x8000, vf_ = vf - 0x8000;                                         \
            const int iu0 = (uf_ >> 16) & mu, iu1 = ((uf_ >> 16) + 1) & mu;                            \
            const int r0_ = (vf_ >> 16) & mv, r1_ = ((vf_ >> 16) + 1) & mv;                            \
            const int b0 = r0_ * mu + r0_, b1 = r1_ * mu + r1_;                                         \
            const uint8x8_t X8 = vreinterpret_u8_u32(vld1_lane_u32(p32 + b1 + iu0, vld1_dup_u32(p32 + b0 + iu0), 1)); \
            const uint8x8_t Y8 = vreinterpret_u8_u32(vld1_lane_u32(p32 + b1 + iu1, vld1_dup_u32(p32 + b0 + iu1), 1)); \
            const int32_t uf = uf_, vf = vf_; uint16x4_t T; LEAN_LERP2(X8, Y8) T0_ = T; }               \
        {   const int32_t uf_ = (uf >> 1) - 0x8000, vf_ = (vf >> 1) - 0x8000;                           \
            const int mu2 = mu >> 1, mv2 = mv >> 1;                                                     \
            const int iu0 = (uf_ >> 16) & mu2, iu1 = ((uf_ >> 16) + 1) & mu2;                          \
            const int b0 = ((vf_ >> 16) & mv2) * tw2, b1 = (((vf_ >> 16) + 1) & mv2) * tw2;             \
            const uint8x8_t X8 = vreinterpret_u8_u32(vld1_lane_u32(p32b + b1 + iu0, vld1_dup_u32(p32b + b0 + iu0), 1)); \
            const uint8x8_t Y8 = vreinterpret_u8_u32(vld1_lane_u32(p32b + b1 + iu1, vld1_dup_u32(p32b + b0 + iu1), 1)); \
            const int32_t uf = uf_, vf = vf_; uint16x4_t T; LEAN_LERP2(X8, Y8) T1_ = T; }               \
        T = vshr_n_u16(vmla_u16(vadd_u16(vshl_n_u16(T0_, 8), vdup_n_u16(0x80)), vsub_u16(T1_, T0_), vmf), 8); \
        }
/* GE_TBILPAIR / GE_BILPAIR (2026-10-04): LEAN_TBIL32's two levels and LEAN_BIL32's one with fewer loads and less
 * address arithmetic. A quad's taps u0, u0 + 1 are adjacent in memory unless u0 is the level's last column (then
 * u0 + 1 wraps to column 0): there {a, b} and {c, d} come in one vld1 each, and one vtrn makes the {a, c}, {b, d}
 * pairs LEAN_LERP2 takes -- two loads and a permute for four single-lane loads; the last column takes the four loads
 * as before (column 0 for u0 + 1). The offsets in bytes, by shifts: the row stride is a power of two (tw = mask + 1,
 * 2^sl), so row * stride * 4 = ((v >> 16) & mv) << (sl + 2) = (v >> (14 - sl)) & (mv << (sl + 2)) for every int v
 * (the bits below sl + 2 of v >> (14 - sl) are fraction bits, masked off), one AND with a shifted operand; the next
 * row's from v + 0x10000 (wrapping: (v >> 16) + 1 and (v + 0x10000) >> 16 agree mod 2^16, so under the mask);
 * the column's (u >> 14) & (mu << 2). Level L + 1 is level L's half: stride 2^(sl - 1) (a level L at least 2 wide;
 * a 1-wide level takes LEAN_TBIL32). The same texels, the same lerps and TRILERP. */
#define LEAN_PAIRB(PB, RB0, RB1, CB, MU4, X8, Y8)                                                       \
        uint8x8_t X8, Y8;                                                                               \
        if (__builtin_expect((CB) != (MU4), 1)) {                                                       \
            const uint8_t *const pc_ = (PB) + (CB);                                                     \
            const uint32x2x2_t q_ = vtrn_u32(vld1_u32((const uint32_t *)(pc_ + (RB0))),                 \
                                             vld1_u32((const uint32_t *)(pc_ + (RB1))));                \
            X8 = vreinterpret_u8_u32(q_.val[0]); Y8 = vreinterpret_u8_u32(q_.val[1]);                   \
        } else {                                                                                        \
            X8 = vreinterpret_u8_u32(vld1_lane_u32((const uint32_t *)((PB) + (RB1) + (CB)),             \
                                                   vld1_dup_u32((const uint32_t *)((PB) + (RB0) + (CB))), 1)); \
            Y8 = vreinterpret_u8_u32(vld1_lane_u32((const uint32_t *)((PB) + (RB1)),                    \
                                                   vld1_dup_u32((const uint32_t *)((PB) + (RB0))), 1));  \
        }
/* the span's constants (sl = log2(mu + 1)): level L's and level L + 1's */
#define LEAN_PAIR_K                                                                                     \
        const int sl_ = 31 - __builtin_clz((unsigned)mu + 1u);                                          \
        const uint8_t *const q8 = (const uint8_t *)p32, *const q8b = (const uint8_t *)p32b;             \
        const int qk = 14 - sl_, qmu4 = mu << 2, qmv = mv << (sl_ + 2);                                 \
        const int qkb = 15 - sl_, qmu4b = (mu >> 1) << 2, qmvb = (mv >> 1) << (sl_ + 1);                \
        (void)q8b; (void)qkb; (void)qmu4b; (void)qmvb;
/* LEAN_BIL32 so (uf, vf biased by -0x8000 as there) */
#define LEAN_BIL32P                                                                                     \
        const int cb_ = (uf >> 14) & qmu4, rb0_ = (vf >> qk) & qmv, rb1_ = ((vf + 0x10000) >> qk) & qmv; \
        LEAN_PAIRB(q8, rb0_, rb1_, cb_, qmu4, X8, Y8)                                                   \
        LEAN_LERP2(X8, Y8)
/* LEAN_TBIL32 so */
#define LEAN_TBIL32P                                                                                    \
        {                                                                                               \
        uint16x4_t T0_, T1_;                                                                            \
        {   const int32_t uf_ = uf - 0x8000, vf_ = vf - 0x8000;                                         \
            const int cb_ = (uf_ >> 14) & qmu4, rb0_ = (vf_ >> qk) & qmv, rb1_ = ((vf_ + 0x10000) >> qk) & qmv; \
            LEAN_PAIRB(q8, rb0_, rb1_, cb_, qmu4, X8, Y8)                                               \
            const int32_t uf = uf_, vf = vf_; uint16x4_t T; LEAN_LERP2(X8, Y8) T0_ = T; }               \
        {   const int32_t uf_ = (uf >> 1) - 0x8000, vf_ = (vf >> 1) - 0x8000;                           \
            const int cb_ = (uf_ >> 14) & qmu4b, rb0_ = (vf_ >> qkb) & qmvb, rb1_ = ((vf_ + 0x10000) >> qkb) & qmvb; \
            LEAN_PAIRB(q8b, rb0_, rb1_, cb_, qmu4b, X8, Y8)                                             \
            const int32_t uf = uf_, vf = vf_; uint16x4_t T; LEAN_LERP2(X8, Y8) T1_ = T; }               \
        T = vshr_n_u16(vmla_u16(vadd_u16(vshl_n_u16(T0_, 8), vdup_n_u16(0x80)), vsub_u16(T1_, T0_), vmf), 8); \
        }
#define LEAN_TBIL                                                                                       \
        T = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(                                       \
                tribil_texel(texpx, texpx2, h->t16, tw, th, ws, wt, mu, mv, uf, vf, mfrac)))));
#define LEAN_TRI16                                                                                      \
        const int iu = (uf >> 16) & mu, iv = (vf >> 16) & mv;                                           \
        LEAN_TRI_LERP(lean_pair(texel_at(texpx, true, iv * mu + iv + iu), texel_at(texpx2, true, (iv >> 1) * tw2 + (iu >> 1))))
/* the pixel's bytes B G R A from its u16 lanes (each <= 255), A = 255: one table lookup in place of vorr a255,
 * the vcombine copy and vmovn -- byte 2k of the lanes for B G R, a byte of the all-ones register for A */
#define LEAN_STORE(M)                                                                                   \
        vst1_lane_u32(fe + i, vreinterpret_u32_u8(vtbl2_u8(lean_tb, lean_ix)), 0);
#define LEAN_TBL(M) const uint8x8x2_t lean_tb = { { vreinterpret_u8_u16(M), vdup_n_u8(0xFF) } };
#define LEAN_LOOP(ZT, ...)                                                                              \
        for (int i = -span; i < 0; i++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) {  \
            const int uz = clamp16_sh8(zq);                                                             \
            if (ZT && uz > ze[i]) continue;                                                             \
            uint16x4_t T;                                                                               \
            __VA_ARGS__                                                                                 \
            const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                \
            const uint16x4_t M = lean_div255(vmul_u16(T, S));                                           \
            LEAN_TBL(M) LEAN_STORE(M)                                                                   \
            if (ZT) ze[i] = (uint16_t)uz;                                                               \
        }
/* K + trunc((S - K) * T / 255) (vk_div255_trunc on 32-bit lanes) in 16-bit lanes: |S - K| * T <= 255 * 255 fits,
 * trunc(P / 255) is sign(P) * floor(|P| / 255) = sign * div255(|P|) (exact to 65,534), and the result lies between
 * K and S, so K + q and K - q never wrap */
#define LEAN_VK_LOOP(ZT, FETCH)                                                                         \
        for (int i = -span; i < 0; i++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc),    \
                                    kacc = vaddq_s32(kacc, dkacc)) {                                     \
            const int uz = clamp16_sh8(zq);                                                             \
            if (ZT && uz > ze[i]) continue;                                                             \
            uint16x4_t T;                                                                               \
            FETCH                                                                                       \
            const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                \
            const uint16x4_t K = vmin_u16(vqshrun_n_s32(kacc, 16), v255);                               \
            const uint16x4_t q = lean_div255(vmul_u16(vabd_u16(S, K), T));                              \
            const uint16x4_t F = vbsl_u16(vcge_u16(S, K), vadd_u16(K, q), vsub_u16(K, q));              \
            LEAN_TBL(F) LEAN_STORE(F)                                                                   \
            if (ZT) ze[i] = (uint16_t)uz;                                                               \
        }
/* The blended texture x shade rows (GE_LEANBLEND; cm_bilin_blend_span_neon's / cm_mip_blend_span_neon's pixel):
 * the alpha fa from the shade (FA 0), the texel (1) or their product (2); below amin nothing is drawn; under 255
 * div255(M * fa + D * (255 - fa)) per channel (each sum <= 255 * 255: 16-bit lanes). The bilinear taps are loaded on
 * the core, as there, for the four-alpha-0 early out (an alpha-0 quad lerps to texel alpha 0, so fa 0 < amin when
 * the texture supplies alpha: the out changes no pixel); the mip texels drop it (the lerped alpha is tested by
 * fa < amin itself, the same pixels). */
#define LEAN_BIL32S                                                                                     \
        const int iu0 = (uf >> 16) & mu, iu1 = ((uf >> 16) + 1) & mu;                                  \
        LEAN_ROWS                                                                                       \
        const uint32_t a = p32[b0 + iu0], b = p32[b0 + iu1], c = p32[b1 + iu0], d = p32[b1 + iu1];      \
        if (fa_src != 0 && !((a | b | c | d) >> 24)) continue;                                          \
        const uint8x8_t X8 = lean_pair(a, c), Y8 = lean_pair(b, d);                                     \
        LEAN_LERP2(X8, Y8)
/* LEAN_BIL32S on a texture whose span's taps all lie inside it (GE_LEANNP2, lean_np2_inside): no wrap, stride tws */
#define LEAN_BIL32N                                                                                     \
        const int iu0 = uf >> 16, b0 = (vf >> 16) * tws, b1 = b0 + tws;                                 \
        const uint32_t a = p32[b0 + iu0], b = p32[b0 + iu0 + 1], c = p32[b1 + iu0], d = p32[b1 + iu0 + 1]; \
        if (fa_src != 0 && !((a | b | c | d) >> 24)) continue;                                          \
        const uint8x8_t X8 = lean_pair(a, c), Y8 = lean_pair(b, d);                                     \
        LEAN_LERP2(X8, Y8)
#define LEAN_CMB_LOOP(FA, ...)                                                                          \
        for (int i = -span; i < 0; i++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) {  \
            const int uz = clamp16_sh8(zq);                                                             \
            if (uz > ze[i] + zbias) continue;                                                           \
            uint16x4_t T;                                                                               \
            const int fa_src = (FA); (void)fa_src;                                                      \
            __VA_ARGS__                                                                                 \
            const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                \
            uint16x4_t M = lean_div255(vmul_u16(T, S));                                                 \
            const unsigned fa = FA == 0 ? vget_lane_u16(S, 3) : FA == 1 ? vget_lane_u16(T, 3) : vget_lane_u16(M, 3); \
            if ((int)fa < amin) continue;                                                               \
            if (fa < 255) {                                                                             \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(fe + i))));   \
                M = lean_div255(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa)))); \
            }                                                                                           \
            LEAN_TBL(M) LEAN_STORE(M)                                                                   \
            if (dmask) ze[i] = (uint16_t)uz;                                                            \
        }
/* The blended K rows (VKA_SPAN_BODY's pixel, GE_LEANBLEND): F as LEAN_VK_LOOP (alpha lane included), fa the
 * texture's (FA 1: F's alpha) or the shade's (FA 0); below amin nothing is drawn; under 255 div255(F * fa + D *
 * (255 - fa)) per channel. */
#define LEAN_VKA_LOOP(ZT, FA, ...)                                                                      \
        for (int i = -span; i < 0; i++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc),    \
                                    kacc = vaddq_s32(kacc, dkacc)) {                                     \
            const int uz = clamp16_sh8(zq);                                                             \
            if (ZT && uz > ze[i] + zbias) continue;                                                     \
            uint16x4_t T;                                                                               \
            __VA_ARGS__                                                                                 \
            const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                \
            const uint16x4_t K = vmin_u16(vqshrun_n_s32(kacc, 16), v255);                               \
            const uint16x4_t q = lean_div255(vmul_u16(vabd_u16(S, K), T));                              \
            uint16x4_t F = vbsl_u16(vcge_u16(S, K), vadd_u16(K, q), vsub_u16(K, q));                    \
            const unsigned fa = FA ? vget_lane_u16(F, 3) : vget_lane_u16(S, 3);                         \
            if ((int)fa < amin) continue;                                                               \
            if (fa < 255) {                                                                             \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(fe + i))));   \
                F = lean_div255(vmla_u16(vmul_u16(F, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa)))); \
            }                                                                                           \
            LEAN_TBL(F) LEAN_STORE(F)                                                                   \
            if (ZT && dmask) ze[i] = (uint16_t)uz;                                                      \
        }
/* GE_TBILSPAN (2026-10-05; GE_MIPLOD=2 costs 3.1 ms on the Dam, most of it the two-level pixel loops): the lean rows'
 * two-level span (LEAN_TBIL32P in LEAN_LOOP, LEAN_VK_LOOP, LEAN_CMB_LOOP, LEAN_VKA_LOOP) as one loop (TBQ_LOOP) that
 * needs fewer core registers. Those loops kept ~20 values for 14 core registers: 12-15 stack loads and stores a pixel
 * (the eight level constants, the three steps, the loop count, even a tap's offset), ~96 instructions a pixel. Here:
 *   - u, v and depth step in one NEON vector ({uf - 0x8000, vf - 0x8000, zq, 0} += {duf, dvf, zdq, 0}: the same
 *     wrapping 32-bit adds as uf += duf etc.), u and v read out to the core by one vmov a pixel, depth by one more --
 *     the three steps leave the core;
 *   - level L's taps from uf, vf biased by -0x8000 once (as LEAN_BIL32P: (uf0 - 0x8000) + i duf is (uf0 + i duf) -
 *     0x8000 mod 2^32); level L + 1's from the same biased b = f - 0x8000: LEAN_TBIL32P's (f >> 1) - 0x8000 is
 *     (b - 0x8000) >> 1 exactly, so its column ((f >> 1) - 0x8000) >> 14 is (b - 0x8000) >> 15, its fraction byte
 *     (b - 0x8000) >> 9, its rows ((f >> 1) - 0x8000) >> qkb and ((f >> 1) + 0x8000) >> qkb are (b - 0x8000) and
 *     (b + 0x18000) >> (qkb + 1), qkb + 1 = qk + 2 (floor(floor(x / 2) / 2^k) = floor(x / 2^(k + 1))). Where b - 0x8000
 *     or b + 0x18000 wraps (f within 0x10000 of the int32 ends) the 32-bit value is off by 2^32: after the shift by
 *     15, 9 or qk + 2 <= 16 + sl that is a multiple of 2^17, 2^23 or 2^(16 + sl), and each result is masked to fewer
 *     bits (qmu4b < 2^13, 0xff, qmvb < 2^(sl + 11)): the same taps;
 *   - a quad's two pair loads go straight into the {a, c}, {b, d} lanes LEAN_LERP2 takes (vld2.32 lane loads: a, b
 *     of row 0 into lane 0 of X and Y, c, d of row 1 into lane 1), no vtrn; on the last column the four single loads
 *     into the same register pair (tbq_ld2, tbq_ld4: in asm, GCC copied each intrinsic load's pair).
 * The same texels, the same lerps, TRILERP, combine, alpha test, blend and depth as the inline loops (rast_test).
 * GE_TBILSPAN=0: LEAN_TBIL32P's loops. GE_BILSPAN (the same loop for LEAN_BIL32P's one-level bilinear spans of the
 * opaque and K rows, TBQ_FETCH1): GE_BILSPAN=0: LEAN_BIL32P's loops. */
static int sTbilSpan SAVESTATE_EXCLUDE = -1;
static int sBilSpan SAVESTATE_EXCLUDE = -1;
static int sClampSpan SAVESTATE_EXCLUDE = -1; /* GE_CLAMPSPAN (see TBQ_LOOPC) */
/* {a, b} at p0 and {c, d} at p1 (two adjacent texels of two rows) as {a, c} in the low half, {b, d} in the high half */
static inline __attribute__((always_inline)) uint8x16_t tbq_ld2(const uint32_t *p0, const uint32_t *p1) {
    uint8x16_t q;
    __asm__("vld2.32 {%e0[], %f0[]}, [%1]\n\tvld2.32 {%e0[1], %f0[1]}, [%2]"
            : "=&w"(q) : "r"(p0), "r"(p1), "m"(*(const uint32_t (*)[2])p0), "m"(*(const uint32_t (*)[2])p1));
    return q;
}
/* {a, c} low, {b, d} high from four texels (a quad on the last column) */
static inline __attribute__((always_inline)) uint8x16_t tbq_ld4(const uint32_t *a, const uint32_t *c, const uint32_t *b,
                                                                const uint32_t *d) {
    uint8x16_t q;
    __asm__("vld1.32 {%e0[0]}, [%1]\n\tvld1.32 {%e0[1]}, [%2]\n\tvld1.32 {%f0[0]}, [%3]\n\tvld1.32 {%f0[1]}, [%4]"
            : "=&w"(q) : "r"(a), "r"(c), "r"(b), "r"(d), "m"(*a), "m"(*c), "m"(*b), "m"(*d));
    return q;
}
/* uf, vf (lanes 0, 1) to the core in one transfer */
static inline __attribute__((always_inline)) void tbq_uv(int32x4_t uvz, int32_t *uf, int32_t *vf) {
    int32_t a, b;
    __asm__("vmov %0, %1, %e2" : "=r"(a), "=r"(b) : "w"(uvz));
    *uf = a; *vf = b;
}
/* a quad of 32-bit texels: X8 = {a, c}, Y8 = {b, d} (as LEAN_PAIRB) */
#define TBQ_PAIRB(PB, RB0, RB1, CB, MU4, X8, Y8)                                                        \
        uint8x16_t q_;                                                                                  \
        if (__builtin_expect((CB) != (MU4), 1)) {                                                       \
            const uint8_t *const pc_ = (PB) + (CB);                                                     \
            q_ = tbq_ld2((const uint32_t *)(pc_ + (RB0)), (const uint32_t *)(pc_ + (RB1)));             \
        } else { /* the last column: u0 + 1 is column 0 */                                              \
            q_ = tbq_ld4((const uint32_t *)((PB) + (RB0) + (CB)), (const uint32_t *)((PB) + (RB1) + (CB)), \
                         (const uint32_t *)((PB) + (RB0)), (const uint32_t *)((PB) + (RB1)));           \
        }                                                                                               \
        const uint8x8_t X8 = vget_low_u8(q_), Y8 = vget_high_u8(q_);
/* LEAN_LERP2 with the two weights given */
#define TBQ_LERP2(X8, Y8, FU, FV)                                                                       \
        const uint16x8_t Hq = vshrq_n_u16(vmlaq_u16(vshll_n_u8(X8, 8), vsubl_u8(Y8, X8),                \
                                                    vdupq_n_u16((uint16_t)(FU))), 8);                    \
        T = vshr_n_u16(vmla_u16(vshl_n_u16(vget_low_u16(Hq), 8), vsub_u16(vget_high_u16(Hq), vget_low_u16(Hq)), \
                                vdup_n_u16((uint16_t)(FV))), 8);
/* LEAN_BIL32P's texel (uf, vf biased) */
#define TBQ_FETCH1()                                                                                    \
        {   const int cb_ = (uf >> 14) & qmu4, rb0_ = (vf >> qk) & qmv, rb1_ = ((vf + 0x10000) >> qk) & qmv; \
            TBQ_PAIRB(q8, rb0_, rb1_, cb_, qmu4, X8, Y8)                                                \
            TBQ_LERP2(X8, Y8, (uf >> 8) & 0xff, (vf >> 8) & 0xff) }
/* LEAN_TBIL32P's texel from the biased uf, vf (above) */
#define TBQ_FETCH()                                                                                     \
        {                                                                                               \
        uint16x4_t T0_, T1_;                                                                            \
        {   const int cb_ = (uf >> 14) & qmu4, rb0_ = (vf >> qk) & qmv, rb1_ = ((vf + 0x10000) >> qk) & qmv; \
            TBQ_PAIRB(q8, rb0_, rb1_, cb_, qmu4, X8, Y8)                                                \
            uint16x4_t T; TBQ_LERP2(X8, Y8, (uf >> 8) & 0xff, (vf >> 8) & 0xff) T0_ = T; }             \
        {   const int32_t ub_ = uf - 0x8000, vb_ = vf - 0x8000;                                         \
            const int cb_ = (ub_ >> 15) & qmu4b, rb0_ = (vb_ >> qk2) & qmvb, rb1_ = ((vf + 0x18000) >> qk2) & qmvb; \
            TBQ_PAIRB(q8b, rb0_, rb1_, cb_, qmu4b, X8, Y8)                                              \
            uint16x4_t T; TBQ_LERP2(X8, Y8, (ub_ >> 9) & 0xff, (vb_ >> 9) & 0xff) T1_ = T; }           \
        T = vshr_n_u16(vmla_u16(vadd_u16(vshl_n_u16(T0_, 8), vdup_n_u16(0x80)), vsub_u16(T1_, T0_), vmf), 8); \
        }
/* GE_CLAMPSPAN's two-level texel: level L from uf, vf as TBQ_FETCH1, level L + 1 from its own biased coordinates u1,
 * v1 with LEAN_TBIL32P's formulas (both clamped, TBQ_LOOP) */
#define TBQ_FETCHC()                                                                                    \
        {                                                                                               \
        uint16x4_t T0_, T1_;                                                                            \
        {   const int cb_ = (uf >> 14) & qmu4, rb0_ = (vf >> qk) & qmv, rb1_ = ((vf + 0x10000) >> qk) & qmv; \
            TBQ_PAIRB(q8, rb0_, rb1_, cb_, qmu4, X8, Y8)                                                \
            uint16x4_t T; TBQ_LERP2(X8, Y8, (uf >> 8) & 0xff, (vf >> 8) & 0xff) T0_ = T; }             \
        {   const int cb_ = (u1 >> 14) & qmu4b, rb0_ = (v1 >> qkb) & qmvb, rb1_ = ((v1 + 0x10000) >> qkb) & qmvb; \
            TBQ_PAIRB(q8b, rb0_, rb1_, cb_, qmu4b, X8, Y8)                                              \
            uint16x4_t T; TBQ_LERP2(X8, Y8, (u1 >> 8) & 0xff, (v1 >> 8) & 0xff) T1_ = T; }             \
        T = vshr_n_u16(vmla_u16(vadd_u16(vshl_n_u16(T0_, 8), vdup_n_u16(0x80)), vsub_u16(T1_, T0_), vmf), 8); \
        }
/* {lane 0, lane 1} of a d register to the core in one transfer */
static inline __attribute__((always_inline)) void tbq_uv2(int32x2_t uv, int32_t *uf, int32_t *vf) {
    int32_t a, b;
    __asm__("vmov %0, %1, %P2" : "=r"(a), "=r"(b) : "w"(uv));
    *uf = a; *vf = b;
}
/* The pixel loop. KIND 0: texture x shade (LEAN_LOOP), 1: K + (S - K) * T (LEAN_VK_LOOP), 2: blended texture x shade
 * (LEAN_CMB_LOOP, always z-tested), 3: blended K rows (LEAN_VKA_LOOP); ZT the depth test; FA the blend's alpha source
 * (KIND 2, 3). In scope: uvz, duvz, acc, dacc, kacc, dkacc, the level constants (LEAN_PAIR_K, qk2), vmf, v255, lean_ix,
 * zbias, amin, dmask (KIND 0, 1: zbias 0 and dmask true, as the lean opaque rows are only taken then). */
#define TBQ_LOOP(KIND, ZT, FA, FETCH) TBQ_LOOPC(KIND, ZT, FA, FETCH, 0, 0)
/* GE_CLAMPSPAN (2026-10-05; Aztec storage spent 12% of its instructions in the span functions): a texture clamped on an
 * axis took the lean loops only for a span whose taps all lie inside the image (lean_span_in, GE_LEANCLAMP); every
 * other span -- a wall's edge texels, the rows near a clamped top or bottom, more of them at the small levels
 * GE_MIPLOD's two-level spans read -- went to cm_bilin_span_neon / cm_mip_span_neon / vk_* (wrapf on every tap, a call
 * with 15-20 arguments). Here those spans run TBQ_LOOPC with CL: each pixel's coordinate clamped before the taps (the
 * same wrapping 32-bit steps; uvz holds the unbiased uf, vf). On a clamped axis wrapf reads taps c0 = clamp(t) and
 * c1 = clamp(t + 1) of t = b >> 16 (b the biased coordinate) and lerps them by f = (b >> 8) & 0xff; clamping b itself
 * to [0, (size - 1) << 16] gives the same texel:
 *   - b inside: unchanged;
 *   - b < 0 (t <= -1): wrapf reads row 0 twice, (x 256 + (x - x) f) >> 8 = x for any f; b = 0 reads rows 0 and 1 with
 *     f = 0: (x 256 + (y - x) 0) >> 8 = x (the wrapping u16 difference times 0 is 0);
 *   - b >= (size - 1) << 16: wrapf reads size - 1 twice; the clamped b = (size - 1) << 16 has f = 0 and reads size - 1
 *     and the masked next one (size & mask = 0: the last-column loads, any texel of the image) with weight 0.
 * The axes are independent (the horizontal lerp reads u alone, the vertical v alone). Level L + 1 (TRI) clamps its own
 * biased coordinate (f >> 1) - 0x8000 (tribil_texel's, computed so from the unbiased f, no wrap) to [0, (size / 2 - 1)
 * << 16] and reads it with LEAN_TBIL32P's formulas (TBQ_FETCHC). A repeating axis is not clamped (bounds INT32_MIN,
 * INT32_MAX): its taps are LEAN_TBIL32P's / LEAN_BIL32P's masked ones, as the span functions' wrapf with a mask. The
 * spans: power-of-two levels, 32-bit texels, the lean rows' other conditions (clampok); bilinear in the opaque and K
 * rows (the blended texture x shade rows keep cm_bilin_blend_span_neon: their loop drops a quad of four alpha-0 taps
 * before the depth write), two-level in all four. Out of line (tbqc_span, tbqc_*): inline, the extra loops cost the
 * repeating rows a few registers (+0.3% on the Dam). They extend GE_BILSPAN's and GE_TBILSPAN's loops: the bilinear
 * spans need GE_BILSPAN, the two-level ones GE_TBILSPAN (and the pair loads, GE_BILPAIR / GE_TBILPAIR). GE_CLAMPSPAN=0:
 * the span functions. */
#define TBQ_LOOPC(KIND, ZT, FA, FETCH, CL, TRI)                                                         \
    for (int i = -span; i < 0; i++, uvz = vaddq_s32(uvz, duvz), acc = vaddq_s32(acc, dacc),             \
                                    kacc = (KIND) & 1 ? vaddq_s32(kacc, dkacc) : kacc) {                 \
        const int uz = clamp16_sh8(vgetq_lane_s32(uvz, 2));                                             \
        if (((KIND) == 2 || (ZT)) && uz > ze[i] + zbias) continue;                                      \
        int32_t uf, vf, u1 = 0, v1 = 0; (void)u1; (void)v1;                                             \
        if (CL) {                                                                                       \
            const int32x2_t o_ = vget_low_s32(uvz);                                                     \
            tbq_uv2(vmin_s32(vmax_s32(vsub_s32(o_, vdup_n_s32(0x8000)), clo0), chi0), &uf, &vf);        \
            if (TRI) tbq_uv2(vmin_s32(vmax_s32(vsub_s32(vshr_n_s32(o_, 1), vdup_n_s32(0x8000)), clo1), chi1), &u1, &v1); \
        } else tbq_uv(uvz, &uf, &vf);                                                                   \
        uint16x4_t T;                                                                                   \
        FETCH()                                                                                         \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M;                                                                                   \
        if ((KIND) & 1) {                                                                               \
            const uint16x4_t K = vmin_u16(vqshrun_n_s32(kacc, 16), v255);                               \
            const uint16x4_t q = lean_div255(vmul_u16(vabd_u16(S, K), T));                              \
            M = vbsl_u16(vcge_u16(S, K), vadd_u16(K, q), vsub_u16(K, q));                               \
        } else {                                                                                        \
            M = lean_div255(vmul_u16(T, S));                                                            \
        }                                                                                               \
        if ((KIND) >= 2) {                                                                              \
            const unsigned fa = (KIND) == 3 ? ((FA) ? vget_lane_u16(M, 3) : vget_lane_u16(S, 3))         \
                              : (FA) == 0 ? vget_lane_u16(S, 3) : (FA) == 1 ? vget_lane_u16(T, 3) : vget_lane_u16(M, 3); \
            if ((int)fa < amin) continue;                                                               \
            if (fa < 255) {                                                                             \
                const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(fe + i))));   \
                M = lean_div255(vmla_u16(vmul_u16(M, vdup_n_u16((uint16_t)fa)), D, vdup_n_u16((uint16_t)(255 - fa)))); \
            }                                                                                           \
        }                                                                                               \
        LEAN_TBL(M) LEAN_STORE(M)                                                                       \
        if ((ZT) && dmask) ze[i] = (uint16_t)uz;                                                        \
    }
/* GE_CLAMPSPAN's loops, out of line (a clamped span is the rarer case: inline, the extra loops cost the repeating
 * rows' code a few registers); the depth test and alpha source at run time */
typedef struct {
    const uint8_t *q8, *q8b;
    int qk, qkb, qmu4, qmu4b, qmv, qmvb;
    uint32_t mf;
    int zbias, amin, zt, fa;
    bool dmask;
    int32_t clo0[2], chi0[2], clo1[2], chi1[2];
    int32_t kacc[4], dkacc[4];
} TbqC;
#define TBQC_FN(NAME, KIND, FETCH, TRI)                                                                 \
static void __attribute__((noinline)) NAME(const TbqC *c, uint32_t *fe, uint16_t *ze, int span, int32x4_t uvz, \
                                           int32x4_t duvz, int32x4_t acc, int32x4_t dacc) {             \
    const uint8_t *const q8 = c->q8, *const q8b = c->q8b; (void)q8b;                                    \
    const int qk = c->qk, qkb = c->qkb, qmu4 = c->qmu4, qmu4b = c->qmu4b, qmv = c->qmv, qmvb = c->qmvb; \
    (void)qkb; (void)qmu4b; (void)qmvb;                                                                 \
    const uint16x4_t vmf = vdup_n_u16((uint16_t)c->mf), v255 = vdup_n_u16(255); (void)vmf;            \
    const uint8x8_t lean_ix = vcreate_u8(0x0f0f0f0f0f040200ull);                                        \
    const int zbias = (KIND) >= 2 ? c->zbias : 0, amin = (KIND) >= 2 ? c->amin : 0;                     \
    const bool dmask = (KIND) >= 2 ? c->dmask : true; (void)amin; (void)dmask;                          \
    const int zt = c->zt, fa_ = c->fa; (void)fa_;                                                       \
    const int32x2_t clo0 = vld1_s32(c->clo0), chi0 = vld1_s32(c->chi0);                                 \
    const int32x2_t clo1 = vld1_s32(c->clo1), chi1 = vld1_s32(c->chi1); (void)clo1; (void)chi1;        \
    int32x4_t kacc = vdupq_n_s32(0), dkacc = kacc;                                                      \
    if ((KIND) & 1) { kacc = vld1q_s32(c->kacc); dkacc = vld1q_s32(c->dkacc); }                         \
    TBQ_LOOPC(KIND, zt, fa_, FETCH, 1, TRI)                                                             \
}
TBQC_FN(tbqc_cm_bil, 0, TBQ_FETCH1, 0)
TBQC_FN(tbqc_cm_tri, 0, TBQ_FETCHC, 1)
TBQC_FN(tbqc_vk_bil, 1, TBQ_FETCH1, 0)
TBQC_FN(tbqc_vk_tri, 1, TBQ_FETCHC, 1)
TBQC_FN(tbqc_cmb_tri, 2, TBQ_FETCHC, 1)
TBQC_FN(tbqc_vka_bil, 3, TBQ_FETCH1, 0)
TBQC_FN(tbqc_vka_tri, 3, TBQ_FETCHC, 1)
#undef TBQC_FN
/* GE_CLAMPSPAN's span: the level constants and the clamp bounds, then the loop (out of line, see above) */
typedef struct {
    const void *texpx, *texpx2;
    int mu, mv, kind, span;
    uint32_t mfrac;
    bool bil;
    uint32_t *fe;
    uint16_t *ze;
    int32_t uvz[4], duvz[4], acc[4], dacc[4], kacc[4], dkacc[4];
} TbqS;
static void __attribute__((noinline)) tbqc_span(const HotTri *h, const TbqS *sp) {
    const int mu = sp->mu, mv = sp->mv, ws = h->ws, wt = h->wt;
    const uint32_t *const p32 = (const uint32_t *)sp->texpx, *const p32b = (const uint32_t *)sp->texpx2;
    LEAN_PAIR_K
    TbqC c;
    c.q8 = q8; c.q8b = q8b; c.qk = qk; c.qkb = qkb; c.qmu4 = qmu4; c.qmu4b = qmu4b; c.qmv = qmv; c.qmvb = qmvb;
    c.mf = sp->mfrac; c.zbias = h->zbias; c.amin = h->amin; c.dmask = h->dmask; c.zt = h->ztest;
    c.fa = sp->kind == 2 ? (!h->a_tex ? 0 : !h->a_in ? 1 : 2) : h->a_tex;
    /* level L: [0, (size - 1) << 16] on a clamped axis; level L + 1: [0, (size / 2 - 1) << 16]; none on a repeating one */
    c.clo0[0] = c.clo1[0] = ws == WRAP_REPEAT ? INT32_MIN : 0; c.clo0[1] = c.clo1[1] = wt == WRAP_REPEAT ? INT32_MIN : 0;
    c.chi0[0] = ws == WRAP_REPEAT ? INT32_MAX : mu << 16; c.chi0[1] = wt == WRAP_REPEAT ? INT32_MAX : mv << 16;
    c.chi1[0] = ws == WRAP_REPEAT ? INT32_MAX : (mu >> 1) << 16; c.chi1[1] = wt == WRAP_REPEAT ? INT32_MAX : (mv >> 1) << 16;
    memcpy(c.kacc, sp->kacc, sizeof c.kacc); memcpy(c.dkacc, sp->dkacc, sizeof c.dkacc);
    const int32x4_t uvz = vld1q_s32(sp->uvz), duvz = vld1q_s32(sp->duvz), acc = vld1q_s32(sp->acc), dacc = vld1q_s32(sp->dacc);
    uint32_t *const fe = sp->fe; uint16_t *const ze = sp->ze; const int span = sp->span;
    switch (sp->kind) {
    case 2: tbqc_cmb_tri(&c, fe, ze, span, uvz, duvz, acc, dacc); break;
    case 3: if (sp->bil) tbqc_vka_bil(&c, fe, ze, span, uvz, duvz, acc, dacc); else tbqc_vka_tri(&c, fe, ze, span, uvz, duvz, acc, dacc); break;
    case 1: if (sp->bil) tbqc_vk_bil(&c, fe, ze, span, uvz, duvz, acc, dacc); else tbqc_vk_tri(&c, fe, ze, span, uvz, duvz, acc, dacc); break;
    default: if (sp->bil) tbqc_cm_bil(&c, fe, ze, span, uvz, duvz, acc, dacc); else tbqc_cm_tri(&c, fe, ze, span, uvz, duvz, acc, dacc); break;
    }
}
/* hot_row_body's span setup with the exact rewrites above, then the inline loops (or the old span functions) */
/* Four VFP results as one NEON vector without the stack: the values are computed into s0..s3, which are q0 (GCC
 * otherwise stored them and reloaded the vector: a store-to-load stall a span). The empty asm only declares that
 * q0 now holds them. */
static inline __attribute__((always_inline)) float32x4_t lean_f4(float a, float b, float c, float d) {
    register float r0 __asm__("s0") = a;
    register float r1 __asm__("s1") = b;
    register float r2 __asm__("s2") = c;
    register float r3 __asm__("s3") = d;
    register float32x4_t q __asm__("q0");
    __asm__("" : "=w"(q) : "t"(r0), "t"(r1), "t"(r2), "t"(r3));
    return q;
}
/* GE_LODLEAN's pick (see n64_lod_tiles_lean) */
static inline __attribute__((always_inline)) void mip_pick_lean(const SoftTex *tex, int tw_l0, int th_l0, float ftw,
                                                                float fth, float du, float dv, float u_a, float v_a,
                                                                float uwdy, float vwdy, float swdy, float rw, float rw_b,
                                                                float fspan, const MipSet *ms, int *lvl, uint32_t *fr,
                                                                bool *tri) {
    const float dux = du * ftw, dvx = dv * fth;
    const float u_b = u_a + du * fspan, v_b = v_a + dv * fspan;
    const float duy = ((uwdy - u_a * swdy) * rw + (uwdy - u_b * swdy) * rw_b) * 0.5f * ftw;
    const float dvy = ((vwdy - v_a * swdy) * rw + (vwdy - v_b * swdy) * rw_b) * 0.5f * fth;
    const uint32x4_t m4 = vbicq_u32(vreinterpretq_u32_f32(lean_f4(dux, dvx, duy, dvy)), vdupq_n_u32(0x80000000u));
    const uint32x2_t axy = vpmax_u32(vget_low_u32(m4), vget_high_u32(m4)); /* {ax, ay} */
    const uint32_t mx = vget_lane_u32(vpmax_u32(axy, axy), 0);
    if (__builtin_expect(mx > 0x7f800000u, 0)) { /* a NaN: the compares' own order decides */
        mip_pick_n64(tex, tw_l0, th_l0, du, dv, u_a, v_a, uwdy, vwdy, swdy, rw, rw_b, fspan, lvl, fr, tri);
        return;
    }
    const int mode = ms->mode;
    union { uint32_t u; float f; } m = { mode == 3 ? vget_lane_u32(vpmin_u32(axy, axy), 0) : mx };
    float rho = m.f * ms->lin;
    if (mode == 2) rho = rho * 0.70710678f;
    const int32_t lod = lod_fbits(rho) < 0x44000000u ? (int32_t)(rho * 32.0f) : 0x7fff;
    const int top = tex->nlev - 1;
    int t1, t2;
    uint32_t f;
    n64_lod_tiles_lean(lod, ms->maxlev, &t1, &t2, &f);
    if (t1 > top) t1 = top;
    if (t2 > top) t2 = top;
    if (!ms->trilin && t2 != t1) {
        if (f < 64u) t2 = t1;
        else if (f >= 192u) t1 = t2;
        else f = (f - 64u) * 2u;
    }
    *lvl = t1;
    *fr = f;
    *tri = t2 != t1 && f != 0u && t1 < ms->far;
}
/* 1.0f / n for a span of n pixels: the same float the divide gives (constant-folded, correctly rounded) */
static const float kLeanRcp[PERSP_SPAN + 1] = { 0.0f, 1.0f / 1, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6,
                                                1.0f / 7, 1.0f / 8, 1.0f / 9, 1.0f / 10, 1.0f / 11, 1.0f / 12,
                                                1.0f / 13, 1.0f / 14, 1.0f / 15, 1.0f / 16 };
/* (x > 1e-12f) ? 1.0f / x : 0.0f without the FPSCR transfer: the VFP divide always (for x <= 1e-12f it is thrown
 * away), the choice by a NEON compare and select. NEON flushes a denormal x to 0, which is not > 1e-12f either, as
 * the VFP's denormal is not; NaN fails both. The kept quotient is the VFP's own. */
static inline __attribute__((always_inline)) float lean_rcp_w(float x) {
    const float r = 1.0f / x;
    return vget_lane_f32(vbsl_f32(vcgt_f32(vdup_n_f32(x), vdup_n_f32(1e-12f)), vdup_n_f32(r), vdup_n_f32(0.0f)), 0);
}
/* GE_LEANNP2 (2026-10-02, the explosions): every bilinear tap of span pixels 0..n-1 -- at u = (uf + i duf - 0x8000) >> 16
 * and u + 1, v likewise, i < n, as the span functions compute them -- inside [0, tw - 1] x [0, th - 1]? The end points
 * in 32 bits with overflow checks (a span whose steps would wrap is not taken); between them u and v are monotonic. */
static int sLeanNp2 SAVESTATE_EXCLUDE = -1; /* GE_LEANNP2=0: cm_bilin_blend_span_neon for every non-power-of-two span */
static inline __attribute__((always_inline)) bool lean_np2_inside(int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n,
                                                                  int tw, int th) {
    int32_t ua, ub, va, vb, du, dv;
    if (__builtin_sub_overflow(uf, 0x8000, &ua) || __builtin_sub_overflow(vf, 0x8000, &va) ||
        __builtin_mul_overflow(duf, n - 1, &du) || __builtin_mul_overflow(dvf, n - 1, &dv) ||
        __builtin_add_overflow(ua, du, &ub) || __builtin_add_overflow(va, dv, &vb))
        return false;
    const int ulo = (ua < ub ? ua : ub) >> 16, uhi = ((ua < ub ? ub : ua) >> 16) + 1;
    const int vlo = (va < vb ? va : vb) >> 16, vhi = ((va < vb ? vb : va) >> 16) + 1;
    return ulo >= 0 && uhi < tw && vlo >= 0 && vhi < th;
}
/* GE_LEANCLAMP (2026-10-04, the final speed sweep): the lean rows' loops above were taken only for textures that repeat
 * on both axes; a clamped texture's spans went to the span functions (cm_*_span_neon, vk_*_span_neon: a call with 15-20
 * arguments a span, wrapf with its mode tests on every tap) -- the Aztec's walls (repeat across, clamped up and down),
 * the Jungle's fronds and the Frigate's grates (clamped both ways): 5-11% of those frames. On a power-of-two level the
 * loops read a tap at c & (size - 1); wrapf clamps c to [0, size - 1]. For a coordinate inside the level both are c.
 * So a span may take the loops when each clamped axis's taps lie inside for the whole span (lean_axis_in): the same
 * texels, lerps, combine, blend and z as the span functions (proven against them in rast_test since 2026-10-01), no new
 * loop. The taps an axis reads, as the loops compute them (f = uf or vf at pixel i: f0 + i df, no 32-bit overflow,
 * so monotonic between the span's ends):
 *   nearest and two-level nearest (LEAN_NEAR32, LEAN_TRI32): f >> 16 at level L, and (f >> 16) >> 1 at L + 1, which is
 *     inside L + 1 whenever f >> 16 is inside L;
 *   bilinear (LEAN_BIL32, the pair loads, LEAN_BIL32S): (f - 0x8000) >> 16 and + 1 at level L;
 *   GE_MIPLOD's two levels (LEAN_TBIL32, the pair loads): those, and ((f >> 1) - 0x8000) >> 16 and + 1 at L + 1,
 *     (size >> 1) wide (tribil_texel / tribil_n32: level L + 1 is half of L, its mask halved with it).
 * A pair load's second tap is never the last column's wrap here (inside: the first tap is <= size - 2). The
 * non-power-of-two blended sprites (GE_LEANNP2) are tested on both axes by lean_np2_inside, which holds for clamp as
 * for repeat, so they need no repeat either. GE_LEANCLAMP=0: repeating textures only, as before. */
static int sLeanClamp SAVESTATE_EXCLUDE = -1;
static inline __attribute__((always_inline)) bool lean_axis_in(int32_t f, int32_t df, int n, int size, int filt) {
    int32_t d, e;
    if (__builtin_mul_overflow(df, n - 1, &d) || __builtin_add_overflow(f, d, &e)) return false;
    const int32_t lo = f < e ? f : e, hi = f < e ? e : f; /* size <= 2048: the bounds below fit */
    if (filt == 0) return lo >= 0 && hi < (size << 16);                      /* f >> 16 in [0, size - 1] */
    if (lo < 0x8000 || hi >= ((size - 1) << 16) + 0x8000) return false;      /* (f - 0x8000) >> 16 and + 1 */
    if (filt == 1) return true;
    return lo >= 0x10000 && (size >> 1) > 0 && (hi >> 1) < (((size >> 1) - 1) << 16) + 0x8000; /* the same at L + 1 */
}
/* GE_LEANCLAMP's span test: each clamped axis inside (a repeating axis needs nothing: the loops' mask is its wrap).
 * filt: the taps the loop reads -- 0 nearest / two-level nearest, 1 bilinear, 2 both levels bilinear. Out of line: the
 * repeating textures' rows (most of every frame) keep their code as it was. */
static bool __attribute__((noinline)) lean_span_in(int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n, int tw, int th,
                                                   int ws, int wt, int filt) {
    return (ws == WRAP_REPEAT || lean_axis_in(uf, duf, n, tw, filt)) && (wt == WRAP_REPEAT || lean_axis_in(vf, dvf, n, th, filt));
}
/* GE_LEANASM (2026-10-03, the final exact speed check: the explosions): GE_LEANNP2's span for the explosions' and
 * the smoke's alpha source (texture x shade on colour and alpha, FA 2) as one hand-scheduled ARM/NEON loop. Compiled
 * from C, LEAN_CMB_LOOP(2, LEAN_BIL32N) inside hot_row_lean_body kept ~17 values for 14 core registers: six stack
 * reloads a pixel (zbias, zdq, amin, dmask, duf, dvf), the 255 and the store's table rebuilt, three register copies,
 * four tap loads and two core-to-NEON pairs. Here every invariant stays in a register: u and v step as one NEON pair
 * ({uf, vf} + {duf, dvf}; the taps' column and row leave it by one vmov), the two rows of taps are two 64-bit loads
 * ({a, b}, {c, d}: the span's taps lie inside the texture, lean_np2_inside) put in {a, c}, {b, d} order by one vtrn,
 * the four-alpha-0 early out tests their OR on the core, and the store takes its bytes by a one-register vtbl and
 * an OR with A = 255. The pixel is LEAN_CMB_LOOP's, operation for operation: the z test (clamp16_sh8, zbias), the
 * early out, the one-multiply lerp (u then v, 16-bit lanes), S = min(vqshrun(acc, 16), 255), M = div255(T * S),
 * fa = M's alpha, the amin reject, the blend div255(M * fa + D * (255 - fa)) under 255, the store, the z write under
 * dmask; zq, u, v and the colour step every pixel, drawn or not. Default OFF since the t6 device Bench (2026-10-03:
 * explosions 52.05 ms with it, 51.87 ms with the C loop, both runs); GE_LEANASM=1 turns it on. Bit-exact:
 * port/hh/tools/rast_test.sh (variant noasm; ARM against the C loop, the scalar loops and x86). */
typedef struct {
    uint32_t *fe;          /*  0: the span's end in the colour buffer */
    uint16_t *ze;          /*  4: and in the depth buffer */
    int32_t i2;            /*  8: -2 * span (the byte offset of the first pixel in ze) */
    int32_t zq, zdq;       /* 12, 16 */
    int32_t zbias;         /* 20 */
    const uint32_t *p32;   /* 24: the level's texels */
    int32_t tws4;          /* 28: its row stride in bytes */
    int32_t amin, dmask;   /* 32, 36 */
    int32_t uf, vf;        /* 40, 44: biased by -0x8000 */
    int32_t duf, dvf;      /* 48, 52 */
    int32_t acc[4], dacc[4]; /* 56, 72: b g r a, 16.16 */
} LeanAsmRow;
void ge_lean_np2_cmb2(const LeanAsmRow *a) __attribute__((visibility("hidden")));
__asm__(
    "	.text\n"
    "	.align 2\n"
    "	.syntax unified\n"
    "	.arm\n"
    "	.fpu neon\n"
    "	.type ge_lean_np2_cmb2, %function\n"
    /* the pixel loop; flat: S in d3, no colour step */
    "	.macro ge_np2px flat\n"
    "0:	usat r10, #16, r3, asr #8\n"           /* uz = clamp16_sh8(zq) */
    "	ldrh lr, [r1, r0]\n"
    "	add lr, lr, r5\n"
    "	cmp r10, lr\n"
    "	bgt 2f\n"                               /* uz > ze[i] + zbias */
    "	vshr.s32 d0, d20, #16\n"
    "	vmov r11, r12, d0\n"                    /* uf >> 16, vf >> 16 */
    "	mla r12, r12, r7, r6\n"
    "	add r12, r12, r11, lsl #2\n"            /* &p32[(vf >> 16) * tws + (uf >> 16)] */
    "	vld1.32 {d0}, [r12], r7\n"              /* {a, b} */
    "	vld1.32 {d1}, [r12]\n"                  /* {c, d} */
    "	vorr d2, d0, d1\n"
    "	vmov r10, r11, d2\n"
    "	orr r10, r10, r11\n"
    "	tst r10, #0xff000000\n"
    "	beq 2f\n"                               /* four alpha-0 taps */
    "	vdup.8 d4, d20[1]\n"                    /* (uf >> 8) & 0xff */
    "	vdup.8 d5, d20[5]\n"                    /* (vf >> 8) & 0xff */
    "	vtrn.32 d0, d1\n"                       /* d0 {a, c} = X8, d1 {b, d} = Y8 */
    "	vmovl.u8 q3, d4\n"
    "	vmovl.u8 q14, d5\n"
    "	vshll.u8 q15, d0, #8\n"
    "	vsubl.u8 q13, d1, d0\n"
    "	vmla.i16 q15, q13, q3\n"
    "	vshr.u16 q15, q15, #8\n"                /* Hq */
    "	vshl.i16 d26, d30, #8\n"
    "	vsub.i16 d27, d31, d30\n"
    "	vmla.i16 d26, d27, d28\n"
    "	vshr.u16 d26, d26, #8\n"                /* T */
    "	.if \\flat\n"
    "	vmul.i16 d26, d26, d3\n"
    "	.else\n"
    "	vqshrun.s32 d27, q8, #16\n"
    "	vmin.u16 d27, d27, d22\n"               /* S */
    "	vmul.i16 d26, d26, d27\n"
    "	.endif\n"
    "	vsra.u16 d26, d26, #8\n"
    "	vadd.i16 d26, d26, d23\n"
    "	vshr.u16 d26, d26, #8\n"                /* M = div255(T * S) */
    "	vmov.u16 r10, d26[3]\n"                 /* fa */
    "	add r11, r2, r0, lsl #1\n"              /* &fe[i] */
    "	cmp r10, r8\n"
    "	blt 2f\n"                               /* fa < amin */
    "	cmp r10, #255\n"
    "	bge 3f\n"
    "	vld1.32 {d0[]}, [r11]\n"                /* D's bytes */
    "	vdup.16 d1, r10\n"
    "	rsb r10, r10, #255\n"
    "	vdup.8 d2, r10\n"
    "	vmul.i16 d26, d26, d1\n"
    "	vmlal.u8 q13, d0, d2\n"                 /* lanes 0..3: M * fa + D * (255 - fa) */
    "	vsra.u16 d26, d26, #8\n"
    "	vadd.i16 d26, d26, d23\n"
    "	vshr.u16 d26, d26, #8\n"
    "3:	vtbl.8 d0, {d26}, d24\n"
    "	vorr d0, d0, d25\n"
    "	vst1.32 {d0[0]}, [r11]\n"
    "	cmp r9, #0\n"
    "	usatne r10, #16, r3, asr #8\n"
    "	strhne r10, [r1, r0]\n"
    "2:	add r3, r3, r4\n"
    "	vadd.i32 d20, d20, d21\n"
    "	.if \\flat == 0\n"
    "	vadd.i32 q8, q8, q9\n"
    "	.endif\n"
    "	adds r0, r0, #2\n"
    "	bne 0b\n"
    "	.endm\n"
    "ge_lean_np2_cmb2:\n"
    "	push {r4-r11, lr}\n"
    "	add r12, r0, #40\n"
    "	vld1.32 {d20-d21}, [r12]!\n"            /* d20 {uf, vf}, d21 {duf, dvf} */
    "	vld1.32 {d16-d19}, [r12]\n"             /* q8 acc, q9 dacc */
    "	ldr r2, [r0, #0]\n"                     /* fe end */
    "	ldr r1, [r0, #4]\n"                     /* ze end */
    "	ldr r3, [r0, #12]\n"                    /* zq */
    "	ldr r4, [r0, #16]\n"                    /* zdq */
    "	ldr r5, [r0, #20]\n"                    /* zbias */
    "	ldr r6, [r0, #24]\n"                    /* p32 */
    "	ldr r7, [r0, #28]\n"                    /* tws * 4 */
    "	ldr r8, [r0, #32]\n"                    /* amin */
    "	ldr r9, [r0, #36]\n"                    /* dmask */
    "	ldr r0, [r0, #8]\n"                     /* i2 = -2 * span */
    "	vmov.i16 d22, #255\n"
    "	vmov.i16 d23, #1\n"
    "	vmov.i32 d25, #0xff000000\n"           /* A = 255 */
    "	movw r10, #0x0200\n"
    "	movt r10, #0x0f04\n"
    "	movw lr, #0x0f0f\n"
    "	movt lr, #0x0f0f\n"
    "	vmov d24, r10, lr\n"                    /* bytes 0 2 4 of the lanes: B G R; index 15: 0 */
    "	cmp r0, #0\n"
    "	beq 9f\n"
    /* dacc all zero (a flat triangle: explosion.c's quads): S is the same every pixel and acc never moves -- S once */
    "	vorr d0, d18, d19\n"
    "	vmov r10, r11, d0\n"
    "	orrs r10, r10, r11\n"
    "	bne 5f\n"
    "	vqshrun.s32 d27, q8, #16\n"
    "	vmin.u16 d3, d27, d22\n"
    "1:	ge_np2px 1\n"
    "	b 9f\n"
    "5:	ge_np2px 0\n"
    "9:	pop {r4-r11, pc}\n"
    "	.size ge_lean_np2_cmb2, .-ge_lean_np2_cmb2\n");
static int sLeanAsm SAVESTATE_EXCLUDE = -1; /* default (and GE_LEANASM=0): LEAN_CMB_LOOP(2, LEAN_BIL32N) as compiled */
/* GE_TBILSPAN / GE_BILSPAN: the span's level constants (LEAN_PAIR_K) and TBQ_LOOP. UB: uf, vf's bias (0x8000 for the
 * two-level spans; 0 for the bilinear ones, biased already) */
#define TBQ_GO(KIND, ZT, FA, FETCH, UB)                                                                 \
        {   LEAN_PAIR_K                                                                                 \
            const int qk2 = qk + 2; (void)qk2;                                                          \
            const int zbias_ = (KIND) >= 2 ? h->zbias : 0, amin_ = (KIND) >= 2 ? h->amin : 0;           \
            const bool dmask_ = (KIND) >= 2 ? h->dmask : true;                                          \
            int32x4_t uvz = { uf - (UB), vf - (UB), zq, 0 };                                            \
            const int32x2_t clo0 = vdup_n_s32(0), chi0 = clo0, clo1 = clo0, chi1 = clo0; /* unused (CL 0) */ \
            const int32x4_t duvz = { duf, dvf, zdq, 0 };                                                \
            { const int zbias = zbias_, amin = amin_; const bool dmask = dmask_; (void)amin; (void)dmask; \
              TBQ_LOOP(KIND, ZT, FA, FETCH) } }
/* GE_SPANVEC (2026-10-07, the cross-port speed round; SM64_SPANVEC, the SM64 port 03b1f32..e914b1a; default 1): the lean
 * rows' span set-up with the row's float state as NEON vectors -- P = { u/w, v/w, 1/w, z }, C = { b, g, r, a } (the
 * shade, in the pixel's byte order) and, in the K rows, K = { b, g, r, a } -- each stepped to the next span's start
 * by one multiply-add (P + dP * n: vmla rounds the product, then the sum, as the VFP's multiply and add do), where
 * the scalar set-up kept nine to thirteen floats and their steps (GCC spilled them around the mip pick; the whole
 * set-up was ~295 instructions a span on the Dam, ~43% of hot_row_lean). The same operations in the
 * same order, lane by lane: the perspective ends (u/w, v/w) * (1/w) as one two-lane multiply, the PSHADE colours
 * c * (1/w) at both ends and (end - start) * (1 / n) as four-lane multiplies and a subtract; the conversions to
 * 16.16 as vcvt #16 (x * 2^16 exactly, then the same truncation and saturation as vcvt of the VFP's x * 65536.0f,
 * a product that is exact; a denormal x gives 0 either way); the divides (1/w and a short span's du, dv) stay VFP
 * divides. NEON flushes denormals where the VFP keeps them: no value here comes near one (texture coordinates,
 * 1/w, colours 0..255 and their products with 1/w, depth and their per-pixel steps are each 0 or far above 1e-30).
 * The mip pick, the depth and the pixel loops are untouched. GE_SPANVEC=0: the scalar set-up. Same pixels.
 * GE_ZSKIP (2026-10-07; SM64_ZSKIP; default 1): a depth-tested span whose nearest depth is further than the
 * farthest depth stored under it (plus the decal window) fails the depth test at every pixel, so it writes nothing:
 * it is skipped before its set-up (perspective, mip pick, conversions) and its pixel loop. The depth is linear
 * along the span in 24.8 (zq + i * zdq, the loops' own values), clamp16_sh8 is monotonic, so the nearest pixel is
 * one of the span's ends (when zq + (span - 1) zdq stays in int32: otherwise no skip); every pixel loop here tests
 * uz > ze[i] + zbias (the opaque lean loops only run with zbias 0). A sufficient test, never a guess: every other
 * span is drawn as before. The next span starts from the stepped state as a row's first span does (u/w * 1/w at
 * its start: the very floats the carried ends were, the same operations on the same values; its 1/w as the skipped
 * span's end 1/w, computed when a span is drawn). A triangle stops testing after four misses running (speed only).
 * 15-30% of the depth-tested lean spans of the bench views are wholly behind (Dam 21%). GE_ZSKIP=0: off. Same
 * pixels. (SM64's round had three more: SHADEPX -- one { b, g, r, a } 16.16 vector a pixel -- and WRAPRR -- a texture
 * repeating on both axes wrapped by the AND alone -- are how the lean loops already work; NEONNOTO's case, the texel
 * alone without a depth test, draws no span here: BUILDLOG 2026-10-07.) */
static int sSpanVec SAVESTATE_EXCLUDE = -1; /* GE_SPANVEC */
static int sZSkip SAVESTATE_EXCLUDE = -1;   /* GE_ZSKIP */
static inline __attribute__((always_inline)) void hot_row_lean_body(const HotTri *h, const int kind, const bool nl, const int PV,
                                                                    int idx, int x, int xe,
                                                                    float zf, float cr, float cg, float cb, float ca,
                                                                    float uw, float vw, float sw, float rw, float ckr,
                                                                    float ckg, float ckb, float cka) {
    const bool vk = kind == 1, blend = kind == 2; /* 0: texture x shade, 1: K + (S - K) * T, 2: texture x shade blended */
    const int ws = h->ws, wt = h->wt;
    const uint16x4_t v255 = vdup_n_u16(255);
    const uint8x8_t lean_ix = vcreate_u8(0x0f0f0f0f0f040200ull); /* B G R from lanes 0..2, A from the 0xFF register */
    /* the inline loops: both axes repeating (+ the masks, per span); the opaque rows with no decal bias and z written
     * when tested (or untested: the sky's K layers, GE_LEANNOZ's rows), the blended ones with any bias and z write;
     * the VK rows 32-bit with the texel's own colour (no rgb_texa), the blended texture x shade rows 32-bit */
    const bool fast = ws == WRAP_REPEAT && wt == WRAP_REPEAT &&
                      (blend ? !h->t16 : vk ? !h->texa && !h->t16 && (h->alpha ? sLeanBlend > 0 : !h->ztest || (h->zbias == 0 && h->dmask))
                                            : !h->ztest || (h->zbias == 0 && h->dmask));
    /* GE_LEANCLAMP: fast's conditions but the wrap (a span's clamped axes then need their taps inside: lean_axis_in) */
    const bool clampok = sLeanClamp > 0 && !fast &&
                         (blend ? !h->t16 : vk ? !h->texa && !h->t16 && (h->alpha ? sLeanBlend > 0 : !h->ztest || (h->zbias == 0 && h->dmask))
                                            : !h->ztest || (h->zbias == 0 && h->dmask));
    /* the CM rows with and without the depth test (GE_LEANNOZ: the no-depth ones -- the first-person gun and hands,
     * the Frigate's sea -- come here only when every span is fast, see raster_tri_body) */
#define LEAN_LOOP_Z(...) if (h->ztest) { LEAN_LOOP(1, __VA_ARGS__) } else { LEAN_LOOP(0, __VA_ARGS__) }
    int32x4_t dacc = { h->bdq, h->gdq, h->rdq, h->adq };
    int32x4_t dkacc = { h->kdq_b, h->kdq_g, h->kdq_r, h->kdq_a };
    bool first = true;
    MipSet mset; /* GE_LODLEAN: the pick's switches, once a row */
    mip_set_get(&mset);
    const bool lodlean = nl && h->lodlean;
    float ua_n = 0.0f, va_n = 0.0f;                                         /* the next span's u_a, v_a */
    float an_r = 0.0f, an_g = 0.0f, an_b = 0.0f, an_a = 0.0f;               /* and PSHADE colour/w */
    float kn_r = 0.0f, kn_g = 0.0f, kn_b = 0.0f, kn_a = 0.0f;               /* and K/w */
    /* GE_SPANVEC's state (PV): P = { u/w, v/w, 1/w, z }, C and K = { b, g, r, a }; the carried ends uvn, cn, kn */
    float32x4_t P = { uw, vw, sw, zf }, C = { cb, cg, cr, ca }, K = { ckb, ckg, ckr, cka };
    float32x2_t uvn = vdup_n_f32(0.0f);
    float32x4_t cn = vdupq_n_f32(0.0f), kn = cn;
    bool rw_stale = false; /* GE_ZSKIP: rw is the skipped spans' end 1/w, not computed yet */
    while (x < xe) {
        int span = xe - x;
        if (span > PERSP_SPAN) span = PERSP_SPAN;
        const float fspan = (float)span;
        float32x4_t Pn = P, Cn = C, Kn = K; /* PV: the next span's start */
        if (PV) {
            const float32x4_t fs4 = vdupq_n_f32(fspan);
            Pn = vmlaq_f32(P, h->dp, fs4); Cn = vmlaq_f32(C, h->dc, fs4);
            if (vk) Kn = vmlaq_f32(K, h->dk, fs4);
            zf = vgetq_lane_f32(P, 3);
        }
        /* (int32_t)(clamp(zf, -1, 2) * (65535 * 256)): the clamp as vmax/vmin, the same value but for NaN (then both 0) */
        int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(vmin_f32(vmax_f32(vdup_n_f32(zf), vdup_n_f32(-1.0f)),
                                                                     vdup_n_f32(2.0f)), 65535.0f * 256.0f)), 0);
        if (span_z_out(zf)) zq = span_zq_far(zf, h->zdx, span, h->zdq); /* GE_NEARW (span_zq_far) */
        if (h->zsk > 0) { /* GE_ZSKIP (above): h->zsk > 0 only for depth-tested rows */
            const int64_t zl = (int64_t)zq + (int64_t)(span - 1) * h->zdq;
            const uint16_t *const zp = h->zb + idx;
            int zmax;
            if (span >= 8) {
                const uint16x8_t m8 = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8));
                uint16x4_t m4 = vmax_u16(vget_low_u16(m8), vget_high_u16(m8));
                m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);
                zmax = vget_lane_u16(m4, 0);
            } else if (span >= 4) {
                uint16x4_t m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));
                m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);
                zmax = vget_lane_u16(m4, 0);
            } else {
                zmax = zp[0];
                for (int i = 1; i < span; i++) if (zp[i] > zmax) zmax = zp[i];
            }
            const int z0 = clamp16_sh8(zq), z1 = clamp16_sh8((int32_t)zl);
            if (zl == (int32_t)zl && (z0 < z1 ? z0 : z1) > zmax + h->zbias) {
                ((HotTri *)h)->zsk = 4;
                idx += span;
                if (PV) { P = Pn; C = Cn; K = Kn; }
                else {
                    zf += h->zdx * fspan;
                    cr += h->crdx * fspan; cg += h->cgdx * fspan; cb += h->cbdx * fspan; ca += h->cadx * fspan;
                    if (vk) { ckr += h->ckrdx * fspan; ckg += h->ckgdx * fspan; ckb += h->ckbdx * fspan; cka += h->ckadx * fspan; }
                    uw += h->uwdx * fspan;
                    vw += h->vwdx * fspan;
                    sw = sw + h->swdx * fspan;
                }
                rw_stale = h->persp;
                first = true;
                x += span;
                continue;
            }
            ((HotTri *)h)->zsk--; /* the triangle's spans in front: after four misses running it stops testing */
        }
        if (rw_stale) { rw = lean_rcp_w(PV ? vgetq_lane_f32(P, 2) : sw); rw_stale = false; }
        const float sw_b = PV ? vgetq_lane_f32(Pn, 2) : sw + h->swdx * fspan;
        float rw_b = 0.0f;
        float ckr_s = ckr, ckg_s = ckg, ckb_s = ckb, cka_s = cka;
        float u_a, v_a, du, dv;
        if (PV && h->persp) {
            rw_b = lean_rcp_w(sw_b);
            const float32x2_t uva = first ? vmul_n_f32(vget_low_f32(P), rw) : uvn; /* as below, two lanes */
            uvn = vmul_n_f32(vget_low_f32(Pn), rw_b);
            const float32x2_t nuv = vsub_f32(uvn, uva);
            u_a = vget_lane_f32(uva, 0); v_a = vget_lane_f32(uva, 1);
            if (span == PERSP_SPAN) { const float32x2_t d2 = vmul_n_f32(nuv, 1.0f / PERSP_SPAN); du = vget_lane_f32(d2, 0); dv = vget_lane_f32(d2, 1); }
            else                    { du = vget_lane_f32(nuv, 0) / fspan; dv = vget_lane_f32(nuv, 1) / fspan; }
        } else if (PV) {
            u_a = vgetq_lane_f32(P, 0); v_a = vgetq_lane_f32(P, 1); du = h->uwdx; dv = h->vwdx;
        } else if (h->persp) {
            rw_b = lean_rcp_w(sw_b);
            /* a span's u/w * w at its start is the last span's at its end: (uw + uwdx * n) * rw_b there, with uw
             * then stepped by the same uwdx * n and rw set to rw_b -- the same operations on the same values */
            if (first) { u_a = uw * rw; v_a = vw * rw; } else { u_a = ua_n; v_a = va_n; }
            ua_n = (uw + h->uwdx * fspan) * rw_b; va_n = (vw + h->vwdx * fspan) * rw_b;
            const float nu = ua_n - u_a;
            const float nv = va_n - v_a;
            if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); }
            else                    { du = nu / fspan;               dv = nv / fspan; }
        } else {
            u_a = uw; v_a = vw; du = h->uwdx; dv = h->vwdx;
        }
        const void *texpx = h->texpx, *texpx2 = NULL;
        int tw = h->tw, th = h->th, tmask_u = h->tmask_u, tmask_v = h->tmask_v, tw2 = 1;
        float su = h->su, sv = h->sv;
        bool mtri = false, use_bilin = h->bilin_l0;
        uint32_t mfrac = 0;
        int mlvl = 0; /* the span's level (span_clear_mip) */
        if (h->mip) { /* raster_tri's mip block, line for line (as hot_row_body) */
            const SoftTex *const tex = h->tex;
            const int tw_l0 = h->tw_l0, th_l0 = h->th_l0;
            const float dux = du * (float)tw_l0, dvx = dv * (float)th_l0;
            const float duy = (h->uwdy - u_a * h->swdy) * rw * (float)tw_l0, dvy = (h->vwdy - v_a * h->swdy) * rw * (float)th_l0;
            const float rx = dux * dux + dvx * dvx, ry = duy * duy + dvy * dvy;
            union { float f; uint32_t u; } rb;
            rb.f = (rx > ry ? rx : ry) * sMipScale;
            int lvl;
            uint32_t fr;
            bool ntri = false;
            if (nl) {
                if (lodlean)
                    mip_pick_lean(tex, tw_l0, th_l0, h->ftw_l0, h->fth_l0, du, dv, u_a, v_a, h->uwdy, h->vwdy, h->swdy, rw, rw_b,
                                  fspan, &mset, &lvl, &fr, &ntri);
                else if (h->lodfast)
                    mip_pick_fast(tex, tw_l0, th_l0, h->ftw_l0, h->fth_l0, du, dv, u_a, v_a, h->uwdy, h->vwdy, h->swdy, rw, rw_b,
                                  fspan, &lvl, &fr, &ntri);
                else
                    mip_pick_n64(tex, tw_l0, th_l0, du, dv, u_a, v_a, h->uwdy, h->vwdy, h->swdy, rw, rw_b, fspan, &lvl, &fr, &ntri);
            } else {
            lean_mip_lf(rb.u, &lvl, &fr);
            if (lvl >= tex->nlev - 1) { lvl = tex->nlev - 1; fr = 0; }
            if (!sTrilin && fr >= 192u) { lvl++; fr = 0; }
            }
            mlvl = lvl;
            texpx = tex->lev[lvl];
            tw = tw_l0 >> lvl; th = th_l0 >> lvl;
            {
                union { uint32_t u; float f; } inv = { (uint32_t)(127 - lvl) << 23 };
                su = (float)tex->w * inv.f; sv = (float)tex->h * inv.f;
            }
            tmask_u = h->tmu_l0 >= 0 ? tw - 1 : -1; tmask_v = h->tmv_l0 >= 0 ? th - 1 : -1;
            mtri = nl ? ntri : sTrilin ? fr > 0u : fr > 64u;
            if (mtri) {
                texpx2 = tex->lev[lvl + 1];
                tw2 = tw >> 1;
                mfrac = (nl || sTrilin) ? fr : (fr - 64u) * 2u;
            }
            use_bilin = h->bilin_l0 && (lvl == 0 || (nl && lvl < sMipFar)) && !mtri; /* GE_MIPFAR */
        }
        const float fu0 = u_a * su, fdu = du * su, fv0 = v_a * sv, fdv = dv * sv;
        float32x4_t fv4 = lean_f4(fu0, fdu, fv0, fdv);
        /* wrap_far's range test on all four at once: |x| <= 16000 (x <= 16000 && x >= -16000 for every float; NaN
         * goes to wrap_far, as before; wrap_far returns an in-range x as it is) -- one NEON compare and one transfer
         * instead of four FPSCR transfers */
        {
            const uint32x4_t inr = vcaleq_f32(fv4, vdupq_n_f32(16000.0f));
            const uint32x2_t in2 = vand_u32(vget_low_u32(inr), vget_high_u32(inr));
            if (vget_lane_u32(vpmin_u32(in2, in2), 0) != ~0u)
                fv4 = lean_f4(wrap_far(fu0, tw, ws), fdu, wrap_far(fv0, th, wt), fdv);
        }
        const float32x4_t fc4 = vminq_f32(vmaxq_f32(fv4, vdupq_n_f32(-16000.0f)), vdupq_n_f32(16000.0f));
        const int32x4_t fx = PV ? vcvtq_n_s32_f32(fc4, 16) : vcvtq_s32_f32(vmulq_n_f32(fc4, 65536.0f));
        int32_t uf = vgetq_lane_s32(fx, 0), duf = vgetq_lane_s32(fx, 1), vf = vgetq_lane_s32(fx, 2), dvf = vgetq_lane_s32(fx, 3);
        int32x4_t acc;
        int32x4_t kacc = vdupq_n_s32(0);
        if (PV) {
            if (h->pshade) { /* colour/w at both ends (the start carried, as u_a), (end - start) * (1 / n) */
                const float is = kLeanRcp[span];
                const float32x4_t ca4 = first ? vmulq_n_f32(C, rw) : cn;
                cn = vmulq_n_f32(Cn, rw_b);
                acc = vcvtq_n_s32_f32(ca4, 16);
                dacc = vcvtq_n_s32_f32(vmulq_n_f32(vsubq_f32(cn, ca4), is), 16);
                if (vk) {
                    const float32x4_t ka4 = first ? vmulq_n_f32(K, rw) : kn;
                    kn = vmulq_n_f32(Kn, rw_b);
                    kacc = vcvtq_n_s32_f32(ka4, 16);
                    dkacc = vcvtq_n_s32_f32(vmulq_n_f32(vsubq_f32(kn, ka4), is), 16);
                }
            } else {
                acc = vcvtq_n_s32_f32(C, 16);
                if (vk) kacc = vcvtq_n_s32_f32(K, 16);
            }
        } else if (h->pshade) {
            /* 1/fspan from the table (the same float); colour/w at the start is the last span's at its end (as u_a) */
            const float is = kLeanRcp[span];
            float ar, ag, ab, aa;
            if (first) { ar = cr * rw; ag = cg * rw; ab = cb * rw; aa = ca * rw; }
            else       { ar = an_r; ag = an_g; ab = an_b; aa = an_a; }
            an_r = (cr + h->crdx * fspan) * rw_b; an_g = (cg + h->cgdx * fspan) * rw_b;
            an_b = (cb + h->cbdx * fspan) * rw_b; an_a = (ca + h->cadx * fspan) * rw_b;
            acc = vcvtq_s32_f32(lean_f4(ab * 65536.0f, ag * 65536.0f, ar * 65536.0f, aa * 65536.0f));
            dacc = vcvtq_s32_f32(lean_f4((an_b - ab) * is * 65536.0f, (an_g - ag) * is * 65536.0f,
                                         (an_r - ar) * is * 65536.0f, (an_a - aa) * is * 65536.0f));
            if (vk) {
                float akr, akg, akb, aka;
                if (first) { akr = ckr * rw; akg = ckg * rw; akb = ckb * rw; aka = cka * rw; }
                else       { akr = kn_r; akg = kn_g; akb = kn_b; aka = kn_a; }
                kn_r = (ckr + h->ckrdx * fspan) * rw_b; kn_g = (ckg + h->ckgdx * fspan) * rw_b;
                kn_b = (ckb + h->ckbdx * fspan) * rw_b; kn_a = (cka + h->ckadx * fspan) * rw_b;
                ckr_s = akr; ckg_s = akg; ckb_s = akb; cka_s = aka;
                dkacc = vcvtq_s32_f32(lean_f4((kn_b - akb) * is * 65536.0f, (kn_g - akg) * is * 65536.0f,
                                              (kn_r - akr) * is * 65536.0f, (kn_a - aka) * is * 65536.0f));
            }
        } else {
            acc = vcvtq_s32_f32(lean_f4(cb * 65536.0f, cg * 65536.0f, cr * 65536.0f, ca * 65536.0f));
        }
        if (!PV && vk) kacc = vcvtq_s32_f32(lean_f4(ckb_s * 65536.0f, ckg_s * 65536.0f, ckr_s * 65536.0f, cka_s * 65536.0f));
        first = false;
        if (blend && h->arows && ((h->mip && (nl && h->tbil) && (mlvl > 0 || mtri))
                                      ? span_clear_mipb(h->arows, uf, vf, duf, dvf, span, ws, wt, tw, th, mlvl, mtri)
                                  : (use_bilin || !h->mip) ? span_clear(h->arows, uf, vf, duf, dvf, span, ws, wt)
                                                        : span_clear_mip(h->arows, uf, vf, duf, dvf, span, ws, wt, tw, th, mlvl, mtri))) {
#if defined(MIYOO)
            SPANSKIP_ADD(span); /* every tap alpha 0: nothing drawn (raster_tri) */
#else
            gfx_soft_spanskip_spans++; gfx_soft_spanskip_px += (uint32_t)span; /* every tap alpha 0: nothing drawn (raster_tri) */
#endif
        } else if (tmask_u >= 0 && tmask_v >= 0 &&
                   (__builtin_expect(fast, 1) || (clampok && lean_span_in(uf, vf, duf, dvf, span, tw, th, ws, wt,
                                                                         use_bilin ? 1 : (mtri && nl && h->tbil) ? 2 : 0)))) {
            const int mu = tmask_u, mv = tmask_v;
            const int32_t zdq = h->zdq;
            uint32_t *const fe = h->fbuf + idx + span;
            uint16_t *const ze = h->zb + idx + span;
            const uint32_t *const p32 = (const uint32_t *)texpx, *const p32b = (const uint32_t *)texpx2;
            /* GE_TBILPAIR (a level L at least 2 wide), GE_BILPAIR: LEAN_TBIL32P, LEAN_BIL32P */
            const bool tpair = h->tbilpair && mu > 0, bpair = h->bilpair; (void)tpair; (void)bpair;
            if (blend) {
                const int zbias = h->zbias, amin = h->amin; const bool dmask = h->dmask;
                const int fsrc = !h->a_tex ? 0 : !h->a_in ? 1 : 2; /* fa: the shade's, the texel's or the product's alpha */
                if (use_bilin) {
                    uf -= 0x8000; vf -= 0x8000;
                    if (fsrc == 0) { LEAN_CMB_LOOP(0, LEAN_BIL32S) } else if (fsrc == 1) { LEAN_CMB_LOOP(1, LEAN_BIL32S) }
                    else { LEAN_CMB_LOOP(2, LEAN_BIL32S) }
                } else if (mtri && (nl && h->tbil)) {
                    const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                    if (tpair && sTbilSpan > 0) { /* GE_TBILSPAN */
                        if (fsrc == 0) TBQ_GO(2, 1, 0, TBQ_FETCH, 0x8000) else if (fsrc == 1) TBQ_GO(2, 1, 1, TBQ_FETCH, 0x8000) else TBQ_GO(2, 1, 2, TBQ_FETCH, 0x8000)
                    } else if (tpair) { LEAN_PAIR_K
                        if (fsrc == 0) { LEAN_CMB_LOOP(0, LEAN_TBIL32P) } else if (fsrc == 1) { LEAN_CMB_LOOP(1, LEAN_TBIL32P) }
                        else { LEAN_CMB_LOOP(2, LEAN_TBIL32P) }
                    } else {
                        if (fsrc == 0) { LEAN_CMB_LOOP(0, LEAN_TBIL32) } else if (fsrc == 1) { LEAN_CMB_LOOP(1, LEAN_TBIL32) }
                        else { LEAN_CMB_LOOP(2, LEAN_TBIL32) }
                    }
                } else if (mtri) {
                    const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                    if (fsrc == 0) { LEAN_CMB_LOOP(0, LEAN_TRI32) } else if (fsrc == 1) { LEAN_CMB_LOOP(1, LEAN_TRI32) }
                    else { LEAN_CMB_LOOP(2, LEAN_TRI32) }
                } else {
                    if (fsrc == 0) { LEAN_CMB_LOOP(0, LEAN_NEAR32) } else if (fsrc == 1) { LEAN_CMB_LOOP(1, LEAN_NEAR32) }
                    else { LEAN_CMB_LOOP(2, LEAN_NEAR32) }
                }
            } else if (vk && h->alpha) { /* blended K rows: the Jungle's foliage (GE_LEANBLEND) */
                const int zbias = h->zbias, amin = h->amin; const bool dmask = h->dmask;
#define LEAN_VKA_Z(FA, ...) if (h->ztest) { LEAN_VKA_LOOP(1, FA, __VA_ARGS__) } else { LEAN_VKA_LOOP(0, FA, __VA_ARGS__) }
#define LEAN_VKA_F(...) if (h->a_tex) { LEAN_VKA_Z(1, __VA_ARGS__) } else { LEAN_VKA_Z(0, __VA_ARGS__) }
                if (use_bilin) {
                    uf -= 0x8000; vf -= 0x8000;
                    if (bpair && sBilSpan > 0) { /* GE_BILSPAN */
                        if (h->ztest) { if (h->a_tex) TBQ_GO(3, 1, 1, TBQ_FETCH1, 0) else TBQ_GO(3, 1, 0, TBQ_FETCH1, 0) }
                        else { if (h->a_tex) TBQ_GO(3, 0, 1, TBQ_FETCH1, 0) else TBQ_GO(3, 0, 0, TBQ_FETCH1, 0) }
                    } else if (bpair) { LEAN_PAIR_K LEAN_VKA_F(LEAN_BIL32P) } else { LEAN_VKA_F(LEAN_BIL32) }
                } else if (mtri && (nl && h->tbil)) {
                    const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                    if (tpair && sTbilSpan > 0) { /* GE_TBILSPAN */
                        if (h->ztest) { if (h->a_tex) TBQ_GO(3, 1, 1, TBQ_FETCH, 0x8000) else TBQ_GO(3, 1, 0, TBQ_FETCH, 0x8000) }
                        else { if (h->a_tex) TBQ_GO(3, 0, 1, TBQ_FETCH, 0x8000) else TBQ_GO(3, 0, 0, TBQ_FETCH, 0x8000) }
                    } else if (tpair) { LEAN_PAIR_K LEAN_VKA_F(LEAN_TBIL32P) } else { LEAN_VKA_F(LEAN_TBIL32) }
                } else if (mtri) {
                    const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                    LEAN_VKA_F(LEAN_TRI32)
                } else {
                    LEAN_VKA_F(LEAN_NEAR32)
                }
#undef LEAN_VKA_F
#undef LEAN_VKA_Z
            } else if (vk) {
                if (use_bilin) {
                    uf -= 0x8000; vf -= 0x8000;
                    if (bpair && sBilSpan > 0) { if (h->ztest) TBQ_GO(1, 1, 0, TBQ_FETCH1, 0) else TBQ_GO(1, 0, 0, TBQ_FETCH1, 0) } /* GE_BILSPAN */
                    else if (bpair) { LEAN_PAIR_K if (h->ztest) { LEAN_VK_LOOP(1, LEAN_BIL32P) } else { LEAN_VK_LOOP(0, LEAN_BIL32P) } }
                    else { if (h->ztest) { LEAN_VK_LOOP(1, LEAN_BIL32) } else { LEAN_VK_LOOP(0, LEAN_BIL32) } }
                } else if (mtri && (nl && h->tbil)) {
                    const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                    if (tpair && sTbilSpan > 0) { if (h->ztest) TBQ_GO(1, 1, 0, TBQ_FETCH, 0x8000) else TBQ_GO(1, 0, 0, TBQ_FETCH, 0x8000) } /* GE_TBILSPAN */
                    else if (tpair) { LEAN_PAIR_K if (h->ztest) { LEAN_VK_LOOP(1, LEAN_TBIL32P) } else { LEAN_VK_LOOP(0, LEAN_TBIL32P) } }
                    else { if (h->ztest) { LEAN_VK_LOOP(1, LEAN_TBIL32) } else { LEAN_VK_LOOP(0, LEAN_TBIL32) } }
                } else if (mtri) {
                    const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                    if (h->ztest) { LEAN_VK_LOOP(1, LEAN_TRI32) } else { LEAN_VK_LOOP(0, LEAN_TRI32) }
                } else {
                    if (h->ztest) { LEAN_VK_LOOP(1, LEAN_NEAR32) } else { LEAN_VK_LOOP(0, LEAN_NEAR32) }
                }
            } else if (use_bilin) {
                uf -= 0x8000; vf -= 0x8000;
                if (h->t16) { LEAN_LOOP_Z(LEAN_BIL16) }
                else if (bpair && sBilSpan > 0) { if (h->ztest) TBQ_GO(0, 1, 0, TBQ_FETCH1, 0) else TBQ_GO(0, 0, 0, TBQ_FETCH1, 0) } /* GE_BILSPAN */
                else if (bpair) { LEAN_PAIR_K LEAN_LOOP_Z(LEAN_BIL32P) }
                else        { LEAN_LOOP_Z(LEAN_BIL32) }
            } else if (mtri && (nl && h->tbil)) {
                const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                if (h->t16) { LEAN_LOOP_Z(LEAN_TBIL) }
                else if (tpair && sTbilSpan > 0) { if (h->ztest) TBQ_GO(0, 1, 0, TBQ_FETCH, 0x8000) else TBQ_GO(0, 0, 0, TBQ_FETCH, 0x8000) } /* GE_TBILSPAN */
                else if (tpair) { LEAN_PAIR_K LEAN_LOOP_Z(LEAN_TBIL32P) }
                else        { LEAN_LOOP_Z(LEAN_TBIL32) }
            } else if (mtri) {
                const uint16x4_t vmf = vdup_n_u16((uint16_t)mfrac);
                if (h->t16) { LEAN_LOOP_Z(LEAN_TRI16) }
                else        { LEAN_LOOP_Z(LEAN_TRI32) }
            } else {
                if (h->t16) { LEAN_LOOP_Z(LEAN_NEAR16) }
                else        { LEAN_LOOP_Z(LEAN_NEAR32) }
            }
        } else if (blend && (fast || clampok) && use_bilin && sLeanNp2 > 0 && lean_np2_inside(uf, vf, duf, dvf, span, tw, th)) {
            /* GE_LEANNP2: a non-power-of-two repeating texture (explosion.c's 56x56 sprites) whose span reads only
             * texels inside the image: wrapf returns every tap as it is there, so the taps are read directly -- the
             * lean blended loop with the row stride tw and no masks, in place of cm_bilin_blend_span_neon (the same
             * fetch, lerp, combine, alpha source, reject, blend and z, as the power-of-two rows above) */
            const int32_t zdq = h->zdq;
            uint32_t *const fe = h->fbuf + idx + span;
            uint16_t *const ze = h->zb + idx + span;
            const uint32_t *const p32 = (const uint32_t *)texpx;
            const int zbias = h->zbias, amin = h->amin; const bool dmask = h->dmask;
            const int fsrc = !h->a_tex ? 0 : !h->a_in ? 1 : 2;
            const int tws = tw;
            uf -= 0x8000; vf -= 0x8000;
            if (fsrc == 2 && sLeanAsm > 0) { /* GE_LEANASM: the same loop, hand-scheduled (ge_lean_np2_cmb2) */
                LeanAsmRow ar;
                ar.fe = fe; ar.ze = ze; ar.i2 = -2 * span; ar.zq = zq; ar.zdq = zdq; ar.zbias = zbias; ar.p32 = p32;
                ar.tws4 = tws * 4; ar.amin = amin; ar.dmask = dmask; ar.uf = uf; ar.vf = vf; ar.duf = duf; ar.dvf = dvf;
                vst1q_s32(ar.acc, acc); vst1q_s32(ar.dacc, dacc);
                ge_lean_np2_cmb2(&ar);
            } else if (fsrc == 0) { LEAN_CMB_LOOP(0, LEAN_BIL32N) } else if (fsrc == 1) { LEAN_CMB_LOOP(1, LEAN_BIL32N) }
            else { LEAN_CMB_LOOP(2, LEAN_BIL32N) }
        } else if (sClampSpan > 0 && clampok && tmask_u >= 0 && tmask_v >= 0 && !h->t16 &&
                   (use_bilin ? h->bilpair && sBilSpan > 0 && !blend
                              : mtri && nl && h->tbil && h->tbilpair && tmask_u > 0 && sTbilSpan > 0)) {
            /* GE_CLAMPSPAN (see TBQ_LOOPC): a clamped texture's span whose taps leave the image, in the lean loops with
             * its coordinates clamped -- the bilinear spans of the opaque and K rows, the two-level spans of all */
            TbqS sp;
            sp.texpx = texpx; sp.texpx2 = texpx2; sp.mu = tmask_u; sp.mv = tmask_v; sp.mfrac = mfrac; sp.bil = use_bilin;
            sp.kind = blend ? 2 : vk && h->alpha ? 3 : vk ? 1 : 0; sp.span = span;
            sp.fe = h->fbuf + idx + span; sp.ze = h->zb + idx + span;
            vst1q_s32(sp.uvz, (int32x4_t){ uf, vf, zq, 0 }); vst1q_s32(sp.duvz, (int32x4_t){ duf, dvf, h->zdq, 0 });
            vst1q_s32(sp.acc, acc); vst1q_s32(sp.dacc, dacc);
            if (vk) { vst1q_s32(sp.kacc, kacc); vst1q_s32(sp.dkacc, dkacc); }
            tbqc_span(h, &sp);
        } else {
            const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, h->t16 };
            if (blend) {
                if (use_bilin)
                    cm_bilin_blend_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, &ct,
                                             acc, dacc, h->a_tex, h->a_in, h->amin);
                else
                    cm_mip_blend_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, &ct,
                                           mtri, texpx2, tw2, mfrac, acc, dacc, h->a_tex, h->a_in, h->amin, (nl && h->tbil) && mtri);
            } else if (vk) {
                if (h->alpha) {
                    if (use_bilin)
                        vk_bilin_blend_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask,
                                                 h->ztest, h->texa, &ct, acc, dacc, kacc, dkacc, h->a_tex, h->amin);
                    else
                        vk_mip_blend_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask,
                                               h->ztest, h->texa, &ct, mtri, texpx2, tw2, mfrac, acc, dacc, kacc, dkacc,
                                               h->a_tex, h->amin, (nl && h->tbil) && mtri);
                } else if (use_bilin)
                    vk_bilin_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, h->ztest,
                                       h->texa, &ct, acc, dacc, kacc, dkacc);
                else
                    vk_mip_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, h->ztest,
                                     h->texa, &ct, mtri, texpx2, tw2, mfrac, acc, dacc, kacc, dkacc, (nl && h->tbil) && mtri);
            } else if (use_bilin)
                cm_bilin_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, &ct, acc, dacc);
            else
                cm_mip_span_neon(h->fbuf + idx, h->zb + idx, span, uf, vf, duf, dvf, zq, h->zdq, h->zbias, h->dmask, &ct,
                                 mtri, texpx2, tw2, mfrac, acc, dacc, (nl && h->tbil) && mtri);
        }
        idx += span;
        if (PV) { P = Pn; C = Cn; K = Kn; }
        else {
        zf += h->zdx * fspan;
        cr += h->crdx * fspan; cg += h->cgdx * fspan; cb += h->cbdx * fspan; ca += h->cadx * fspan;
        if (vk) { ckr += h->ckrdx * fspan; ckg += h->ckgdx * fspan; ckb += h->ckbdx * fspan; cka += h->ckadx * fspan; }
        uw += h->uwdx * fspan;
        vw += h->vwdx * fspan;
        sw = sw_b;
        }
        rw = rw_b;
        x += span;
    }
}
#define HOT_ROW_LEAN_FNS(SUF, PV)                                                                       \
static void __attribute__((noinline)) hot_row_lean##SUF(const HotTri *h, int idx, int x, int xe, float zf, float cr, float cg, \
                                                   float cb, float ca, float uw, float vw, float sw, float rw) {   \
    if (h->nlod) hot_row_lean_body(h, 0, true, PV, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0.0f, 0.0f, 0.0f, 0.0f); \
    else hot_row_lean_body(h, 0, false, PV, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0.0f, 0.0f, 0.0f, 0.0f); \
}                                                                                                       \
static void __attribute__((noinline)) hot_row_blend_lean##SUF(const HotTri *h, int idx, int x, int xe, float zf, float cr, \
                                                         float cg, float cb, float ca, float uw, float vw, float sw, float rw) { \
    if (h->nlod) hot_row_lean_body(h, 2, true, PV, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0.0f, 0.0f, 0.0f, 0.0f); \
    else hot_row_lean_body(h, 2, false, PV, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, 0.0f, 0.0f, 0.0f, 0.0f); \
}                                                                                                       \
static void __attribute__((noinline)) hot_row_vk_lean##SUF(const HotTri *h, int idx, int x, int xe, float zf, float cr, \
                                                      float cg, float cb, float ca, float uw, float vw, float sw, float rw, \
                                                      float ckr, float ckg, float ckb, float cka) {            \
    if (h->nlod) hot_row_lean_body(h, 1, true, PV, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka); \
    else hot_row_lean_body(h, 1, false, PV, idx, x, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka); \
}
HOT_ROW_LEAN_FNS(, 0)
HOT_ROW_LEAN_FNS(_v, 1) /* GE_SPANVEC */
#undef HOT_ROW_LEAN_FNS
/* GE_LEANTRI (2026-10-01): a hot triangle's whole row loop in one function as well. raster_tri_body called
 * hot_row_lean once a row -- thirteen arguments moved, the callee's prologue (nine core and eight VFP registers
 * saved), the HotTri's fields reloaded -- and GoldenEye's triangles are small (the Dam: ~1,500 a frame at ~65 px,
 * rows of ~8 px), so that was a good part of each row. Here the rows are raster_tri_body's, statement for statement:
 * the same row bases, edge solves (EDGE_SOLVE), snap, deferral hook, start values and 1/w, then the lean row body
 * inline. The per-triangle values come in a LeanTri. Default OFF since the device Bench of 2026-10-02 (the Dam, 2 runs:
 * all on 34.47 ms wall, "lean triangles OFF" 34.15 -- the bigger function costs more than the calls it saves, likely
 * register spills and I-cache); GE_LEANTRI=1 turns it on. Both ways draw the same pixels (rast_test.sh). */
typedef struct {
    int minx, maxx, miny, maxy, ek0, ek1, ek2, sw;
    float inv_area, w0dx, w1dx, w2dx, e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y;
    float z0, z1, z2, r0, r1, r2, g0, g1, g2, bl0, bl1, bl2, a0, a1, a2;
    float uw0, uw1, uw2, vw0, vw1, vw2, iw0, iw1, iw2;
    float kr0, kr1, kr2, kg0, kg1, kg2, kb0, kb1, kb2, ka0, ka1, ka2;
    bool flat, persp;
    bool own; OwnTri oe; /* GE_FILLRULE (see own_edge) */
} LeanTri;
static int sLeanTri SAVESTATE_EXCLUDE = -1; /* default 0 (hot_row_lean / hot_row_vk_lean a row); GE_LEANTRI=1: the row loop here */
/* The edge solve's clamp and ceil / floor without FPSCR transfers (lean triangles): the same int as
 *   if (q > 1e6f) q = 1e6f; iceilf(q)  /  ifloorf(q)      (port.h: (int)q +- (q >/< (float)(int)q))
 * with the clamp as vmin, the conversions as NEON vcvt (toward zero, saturating, NaN -> 0: VFP's) and the compare as
 * vcgt/vclt -- one transfer to the core instead of two VFP compares and FPSCR transfers. NEON flushes denormals, the
 * VFP does not; q here is x - 1e-5f or x + 1e-5f for a float x, which is 0 or at least 2^-41 in magnitude (near
 * 1e-5f both are multiples of its ulp, elsewhere the difference is >= 5e-6), never denormal. Checked against the
 * scalar form on every float class and random inputs (rast_test ops). */
static inline __attribute__((always_inline)) int lean_iceil_clamp(float q) {
    const float32x2_t v = vmin_f32(vdup_n_f32(q), vdup_n_f32(1e6f));
    const int32x2_t t = vcvt_s32_f32(v);
    return vget_lane_s32(vsub_s32(t, vreinterpret_s32_u32(vcgt_f32(v, vcvt_f32_s32(t)))), 0);
}
static inline __attribute__((always_inline)) int lean_ifloor_clamp(float q) {
    const float32x2_t v = vmin_f32(vdup_n_f32(q), vdup_n_f32(1e6f));
    const int32x2_t t = vcvt_s32_f32(v);
    return vget_lane_s32(vadd_s32(t, vreinterpret_s32_u32(vclt_f32(v, vcvt_f32_s32(t)))), 0);
}
static inline __attribute__((always_inline)) int hot_tri_lean_body(const HotTri *h, const LeanTri *t, const int kind) {
    const bool vk = kind == 1;
    const int minx = t->minx, maxx = t->maxx, miny = t->miny, maxy = t->maxy, ek0 = t->ek0, ek1 = t->ek1, ek2 = t->ek2;
    const float inv_area = t->inv_area, w0dx = t->w0dx, w1dx = t->w1dx, w2dx = t->w2dx;
    const float e0c = t->e0c, e0x = t->e0x, e0y = t->e0y, e1c = t->e1c, e1x = t->e1x, e1y = t->e1y;
    const float e2c = t->e2c, e2x = t->e2x, e2y = t->e2y;
    const bool own = t->own;
    uint32_t tri_px = 0;
    for (int y = miny; y < maxy; y++) {
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
#define EDGE_SOLVE(B, K, D)                                                                              \
        if (K > 0) {                                                                                    \
            if (B < 0.0f) {                                                                             \
                int a = minx + lean_iceil_clamp(-B / D - 1e-5f);                                        \
                if (a > xs) xs = a;                                                                     \
            }                                                                                           \
        } else if (K < 0) {                                                                             \
            int a = minx + 1 + lean_ifloor_clamp(-B / D + 1e-5f);                                       \
            if (a < xe) xe = a;                                                                         \
        } else if (B < 0.0f) {                                                                          \
            continue; /* row entirely outside this edge */                                              \
        }
        EDGE_SOLVE(b0, ek0, w0dx)
        EDGE_SOLVE(b1, ek1, w1dx)
        EDGE_SOLVE(b2, ek2, w2dx)
#undef EDGE_SOLVE
        if (xs >= xe) continue;
        if (own) { /* GE_FILLRULE: raster_tri_body's owned-edge ends (see own_edge) */
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;
            if (sOwnFast > 0) own_row(&t->oe, py, &xs, &xe);
            else {
                while (xs < xe && !own_px(&t->oe, xs, py)) xs++;
                while (xe > xs && !own_px(&t->oe, xe - 1, py)) xe--;
            }
        } else
        while (xs < xe) {
            const float o = (float)(xs - minx);
            if (b0 + o * w0dx >= 0 && b1 + o * w1dx >= 0 && b2 + o * w2dx >= 0) break;
            xs++;
        }
        if (xs >= xe) continue;
        if (sDefOp != DEF_OP_NONE && def_row(y, xs, xe)) continue;
        tri_px += (uint32_t)(xe - xs);
        const float o = (float)(xs - minx);
        float w0 = b0 + o * w0dx;
        float w1 = b1 + o * w1dx;
        float w2 = b2 + o * w2dx;
        float zf = w0*t->z0 + w1*t->z1 + w2*t->z2;
        float cr, cg, cb, ca;
        if (t->flat) { cr = t->r0; cg = t->g0; cb = t->bl0; ca = t->a0; }
        else { cr = w0*t->r0 + w1*t->r1 + w2*t->r2; cg = w0*t->g0 + w1*t->g1 + w2*t->g2;
               cb = w0*t->bl0 + w1*t->bl1 + w2*t->bl2; ca = w0*t->a0 + w1*t->a1 + w2*t->a2; }
        float uw = w0*t->uw0 + w1*t->uw1 + w2*t->uw2;
        float vw = w0*t->vw0 + w1*t->vw1 + w2*t->vw2;
        float sw = w0*t->iw0 + w1*t->iw1 + w2*t->iw2;
        float ckr = 0.0f, ckg = 0.0f, ckb = 0.0f, cka = 0.0f;
        if (vk) { ckr = w0*t->kr0 + w1*t->kr1 + w2*t->kr2; ckg = w0*t->kg0 + w1*t->kg1 + w2*t->kg2;
                  ckb = w0*t->kb0 + w1*t->kb1 + w2*t->kb2; cka = w0*t->ka0 + w1*t->ka1 + w2*t->ka2; }
        const int idx = y * t->sw + xs;
        float rw = 0.0f;
        if (t->persp) rw = lean_rcp_w(sw);
        hot_row_lean_body(h, kind, h->nlod, 0, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
    }
    return (int)tri_px;
}
static int __attribute__((noinline)) hot_tri_lean(const HotTri *h, const LeanTri *t) { return hot_tri_lean_body(h, t, 0); }
static int __attribute__((noinline)) hot_tri_vk_lean(const HotTri *h, const LeanTri *t) { return hot_tri_lean_body(h, t, 1); }
static int __attribute__((noinline)) hot_tri_blend_lean(const HotTri *h, const LeanTri *t) { return hot_tri_lean_body(h, t, 2); }
/* GE_FASTROWS (2026-10-07, the cross-port speed round; SM64_FASTROWS, the SM64 port 03b1f32; default 0 since the device
 * Benches of 2026-10-10): a lean
 * triangle's row loop in a small function of its own (fast_rows_*), each row still drawn by its own lean row call
 * (hot_row_lean & co.: GE_LEANTRI's inline row body was slower on the device, as SM64's ROWLOOP). raster_tri_body kept
 * ~60 values live across its row loop and reloaded or spilled most of them every row: the loop was ~62% of its
 * instructions on the Dam (~290 a row, ~12% of the frame). Here, raster_tri_body's rows statement for statement:
 * the same row bases, the edge solves with the slope signs once a triangle (as hot_tri_lean_body: the clamp and
 * ceil / floor in NEON, rast_test ops), the owned-edge ends or the snap, the deferral hook, then
 *   - B < 0 from the float's bits (negative, not -0, not NaN: one unsigned compare, the VFP compare's answer for
 *     every float, no FPSCR transfer);
 *   - the snap's three tests b_i + o * w_i >= 0 in NEON lanes (vmla: the product rounded, then the sum, as the
 *     VFP's), all three at once;
 *   - the row's start values (depth, shade, u/w, v/w, 1/w; K in the K rows) as NEON lanes: w0 * v0 + w1 * v1 +
 *     w2 * v2 per lane, multiply then two multiply-adds, each rounded as the VFP rounds them; a flat triangle's
 *     shade the vertex colour.
 * NEON flushes denormals: no value here comes near one (barycentrics and their steps are 0 or above ~1e-15, the
 * vertex values are colours, depths, 1/w and texture coordinates). Not with GE_EDGESKIP=1 (raster_tri_body's loop
 * then) or GE_LEANTRI=1. Same pixels either way. Default OFF since the device Benches of 2026-10-10 (raster_tri_body's
 * own loop: Bench 1's Dam 34.47 ms off vs 35.20 on, Bench 2's Jungle start 73.12 vs 73.72; qemu had it 1.8% cheaper on
 * the Dam); GE_FASTROWS=1 turns it on. */
static int sFastRows SAVESTATE_EXCLUDE = -1; /* GE_FASTROWS (default off) */
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's { z, r, g, b } and { a, u/w, v/w, 1/w } */
    float32x4_t vk0, vk1, vk2;                /* and its K { r, g, b, a } (the K rows) */
    float32x4_t fa, fb;                       /* flat: { -, r0, g0, b0 }, { a0, -, -, - } */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    int minx, maxx, sw, ek0, ek1, ek2;
    bool flat, persp, own, spanvec;
    const OwnTri *oe;
} FastRows;
/* x < 0.0f for every float x (negative and not -0: bits 0x80000001..0xff800000; NaN lies above), on the core */
static inline __attribute__((always_inline)) bool fr_neg(float x) {
    union { float f; uint32_t u; } b = { x };
    return b.u - 0x80000001u < 0x7f800000u;
}
static inline __attribute__((always_inline)) uint32_t fast_rows_t(const HotTri *h, const FastRows *T, int y, const int maxy,
                                                                  const int kind) {
    const float e0c = T->e0c, e0x = T->e0x, e0y = T->e0y, e1c = T->e1c, e1x = T->e1x, e1y = T->e1y;
    const float e2c = T->e2c, e2x = T->e2x, e2y = T->e2y, inv_area = T->inv_area;
    const float w0dx = T->w0dx, w1dx = T->w1dx, w2dx = T->w2dx;
    const int minx = T->minx, maxx = T->maxx, ek0 = T->ek0, ek1 = T->ek1, ek2 = T->ek2;
    const bool own = T->own;
    const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };
    uint32_t px = 0;
    for (; y < maxy; y++) {
        const float py = y + 0.5f;
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;
        int xs = minx, xe = maxx;
#define FR_EDGE(B, K, D)                                                                                 \
        if (K > 0) {                                                                                    \
            if (fr_neg(B)) {                                                                            \
                const int a = minx + lean_iceil_clamp(-B / D - 1e-5f);                                  \
                if (a > xs) xs = a;                                                                     \
            }                                                                                           \
        } else if (K < 0) {                                                                             \
            const int a = minx + 1 + lean_ifloor_clamp(-B / D + 1e-5f);                                 \
            if (a < xe) xe = a;                                                                         \
        } else if (fr_neg(B)) {                                                                         \
            continue; /* row entirely outside this edge */                                              \
        }
        FR_EDGE(b0, ek0, w0dx)
        FR_EDGE(b1, ek1, w1dx)
        FR_EDGE(b2, ek2, w2dx)
#undef FR_EDGE
        if (xs >= xe) continue;
        const float32x4_t B3 = lean_f4(b0, b1, b2, 0.0f);
        float32x4_t W;
        if (own) { /* GE_FILLRULE: raster_tri_body's owned-edge ends (see own_edge) */
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;
            if (sOwnFast > 0) own_row(T->oe, py, &xs, &xe);
            else {
                while (xs < xe && !own_px(T->oe, xs, py)) xs++;
                while (xe > xs && !own_px(T->oe, xe - 1, py)) xe--;
            }
            if (xs >= xe) continue;
            W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
        } else {
            /* the snap: b_i + (xs - minx) * w_i_dx >= 0 for all three (lane 3: 0 + 0) */
            for (;;) {
                W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));
                const uint32x4_t ge = vcgeq_f32(W, vdupq_n_f32(0.0f));
                const uint32x2_t g2 = vand_u32(vget_low_u32(ge), vget_high_u32(ge));
                if (vget_lane_u32(vpmin_u32(g2, g2), 0)) break;
                if (++xs >= xe) break;
            }
            if (xs >= xe) continue;
        }
        if (sDefOp != DEF_OP_NONE && def_row(y, xs, xe)) continue;
        px += (uint32_t)(xe - xs);
        /* w0 * v0 + w1 * v1 + w2 * v2 for every value (W: w0, w1, w2 as raster_tri_body makes them) */
        const float32x2_t wl = vget_low_f32(W), wh = vget_high_f32(W);
        float32x4_t A = vmulq_lane_f32(T->va0, wl, 0), B = vmulq_lane_f32(T->vb0, wl, 0);
        A = vmlaq_lane_f32(A, T->va1, wl, 1); B = vmlaq_lane_f32(B, T->vb1, wl, 1);
        A = vmlaq_lane_f32(A, T->va2, wh, 0); B = vmlaq_lane_f32(B, T->vb2, wh, 0);
        if (T->flat) { /* the shade is the vertex colour (no barycentric build) */
            static const uint32_t ma[4] = { 0, ~0u, ~0u, ~0u }, mb[4] = { ~0u, 0, 0, 0 };
            A = vbslq_f32(vld1q_u32(ma), T->fa, A); B = vbslq_f32(vld1q_u32(mb), T->fb, B);
        }
        const float sw = vgetq_lane_f32(B, 3);
        const float rw = T->persp ? lean_rcp_w(sw) : 0.0f;
        const int idx = y * T->sw + xs;
        const float zf = vgetq_lane_f32(A, 0), cr = vgetq_lane_f32(A, 1), cg = vgetq_lane_f32(A, 2), cb = vgetq_lane_f32(A, 3);
        const float ca = vgetq_lane_f32(B, 0), uw = vgetq_lane_f32(B, 1), vw = vgetq_lane_f32(B, 2);
        if (kind == 1) {
            float32x4_t Kv = vmulq_lane_f32(T->vk0, wl, 0);
            Kv = vmlaq_lane_f32(Kv, T->vk1, wl, 1);
            Kv = vmlaq_lane_f32(Kv, T->vk2, wh, 0);
            const float ckr = vgetq_lane_f32(Kv, 0), ckg = vgetq_lane_f32(Kv, 1), ckb = vgetq_lane_f32(Kv, 2), cka = vgetq_lane_f32(Kv, 3);
            if (T->spanvec) hot_row_vk_lean_v(h, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
            else hot_row_vk_lean(h, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
        } else if (kind == 2) {
            if (T->spanvec) hot_row_blend_lean_v(h, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else hot_row_blend_lean(h, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw);
        } else {
            if (T->spanvec) hot_row_lean_v(h, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else hot_row_lean(h, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw);
        }
    }
    return px;
}
static uint32_t __attribute__((noinline)) fast_rows_cm(const HotTri *h, const FastRows *T, int y, int maxy) { return fast_rows_t(h, T, y, maxy, 0); }
static uint32_t __attribute__((noinline)) fast_rows_vk(const HotTri *h, const FastRows *T, int y, int maxy) { return fast_rows_t(h, T, y, maxy, 1); }
static uint32_t __attribute__((noinline)) fast_rows_blend(const HotTri *h, const FastRows *T, int y, int maxy) { return fast_rows_t(h, T, y, maxy, 2); }
#undef LEAN_LERP2
#undef LEAN_ROWS
#undef LEAN_BIL32
#undef LEAN_BIL16
#undef LEAN_NEAR32
#undef LEAN_NEAR16
#undef LEAN_TRI_LERP
#undef LEAN_TRI32
#undef LEAN_TRI16
#undef LEAN_TBIL
#undef LEAN_TBIL32
#undef LEAN_TBIL32P
#undef TBQ_GO
#undef TBQ_LOOPC
#undef TBQ_FETCHC
#undef TBQ_FETCH1
#undef TBQ_LOOP
#undef TBQ_FETCH
#undef TBQ_LERP2
#undef TBQ_PAIRB
#undef LEAN_BIL32P
#undef LEAN_PAIRB
#undef LEAN_PAIR_K
#undef LEAN_STORE
#undef LEAN_TBL
#undef LEAN_LOOP
#undef LEAN_LOOP_Z
#undef LEAN_VK_LOOP
#undef LEAN_CMB_LOOP
#undef LEAN_VKA_LOOP
#undef LEAN_BIL32S
#endif

/* Returns the span pixels the triangle produced, or -1 if it was rejected
 * before span setup (the caller keeps the 3D-only profiling counters). */
/* Deferred backgrounds (round 24; see def_record below): raster_tri's per-row hook. With sDefOp set,
 * every row's final span [xs, xe) -- the pixels the row draws, exactly -- goes to def_row first. */
static const SoftTex *sTexOverride SAVESTATE_EXCLUDE; /* a resolve's copy of its batch's texture */
/* The RDP's detail texture (round 32, GE_DETAILGRAIN; gfx_pc.c gfx_tri_rebuild_state_detail): GoldenEye's
 * ground (the Runway, the Dam, both Surfaces, the Depot) draws in detail mode with the TRILERP combiner,
 * (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0, where magnified TEXEL0 is tile 0 (the grain: the Runway's
 * and the Dam's own image at twice the frequency, the Surfaces' and the Depot's a texture of its own)
 * and TEXEL1 tile 1 (the base, level 0). LOD_FRACTION is the texels per pixel, but never below the
 * prim colour's min level (26/32 there, 21/32 in the Depot): up close the grain is 19% (34%) of the
 * texel; from 1:1 on it is the base's mip chain alone, as the port drew it. The port drew the base
 * alone everywhere, which reads soft up close (the user's IMG_0175). Here, on a span whose base is
 * magnified, each pixel lerps the grain's bilinear texel (its coordinate: the base's scaled by the
 * tiles' shifts, offset by their corners) into the base's by that fraction. */
static struct { uint32_t id; int32_t ks, kt, os, ot; float minf; } sGrain SAVESTATE_EXCLUDE;
void gfx_soft_set_grain(uint32_t id, float ks, float kt, float os, float ot, float minf) {
    sGrain.id = id;
    sGrain.ks = (int32_t)(ks * 65536.0f); sGrain.kt = (int32_t)(kt * 65536.0f);
    sGrain.os = (int32_t)(os * 65536.0f); sGrain.ot = (int32_t)(ot * 65536.0f);
    sGrain.minf = minf;
}
/* GE_CVGALPHA (default 1): a texture-edge draw (CVG_X_ALPHA) whose combined alpha does not come from the
 * texture -- GoldenEye's destroyed objects: modelApplyRenderModeType3/4 with an env alpha of 150/200/250
 * added to the shade alpha, G_RM_AA_ZB_TEX_EDGE2, no FORCE_BL. The RDP (angrylion combiner.c / zbuffer.c /
 * blender.c) multiplies the pixel's coverage by that alpha, cvg = ((a * 8 + 4) >> 3) >> 5 (a 255 as 256),
 * so an interior pixel keeps a coverage of 1..8 for a >= 32 and none below; over the full coverage already
 * in memory that overflows, so blend_en is off and the pixel is written opaque (the colour, not a mix), and
 * the VI leaves it (its neighbours are just as partial). The port blended by the alpha instead (cut at 0x55):
 * the destroyed crate drew as a see-through box tinted by the fog colour (the user's Dam report). Now: all
 * three vertex alphas >= 32 draws opaque, all < 32 draws nothing (the alpha is a convex mix of the three,
 * so the whole triangle is on one side), a mixed triangle tests each pixel (>= 32 drawn, never blended).
 * Texture-alpha edges (foliage, fences) keep the old blend, which stands in for the VI's edge filter there.
 * GE_CVGALPHA=0: the old path. */
static bool sCvgMixed SAVESTATE_EXCLUDE;
static int sCvgAlpha SAVESTATE_EXCLUDE = -1;
unsigned long gfx_soft_cvg_tris[3] SAVESTATE_EXCLUDE; /* triangles taken opaque, dropped, mixed (cvgalpha_test) */
/* GE_EDGESKIP (2026-10-05, the Cradle's lattice): raster_tri_body's right-hand edge solve (K < 0) skipped on a row whose
 * last pixel of the box is clearly inside that edge -- the solve could only give xe >= maxx there, which changes
 * nothing. The edge's row base is B (b0..b2), its x step D < 0, n = maxx - 1 - minx; the solve's
 *   a = minx + 1 + floor(fl(fl(-B / D) + 1e-5))   (q clamped to 1e6, n < 1e6)
 * is >= maxx whenever the exact -B / D >= n, i.e. the exact B + n D >= 0 (D < 0; division and the add round
 * monotonically, n is a float exactly). The row test: T = fl(B + P), P = fl(n D) once a triangle, T >= M with
 *   M = max(-P * 2^-20, 1e-30).
 * Rounding: P = nD + e1, |e1| <= 2^-24 |nD| + 2^-150 (subnormal); B + P >= T / (1 + 2^-24); so the exact
 * B + nD >= T (1 - 2^-24) - 2^-24 |P| (1 + 2^-23) - 2^-149 > 0 when T >= 2^-20 |P| and T >= 1e-30.
 * NaN or infinite B, D or P never pass (or, B = +inf, skip a solve that changes nothing either). rast_test
 * "edge" checks the claim on random and adversarial rows against the solve itself. The left edges (K > 0) need no
 * test: their solve already runs only when B < 0, where the pixel at minx is outside.
 * Default OFF since the device Bench 2 of 2026-10-05 (railings 69.83 ms off vs 69.97 on, bridge 57.14 vs 57.22: the
 * row test costs more than the divides it saves there); GE_EDGESKIP=1 turns it on. */
static int sEdgeSkip SAVESTATE_EXCLUDE = -1;
static int sEmptyTri SAVESTATE_EXCLUDE = -1; /* GE_EMPTYTRI: see raster_tri_body */
static inline void edge_end_setup(int n, float d, float *p, float *m) {
    *p = (float)n * d;
    const float t = -*p * (1.0f / 1048576.0f);
    *m = t > 1e-30f ? t : 1e-30f; /* NaN P: NaN M, never passes */
    if (*p != *p) *m = *p;
}
static inline __attribute__((always_inline)) bool edge_end_inside(float b, float p, float m) { return b + p >= m; }

static int raster_tri_body(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z);
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
    if (!sUseAlpha || !sCurShader || !sCurShader->cc.opt_texture_edge) return raster_tri_body(v0, v1, v2, use_z);
    if (sCvgAlpha < 0) { const char *e = getenv("GE_CVGALPHA"); sCvgAlpha = !(e && e[0] == '0'); }
    if (!sCvgAlpha) return raster_tri_body(v0, v1, v2, use_z);
    /* raster_tri_body's a_tex: the texture supplies the alpha only when a valid texture is bound */
    const SoftTex *tex = (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])
                             ? (sTexOverride ? sTexOverride : &sTex[sBoundTex[0]]) : NULL;
    if (tex && tex->valid && sCurShader->a_tex) return raster_tri_body(v0, v1, v2, use_z);
    const int amn = v0->a < v1->a ? (v0->a < v2->a ? v0->a : v2->a) : (v1->a < v2->a ? v1->a : v2->a);
    const int amx = v0->a > v1->a ? (v0->a > v2->a ? v0->a : v2->a) : (v1->a > v2->a ? v1->a : v2->a);
    int n;
#if defined(MIYOO)
    /* the same choices through this thread's flags; counted on the game thread only (a batch share's worker sees
     * every triangle of the batch too) */
    if (amx < 32) { if (!tlSplit) gfx_soft_cvg_tris[1]++; return -1; }
    if (amn >= 32) {
        if (!tlSplit) gfx_soft_cvg_tris[0]++;
        tlAlphaOff = true;
        n = raster_tri_body(v0, v1, v2, use_z);
        tlAlphaOff = false;
        return n;
    }
    if (!tlSplit) gfx_soft_cvg_tris[2]++;
    tlCvgMixed = true;
    n = raster_tri_body(v0, v1, v2, use_z);
    tlCvgMixed = false;
    return n;
#endif
    if (amx < 32) { gfx_soft_cvg_tris[1]++; return -1; }
    if (amn >= 32) {
        gfx_soft_cvg_tris[0]++;
        sUseAlpha = false;
        n = raster_tri_body(v0, v1, v2, use_z);
        sUseAlpha = true;
        return n;
    }
    gfx_soft_cvg_tris[2]++;
    sCvgMixed = true;
    n = raster_tri_body(v0, v1, v2, use_z);
    sCvgMixed = false;
    return n;
}

#if defined(MIYOO)
/* The split's size: a triangle is split when it spans at least sSplitRows rows (GE_SPLITROWS) and its rows hold
 * about sSplitPx pixels or more (GE_SPLITPX). The handover costs ~0.5 us and the worker repeats the triangle's
 * setup (~1,000 instructions) beside the game thread's rows; a GoldenEye pixel costs ~0.2 us (the Mini Plus's PERF
 * lines: r3d 25 ms over ~115 kpx), so a few dozen pixels already pay. */
static int sSplitPx SAVESTATE_EXCLUDE = -1, sSplitRows SAVESTATE_EXCLUDE = 4;
unsigned gfx_soft_split_tris SAVESTATE_EXCLUDE; /* triangles split (hh_gfx.c's PERF line reads and clears both) */
uint32_t gfx_soft_split_wpx SAVESTATE_EXCLUDE;  /* the span pixels the worker's shares drew */
static int raster_tri_body(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z);
static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z);
static void split_job(void *arg) {
    SplitJob *const j = (SplitJob *)arg;
    j->skip_spans = j->skip_px = 0;
    tlSplit = j;
    tlRowLo = j->ylo;
    if (j->bv != NULL) { /* a batch share: every triangle of the batch through raster_tri, the band's rows */
        for (int i = 0; i < j->bn; i++)
            j->bpx[i] = raster_tri(&j->bv[i * 3], &j->bv[i * 3 + 1], &j->bv[i * 3 + 2], j->use_z);
    } else {
        tlAlphaOff = j->alpha_off; tlCvgMixed = j->cvg_mixed;
        j->px = raster_tri_body(j->v0, j->v1, j->v2, j->use_z);
        tlAlphaOff = tlCvgMixed = false;
    }
    tlRowLo = 0;
    tlSplit = NULL;
}
/* The shares go to the worker through one long job, split_poller, that stays on the second core while the frame's
 * triangles come: a big triangle comes every ~100-200 us in play, longer than miyoo_worker's own spin before it
 * sleeps (~30 us), and every handover would otherwise wait for a futex wake. The poller spins on sPollState (one
 * cache line, no system call) and returns after POLL_IDLE_US without a share, or when the frame ends
 * (split_stop, gfx_soft_end_frame), so the worker sleeps between frames and through the game's own work.
 * States: 0 the poller waits, 1 a share posted, 2 its rows drawn, 3 no poller, 4 the game thread asks it to stop.
 * Only the game thread moves 0 -> 1, 2 -> 0 and 0 -> 4; only the poller 1 -> 2, 0 -> 3 and 4 -> 3. */
#define POLL_IDLE_US 1500u
static volatile int sPollState SAVESTATE_EXCLUDE = 3;
static SplitJob *volatile sPollJob SAVESTATE_EXCLUDE;
static inline void split_relax(void) { __asm__ __volatile__("yield" ::: "memory"); }
static inline int poll_ld(void) { return __atomic_load_n(&sPollState, __ATOMIC_ACQUIRE); }
static inline bool poll_cas(int from, int to) {
    return __atomic_compare_exchange_n(&sPollState, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static void split_poller(void *unused) {
    (void)unused;
    uint32_t last = port_time_us(), n = 0;
    for (;;) {
        const int st = poll_ld();
        if (st == 1) {
            split_job(sPollJob);
            __atomic_store_n(&sPollState, 2, __ATOMIC_RELEASE);
            last = port_time_us();
            continue;
        }
        if (st == 4) { __atomic_store_n(&sPollState, 3, __ATOMIC_RELEASE); return; }
        if (st == 0 && (++n & 255u) == 0 && port_time_us() - last >= POLL_IDLE_US && poll_cas(0, 3)) return;
        split_relax();
    }
}
/* the game thread: hand the share over (starting a poller if none is running) */
static void split_post(SplitJob *j) {
    sPollJob = j;
    for (;;) {
        if (poll_cas(0, 1)) return;
        if (poll_ld() == 3) { /* none (or it has just timed out): its job ends, a new one starts */
            miyoo_worker_wait();
            __atomic_store_n(&sPollState, 0, __ATOMIC_RELEASE);
            miyoo_worker_run(split_poller, NULL);
        }
    }
}
/* the game thread: until the share is drawn */
static void split_wait(void) {
    unsigned n = 0;
    while (poll_ld() != 2) {
        split_relax();
        if (++n >= 200000u) { sched_yield(); n = 0; } /* the worker off its core (another thread there) */
    }
    __atomic_store_n(&sPollState, 0, __ATOMIC_RELEASE);
}
/* the frame's end: the poller returns, the worker sleeps until the next frame's first share */
static void split_stop(void) {
    if (poll_cas(0, 4)) while (poll_ld() != 3) split_relax();
    miyoo_worker_wait();
}
/* The row that splits rows [ylo, yhi) into two shares of about the same pixel count, or -1 for a triangle too
 * small to split: each row's width from the edges' x at its centre, clipped to [minx, maxx), every row up to 32
 * rows and every k-th row (k-row blocks) above. Only the balance depends on it -- every pixel is drawn by the same
 * code whichever share it falls in. area2: the triangle's doubled area (raster_tri_body's), for a quick no. */
static int split_row(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, float area2, int minx, int maxx, int ylo,
                     int yhi) {
    if (sSplitPx < 0) {
        const char *e = getenv("GE_SPLITPX"), *r = getenv("GE_SPLITROWS");
        sSplitPx = e && e[0] ? atoi(e) : 96;
        if (r && r[0] && atoi(r) >= 2) sSplitRows = atoi(r);
    }
    if (!((area2 < 0.0f ? -area2 : area2) * 0.5f >= (float)sSplitPx)) return -1; /* even unclipped too small (or NaN) */
    const SoftVtx *a = v0, *b = v1, *c = v2, *t;
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y < b->y) { t = b; b = c; c = t; }
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (!(c->y > a->y)) return -1; /* flat or NaN */
    const float kac = (c->x - a->x) / (c->y - a->y);
    const float kab = b->y > a->y ? (b->x - a->x) / (b->y - a->y) : 0.0f;
    const float kbc = c->y > b->y ? (c->x - b->x) / (c->y - b->y) : 0.0f;
    const float fx0 = (float)minx, fx1 = (float)maxx;
    const int rows = yhi - ylo, k = (rows + 31) / 32; /* rows a block */
    float w[32], total = 0.0f;
    int nb = 0;
    for (int y = ylo; y < yhi; y += k, nb++) {
        const float py = (float)y + 0.5f * (float)(y + k <= yhi ? k : yhi - y); /* the block's middle */
        float wi = 0.0f;
        if (py >= a->y && py <= c->y) {
            const float x1 = a->x + (py - a->y) * kac;
            const float x2 = py < b->y ? a->x + (py - a->y) * kab : b->x + (py - b->y) * kbc;
            float l = x1 < x2 ? x1 : x2, r = x1 < x2 ? x2 : x1;
            if (l < fx0) l = fx0;
            if (r > fx1) r = fx1;
            wi = r > l ? (r - l) * (float)(y + k <= yhi ? k : yhi - y) : 0.0f;
        }
        w[nb] = wi;
        total += wi;
    }
    if (!(total >= (float)sSplitPx)) return -1;
    const float half = total * 0.5f;
    float acc = 0.0f;
    for (int i = 0; i < nb; i++) {
        if (acc + w[i] >= half) { /* the half falls in block i: the row inside it, pro rata */
            const int y = ylo + i * k, n = y + k <= yhi ? k : yhi - y;
            const int ym = y + (w[i] > 0.0f ? (int)((half - acc) / w[i] * (float)n + 0.5f) : 0);
            return ym;
        }
        acc += w[i];
    }
    return -1;
}
#endif

static int raster_tri_body(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z) {
#if defined(MIYOO)
    /* raster_tri's GE_CVGALPHA choices, this thread's (tlAlphaOff, tlCvgMixed): read once, the body uses these */
    const bool rtUseAlpha = sUseAlpha && !tlAlphaOff, rtCvgMixed = tlCvgMixed;
#define sUseAlpha rtUseAlpha
#define sCvgMixed rtCvgMixed
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
#if defined(MIYOO)
    if (miny < tlRowLo) miny = tlRowLo; /* this thread's rows (a split share): a y scissor */
    if (maxy > tlRowHi) maxy = tlRowHi;
#endif
    if (minx >= maxx || miny >= maxy) return -1;
    /* Barycentrics = edge/area with SIGNED area: inside is all-w >= 0 for
     * either winding, no separate sign handling needed. (Divide sits after
     * the bbox reject so off-screen triangles don't pay for it.) */
    const float inv_area = 1.0f / area;

    /* Barycentric x-gradients (normalized by the signed area, so inside is
     * simply all-w >= 0 whichever way the triangle winds). */
    const float w0dx = (v2->y - v1->y) * inv_area;
    const float w1dx = (v0->y - v2->y) * inv_area;
    const float w2dx = (v1->y - v0->y) * inv_area;

    const int ek0 = w0dx > 0.0f ? 1 : w0dx < 0.0f ? -1 : 0; /* edge slope signs (0: flat or NaN) */
    const int ek1 = w1dx > 0.0f ? 1 : w1dx < 0.0f ? -1 : 0;
    const int ek2 = w2dx > 0.0f ? 1 : w2dx < 0.0f ? -1 : 0;
    const float fx = minx + 0.5f;
    const float e0c = (fx - v1->x) * (v2->y - v1->y), e0x = v2->x - v1->x, e0y = v1->y; /* edge(v1,v2) */
    const float e1c = (fx - v2->x) * (v0->y - v2->y), e1x = v0->x - v2->x, e1y = v2->y; /* edge(v2,v0) */
    const float e2c = (fx - v0->x) * (v1->y - v0->y), e2x = v1->x - v0->x, e2y = v0->y; /* edge(v0,v1) */
    OwnTri oe; /* GE_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    if (sOwnFast < 0) { const char *e = getenv("GE_OWNFAST"); sOwnFast = e && e[0] == '1'; } /* off since the t10 Bench (0.25 ms slower on) */
    const bool ownfast = sOwnFast > 0; /* GE_OWNFAST: own_row */
    if (own) {
        const float s = area > 0.0f ? 1.0f : -1.0f;
        own_edge(&oe.e[0], v1, v2, s); own_edge(&oe.e[1], v2, v0, s); own_edge(&oe.e[2], v0, v1, s);
    }
    if (sEdgeSkip < 0) { const char *e = getenv("GE_EDGESKIP"); sEdgeSkip = e && e[0] == '1'; } /* default off (Bench 2) */
    /* GE_EDGESKIP: per right-hand edge, P and M of edge_end_inside (unused when off or a left edge) */
    const bool eskip = sEdgeSkip > 0;
    float ep0 = 0.0f, em0 = 0.0f, ep1 = 0.0f, em1 = 0.0f, ep2 = 0.0f, em2 = 0.0f;
    if (eskip) {
        const int en = maxx - 1 - minx;
        if (ek0 < 0) edge_end_setup(en, w0dx, &ep0, &em0);
        if (ek1 < 0) edge_end_setup(en, w1dx, &ep1, &em1);
        if (ek2 < 0) edge_end_setup(en, w2dx, &ep2, &em2);
    }
    /* The row ends: the same in the scan for the first covered row (GE_EMPTYTRI) and the row loop. */
#define EDGE_SOLVE(B, K, D, EP, EM)                                                                      \
        if (K > 0) {                                                                                    \
            if (B < 0.0f) {                                                                             \
                float q = -B / D - 1e-5f; if (q > 1e6f) q = 1e6f;                                       \
                int a = minx + iceilf(q);                                                               \
                if (a > xs) xs = a;                                                                     \
            }                                                                                           \
        } else if (K < 0) {                                                                             \
            if (!eskip || !edge_end_inside(B, EP, EM)) { /* GE_EDGESKIP: the box's last pixel inside */ \
                float q = -B / D + 1e-5f; if (q > 1e6f) q = 1e6f;                                       \
                int a = minx + 1 + ifloorf(q);                                                          \
                if (a < xe) xe = a;                                                                     \
            }                                                                                           \
        } else if (B < 0.0f) {                                                                          \
            continue; /* row entirely outside this edge */                                              \
        }
#define TRI_ROW_ENDS() \
        const float py = y + 0.5f;                                                                                     \
        /* Row bases recomputed exactly each row (no row-to-row drift). */                                             \
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;                                                          \
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;                                                          \
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;                                                          \
\
        /* Solve b_i + (x - minx) * w_i_dx >= 0 for the pixel interval. */                                             \
        int xs = minx, xe = maxx;                                                                                      \
        /* Bias both bounds toward inclusion: a 1-px overlap between neighbouring triangles is                         \
         * invisible, a 1-px crack is not. The snap loop below restores exactness on the left edge.                    \
         * A near-horizontal edge (|ds| ~ 1 ulp) on a large off-screen polygon makes the quotient                      \
         * ~1e11: the int conversion saturates to INT_MAX and `minx + 1 + INT_MAX` wrapped negative,                   \
         * so xe went to INT_MIN and the WHOLE triangle vanished for a frame (pre-existing; round                      \
         * 13). Clamping the float first is exact for every quotient that fits and just means "way                     \
         * past the screen" for the ones that do not. Each edge's slope sign is the triangle's                         \
         * (ek0..ek2, above; round 20: the three edges unrolled, the same operations in the same                       \
         * order, without two float compares per edge per row). */                                                     \
        EDGE_SOLVE(b0, ek0, w0dx, ep0, em0)                                                                            \
        EDGE_SOLVE(b1, ek1, w1dx, ep1, em1)                                                                            \
        EDGE_SOLVE(b2, ek2, w2dx, ep2, em2)                                                                            \
        if (xs >= xe) continue;                                                                                        \
        /* Snap against rounding from the divisions above so the first pixel                                           \
         * really is inside per the linear edge functions. */                                                          \
        if (own) { /* GE_FILLRULE: the row's ends by the owned-edge test, one pixel either side (see own_edge) */      \
            xs = xs > minx ? xs - 1 : minx; xe = xe < maxx ? xe + 1 : maxx;                                            \
            if (ownfast) own_row(&oe, py, &xs, &xe);                                                                   \
            else {                                                                                                     \
                while (xs < xe && !own_px(&oe, xs, py)) xs++;                                                          \
                while (xe > xs && !own_px(&oe, xe - 1, py)) xe--;                                                      \
            }                                                                                                          \
        } else                                                                                                         \
        while (xs < xe) {                                                                                              \
            const float o = (float)(xs - minx);                                                                        \
            if (b0 + o * w0dx >= 0 && b1 + o * w1dx >= 0 && b2 + o * w2dx >= 0) break;                                 \
            xs++;                                                                                                      \
        }                                                                                                              \
        if (xs >= xe) continue;                                                                                        

    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1]))
                             ? (sTexOverride ? sTexOverride : &sTex[sBoundTex[0]]) : NULL;
    if (tex && !tex->valid) {
        /* A textured draw without a texture comes out as a flat block of the combiner's colour (OoT's
         * v35 sunset bar, a texture cache reset under the draw's own imports). Said on the console a few
         * times a session, so a device run catches it; GE_UNTEXCOUNT=1 counts them all (the stress sweeps). */
        static int sUntexLogs SAVESTATE_EXCLUDE, sCountAll = -1;
        static unsigned long sUntexTotal SAVESTATE_EXCLUDE;
#if defined(MIYOO)
        if (!tlSplit) { /* the game thread counted this triangle already */
#endif
        if (sCountAll < 0) sCountAll = getenv("GE_UNTEXCOUNT") != NULL;
        sUntexTotal++;
        if (sUntexLogs < 4 || (sCountAll && (sUntexTotal & (sUntexTotal - 1)) == 0)) {
            sUntexLogs++;
            port_log_always("gfx: untextured draw: tex %u not valid (bound %u/%u), %lu so far\n", (unsigned)sBoundTex[0],
                            (unsigned)sBoundTex[0], (unsigned)sBoundTex[1], sUntexTotal);
        }
#if defined(MIYOO)
        }
#endif
        tex = NULL;
    }
    /* GE_EMPTYTRI (2026-10-05, the Cradle's lattice): find the first row with a pixel BEFORE the rest of the setup (the
     * filter, colours, gradients, the lean rows' block: ~1,000 instructions). A quarter of the triangles the Cradle's
     * platform sends here draw no pixel (rails and wires thinner than a pixel, between pixel centres): they return
     * here, after the untextured-draw count above (per draw, as before). Exact: rows before y0 end in "continue" in
     * the row loop before anything is written or marked (def_row runs only on a row with pixels), the loop then
     * starts at y0, and the rest of the setup leaves nothing behind for a triangle that draws nothing (alpha_rows is
     * a cache, mip_lod_init and the switches read the environment once, sDbg* are read only after a pixel changed).
     * GE_EMPTYTRI=0: the old order. */
    if (sEmptyTri < 0) { const char *e = getenv("GE_EMPTYTRI"); sEmptyTri = !(e && e[0] == '0'); }
    int y0 = miny;
    if (sEmptyTri) {
        for (; y0 < maxy; y0++) {
            const int y = y0;
            TRI_ROW_ENDS()
            break;
        }
        if (y0 >= maxy) return 0;
    }

    int tmask_u = tex ? pow2mask(tex->w) : -1, tmask_v = tex ? pow2mask(tex->h) : -1; /* per span with mips */
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
#if defined(MIYOO)
    if (!tlSplit)
#endif
    sDbgScr = sDbgUvc = -1.0f;
    if (use_bilin && gfx_soft_magbilin && !sN64Filter) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
#if defined(MIYOO)
        if (!tlSplit) { sDbgScr = scr; sDbgUvc = uvc; }
#else
        sDbgScr = scr; sDbgUvc = uvc;
#endif
    }
#if defined(MIYOO)
    if (!tlSplit)
#endif
    sDbgBilin = use_bilin;
    const bool cvg_nb = sCvgMixed; /* GE_CVGALPHA's mixed triangle: cut at 32, never blended (see raster_tri) */
    const int amin = cvg_nb ? 32 : tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
    const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
    const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn;
    const bool rgb_texa = sCurShader && sCurShader->rgb_texa;
    const void *texpx = tex ? tex->px : NULL; const bool t16 = tex && tex->is16;
    /* Flat shade (one vertex colour) skips the per-pixel colour stepping. */
    const bool flat = v0->r == v1->r && v1->r == v2->r && v0->g == v1->g && v1->g == v2->g &&
                      v0->b == v1->b && v1->b == v2->b && v0->a == v1->a && v1->a == v2->a;
    /* Per-vertex base colour K (see SoftVtx): only textured triangles with some K != 0. */
    const bool vk = tex && ((v0->kr | v0->kg | v0->kb | v0->ka | v1->kr | v1->kg | v1->kb | v1->ka |
                             v2->kr | v2->kg | v2->kb | v2->ka) != 0);
    if (sPShade < 0) { const char *e = getenv("GE_PSHADE"); sPShade = !(e && e[0] == '0'); }
    const bool pshade = sPShade && tex && ztest && !sUiMode && !flat && v0->invw > 0.0f && v1->invw > 0.0f && v2->invw > 0.0f;
    const float pm0 = pshade ? v0->invw : 1.0f, pm1 = pshade ? v1->invw : 1.0f, pm2 = pshade ? v2->invw : 1.0f;
    const float kr0 = v0->kr * pm0, kr1 = v1->kr * pm1, kr2 = v2->kr * pm2, kg0 = v0->kg * pm0, kg1 = v1->kg * pm1, kg2 = v2->kg * pm2;
    const float kb0 = v0->kb * pm0, kb1 = v1->kb * pm1, kb2 = v2->kb * pm2, ka0 = v0->ka * pm0, ka1 = v1->ka * pm1, ka2 = v2->ka * pm2;

    /* Vertex fields as locals (device round 13): the previous row's framebuffer
     * stores are an aliasing barrier under -fno-strict-aliasing, so GCC was
     * reloading -- and ARM->VFP transferring, ~10 stall cycles each -- all 18 of
     * these on EVERY scanline (12 ldrb + 12 vmov + 12 vcvt in the y-loop).
     * uint8->float and float copies are exact, so every use is bit-identical. */
    const float z0 = v0->z, z1 = v1->z, z2 = v2->z;
    const float r0 = v0->r * pm0, r1 = v1->r * pm1, r2 = v2->r * pm2, g0 = v0->g * pm0, g1 = v1->g * pm1, g2 = v2->g * pm2;
    const float bl0 = v0->b * pm0, bl1 = v1->b * pm1, bl2 = v2->b * pm2, a0 = v0->a * pm0, a1 = v1->a * pm1, a2 = v2->a * pm2;
    const float iw0 = v0->invw, iw1 = v1->invw, iw2 = v2->invw;

    const bool persp = tex && iw0 > 0.0f && iw1 > 0.0f && iw2 > 0.0f;
    const float uw0 = persp ? v0->u * iw0 : v0->u, uw1 = persp ? v1->u * iw1 : v1->u, uw2 = persp ? v2->u * iw2 : v2->u;
    const float vw0 = persp ? v0->v * iw0 : v0->v, vw1 = persp ? v1->v * iw1 : v1->v, vw2 = persp ? v2->v * iw2 : v2->v;

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
    float ckrdx = 0.0f, ckgdx = 0.0f, ckbdx = 0.0f, ckadx = 0.0f;
    int32_t kdq_r_t = 0, kdq_g_t = 0, kdq_b_t = 0, kdq_a_t = 0;
    if (vk) {
        ckrdx = w0dx*kr0 + w1dx*kr1 + w2dx*kr2; ckgdx = w0dx*kg0 + w1dx*kg1 + w2dx*kg2;
        ckbdx = w0dx*kb0 + w1dx*kb1 + w2dx*kb2; ckadx = w0dx*ka0 + w1dx*ka1 + w2dx*ka2;
        kdq_r_t = sat_i32(ckrdx * 65536.0f); kdq_g_t = sat_i32(ckgdx * 65536.0f);
        kdq_b_t = sat_i32(ckbdx * 65536.0f); kdq_a_t = sat_i32(ckadx * 65536.0f);
    }
    /* Fixed-point deltas are invariant too (were rebuilt every 16-px span). */
    const int32_t zdq = span_zdq(zdx);
    const int32_t rdq_t = sat_i32(crdx * 65536.0f), gdq_t = sat_i32(cgdx * 65536.0f), bdq_t = sat_i32(cbdx * 65536.0f), adq_t = sat_i32(cadx * 65536.0f);

    /* Flat triangles: the shade colour the pixel loop would clamp every pixel
     * (rq = sat_i32(cr * 65536.0f) with cr == r0 on every row and span, since
     * every colour step is 0), clamped once. (MK64 perf round 24.) */
    const int fsr = clamp8_sh16(sat_i32(r0 * 65536.0f)), fsg = clamp8_sh16(sat_i32(g0 * 65536.0f));
    const int fsb = clamp8_sh16(sat_i32(bl0 * 65536.0f)), fsa = clamp8_sh16(sat_i32(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const int zbias = sZDecal ? 4 : 0; const bool dmask = sDepthMask, texadd = sTexAdd;
    const bool texlerp = sTexLerp; const int lkr = sLerpR, lkg = sLerpG, lkb = sLerpB;
    const bool cm_mod = rgb_tex && rgb_in && !texlerp && !texadd && !rgb_texa; /* texture x shade: see SPAN_LOOP_CM */
    int tw = tex ? tex->w : 0, th = tex ? tex->h : 0; /* per span with mips */
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
    /* Mipmaps (see build_mips): perspective 3D, opaque or blended (the file select's
     * folders and crests blend); tagged HUD and menus keep the full image. The level is
     * chosen per span from the larger of the x and y texel steps (the RDP's LOD rule), so
     * the y gradients of u/w, v/w and 1/w are needed too. */
    mip_lod_init();
    /* GE_MIPLOD 1: only the RDP's LOD draws, with more than one level (as the N64: the rest read tile 0) */
    const bool mip = tex && tex->nlev > 1 && persp && !sUiMode &&
                     (sMipLod == 1 ? sTexLod && sMaxLevel > 0 : (!sN64Filter || sTexLod));
    /* the RDP's rule for this draw's level (mip_pick_n64): GE_MIPLOD on and a G_TL_LOD draw; 2 and 3 leave the
     * other draws (the sky, characters, the gun) on the old pick */
    const bool nlod = sMipLod > 0 && sTexLod;
    /* the sprite span skip (span_clear): blended, alpha from the texture, no K, level 0 */
    if (sMipSkip < 0) { const char *e = getenv("GE_MIPSKIP"); sMipSkip = !(e && e[0] == '0'); }
#if defined(MIYOO)
    const AlphaRows *const arows = (tex && sUseAlpha && a_tex && !vk &&
                                    (!mip || (sMipSkip && tex->mw == tex->w && tex->mh == tex->h)))
                                       ? (tlSplit ? tlSplit->arows : alpha_rows(sBoundTex[0], tex)) : NULL;
#else
    const AlphaRows *const arows = (tex && sUseAlpha && a_tex && !vk &&
                                    (!mip || (sMipSkip && tex->mw == tex->w && tex->mh == tex->h)))
                                       ? alpha_rows(sBoundTex[0], tex) : NULL;
#endif
    const void *const px_l0 = texpx;
    const int tw_l0 = mip ? tex->mw : tw, th_l0 = mip ? tex->mh : th;
    const int tmu_l0 = mip ? pow2mask(tex->mw) : tmask_u, tmv_l0 = mip ? pow2mask(tex->mh) : tmask_v;
    float su = (float)tw, sv = (float)th; /* texels per unit u/v at this span's level: the full image's, halved per level */
    const bool bilin_l0 = use_bilin;
    const bool tbil = nlod && bilin_l0; /* GE_MIPLOD: a two-level span filters both levels (tribil_texel) */
    if (sLodFast < 0) { const char *e = getenv("GE_LODFAST"); sLodFast = !(e && e[0] == '0'); }
    const bool lodfast = sLodFast > 0;                  /* GE_LODFAST: mip_pick_fast */
    const float ftw_l0 = (float)tw_l0, fth_l0 = (float)th_l0;
    bool mtri = false; /* this span blends level L (texpx..) with L+1 (texpx2..) by mfrac/256 */
    int mlvl = 0;      /* this span's level L (span_clear_mip) */
    const void *texpx2 = NULL;
    int tw2 = 1;
    uint32_t mfrac = 0;
    float uwdy = 0.0f, vwdy = 0.0f, swdy = 0.0f;
    if (mip) {
        const float w0dy = (v1->x - v2->x) * inv_area, w1dy = (v2->x - v0->x) * inv_area, w2dy = (v0->x - v1->x) * inv_area;
        uwdy = w0dy * uw0 + w1dy * uw1 + w2dy * uw2;
        vwdy = w0dy * vw0 + w1dy * vw1 + w2dy * vw2;
        swdy = w0dy * iw0 + w1dy * iw1 + w2dy * iw2;
    }
    (void)px_l0;

    /* Row edge bases (device round 13): edge(a,b,fx,py) is
     *   (fx - a.x)*(b.y - a.y) - (py - a.y)*(b.x - a.x)
     * and fx never changes with y, so the first product is a whole-triangle
     * constant and (b.x - a.x) is triangle-invariant. Per row only the second
     * product and the final subtract/scale remain -- the SAME operations in the
     * same order as edge() (bit-identical, still no row-to-row accumulation, so
     * shared edges stay crack-free), minus 12 vertex loads + 9 subs + 3 muls. */
    const SoftTex *gtex = (sGrain.id != 0 && tex && persp && ztest && (cm_mod || (vk && rgb_tex)) && !sUseAlpha && !sUiMode) ? &sTex[sGrain.id] : NULL;
    if (gtex && !gtex->valid) gtex = NULL;
    const int gmu = gtex ? pow2mask(gtex->w) : -1, gmv = gtex ? pow2mask(gtex->h) : -1;
    const int32_t gks = sGrain.ks, gkt = sGrain.kt, gos = sGrain.os, got = sGrain.ot;
#if defined(HH_CM_NEON)
    if (sHotRow < 0) { const char *e = getenv("GE_HOTROW"); sHotRow = !(e && e[0] == '0'); }
    if (sLeanRow < 0) { const char *e = getenv("GE_LEANROW"); sLeanRow = !(e && e[0] == '0'); }
    if (sLeanVk < 0) { const char *e = getenv("GE_LEANVK"); sLeanVk = !(e && e[0] == '0'); }
    if (sMipNeon < 0) sMipNeon = !(getenv("GE_MIPNEON") && getenv("GE_MIPNEON")[0] == '0');
    if (sVkNeon < 0) { const char *e = getenv("GE_VKNEON"); sVkNeon = !(e && e[0] == '0'); }
    const bool hotrow = sHotRow && tex && ztest && cm_mod && !vk && !sUseAlpha && !sUiMode && sCmNeon && sMipNeon && !gtex;
    /* VK rows: raster_tri's vk branch takes vk_bilin/vk_mip_span_neon for every span of these */
    if (sVkaNeon < 0) { const char *e = getenv("GE_VKANEON"); sVkaNeon = !(e && e[0] == '0'); }
    const bool hotvk = sHotRow && vk && (!sUseAlpha || (sVkaNeon && !cvg_nb)) && rgb_tex && sVkNeon && sMipNeon && !gtex;
    /* GE_LEANNOZ (2026-10-01): hot_row's case without the depth test -- raster_tri's spans called
     * cm_bilin_span_neon_noz, or the scalar loops for the nearest and two-level mip texels -- through the lean rows,
     * when both axes repeat on a power of two (every span then takes their inline loops: no fallback needed) */
    if (sLeanNoz < 0) { const char *e = getenv("GE_LEANNOZ"); sLeanNoz = !(e && e[0] == '0'); }
    const bool hotnoz = sLeanNoz && sLeanRow && sHotRow && tex && !ztest && cm_mod && !vk && !sUseAlpha && !sUiMode &&
                        sCmNeon && sMipNeon && !gtex && ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmu_l0 >= 0 && tmv_l0 >= 0;
    /* GE_LEANBLEND (2026-10-01): the blended texture x shade rows (explosions and smoke, cut-outs at a distance) --
     * raster_tri's spans called the span skip and cm_bilin_blend_span_neon / cm_mip_blend_span_neon a span */
    if (sLeanBlend < 0) { const char *e = getenv("GE_LEANBLEND"); sLeanBlend = !(e && e[0] == '0'); }
    if (sLeanNp2 < 0) { const char *e = getenv("GE_LEANNP2"); sLeanNp2 = !(e && e[0] == '0'); }
    if (sLeanAsm < 0) { const char *e = getenv("GE_LEANASM"); sLeanAsm = e && e[0] == '1'; } /* default off (t6 Bench) */
    if (sCmbNeon < 0) { const char *e = getenv("GE_CMBNEON"); sCmbNeon = !(e && e[0] == '0'); }
    const bool hotblend = sLeanBlend && sLeanRow && sHotRow && tex && ztest && cm_mod && !vk && sUseAlpha && !cvg_nb &&
                          !sUiMode && sCmNeon && sMipNeon && sCmbNeon && !gtex;
    HotTri ht;
    if (hotrow || hotvk || hotnoz || hotblend) {
        ht.fbuf = fbuf; ht.zb = zb; ht.zbias = zbias; ht.dmask = dmask; ht.persp = persp; ht.mip = mip; ht.t16 = t16;
        ht.bilin_l0 = bilin_l0; ht.tbil = tbil; ht.nlod = nlod; ht.lodfast = lodfast;
        if (sTbilPair < 0) { const char *e = getenv("GE_TBILPAIR"); sTbilPair = !(e && e[0] == '0'); }
        if (sBilPair < 0) { const char *e = getenv("GE_BILPAIR"); sBilPair = !(e && e[0] == '0'); }
        if (sLodLean < 0) { const char *e = getenv("GE_LODLEAN"); sLodLean = !(e && e[0] == '0'); }
        ht.lodlean = sLodLean > 0 && lodfast;
        if (sTbilSpan < 0) { const char *e = getenv("GE_TBILSPAN"); sTbilSpan = !(e && e[0] == '0'); }
        if (sBilSpan < 0) { const char *e = getenv("GE_BILSPAN"); sBilSpan = !(e && e[0] == '0'); }
        if (sClampSpan < 0) { const char *e = getenv("GE_CLAMPSPAN"); sClampSpan = !(e && e[0] == '0'); }
        ht.tbilpair = sTbilPair > 0; ht.bilpair = sBilPair > 0; ht.ftw_l0 = ftw_l0; ht.fth_l0 = fth_l0; ht.tex = tex; ht.ws = ws; ht.wt = wt;
        ht.tw_l0 = tw_l0; ht.th_l0 = th_l0; ht.tmu_l0 = tmu_l0; ht.tmv_l0 = tmv_l0;
        ht.texpx = texpx; ht.tw = tw; ht.th = th; ht.tmask_u = tmask_u; ht.tmask_v = tmask_v; ht.su = su; ht.sv = sv;
        ht.zdx = zdx; ht.crdx = crdx; ht.cgdx = cgdx; ht.cbdx = cbdx; ht.cadx = cadx;
        ht.uwdx = uwdx; ht.vwdx = vwdx; ht.swdx = swdx; ht.uwdy = uwdy; ht.vwdy = vwdy; ht.swdy = swdy;
        ht.zdq = zdq; ht.rdq = rdq_t; ht.gdq = gdq_t; ht.bdq = bdq_t; ht.adq = adq_t; ht.pshade = pshade;
        ht.ztest = ztest; ht.texa = rgb_texa; ht.alpha = sUseAlpha; ht.a_tex = a_tex; ht.amin = amin;
        ht.a_in = a_in; ht.arows = arows;
        ht.ckrdx = ckrdx; ht.ckgdx = ckgdx; ht.ckbdx = ckbdx; ht.ckadx = ckadx;
        ht.kdq_r = kdq_r_t; ht.kdq_g = kdq_g_t; ht.kdq_b = kdq_b_t; ht.kdq_a = kdq_a_t;
        if (sSpanVec < 0) { const char *e = getenv("GE_SPANVEC"); sSpanVec = !(e && e[0] == '0'); }
        if (sZSkip < 0) { const char *e = getenv("GE_ZSKIP"); sZSkip = !(e && e[0] == '0'); }
        ht.dp = (float32x4_t){ uwdx, vwdx, swdx, zdx }; ht.dc = (float32x4_t){ cbdx, cgdx, crdx, cadx };
        ht.dk = (float32x4_t){ ckbdx, ckgdx, ckrdx, ckadx };
        ht.zsk = sZSkip > 0 && ztest ? 4 : 0; /* GE_ZSKIP: depth-tested rows only */
    }
#endif
    uint32_t tri_px = 0; /* span pixels of this triangle (<= 57,600); folded into the global once below */
#if defined(HH_CM_NEON)
    if (sTbilNeon < 0) { const char *e = getenv("GE_TBILNEON"); sTbilNeon = !(e && e[0] == '0'); }
    if (sLeanClamp < 0) { const char *e = getenv("GE_LEANCLAMP"); sLeanClamp = !(e && e[0] == '0'); }
#endif
#if defined(HH_CM_NEON)
    if (sLeanTri < 0) { const char *e = getenv("GE_LEANTRI"); sLeanTri = e && e[0] == '1'; }
    if (sLeanTri && sLeanRow && (hotrow || hotnoz || hotblend || (hotvk && sLeanVk))) { /* the row loop in hot_tri_lean (GE_LEANTRI) */
        const LeanTri lt = { minx, maxx, miny, maxy, ek0, ek1, ek2, sW,
                             inv_area, w0dx, w1dx, w2dx, e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y,
                             z0, z1, z2, r0, r1, r2, g0, g1, g2, bl0, bl1, bl2, a0, a1, a2,
                             uw0, uw1, uw2, vw0, vw1, vw2, iw0, iw1, iw2,
                             kr0, kr1, kr2, kg0, kg1, kg2, kb0, kb1, kb2, ka0, ka1, ka2, flat, persp, own, oe };
        return hotvk ? hot_tri_vk_lean(&ht, &lt) : hotblend ? hot_tri_blend_lean(&ht, &lt) : hot_tri_lean(&ht, &lt);
    }
    const bool spanvec = sSpanVec > 0; /* GE_SPANVEC: the lean rows' _v set-up (set with the HotTri, read with it) */
    if (sFastRows < 0) { const char *e = getenv("GE_FASTROWS"); sFastRows = e && e[0] == '1'; } /* default off (2026-10-10) */
    if (sFastRows > 0 && !eskip && sLeanRow && (hotrow || hotnoz || hotblend || (hotvk && sLeanVk))) { /* GE_FASTROWS */
        FastRows fr;
        fr.va0 = (float32x4_t){ z0, r0, g0, bl0 }; fr.vb0 = (float32x4_t){ a0, uw0, vw0, iw0 };
        fr.va1 = (float32x4_t){ z1, r1, g1, bl1 }; fr.vb1 = (float32x4_t){ a1, uw1, vw1, iw1 };
        fr.va2 = (float32x4_t){ z2, r2, g2, bl2 }; fr.vb2 = (float32x4_t){ a2, uw2, vw2, iw2 };
        fr.vk0 = (float32x4_t){ kr0, kg0, kb0, ka0 }; fr.vk1 = (float32x4_t){ kr1, kg1, kb1, ka1 };
        fr.vk2 = (float32x4_t){ kr2, kg2, kb2, ka2 };
        fr.fa = (float32x4_t){ 0.0f, r0, g0, bl0 }; fr.fb = (float32x4_t){ a0, 0.0f, 0.0f, 0.0f };
        fr.e0c = e0c; fr.e0x = e0x; fr.e0y = e0y; fr.e1c = e1c; fr.e1x = e1x; fr.e1y = e1y;
        fr.e2c = e2c; fr.e2x = e2x; fr.e2y = e2y; fr.inv_area = inv_area; fr.w0dx = w0dx; fr.w1dx = w1dx; fr.w2dx = w2dx;
        fr.minx = minx; fr.maxx = maxx; fr.sw = sW; fr.ek0 = ek0; fr.ek1 = ek1; fr.ek2 = ek2;
        fr.flat = flat; fr.persp = persp; fr.own = own; fr.spanvec = spanvec; fr.oe = &oe;
        return (int)(hotvk ? fast_rows_vk(&ht, &fr, y0, maxy) : hotblend ? fast_rows_blend(&ht, &fr, y0, maxy)
                                                                : fast_rows_cm(&ht, &fr, y0, maxy));
    }
#endif

#if defined(MIYOO)
    /* the second core: the bottom share of a big triangle's rows (see SplitJob). Not on the worker itself, and not
     * while a deferred batch is marked (DEF_OP_MARK draws nothing and counts its pixels in one sum); def_row's
     * other ops read and write only their own row's marks (sPend's row, sPendRow[y], sCovRow[y]). */
    int yend = maxy;
    SplitJob job;
    bool split = false;
    if (!tlSplit && tlRowHi == 0x7fffffff && sDefOp != DEF_OP_MARK && miyoo_worker_ready() && maxy - y0 >= sSplitRows) {
        const int ym = split_row(v0, v1, v2, area, minx, maxx, y0, maxy);
        if (ym > y0 && ym < maxy) {
            /* a switch the rows read and set on first use, set here first so the two threads never race on it */
            if (sCmNoZ < 0) sCmNoZ = !(getenv("GE_CMNOZ") && getenv("GE_CMNOZ")[0] == '0');
            job.v0 = v0; job.v1 = v1; job.v2 = v2; job.use_z = use_z; job.ylo = ym; job.arows = arows;
            job.bv = NULL; job.alpha_off = tlAlphaOff; job.cvg_mixed = tlCvgMixed;
            split_post(&job);
            yend = ym;
            split = true;
        }
    }
    for (int y = y0; y < yend; y++) {
#else
    for (int y = y0; y < maxy; y++) {
#endif
        TRI_ROW_ENDS()
        if (sDefOp != DEF_OP_NONE && def_row(y, xs, xe)) continue;
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
        float ckr = 0.0f, ckg = 0.0f, ckb = 0.0f, cka = 0.0f;
        if (vk) { ckr = w0*kr0 + w1*kr1 + w2*kr2; ckg = w0*kg0 + w1*kg1 + w2*kg2; ckb = w0*kb0 + w1*kb1 + w2*kb2; cka = w0*ka0 + w1*ka1 + w2*ka2; }

        int idx = y * sW + xs;

        /* 1/w at the span start. The old loop recomputed it every span, but
         * the next span's start w IS this span's end w (same float op), so
         * carry the reciprocal forward: one divide per span instead of two.
         * Only perspective spans need it -- round 12 had hoisted the divides
         * out of that test, so untextured/affine spans paid a vdiv for nothing. */
        float rw = 0.0f;
        if (persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
#if defined(HH_CM_NEON)
        if (hotnoz) { (spanvec ? hot_row_lean_v : hot_row_lean)(&ht, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw); continue; }
        if (hotblend) { (spanvec ? hot_row_blend_lean_v : hot_row_blend_lean)(&ht, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw); continue; }
        if (hotrow) {
            if (sLeanRow) (spanvec ? hot_row_lean_v : hot_row_lean)(&ht, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else hot_row(&ht, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            continue;
        }
        if (hotvk) {
            if (sLeanRow && sLeanVk)
                (spanvec ? hot_row_vk_lean_v : hot_row_vk_lean)(&ht, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
            else hot_row_vk(&ht, idx, xs, xe, zf, cr, cg, cb, ca, uw, vw, sw, rw, ckr, ckg, ckb, cka);
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
            if (mip) {
                const float dux = du * (float)tw_l0, dvx = dv * (float)th_l0;
                const float duy = (uwdy - u_a * swdy) * rw * (float)tw_l0, dvy = (vwdy - v_a * swdy) * rw * (float)th_l0;
                const float rx = dux * dux + dvx * dvx, ry = duy * duy + dvy * dvy;
                union { float f; uint32_t u; } rb;
                rb.f = (rx > ry ? rx : ry) * sMipScale;
                /* log2(rho) = (exponent + log2(mantissa)) / 2, log2(m) ~ m - 1 */
                const int e = (int)((rb.u >> 23) & 0xff) - 127;
                const float l2 = ((float)e + (float)(rb.u & 0x7fffffu) * (1.0f / 8388608.0f)) * 0.5f;
                int lvl = 0;
                uint32_t fr = 0;
                bool ntri = false;
                if (nlod) {
                    if (lodfast)
                        mip_pick_fast(tex, tw_l0, th_l0, ftw_l0, fth_l0, du, dv, u_a, v_a, uwdy, vwdy, swdy, rw, rw_b, fspan,
                                      &lvl, &fr, &ntri);
                    else
                        mip_pick_n64(tex, tw_l0, th_l0, du, dv, u_a, v_a, uwdy, vwdy, swdy, rw, rw_b, fspan, &lvl, &fr, &ntri);
                } else {
                if (l2 > 0.0f) { lvl = l2 < 7.0f ? (int)l2 : 7; fr = (uint32_t)((l2 - (float)lvl) * 256.0f); }
                if (lvl >= tex->nlev - 1) { lvl = tex->nlev - 1; fr = 0; }
                /* blend only across the middle half of each level step (the outer quarters
                 * take one level): half the spans fetch twice, still no visible step */
                if (!sTrilin && fr >= 192u) { lvl++; fr = 0; }
                }
                mlvl = lvl;
                texpx = tex->lev[lvl];
                tw = tw_l0 >> lvl; th = th_l0 >> lvl;
                {
                    /* 1 / 2^lvl built directly (exact; the float divide was ~4% of raster_tri) */
                    union { uint32_t u; float f; } inv = { (uint32_t)(127 - lvl) << 23 };
                    su = (float)tex->w * inv.f; sv = (float)tex->h * inv.f;
                }
                tmask_u = tmu_l0 >= 0 ? tw - 1 : -1; tmask_v = tmv_l0 >= 0 ? th - 1 : -1;
                mtri = nlod ? ntri : sTrilin ? fr > 0u : fr > 64u;
                if (mtri) {
                    texpx2 = tex->lev[lvl + 1];
                    tw2 = tw >> 1;
                    mfrac = (nlod || sTrilin) ? fr : (fr - 64u) * 2u;
                }
                use_bilin = bilin_l0 && (lvl == 0 || (nlod && lvl < sMipFar)) && !mtri; /* GE_MIPLOD: every level filtered */
            }
            int grain_f = 256; /* < 256: this span lerps the detail grain in (gtex) */
            if (gtex) {
                const float gdux = du * (float)tex->w, gdvx = dv * (float)tex->h;
                const float gduy = (uwdy - u_a * swdy) * rw * (float)tex->w, gdvy = (vwdy - v_a * swdy) * rw * (float)tex->h;
                const float grx = gdux * gdux + gdvx * gdvx, gry = gduy * gduy + gdvy * gdvy;
                const float grho2 = grx > gry ? grx : gry;
                if (grho2 < 1.0f) {
                    float grho = sqrtf(grho2);
                    if (grho < sGrain.minf) grho = sGrain.minf;
                    grain_f = (int)(grho * 256.0f);
                    if (grain_f > 255) grain_f = 255;
                }
            }
            int32_t uf = 0, vf = 0, duf = 0, dvf = 0;
            if (tex) {
#if defined(HH_CM_NEON)
                /* to_fix16 on all four at once: clamp to +-16000, * 65536, truncate -- NEON's
                 * min/max and convert give the scalar results (NaN -> 0 on both) (round 18) */
                const float fin[4] = { wrap_far(u_a * su, tw, ws), du * su, wrap_far(v_a * sv, th, wt), dv * sv };
                const int32x4_t fx = vcvtq_s32_f32(vmulq_n_f32(vminq_f32(vmaxq_f32(vld1q_f32(fin), vdupq_n_f32(-16000.0f)),
                                                                         vdupq_n_f32(16000.0f)), 65536.0f));
                uf = vgetq_lane_s32(fx, 0); duf = vgetq_lane_s32(fx, 1); vf = vgetq_lane_s32(fx, 2); dvf = vgetq_lane_s32(fx, 3);
#else
                uf = to_fix16(wrap_far(u_a * su, tw, ws)); duf = to_fix16(du * su);
                vf = to_fix16(wrap_far(v_a * sv, th, wt)); dvf = to_fix16(dv * sv);
#endif
            }
            int32_t zq = span_z_out(zf) ? span_zq_far(zf, zdx, span, zdq)
                                        : (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            int32_t rq = sat_i32(cr * 65536.0f), gq = sat_i32(cg * 65536.0f), bq = sat_i32(cb * 65536.0f), aq = sat_i32(ca * 65536.0f);
            int32_t kq_r = 0, kq_g = 0, kq_b = 0, kq_a = 0;
            if (vk) { kq_r = sat_i32(ckr * 65536.0f); kq_g = sat_i32(ckg * 65536.0f); kq_b = sat_i32(ckb * 65536.0f); kq_a = sat_i32(cka * 65536.0f); }
            int32_t rdq = rdq_t, gdq = gdq_t, bdq = bdq_t, adq = adq_t;
            int32_t kdq_r = kdq_r_t, kdq_g = kdq_g_t, kdq_b = kdq_b_t, kdq_a = kdq_a_t;
            if (pshade) { /* colour/w at both span ends (rw, rw_b: persp is set), linear between */
                const float is = 1.0f / fspan;
                const float ar = cr * rw, ag = cg * rw, ab = cb * rw, aa = ca * rw;
                rq = sat_i32(ar * 65536.0f); gq = sat_i32(ag * 65536.0f); bq = sat_i32(ab * 65536.0f); aq = sat_i32(aa * 65536.0f);
                rdq = sat_i32(((cr + crdx * fspan) * rw_b - ar) * is * 65536.0f);
                gdq = sat_i32(((cg + cgdx * fspan) * rw_b - ag) * is * 65536.0f);
                bdq = sat_i32(((cb + cbdx * fspan) * rw_b - ab) * is * 65536.0f);
                adq = sat_i32(((ca + cadx * fspan) * rw_b - aa) * is * 65536.0f);
                if (vk) {
                    const float akr = ckr * rw, akg = ckg * rw, akb = ckb * rw, aka = cka * rw;
                    kq_r = sat_i32(akr * 65536.0f); kq_g = sat_i32(akg * 65536.0f); kq_b = sat_i32(akb * 65536.0f); kq_a = sat_i32(aka * 65536.0f);
                    kdq_r = sat_i32(((ckr + ckrdx * fspan) * rw_b - akr) * is * 65536.0f);
                    kdq_g = sat_i32(((ckg + ckgdx * fspan) * rw_b - akg) * is * 65536.0f);
                    kdq_b = sat_i32(((ckb + ckbdx * fspan) * rw_b - akb) * is * 65536.0f);
                    kdq_a = sat_i32(((cka + ckadx * fspan) * rw_b - aka) * is * 65536.0f);
                }
            }

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
/* The detail grain (sGrain): the base texel (bilinear, level 0) and the grain's, lerped by grain_f. */
#define FETCH_GRAIN                                                                                    \
                    const uint32_t tb_ = use_bilin ? bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v) \
                        : texel_at(texpx, t16, wrapf(vf >> 16, th, wt, tmask_v) * tw + wrapf(uf >> 16, tw, ws, tmask_u)); \
                    const int32_t gu_ = (int32_t)(((int64_t)uf * gks) >> 16) + gos;                     \
                    const int32_t gv_ = (int32_t)(((int64_t)vf * gkt) >> 16) + got;                     \
                    const uint32_t t = lerp_argb(bilerp(gtex->px, gtex->is16, gtex->w, gtex->h, gtex->wrap_s, gtex->wrap_t, \
                                                        gu_, gv_, gmu, gmv), tb_, (uint32_t)grain_f);
/* Mipmaps: the nearest texel of level L and of L+1, lerped. Level L+1's texel is L's index
 * halved: the levels have even sizes, so repeat (mask or modulo) and clamp commute with it. */
#define FETCH_TRILIN                                                                                   \
                    uint32_t t;                                                                         \
                    if (tbil) t = tribil_texel(texpx, texpx2, t16, tw, th, ws, wt, tmask_u, tmask_v, uf, vf, mfrac); \
                    else {                                                                              \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    t = lerp_argb(texel_at(texpx, t16, iv * tw + iu),                                   \
                                  texel_at(texpx2, t16, (iv >> 1) * tw2 + (iu >> 1)), mfrac);           \
                    }
/* UI quads squeezed 4:3 -> 1:1 like the 2D sprites: area average along S. */
#define FETCH_BOX                                                                                      \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_box(texpx, t16, iv * tw, uf, duf, tw, ws, tmask_u, sUseAlpha);
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once per
 * triangle above) -- the four colour accumulators and their steps drop out of
 * the pixel loop. Same values: flat => every step is 0.
 * CM=1: the combiner is plain texture x shade (cm_mod), so the per-pixel walk
 * of the combine flags drops out. (Both MK64 perf round 24, bit-exact.) */
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
                    const int tr = (!CM && rgb_texa) ? ta : (t>>16)&0xff, tg = (!CM && rgb_texa) ? ta : (t>>8)&0xff, tb = (!CM && rgb_texa) ? ta : t&0xff; \
                    if (CM)             { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (texlerp)   { fr=lkr+((sr-lkr)*tr)/255; fg=lkg+((sg-lkg)*tg)/255; fb=lkb+((sb-lkb)*tb)/255; } \
                    else if (texadd)    { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = !a_tex ? sa : (!a_in ? ta : div255(ta*sa));                                    \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (ALPHA) {                                                                            \
                    if (fa < amin) continue;                                                            \
                    if (fa < 255 && !cvg_nb) {                                                          \
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
/* K + (vertex - K) * texel with K interpolated per pixel (see SoftVtx). */
#define SPAN_LOOP_VK(ZTEST, ALPHA, BILIN)                                                              \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq, rq += rdq, gq += gdq, bq += bdq, aq += adq, \
                 kq_r += kdq_r, kq_g += kdq_g, kq_b += kdq_b, kq_a += kdq_a) {                          \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias) continue; }                                      \
                const int sr = clamp8_sh16(rq), sg = clamp8_sh16(gq), sb = clamp8_sh16(bq), sa = clamp8_sh16(aq); \
                const int kr = clamp8_sh16(kq_r), kg = clamp8_sh16(kq_g), kb = clamp8_sh16(kq_b), ka = clamp8_sh16(kq_a); \
                FETCH_##BILIN                                                                           \
                const int ta = (t>>24)&0xff;                                                            \
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff; \
                int fr = sr, fg = sg, fb = sb, fa = sa;                                                 \
                if (rgb_tex) { fr = kr + ((sr - kr) * tr) / 255; fg = kg + ((sg - kg) * tg) / 255; fb = kb + ((sb - kb) * tb) / 255; } \
                if (a_tex) fa = ka + ((sa - ka) * ta) / 255;                                            \
                if (ALPHA) {                                                                            \
                    if (fa < amin) continue;                                                            \
                    if (fa < 255 && !cvg_nb) {                                                          \
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
            /* nearest (no mips, no HUD box): floor(u) is one of bilinear's taps, so the same test
             * is exact for it (MK64's finding, round 18) */
            if (arows && ((mip && tbil && (mlvl > 0 || mtri)) ? span_clear_mipb(arows, uf, vf, duf, dvf, span, ws, wt, tw, th, mlvl, mtri)
                          : use_bilin ? span_clear(arows, uf, vf, duf, dvf, span, ws, wt)
                                    : mip ? span_clear_mip(arows, uf, vf, duf, dvf, span, ws, wt, tw, th, mlvl, mtri)
                                          : (!boxspan && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)))) {
                idx += span; /* every tap alpha 0: the span draws nothing */
#if defined(MIYOO)
                SPANSKIP_ADD(span);
#else
                gfx_soft_spanskip_spans++; gfx_soft_spanskip_px += (uint32_t)span;
#endif
            } else if (vk && grain_f < 256 && !sUseAlpha) {
                /* the fogged ground (K + (S - K) * texel) under the RDP's detail mode: the grain lerped in */
                if (ztest) SPAN_LOOP_VK(1, 0, GRAIN) else SPAN_LOOP_VK(0, 0, GRAIN)
            } else if (vk) {
#if defined(HH_CM_NEON)
                if (sVkNeon < 0) { const char *e = getenv("GE_VKNEON"); sVkNeon = !(e && e[0] == '0'); }
                if (use_bilin && !sUseAlpha && rgb_tex && sVkNeon) {
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    const int32_t kaccv[4] = { kq_b, kq_g, kq_r, kq_a }, dkaccv[4] = { kdq_b, kdq_g, kdq_r, kdq_a };
                    vk_bilin_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, ztest,
                                       rgb_texa, &ct, vld1q_s32(accv), vld1q_s32(daccv), vld1q_s32(kaccv), vld1q_s32(dkaccv));
                    idx += span;
                } else if (!use_bilin && !sUseAlpha && rgb_tex && sVkNeon &&
                           (sMipNeon >= 0 ? sMipNeon : (sMipNeon = !(getenv("GE_MIPNEON") && getenv("GE_MIPNEON")[0] == '0')))) {
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    const int32_t kaccv[4] = { kq_b, kq_g, kq_r, kq_a }, dkaccv[4] = { kdq_b, kdq_g, kdq_r, kdq_a };
                    vk_mip_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, ztest,
                                     rgb_texa, &ct, mtri, texpx2, tw2, mfrac, vld1q_s32(accv), vld1q_s32(daccv),
                                     vld1q_s32(kaccv), vld1q_s32(dkaccv), tbil && mtri);
                    idx += span;
                } else if (sUseAlpha && !cvg_nb && rgb_tex && sVkNeon && sMipNeon &&
                           (sVkaNeon >= 0 ? sVkaNeon : (sVkaNeon = !(getenv("GE_VKANEON") && getenv("GE_VKANEON")[0] == '0')))) {
                    /* blended K spans (the Jungle's foliage): bilinear or mip, as the scalar loops below */
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    const int32_t kaccv[4] = { kq_b, kq_g, kq_r, kq_a }, dkaccv[4] = { kdq_b, kdq_g, kdq_r, kdq_a };
                    if (use_bilin)
                        vk_bilin_blend_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, ztest,
                                                 rgb_texa, &ct, vld1q_s32(accv), vld1q_s32(daccv), vld1q_s32(kaccv),
                                                 vld1q_s32(dkaccv), a_tex, amin);
                    else
                        vk_mip_blend_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, ztest,
                                               rgb_texa, &ct, mtri, texpx2, tw2, mfrac, vld1q_s32(accv), vld1q_s32(daccv),
                                               vld1q_s32(kaccv), vld1q_s32(dkaccv), a_tex, amin, tbil && mtri);
                    idx += span;
                } else
#endif
                if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_VK(1, 1, BILINEAR) else SPAN_LOOP_VK(1, 0, BILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP_VK(0, 1, BILINEAR) else SPAN_LOOP_VK(0, 0, BILINEAR) }
                } else {
                    if (ztest) { if (sUseAlpha) { if (mtri) SPAN_LOOP_VK(1, 1, TRILIN) else SPAN_LOOP_VK(1, 1, NEAREST) } else if (mtri) SPAN_LOOP_VK(1, 0, TRILIN) else SPAN_LOOP_VK(1, 0, NEAREST) }
                    else       { if (sUseAlpha) { if (mtri) SPAN_LOOP_VK(0, 1, TRILIN) else SPAN_LOOP_VK(0, 1, NEAREST) } else if (mtri) SPAN_LOOP_VK(0, 0, TRILIN) else SPAN_LOOP_VK(0, 0, NEAREST) }
                }
            } else if (grain_f < 256 && ztest && cm_mod && !boxspan) {
                /* the base magnified under the RDP's detail mode: the grain lerped in (sGrain) */
                if (flat) SPAN_LOOP_CM(1, 1, 0, GRAIN, 1, 1) else SPAN_LOOP_CM(1, 1, 0, GRAIN, 0, 1)
            } else if (tex && ztest && cm_mod && !boxspan) {
                /* the hot path: z-tested texture x shade */
#if defined(HH_CM_NEON)
                if (use_bilin && !sUseAlpha && sCmNeon) {
                    /* flat or not: a flat triangle's colour step is 0 and its rq.. are the fs* before the clamp */
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    cm_bilin_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, &ct,
                                       vld1q_s32(accv), vld1q_s32(daccv));
                    idx += span;
                } else if (use_bilin && sUseAlpha && !cvg_nb && sCmNeon &&
                           (sCmbNeon >= 0 ? sCmbNeon : (sCmbNeon = !(getenv("GE_CMBNEON") && getenv("GE_CMBNEON")[0] == '0')))) {
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    cm_bilin_blend_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, &ct,
                                             vld1q_s32(accv), vld1q_s32(daccv), a_tex, a_in, amin);
                    idx += span;
                } else if (!use_bilin && sUseAlpha && !cvg_nb && sCmNeon && sMipNeon > 0 && (sCmbNeon >= 0 ? sCmbNeon : (sCmbNeon = !(getenv("GE_CMBNEON") && getenv("GE_CMBNEON")[0] == '0')))) {
                    /* blended nearest / two-level mip texels x shade (the Cradle's distant girders) */
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    cm_mip_blend_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, &ct,
                                           mtri, texpx2, tw2, mfrac, vld1q_s32(accv), vld1q_s32(daccv), a_tex, a_in, amin, tbil && mtri);
                    idx += span;
                } else if (!use_bilin && !sUseAlpha && sCmNeon &&
                           (sMipNeon >= 0 ? sMipNeon : (sMipNeon = !(getenv("GE_MIPNEON") && getenv("GE_MIPNEON")[0] == '0')))) {
                    /* nearest or two-level mip texels x shade (flat: zero steps, as above) */
                    const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                    const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                    cm_mip_span_neon(fbuf + idx, zb + idx, span, uf, vf, duf, dvf, zq, zdq, zbias, dmask, &ct,
                                     mtri, texpx2, tw2, mfrac, vld1q_s32(accv), vld1q_s32(daccv), tbil && mtri);
                    idx += span;
                } else
#endif
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) { if (mtri) SPAN_LOOP_CM(1, 1, 1, TRILIN, 1, 1) else SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1) } else if (mtri) SPAN_LOOP_CM(1, 1, 0, TRILIN, 1, 1) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) { if (mtri) SPAN_LOOP_CM(1, 1, 1, TRILIN, 0, 1) else SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1) } else if (mtri) SPAN_LOOP_CM(1, 1, 0, TRILIN, 0, 1) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                }
#if defined(HH_CM_NEON)
            } else if (tex && !ztest && cm_mod && !boxspan && use_bilin && !sUseAlpha && sCmNeon &&
                       (sCmNoZ >= 0 ? sCmNoZ : (sCmNoZ = !(getenv("GE_CMNOZ") && getenv("GE_CMNOZ")[0] == '0')))) {
                const CmTex ct = { texpx, tw, th, ws, wt, tmask_u, tmask_v, t16 };
                const int32_t accv[4] = { bq, gq, rq, aq }, daccv[4] = { bdq, gdq, rdq, adq };
                cm_bilin_span_neon_noz(fbuf + idx, span, uf, vf, duf, dvf, &ct, vld1q_s32(accv), vld1q_s32(daccv));
                idx += span;
#endif
            } else if (tex) {
                if (boxspan) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BOX) else SPAN_LOOP(1, 1, 0, BOX) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BOX) else SPAN_LOOP(1, 0, 0, BOX) }
                } else if (flat && ztest) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 0) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 0) }
                    else           { if (sUseAlpha) { if (mtri) SPAN_LOOP_CM(1, 1, 1, TRILIN, 1, 0) else SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 0) } else if (mtri) SPAN_LOOP_CM(1, 1, 0, TRILIN, 1, 0) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 0) }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR) else SPAN_LOOP(1, 1, 0, BILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BILINEAR) else SPAN_LOOP(1, 0, 0, BILINEAR) }
                } else {
                    if (ztest) { if (sUseAlpha) { if (mtri) SPAN_LOOP(1, 1, 1, TRILIN) else SPAN_LOOP(1, 1, 1, NEAREST) } else if (mtri) SPAN_LOOP(1, 1, 0, TRILIN) else SPAN_LOOP(1, 1, 0, NEAREST) }
                    else       { if (sUseAlpha) { if (mtri) SPAN_LOOP(1, 0, 1, TRILIN) else SPAN_LOOP(1, 0, 1, NEAREST) } else if (mtri) SPAN_LOOP(1, 0, 0, TRILIN) else SPAN_LOOP(1, 0, 0, NEAREST) }
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
#undef SPAN_LOOP_VK
#undef COLSTEP_0
#undef COLSTEP_1
#undef FETCH_NEAREST
#undef FETCH_BILINEAR
#undef FETCH_BOX
#undef FETCH_TRILIN
            zf += zdx * fspan;
            cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;
            if (vk) { ckr += ckrdx * fspan; ckg += ckgdx * fspan; ckb += ckbdx * fspan; cka += ckadx * fspan; }
            uw += uwdx * fspan;
            vw += vwdx * fspan;
            sw = sw_b; rw = rw_b; /* identical to sw += swdx*span; reciprocal carried */
            x += span;
        }
    }
#if defined(MIYOO)
    if (split) {
        split_wait();
        if (job.px > 0) { tri_px += (uint32_t)job.px; gfx_soft_split_wpx += (uint32_t)job.px; }
        gfx_soft_spanskip_spans += job.skip_spans; gfx_soft_spanskip_px += job.skip_px;
        gfx_soft_split_tris++;
    }
#endif
#undef TRI_ROW_ENDS
#undef EDGE_SOLVE
#if defined(MIYOO)
#undef sUseAlpha
#undef sCvgMixed
#endif
    return (int)tri_px;
}

#if defined(MIYOO)
/* Batch shares (the second core, past single big triangles): a batch's triangles drawn by both threads at once, the
 * game thread every triangle's rows above a split row ym, the worker every triangle's rows from ym down (raster_tri
 * with this thread's y scissor, tlRowLo / tlRowHi). The rows are each triangle's own as in a split triangle, so
 * every pixel is the one the batch drawn whole gives; each band sees the triangles in the batch's order, and the
 * bands are disjoint. Small triangles, whose setup a triangle split leaves on the game thread, are shared too: one
 * in the worker's band is set up there only. raster_tri's GE_CVGALPHA choices are per thread (tlAlphaOff,
 * tlCvgMixed), its counts the game thread's, the worker's skipped spans merged after the wait, the alpha_rows
 * lookup done once beforehand (the batch has one texture: every triangle then finds that entry). GE_BATCHPX: a
 * batch's estimated work (span pixels, a triangle's setup taken as 12) below which it is drawn whole; 0: never. */
#define BATCH_MAX 512
static int sBatchPxW[BATCH_MAX] SAVESTATE_EXCLUDE, sBatchPxC[BATCH_MAX] SAVESTATE_EXCLUDE;
static int sBatchMin SAVESTATE_EXCLUDE = -1;
unsigned gfx_soft_batch_splits SAVESTATE_EXCLUDE; /* batches shared (hh_gfx.c's PERF line reads and clears it) */
/* the split row of a batch's work, or -1 for too little to share: each triangle's area spread evenly over its rows
 * plus its setup at its first row, rows [ylo, yhi) (an estimate: only the balance depends on it) */
static int batch_split_row(const SoftVtx *v, int n, int ylo, int yhi) {
    if (sBatchMin < 0) { const char *e = getenv("GE_BATCHPX"); sBatchMin = e && e[0] ? atoi(e) : 384; }
    if (sBatchMin <= 0 || yhi - ylo < 2 || yhi > 512) return -1;
    float dens[513], pm[513], quick = 0.0f;
    for (int i = 0; i < n; i++) {
        const SoftVtx *a = &v[i * 3], *b = &v[i * 3 + 1], *c = &v[i * 3 + 2];
        const float cr = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
        quick += (cr < 0.0f ? -cr : cr) * 0.5f + 12.0f;
    }
    if (!(quick >= (float)sBatchMin)) return -1; /* even unclipped too little (or NaN) */
    for (int y = ylo; y <= yhi; y++) { dens[y] = 0.0f; pm[y] = 0.0f; }
    for (int i = 0; i < n; i++) {
        const SoftVtx *a = &v[i * 3], *b = &v[i * 3 + 1], *c = &v[i * 3 + 2];
        const float y0 = fmin3(a->y, b->y, c->y), y1 = fmax3(a->y, b->y, c->y);
        if (!(y1 > (float)ylo) || !(y0 < (float)yhi)) continue; /* off the rows (or NaN) */
        const int t0 = (int)(y0 > -1e6f ? y0 : -1e6f), t1 = iceilf(y1 < 1e6f ? y1 : 1e6f);
        const int r0 = t0 > ylo ? t0 : ylo, r1 = t1 < yhi ? t1 : yhi;
        if (r0 >= r1) continue;
        const float cr = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
        const float d = (cr < 0.0f ? -cr : cr) * 0.5f / (float)(t1 - t0 > 0 ? t1 - t0 : 1);
        dens[r0] += d;
        dens[r1] -= d;
        pm[r0] += 12.0f;
    }
    float w = 0.0f, total = 0.0f;
    for (int y = ylo; y < yhi; y++) { w += dens[y]; total += (w > 0.0f ? w : 0.0f) + pm[y]; }
    if (!(total >= (float)sBatchMin)) return -1;
    const float half = total * 0.5f;
    float acc = 0.0f;
    w = 0.0f;
    for (int y = ylo; y < yhi; y++) {
        w += dens[y];
        acc += (w > 0.0f ? w : 0.0f) + pm[y];
        if (acc >= half) return y + 1 < yhi ? y + 1 : -1;
    }
    return -1;
}
/* Draw v[0..n) (n <= BATCH_MAX) shared, if worth it: res[i] = raster_tri's result for the whole triangle (-1 when
 * both bands rejected it, else the two bands' span pixels). 0: not shared (the caller draws the batch as before).
 * poll: the game thread's audio poll between triangles (gfx_soft_draw_triangles'), its time added to *away. */
static uint32_t (*sBatchPoll)(void) SAVESTATE_EXCLUDE;
static int batch_raster(const SoftVtx *v, int n, int *res, uint32_t *pollwork, uint32_t *away) {
    if (n < 2 || tlSplit || tlRowHi != 0x7fffffff || !miyoo_worker_ready() || sDefOp == DEF_OP_MARK) return 0;
    const int lo = sScY0 > 0 ? sScY0 : 0, hi = sScY1 < sH ? sScY1 : sH;
    const int ym = batch_split_row(v, n, lo, hi);
    if (ym < 0) return 0;
    /* the batch's one alpha_rows entry, looked up (built) here, before both threads read it */
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1]))
                             ? (sTexOverride ? sTexOverride : &sTex[sBoundTex[0]]) : NULL;
    if (tex && !tex->valid) tex = NULL;
    if (sCmNoZ < 0) sCmNoZ = !(getenv("GE_CMNOZ") && getenv("GE_CMNOZ")[0] == '0'); /* as for a triangle share */
    SplitJob job;
    memset(&job, 0, sizeof(job));
    job.use_z = true; job.ylo = ym; job.arows = tex ? alpha_rows(sBoundTex[0], tex) : NULL;
    job.bv = v; job.bn = n; job.bpx = sBatchPxW;
    split_post(&job);
    tlRowHi = ym;
    for (int i = 0; i < n; i++) {
        const int px = raster_tri(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], true);
        sBatchPxC[i] = px;
        if (pollwork && (*pollwork += (uint32_t)(px > 0 ? px : 0) + 64u) >= 8192u && sBatchPoll != NULL) {
            *pollwork = 0;
            *away += sBatchPoll();
        }
    }
    tlRowHi = 0x7fffffff;
    split_wait();
    gfx_soft_spanskip_spans += job.skip_spans; gfx_soft_spanskip_px += job.skip_px;
    for (int i = 0; i < n; i++) {
        const int c = sBatchPxC[i], w = sBatchPxW[i];
        res[i] = c < 0 && w < 0 ? -1 : (c > 0 ? c : 0) + (w > 0 ? w : 0);
        if (w > 0) gfx_soft_split_wpx += (uint32_t)w;
    }
    gfx_soft_batch_splits++;
    return 1;
}
#endif

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
    const char *dir = getenv("GE_DUMP"); /* only with a dump dir: never into the working tree */
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

/* ------------------------------------------------ deferred backgrounds (round 24) --- */
/* GoldenEye opens most outdoor frames with its sky: one opaque quad drawn with no depth test and
 * no depth write, textured (a 64x64 cloud texture, perspective, mipmapped, bilinear) -- ~95 ARM
 * instructions a pixel, 27-30k pixels a frame -- and the world then paints over much of it (the
 * Dam: 0-15k of the sky's ~28k pixels still show at the frame end). F-Zero X's deferred
 * backgrounds (the F-Zero X port 823c09c) draw such a batch only where it stays visible; here the
 * world is depth-tested, so the rule is GoldenEye's own:
 *  - def_record keeps the batch (its vertices, render state, shader and texture, copied) and marks
 *    the pixels it covers as pending (sPend: the latest pending batch per pixel). It only defers
 *    while nothing has written depth since the frame's clear (sDepthWritten): every pixel still
 *    holds 0xFFFF.
 *  - Depth changes only where a depth-tested batch draws, and GoldenEye clears depth only at the
 *    frame start (gfx_pc ignores fills of the depth image), so a pending pixel keeps 0xFFFF. Every
 *    depth test there passes (uz <= 65535 + zbias, zbias 0 or 4), and an OPAQUE batch (no
 *    blending, so no alpha compare either: the span loops' only other skips) writes every pixel
 *    of every row span it draws -- whatever it tests. Its rows clear those pixels from sPend
 *    (DEF_OP_COVER): the sky under them is never drawn.
 *  - A blended batch reads the colour under it: the pending pixels in the rows its triangles can
 *    touch are drawn first (def_resolve_rows). So does everything else that reads or writes the
 *    target outside raster_tri (2D, the phase switch, the frame end, readbacks): def_resolve_all.
 *  - A resolve runs raster_tri on the kept batch with its state restored and its rows clipped (the
 *    scissor's y only: minx and every row's values are unchanged, rows are independent). A row that
 *    nothing covered and no other deferred batch marked (sCovRow 0) still has every pixel of its
 *    spans pending, so it is drawn straight into the target (DEF_OP_DIRECT); the other rows are
 *    drawn into a scratch buffer and only their pending pixels copied (DEF_OP_RESOLVE).
 * Every pixel ends as the last opaque write over it, or the sky's own colour: bit-identical.
 * The row spans come from raster_tri itself (def_row), so marks, covers and draws agree exactly.
 * GE_DEFER=0 draws every batch at once (the A/B). */
#define DEF_MAX 8               /* batches deferred a frame: bit k of sPendRow */
#define DEF_VTX (3 * 64)        /* vertices kept across them */
#define DEF_TEXB (256 * 1024)   /* texel bytes kept across them (the sky: 64x64 x 4 + levels, ~22 KB) */
typedef struct { /* every render-state global raster_tri reads (the rest are fixed knobs) */
    bool depth_test, depth_mask, zdecal, use_alpha, tex_add, tex_lerp, force_in, ui_mode, n64_filter, tex_lod, own;
    int vpx, vpy, vpw, vph, scx0, scy0, scx1, scy1, lerp_r, lerp_g, lerp_b;
    int bilinear, magbilin, max_level;
    float gate_scale, mip_scale;
    struct ShaderProgram *shader;
} RastState;
static void rs_save(RastState *r) {
    r->depth_test = sDepthTest; r->depth_mask = sDepthMask; r->zdecal = sZDecal; r->use_alpha = sUseAlpha;
    r->tex_add = sTexAdd; r->tex_lerp = sTexLerp; r->force_in = sForceIn; r->ui_mode = sUiMode;
    r->n64_filter = sN64Filter; r->tex_lod = sTexLod; r->own = sOwnTri; r->max_level = sMaxLevel;
    r->vpx = sVpX; r->vpy = sVpY; r->vpw = sVpW; r->vph = sVpH;
    r->scx0 = sScX0; r->scy0 = sScY0; r->scx1 = sScX1; r->scy1 = sScY1;
    r->lerp_r = sLerpR; r->lerp_g = sLerpG; r->lerp_b = sLerpB;
    r->bilinear = gfx_soft_bilinear; r->magbilin = gfx_soft_magbilin;
    r->gate_scale = sGateScale; r->mip_scale = sMipScale; r->shader = sCurShader;
}
static void rs_load(const RastState *r) {
    sDepthTest = r->depth_test; sDepthMask = r->depth_mask; sZDecal = r->zdecal; sUseAlpha = r->use_alpha;
    sTexAdd = r->tex_add; sTexLerp = r->tex_lerp; sForceIn = r->force_in; sUiMode = r->ui_mode;
    sN64Filter = r->n64_filter; sTexLod = r->tex_lod; sOwnTri = r->own; sMaxLevel = r->max_level;
    sVpX = r->vpx; sVpY = r->vpy; sVpW = r->vpw; sVpH = r->vph;
    sScX0 = r->scx0; sScY0 = r->scy0; sScX1 = r->scx1; sScY1 = r->scy1;
    sLerpR = r->lerp_r; sLerpG = r->lerp_g; sLerpB = r->lerp_b;
    gfx_soft_bilinear = r->bilinear; gfx_soft_magbilin = r->magbilin;
    sGateScale = r->gate_scale; sMipScale = r->mip_scale; sCurShader = r->shader;
}
typedef struct {
    RastState st;
    struct ShaderProgram shader; /* copies: shaders and textures can change before the resolve */
    SoftTex tex;                 /* (a texture cache reset mid-frame rewrites the arena) */
    bool has_shader, has_tex;
    int vtx0, ntris;
} DefBatch;
static DefBatch sDef[DEF_MAX] SAVESTATE_EXCLUDE;
static int sDefN SAVESTATE_EXCLUDE, sDefVtxN SAVESTATE_EXCLUDE;
static size_t sDefTexN SAVESTATE_EXCLUDE;
static SoftVtx sDefVtx[DEF_VTX] SAVESTATE_EXCLUDE;
static uint8_t *sDefTex SAVESTATE_EXCLUDE;      /* DEF_TEXB bytes */
static uint8_t *sPend SAVESTATE_EXCLUDE;        /* per pixel of the deferring target: pending batch + 1, 0 = none */
static uint8_t *sPendRow SAVESTATE_EXCLUDE;     /* per row: bit k = maybe pending for batch k */
static uint8_t *sCovRow SAVESTATE_EXCLUDE;      /* per row: 1 = a cover or a second batch's mark touched it */
bool gfx_soft_depth_written SAVESTATE_EXCLUDE;  /* a depth-writing batch drew since the frame's depth clear */
static uint32_t *sDefScratch SAVESTATE_EXCLUDE; /* resolves rasterize here */
static uint32_t *sDefFB SAVESTATE_EXCLUDE;      /* the target the batches were deferred on */
static int sDefCap SAVESTATE_EXCLUDE, sDefRows SAVESTATE_EXCLUDE, sDefW SAVESTATE_EXCLUDE;
static uint8_t sDefId SAVESTATE_EXCLUDE;        /* DEF_OP_MARK / DEF_OP_RESOLVE: the batch's id */
int gfx_soft_defer SAVESTATE_EXCLUDE = -1;      /* GE_DEFER=0: draw every batch at once (A/B) */
uint32_t gfx_soft_defer_px SAVESTATE_EXCLUDE;   /* sky pixels a resolve drew (tests) */
/* Where the world covers little of the sky (the Cradle: nearly all of it shows) deferring only
 * costs: a frame whose resolves drew over 3/4 of the pixels it marked turns deferral off for the
 * next DEF_REST frames, then it is tried again. Either way every pixel is the same. */
#define DEF_REST 8
static uint32_t sDefMarkPx SAVESTATE_EXCLUDE, sDefDrawPx SAVESTATE_EXCLUDE; /* this frame */
static int sDefRest SAVESTATE_EXCLUDE;

static bool def_row(int y, int xs, int xe) {
    uint8_t *const p = sPend + y * sDefW;
    const uint8_t bit = (uint8_t)(1u << (sDefId - 1));
    switch (sDefOp) {
    case DEF_OP_MARK:
        memset(p + xs, sDefId, (size_t)(xe - xs));
        sDefMarkPx += (uint32_t)(xe - xs);
        if (sPendRow[y] & (uint8_t)~bit) sCovRow[y] = 1; /* another batch's pixels on this row */
        sPendRow[y] |= bit;
        return true;
    case DEF_OP_COVER:
        if (sPendRow[y]) { memset(p + xs, 0, (size_t)(xe - xs)); sCovRow[y] = 1; }
        return false;
    case DEF_OP_DIRECT: /* an untouched row: every pixel of the span is this batch's, pending */
        if (!(sPendRow[y] & bit) || sCovRow[y]) return true;
        memset(p + xs, 0, (size_t)(xe - xs));
        return false;
    case DEF_OP_RESOLVE:
        if (!(sPendRow[y] & bit) || !sCovRow[y]) return true;
        return memchr(p + xs, sDefId, (size_t)(xe - xs)) == NULL; /* nothing of this batch pending here */
    }
    return false;
}
/* ------------------------------------------------------- T-junction cracks (GE_TJFILL) --- */
/* GoldenEye's level meshes have T-junctions: a vertex shared by two triangles should sit on the long edge of a third
 * but misses it by about a model unit (the s16 vertex grid). Up close the gap between that edge and the two short ones
 * is a sliver under a pixel wide, and the port, which samples each pixel at its centre, leaves the pixel centres inside
 * it unwritten: the sky drawn first shows through as a dotted or solid hairline (the user's Dam tunnel ceiling,
 * 2026-10-04: a line of 1-3 px a row across the top right, the same in every build since the first). The N64 draws the
 * world with coverage AA (G_RM_AA_ZB_OPA_SURF) over a sky of full coverage: a pixel any of whose subsamples a triangle
 * touches takes that triangle's colour unblended (the coverage sum overflows), so such a sliver never shows
 * (angrylion: no line at the same place).
 * The fix, at the end of each 3D phase (tj_fill), only where the port left a hole the N64 would not have:
 *  - a pixel no depth write reached (0xFFFF), not under a cut-out draw (its holes) nor a draw without a z test
 *    once the frame has depth (the gun) (tj_mark_batch);
 *  - with written depth right next to it on both sides along its column or its row (a gap of 1 or 2 pixels; along the
 *    top and bottom rows up to 4, where a shallow crack's column has no pixel above or below);
 *  - the depth continuous across the gap (the step over it lies between the two sides' own slopes, give or take: as
 *    where two faces meet);
 *  - and closed: every unwritten pixel joined to it side by side passes the same test. A notch in an outline, or a slit
 *    between two separate objects (the Cradle's girders far off), opens onto the sky and is left alone.
 * Such a pixel takes its neighbours' colour and depth: the mean for a 1-px gap, the nearer side's in a wider one.
 * Every other pixel is untouched (port/hh/tools/tjfill_test.sh). GE_TJFILL=0: off (the old frames); =2: each filled
 * pixel drawn red, its depth's high byte in blue (census). */
static int sTjFill SAVESTATE_EXCLUDE = -1;
uint32_t gfx_soft_tj_px SAVESTATE_EXCLUDE; /* pixels filled since start (tests, census) */
static void tj_prepare(void) { /* select_target(true): the mask for the 3D target */
    if (sTjFill < 0) { const char *e = getenv("GE_TJFILL"); sTjFill = !(e && e[0] == '0') ? (e && e[0] == '2' ? 2 : 1) : 0; }
    if (!sTjFill) return;
    if (sTjMask && sTjMaskW == sW && sTjMaskH == sH) return;
    free(sTjMask); free(sTjRow);
    sTjMask = (uint8_t *)calloc((size_t)sW * sH, 1);
    sTjRow = (uint8_t *)calloc((size_t)sH, 1);
    if (!sTjMask || !sTjRow) { free(sTjMask); free(sTjRow); sTjMask = sTjRow = NULL; sTjMaskW = sTjMaskH = 0; return; }
    sTjMaskW = sW; sTjMaskH = sH;
}
static inline int tj_iabs(int v) { return v < 0 ? -v : v; }
/* Written depth at a and b on either side of an n-pixel gap, the next pixels out at aa and bb (-1: none, off-screen or
 * unwritten): continuous across the gap -- the depth step over it lies between the two sides' own slopes? */
static inline bool tj_continuous(int za, int zb, int zaa, int zbb, int n) {
    const int d = zb - za;
    int sa = zaa >= 0 ? za - zaa : -0x10000, sb = zbb >= 0 ? zbb - zb : -0x10000;
    if (sa == -0x10000) sa = sb == -0x10000 ? 0 : sb;
    if (sb == -0x10000) sb = sa;
    const int lo = (sa < sb ? sa : sb) * (n + 1), hi = (sa < sb ? sb : sa) * (n + 1);
    const int tol = 16 + 2 * (tj_iabs(sa) + tj_iabs(sb));
    return d >= lo - tol && d <= hi + tol;
}
static inline uint32_t tj_mean(uint32_t a, uint32_t b) {
    return 0xFF000000u | ((((a & 0x00FEFEFEu) >> 1) + ((b & 0x00FEFEFEu) >> 1) + (a & b & 0x00010101u)) & 0x00FFFFFFu);
}
/* Pixel i (unwritten, unmarked) in a gap along a column (step w) or a row (step 1): the free pixels before it (k0) and
 * after it, the run n = 1..nmax between written depth at a and b, continuous. Returns the side its colour comes from
 * (a or b; -2: the mean of both), -1 if not in such a gap. */
static inline int tj_gap_dir(const uint16_t *zb, int i, int x, int y, int pos, int len, int step, int dx, int dy, int nmax,
                             int *pa, int *pb) {
    int k0 = 0, k1 = 0;
    while (k0 < nmax && pos - k0 - 1 >= 0 && zb[i - (k0 + 1) * step] == 0xFFFF) {
        if (TJ_MARKED(x - (k0 + 1) * dx, y - (k0 + 1) * dy)) return -1;
        k0++;
    }
    if (pos - k0 - 1 < 0 || zb[i - (k0 + 1) * step] == 0xFFFF) return -1;
    while (k0 + k1 + 1 <= nmax && pos + k1 + 1 < len && zb[i + (k1 + 1) * step] == 0xFFFF) {
        if (TJ_MARKED(x + (k1 + 1) * dx, y + (k1 + 1) * dy)) return -1;
        k1++;
    }
    const int n = k0 + k1 + 1;
    if (n > nmax || pos + k1 + 1 >= len || zb[i + (k1 + 1) * step] == 0xFFFF) return -1;
    const int a = i - (k0 + 1) * step, b = i + (k1 + 1) * step;
    const int zaa = pos - k0 - 2 >= 0 && zb[a - step] != 0xFFFF ? zb[a - step] : -1;
    const int zbb = pos + k1 + 2 < len && zb[b + step] != 0xFFFF ? zb[b + step] : -1;
    if (!tj_continuous(zb[a], zb[b], zaa, zbb, n)) return -1;
    *pa = a; *pb = b;
    return n == 1 ? -2 : (2 * k0 < n ? a : b);
}
static inline bool tj_gap(const uint16_t *zb, int w, int h, int i, int x, int y, const uint32_t *fb, uint32_t *col,
                          uint16_t *z) {
    int a, b, s = tj_gap_dir(zb, i, x, y, y, h, w, 0, 1, 2, &a, &b);
    if (s == -1) s = tj_gap_dir(zb, i, x, y, x, w, 1, 1, 0, y == 0 || y == h - 1 ? 4 : 2, &a, &b);
    if (s == -1) return false;
    if (s == -2) { *col = tj_mean(fb[a], fb[b]); *z = (uint16_t)((zb[a] + zb[b] + 1) >> 1); }
    else { *col = fb[s]; *z = zb[s]; }
    return true;
}
#define TJ_BITW 16 /* 64-bit words a row: up to 1024 px (the 2x supersampled intro logo is 480) */
static uint64_t sTjBits[3 * TJ_BITW] SAVESTATE_EXCLUDE;
/* bit x of bits[]: pixel x of the row unwritten (0xFFFF) */
static void tj_row_bits(const uint16_t *zr, int w, uint64_t *bits) {
    int x = 0;
    memset(bits, 0, ((size_t)(w + 63) >> 6) * 8);
#if defined(HH_CM_NEON)
    static const uint8_t kWt[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    const uint8x16_t wt = vld1q_u8(kWt);
    const uint16x8_t ff = vdupq_n_u16(0xFFFF);
    uint16_t *const b16 = (uint16_t *)bits; /* (little-endian: bit x of the u64 row is bit x & 15 of halfword x >> 4) */
    for (; x + 16 <= w; x += 16) {
        const uint8x16_t m = vcombine_u8(vmovn_u16(vceqq_u16(vld1q_u16(zr + x), ff)), vmovn_u16(vceqq_u16(vld1q_u16(zr + x + 8), ff)));
        const uint8x16_t t = vandq_u8(m, wt);
        uint8x8_t p = vpadd_u8(vget_low_u8(t), vget_high_u8(t));
        p = vpadd_u8(p, p); p = vpadd_u8(p, p);
        b16[x >> 4] = vget_lane_u16(vreinterpret_u16_u8(p), 0);
    }
#endif
    for (; x < w; x++) if (zr[x] == 0xFFFF) bits[x >> 6] |= 1ull << (x & 63);
}
static uint8_t *sTjSeen SAVESTATE_EXCLUDE;   /* per pixel: visited by a flood this pass */
static int32_t *sTjList SAVESTATE_EXCLUDE;   /* the flood's pixels, then every visited one (to clear sTjSeen) */
static uint32_t *sTjCol SAVESTATE_EXCLUDE; static uint16_t *sTjZ SAVESTATE_EXCLUDE;
static int sTjCap SAVESTATE_EXCLUDE;
static void __attribute__((noinline)) tj_fill(void) {
    if (sTjFill <= 0 || !sTjMask || sTjMaskW != sW || sTjMaskH != sH || !sFB || !sZB) return;
    if (gfx_soft_depth_written && sW <= TJ_BITW * 64) {
        const int w = sW, h = sH;
        uint32_t *const fb = sFB; uint16_t *const zb = sZB;
        if (sTjCap < w * h) {
            free(sTjSeen); free(sTjList); free(sTjCol); free(sTjZ);
            sTjSeen = (uint8_t *)calloc((size_t)w * h, 1); sTjList = (int32_t *)malloc((size_t)w * h * 4);
            sTjCol = (uint32_t *)malloc((size_t)w * h * 4); sTjZ = (uint16_t *)malloc((size_t)w * h * 2);
            sTjCap = sTjSeen && sTjList && sTjCol && sTjZ ? w * h : 0;
            if (!sTjCap) goto done;
        }
        int nseen = 0; /* sTjList[0 .. nseen): every pixel marked in sTjSeen; a flood's own at its end */
        const int nw = (w + 63) >> 6;
        uint64_t *const rb = sTjBits; /* three rows of "unwritten" bits, rolling: above, this one, below */
        uint64_t *bu = rb, *bc = rb + TJ_BITW, *bd = rb + 2 * TJ_BITW;
        tj_row_bits(zb, w, bc);
        for (int y = 0; y < h; y++) {
            const uint16_t *zr = zb + y * w;
            if (y + 1 < h) tj_row_bits(zr + w, w, bd);
            /* the pixels to look at: every unwritten one, less the sky's own -- unwritten above and below and on both
             * sides, so a run of 3 or more either way, which no gap of 1 or 2 passes through (the top and bottom rows
             * take runs up to 4 and have no row on one side: all of theirs) */
            uint64_t cand[TJ_BITW];
            for (int k = 0; k < nw; k++) {
                if (y == 0 || y == h - 1) { cand[k] = bc[k]; continue; }
                const uint64_t lft = (bc[k] << 1) | (k ? bc[k - 1] >> 63 : 1u);           /* pixel x-1 unwritten (x = 0: yes) */
                const uint64_t rgt = (bc[k] >> 1) | (k + 1 < nw ? bc[k + 1] << 63 : 0); /* pixel x+1 unwritten */
                uint64_t r = rgt;
                if (k == ((w - 1) >> 6)) r |= 1ull << ((w - 1) & 63);                    /* (x = w-1: yes) */
                cand[k] = bc[k] & ~(bu[k] & bd[k] & lft & r);
            }
            for (int k = 0; k < nw; k++) {
                uint64_t bitsk = cand[k];
                while (bitsk) {
                    const uint32_t lo = (uint32_t)bitsk; /* (a 32-bit count: libgcc's 64-bit one is a call on ARM) */
                    const int x = (k << 6) + (lo ? __builtin_ctz(lo) : 32 + __builtin_ctz((uint32_t)(bitsk >> 32)));
                    bitsk &= bitsk - 1;
                    if (x >= w) break;
                    const int i0 = y * w + x;
                    if (sTjSeen[i0] || TJ_MARKED(x, y)) continue;
                    uint32_t c0; uint16_t z0;
                    if (!tj_gap(zb, w, h, i0, x, y, fb, &c0, &z0)) continue;
                    /* the unwritten pixels joined to it side by side: every one must be in a gap too (a crack is closed;
                     * a notch in an outline or a slit between two separate objects opens onto the sky) */
                    const int first = nseen;
                    bool ok = true;
                    sTjSeen[i0] = 1; sTjCol[nseen] = c0; sTjZ[nseen] = z0; sTjList[nseen++] = i0;
                    for (int q = first; q < nseen; q++) {
                        const int j = sTjList[q], jx = j % w, jy = j / w;
                        for (int e = 0; e < 4; e++) {
                            const int dx = e == 0 ? -1 : e == 1 ? 1 : 0, dy = e == 2 ? -1 : e == 3 ? 1 : 0;
                            if (jx + dx < 0 || jx + dx >= w || jy + dy < 0 || jy + dy >= h) continue;
                            const int n = j + dy * w + dx;
                            if (zb[n] != 0xFFFF || sTjSeen[n]) continue;
                            uint32_t cn; uint16_t zn;
                            if (TJ_MARKED(jx + dx, jy + dy) || !tj_gap(zb, w, h, n, jx + dx, jy + dy, fb, &cn, &zn)) { ok = false; continue; }
                            sTjSeen[n] = 1; sTjCol[nseen] = cn; sTjZ[nseen] = zn; sTjList[nseen++] = n;
                        }
                    }
                    if (ok) {
                        const bool paint = sTjFill == 2;
                        for (int q = first; q < nseen; q++) {
                            const int j = sTjList[q];
                            fb[j] = paint ? 0xFFFF0000u | (uint32_t)(sTjZ[q] >> 8) : sTjCol[q]; zb[j] = sTjZ[q];
                        }
                        gfx_soft_tj_px += (uint32_t)(nseen - first);
                    }
                }
            }
            uint64_t *t = bu; bu = bc; bc = bd; bd = t;
        }
        for (int q = 0; q < nseen; q++) sTjSeen[sTjList[q]] = 0;
    }
done:
    for (int y = 0; y < sH; y++) /* the marks are this phase's */
        if (sTjRow[y]) { memset(sTjMask + y * sTjMaskW, 0, (size_t)sTjMaskW); sTjRow[y] = 0; }
}
/* Buffers for the current target; only (re)sized while nothing is pending. */
static bool def_buffers(void) {
    if (sDefCap >= sW * sH && sDefRows >= sH && sDefW == sW && sDefTex) return true;
    if (sDefN) return false;
    free(sPend); free(sPendRow); free(sCovRow); free(sDefScratch);
    sPend = (uint8_t *)calloc((size_t)sW * sH, 1);
    sPendRow = (uint8_t *)calloc((size_t)sH, 1);
    sCovRow = (uint8_t *)calloc((size_t)sH, 1);
    sDefScratch = (uint32_t *)malloc((size_t)sW * sH * 4);
    if (!sDefTex) sDefTex = (uint8_t *)malloc(DEF_TEXB);
    if (!sPend || !sPendRow || !sCovRow || !sDefScratch || !sDefTex) {
        free(sPend); free(sPendRow); free(sCovRow); free(sDefScratch);
        sPend = sPendRow = sCovRow = NULL; sDefScratch = NULL; sDefCap = sDefRows = sDefW = 0;
        return false;
    }
    sDefCap = sW * sH; sDefRows = sH; sDefW = sW;
    return true;
}
/* A copy of texture t (its image and every mip level) in sDefTex; false = no room. */
static bool def_copy_tex(const SoftTex *t, SoftTex *out) {
    *out = *t;
    if (!t->valid || !t->px) return true;
    const size_t bpp = t->is16 ? 2u : 4u;
    const void *src[8]; size_t bytes[8]; int n = 0;
    src[n] = t->px; bytes[n++] = (size_t)t->w * t->h * bpp;
    for (int l = 0; l < t->nlev && l < 7; l++) {
        if (l == 0 && t->lev[0] == t->px) continue;
        const int w = t->mw >> l, h = t->mh >> l;
        src[n] = t->lev[l]; bytes[n++] = (size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1) * bpp;
    }
    size_t at = (sDefTexN + 15u) & ~(size_t)15, need = 0;
    for (int i = 0; i < n; i++) need += (bytes[i] + 15u) & ~(size_t)15;
    if (!src[0] || at + need > DEF_TEXB) return false;
    for (int i = 0; i < n; i++) {
        uint8_t *dst = sDefTex + at;
        memcpy(dst, src[i], bytes[i]);
        if (src[i] == t->px) out->px = dst;
        for (int l = 0; l < t->nlev && l < 7; l++) if (t->lev[l] == src[i]) out->lev[l] = dst;
        at += (bytes[i] + 15u) & ~(size_t)15;
    }
    sDefTexN = at;
    return true;
}
/* Keep an opaque no-depth batch for later and mark its pixels pending; false = draw it now. */
static bool def_record(const SoftVtx *v, size_t ntris) {
    if (sDefN >= DEF_MAX || sDefVtxN + (int)ntris * 3 > DEF_VTX || !def_buffers()) return false;
    if (sDefRest > 0 || gfx_soft_depth_written || (sDefN && sDefFB != sFB)) return false;
    DefBatch *d = &sDef[sDefN];
    const size_t tex_mark = sDefTexN;
    d->has_tex = sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1]);
    if (d->has_tex && !def_copy_tex(&sTex[sBoundTex[0]], &d->tex)) { sDefTexN = tex_mark; return false; }
    d->has_shader = sCurShader != NULL;
    if (sCurShader) d->shader = *sCurShader;
    rs_save(&d->st);
    d->vtx0 = sDefVtxN; d->ntris = (int)ntris;
    memcpy(&sDefVtx[sDefVtxN], v, ntris * 3 * sizeof(SoftVtx));
    sDefVtxN += (int)ntris * 3;
    sDefFB = sFB;
    sDefId = (uint8_t)(++sDefN);
    sDefOp = DEF_OP_MARK;
    for (size_t i = 0; i < ntris; i++) raster_tri(&v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2], true);
    sDefOp = DEF_OP_NONE;
    return true;
}
/* Draw batch k's pending pixels in rows [ry0, ry1). */
static void def_raster(int k, int ry0, int ry1) {
    const DefBatch *d = &sDef[k];
    RastState cur;
    uint32_t *const fb = sFB;
    rs_save(&cur);
    rs_load(&d->st);
    sCurShader = d->has_shader ? (struct ShaderProgram *)&d->shader : NULL;
    sTexOverride = d->has_tex ? &d->tex : NULL;
    if (sScY0 < ry0) sScY0 = ry0;
    if (sScY1 > ry1) sScY1 = ry1;
    const uint8_t id = (uint8_t)(k + 1), bit = (uint8_t)(1u << k);
    bool clean = false, dirty = false;
    for (int y = ry0; y < ry1; y++) { if (sCovRow[y]) dirty = true; else clean = true; }
    sDefId = id;
    uint32_t px = 0;
    for (int pass = 0; pass < 2; pass++) { /* untouched rows straight into the target, then the rest */
        if (pass == 0 ? !clean : !dirty) continue;
        sDefOp = pass == 0 ? DEF_OP_DIRECT : DEF_OP_RESOLVE;
        sFB = pass == 0 ? fb : sDefScratch;
#if defined(MIYOO)
        int i = 0;
        { /* the second core: the resolve's rows shared too (batch_raster; def_row's ops here touch their own row) */
            static int res[BATCH_MAX] SAVESTATE_EXCLUDE;
            while (i < d->ntris) {
                const int cnt = d->ntris - i < BATCH_MAX ? d->ntris - i : BATCH_MAX;
                if (!batch_raster(&sDefVtx[d->vtx0 + i * 3], cnt, res, NULL, NULL)) break;
                for (int k = 0; k < cnt; k++) if (res[k] > 0) px += (uint32_t)res[k];
                i += cnt;
            }
        }
        for (; i < d->ntris; i++) {
#else
        for (int i = 0; i < d->ntris; i++) {
#endif
            const SoftVtx *v = &sDefVtx[d->vtx0 + i * 3];
            const int n = raster_tri(&v[0], &v[1], &v[2], true);
            if (n > 0) px += (uint32_t)n;
        }
    }
    sDefOp = DEF_OP_NONE;
    sFB = fb;
    sTexOverride = NULL;
    rs_load(&cur);
    gfx_soft_span_px += px; gfx_soft_defer_px += px; sDefDrawPx += px;
    for (int y = ry0; y < ry1; y++) {
        if (!(sPendRow[y] & bit)) continue;
        if (!sCovRow[y]) { sPendRow[y] &= (uint8_t)~bit; continue; } /* drawn in place */
        uint8_t *p = sPend + y * sW;
        const uint32_t *src = sDefScratch + y * sW;
        uint32_t *dst = sFB + y * sW;
        const uint32_t all = id * 0x01010101u;
        int x = 0;
        for (; x + 4 <= sW; x += 4) { /* four marks at a time: none, all this batch's, or mixed */
            uint32_t m;
            memcpy(&m, p + x, 4);
            if (m == 0) continue;
            if (m == all) { memcpy(dst + x, src + x, 16); memset(p + x, 0, 4); continue; }
            for (int i = x; i < x + 4; i++) if (p[i] == id) { dst[i] = src[i]; p[i] = 0; }
        }
        for (; x < sW; x++) if (p[x] == id) { dst[x] = src[x]; p[x] = 0; }
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
                need = !sCovRow[y] || memchr(sPend + y * sW, id, (size_t)sW) != NULL; /* untouched: all pending */
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
/* Before a batch that reads the colour under it: the rows its triangles can touch. */
static void def_resolve_batch(const SoftVtx *v, size_t ntris) {
    for (size_t i = 0; i < ntris; i++) {
        const float y0 = fmin3(v[i * 3].y, v[i * 3 + 1].y, v[i * 3 + 2].y), y1 = fmax3(v[i * 3].y, v[i * 3 + 1].y, v[i * 3 + 2].y);
        if (!(y1 >= 0.0f) || !(y0 < (float)sH)) { if (y0 != y0 || y1 != y1) def_resolve_rows(0, sH); continue; } /* NaN: all */
        def_resolve_rows((int)y0 - 1, iceilf(y1) + 1); /* raster_tri's rows lie inside [floor y0, ceil y1) */
    }
}
static void def_resolve_all(void) {
    if (!sDefN) return;
    /* every target switch resolves first, so the batches' target is the current one; if not, draw
     * them on theirs (their raster reads no depth) */
    uint32_t *const fb = sFB; const int w = sW, h = sH;
    if (sDefFB != sFB) { sFB = sDefFB; sW = sDefW; sH = sDefRows; }
    def_resolve_rows(0, sH);
    memset(sCovRow, 0, (size_t)sH);
    sFB = fb; sW = w; sH = h;
    sDefN = 0; sDefVtxN = 0; sDefTexN = 0; sDefFB = NULL;
}

void gfx_scegu_set_sky_pass(bool on);
/* Between the game's HH_TAG_FPMODEL_ON/OFF (bondview2.c): the first-person gun, hands and watch arm.
 * They are opaque, drawn without depth and can fill much of the screen -- the half-resolution sky's test
 * below took them for sky (round 38, the user: "bonds hand and arm have seams" in the pause's watch
 * raise: the arm's pieces drawn at half resolution, dotted lines where the upscale met them; since
 * 6e5152d). GE_FPSKYCOUNT=1: the batches that would have been taken, counted at exit. */
static bool sFpModel SAVESTATE_EXCLUDE;
static unsigned long sFpSkyN SAVESTATE_EXCLUDE;
static void fp_sky_report(void) { fprintf(stderr, "[soft] GE_FPSKYCOUNT: %lu first-person batches kept off the sky pass\n", sFpSkyN); }
void gfx_soft_set_fpmodel(bool on) {
    static int sCount SAVESTATE_EXCLUDE = -1;
    if (sCount < 0) { sCount = getenv("GE_FPSKYCOUNT") != NULL; if (sCount) atexit(fp_sky_report); }
    sFpModel = on;
}
extern uint32_t hh_rsp_poll(void) __attribute__((weak));
static uint32_t sPollWork SAVESTATE_EXCLUDE;
/* GE_TJFILL: a batch that can leave a pixel of its triangles unwritten while writing depth around it -- z-tested,
 * depth-writing and alpha-tested (cut-outs: a fence's holes) -- or draw over unwritten pixels without a z test once the
 * frame has depth (the gun), marks its triangles' pixels by the pixel-centre rule (gfx_soft_draw_triangles, once a
 * batch: nothing in raster_tri). See-through draws that write no depth (water, smoke, glass) are not marked: a crack
 * under them takes its neighbours' colour, which carries the same overlay. */
static void __attribute__((noinline)) tj_mark_batch(const SoftVtx *v, size_t num_tris) {
    if (!sTjMask || !sPhase3D || sSkyPass || !(sDepthTest ? (sUseAlpha && sDepthMask) : gfx_soft_depth_written)) return;
    if (sTjMaskW != sW || sTjMaskH != sH) return;
    const float fw = (float)sW;
    for (size_t i = 0; i < num_tris; i++) {
        /* a scanline walk: the vertices by y, the long edge a-c against a-b then b-c, x at each row's centre */
        const SoftVtx *a = &v[i * 3], *b = &v[i * 3 + 1], *c = &v[i * 3 + 2], *q;
        if (b->y < a->y) { q = a; a = b; b = q; }
        if (c->y < b->y) { q = b; b = c; c = q; }
        if (b->y < a->y) { q = a; a = b; b = q; }
        if (!(c->y > a->y) || !(c->y > 0.0f) || !(a->y < (float)sH)) continue; /* (flat, off-screen or NaN) */
        const float sac = (c->x - a->x) / (c->y - a->y);
        const float sab = b->y > a->y ? (b->x - a->x) / (b->y - a->y) : 0.0f;
        const float sbc = c->y > b->y ? (c->x - b->x) / (c->y - b->y) : 0.0f;
        int y = a->y > 0.5f ? (int)(a->y - 0.5f) : 0;
        if ((float)y + 0.5f < a->y) y++;
        for (; y < sH; y++) {
            const float yc = (float)y + 0.5f;
            if (yc > c->y) break;
            const float xl = a->x + (yc - a->y) * sac;
            const float xs = yc < b->y ? a->x + (yc - a->y) * sab : b->x + (yc - b->y) * sbc;
            const float lo = (xl < xs ? xl : xs) - 0.5f, hi = (xl < xs ? xs : xl) - 0.5f;
            if (!(hi >= 0.0f) || lo >= fw - 1.0f) continue;
            int x0 = lo <= 0.0f ? 0 : (int)lo; if ((float)x0 < lo) x0++;  /* ceil: the first centre at or right of lo */
            const int x1 = hi >= fw - 1.0f ? sW : (int)hi + 1;           /* past the last centre at or left of hi */
            if (x0 < x1) { memset(sTjMask + y * sTjMaskW + x0, 1, (size_t)(x1 - x0)); sTjRow[y] = 1; }
        }
    }
}
static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    PROF_BEGIN();
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    target_ready();
    if (!sUnified) { if (sPhase3D) { sIntDirty = true; sIntClean = false; } else sNatDirty = true; }
    if (sPickX == -2) {
        const char *e = getenv("GE_PICK");
        if (!e || sscanf(e, "%d,%d", &sPickX, &sPickY) != 2) sPickX = -1;
    }
    /* The half-resolution sky (round 24; NOT exact, the user saw the before/after sheets: "looks fine
     * to me"): a large textured opaque no-depth batch -- the sky, and the Dam's backdrop cliff -- is
     * drawn at half resolution and bilinearly doubled (OoT's sky pass, gfx_scegu_set_sky_pass), a
     * quarter of its pixels. Softer clouds. GE_HALFSKY=0: the full-resolution sky (then deferred).
     * Not on the Runway (round 30, the user: its snowy mountains lost their detail at half res; full
     * res costs it ~0.8%): 2 = every stage but the Runway, 1 = every stage.
     * Round 43: OFF by default (the user, 2026-09-30: "half res sky causes more issues than it solves"):
     * the Dam's backdrop cliffs went through it too, stair-stepped against the sky with a dotted crack
     * line across the mountains (IMG_0225). Unset/0 = full resolution everywhere; GE_HALFSKY=2 or 1 to opt in. */
    static int sHalfSky SAVESTATE_EXCLUDE = -1;
    if (sHalfSky < 0) { const char *e = getenv("GE_HALFSKY"); sHalfSky = !e || !e[0] || e[0] == '0' ? 0 : e[0] == '2' ? 2 : 1; }
    extern int bossGetStageNum(void);
    const bool half_sky = sHalfSky == 1 || (sHalfSky == 2 && bossGetStageNum() != 35 /* LEVELID_RUNWAY */);
    /* Only perspective scenery (round 25): the folder screens draw their paper as large flat
     * (affine, one 1/w) textured no-depth polygons too, and at half resolution the paper's edge
     * against the crest fell between two half-size pixels -- a black column the upscale never
     * wrote (the user's IMG_0161, the dossier main menu). The sky and the Dam's cliff are
     * perspective (1/w differs across the batch); the menus' N64-filter screens are left alone too. */
    bool persp_batch = false;
    for (size_t i = 1; i < num_tris * 3 && !persp_batch; i++) persp_batch = v[i].invw != v[0].invw;
    if (half_sky && sPhase3D && !sSkyPass && sPickX < 0 && !sUseAlpha && !sDepthTest && !sDepthMask && num_tris > 0 &&
        persp_batch && !sN64Filter && !sUiMode &&
        sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) {
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (size_t i = 1; i < num_tris * 3; i++) {
            if (v[i].x < x0) x0 = v[i].x; if (v[i].x > x1) x1 = v[i].x;
            if (v[i].y < y0) y0 = v[i].y; if (v[i].y > y1) y1 = v[i].y;
        }
        const float bw = (x1 < sW ? x1 : sW) - (x0 > 0 ? x0 : 0), bh = (y1 < sH ? y1 : sH) - (y0 > 0 ? y0 : 0);
        if (bw > 0 && bh > 0 && bw * bh * 8.0f >= (float)(sW * sH) && sFpModel) sFpSkyN++;
        else if (bw > 0 && bh > 0 && bw * bh * 8.0f >= (float)(sW * sH)) { /* an eighth of the screen or more */
            gfx_scegu_set_sky_pass(true);
            if (sSkyPass) {
                PROF_END(PROF_R3D);
                gfx_soft_draw_triangles(buf_vbo, buf_vbo_len, num_tris); /* halved: sSkyPass */
                gfx_scegu_set_sky_pass(false);
                return;
            }
        }
    }
    if (gfx_soft_defer < 0) { const char *e = getenv("GE_DEFER"); gfx_soft_defer = !(e && e[0] == '0'); }
    bool cover = false;
    const bool writes_depth = sDepthTest && sDepthMask; /* raster_tri writes depth only under its test */
    if (gfx_soft_defer && sPhase3D && !sSkyPass && sPickX < 0) {
        if (!sUseAlpha && !sDepthTest && !sDepthMask && def_record(v, num_tris)) { PROF_END(PROF_R3D); return; }
        if (sDefN) {
            if (sUseAlpha) def_resolve_batch(v, num_tris); /* reads the colour under it */
            else cover = sFB == sDefFB;                    /* opaque: writes every pixel of its rows */
        }
    } else {
        def_resolve_all();
    }
    if (cover) sDefOp = DEF_OP_COVER;
    tj_mark_batch(v, num_tris); /* GE_TJFILL */
    if (writes_depth) gfx_soft_depth_written = true;
    uint32_t away = 0; /* GE_RSPYIELD (hh_os.c): time the kernel ran during the polls below, out of this timer */
#if defined(MIYOO)
    /* the second core: the batch's rows shared (batch_raster), chunk by chunk; not with GE_PICK, the trace or the
     * half-resolution sky pass (their per-triangle code below) */
    if (!sSkyPass && sPickX < 0 && !(gfx_trace_frames > 0) && num_tris >= 2 && miyoo_worker_ready()) {
        static int res[BATCH_MAX] SAVESTATE_EXCLUDE;
        size_t done = 0;
        sBatchPoll = hh_rsp_poll;
        while (done < num_tris) {
            const int cnt = num_tris - done < BATCH_MAX ? (int)(num_tris - done) : BATCH_MAX;
            if (!batch_raster(&v[done * 3], cnt, res, &sPollWork, &away)) break; /* the rest as before */
            if (sPhase3D)
                for (int k = 0; k < cnt; k++)
                    if (res[k] >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)res[k]; }
            done += (size_t)cnt;
        }
        v += done * 3;
        num_tris -= done;
    }
#endif
    for (size_t i = 0; i < num_tris; i++) {
        SoftVtx h[3];
        if (sSkyPass) { /* screen coordinates to the half-resolution sky target */
            for (int k = 0; k < 3; k++) { h[k] = v[i*3+k]; h[k].x *= 0.5f; h[k].y *= 0.5f; }
        }
        static int sPickFrom SAVESTATE_EXCLUDE = -1; /* GE_PICK_FRAME: log from that rendered frame on */
        if (sPickFrom < 0) sPickFrom = getenv("GE_PICK_FRAME") ? atoi(getenv("GE_PICK_FRAME")) : 0;
        const bool pick = sPickX >= 0 && sPickX < sW && sPickY >= 0 && sPickY < sH && gfx_soft_frames_rendered >= (u32)sPickFrom;
        const uint32_t pick_c0 = pick ? sFB[sPickY * sW + sPickX] : 0;
        const uint16_t pick_z0 = pick ? sZB[sPickY * sW + sPickX] : 0;
        const int px = sSkyPass ? raster_tri(&h[0], &h[1], &h[2], true) : raster_tri(&v[i*3+0], &v[i*3+1], &v[i*3+2], true);
        if (pick && (sFB[sPickY * sW + sPickX] != pick_c0 || sZB[sPickY * sW + sPickX] != pick_z0)) {
            const SoftTex *t = &sTex[sBoundTex[0]];
            port_log("  PICK (%d,%d) %08X -> %08X  tex %u %dx%d%s wrap %d/%d bilin %d (scr %.1f uvc %.1f)  uv (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f)  xy (%.6f,%.6f) (%.6f,%.6f) (%.6f,%.6f) sc %d..%d vp %d+%d\n",
                     sPickX, sPickY, (unsigned)pick_c0, (unsigned)sFB[sPickY * sW + sPickX], (unsigned)sBoundTex[0],
                     t->w, t->h, t->is16 ? " 16b" : "", t->wrap_s, t->wrap_t, (int)sDbgBilin, sDbgScr, sDbgUvc,
                     v[i*3].u, v[i*3].v, v[i*3+1].u, v[i*3+1].v, v[i*3+2].u, v[i*3+2].v,
                     v[i*3].x, v[i*3].y, v[i*3+1].x, v[i*3+1].y, v[i*3+2].x, v[i*3+2].y, sScX0, sScX1, sVpX, sVpW);
            pick_dump_texture(sBoundTex[0]);
        }
        /* Profiling counters are 3D-only: this path also serves the ortho
         * (2D-phase) batches, whose time lands in PROF_R2D, and counting their
         * pixels here understated the GFX3D line's ns/px (round 13). */
        if (sPhase3D && px >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)px; }
        /* a long frame lets the audio thread run: a poll per ~8k pixels or ~128 triangles of work */
        if ((sPollWork += (uint32_t)(px > 0 ? px : 0) + 64u) >= 8192u && hh_rsp_poll != NULL) { sPollWork = 0; away += hh_rsp_poll(); }
        {
            extern int gfx_trace_frames;
            extern void port_log(const char *fmt, ...);
            if (gfx_trace_frames > 0)
                port_log("  soft tri px %d (%.1f,%.1f,z%.4f,w%.3f) (%.1f,%.1f,z%.4f) (%.1f,%.1f,z%.4f) 3d %d tgt %dx%d\n", px,
                         v[i*3].x, v[i*3].y, v[i*3].z, v[i*3].invw, v[i*3+1].x, v[i*3+1].y, v[i*3+1].z,
                         v[i*3+2].x, v[i*3+2].y, v[i*3+2].z, (int)sPhase3D, sW, sH);
        }
    }
    sDefOp = DEF_OP_NONE;
    PORT_PROF_ADD(sPhase3D ? PROF_R3D : PROF_R2D, port_time_us() - _pt0 - away);
}
/* gfx_pc tells us when a batch is real perspective geometry (the sky quads
 * also render on the internal target but must not count as "3D present"). */
void gfx_scegu_note_persp(void) { sHad3D = true; }

/* 2D path (HUD, menus, logo): gfx_pc.c emits PSP GU_SPRITES — TWO corners per
 * rect in a VertexColor {s16 u,v; RGBA; u16 x,y,z}: screen pixels + TEXEL UVs
 * (tile-origin already subtracted), combiner colour pre-folded by
 * gfx_eval_vertex_color. GU takes the sprite colour from the second vertex. */
typedef struct __attribute__((aligned(16))) {
    int16_t u, v; /* signed, as gfx_pc.c writes them: a texrect may start left of / above its tile origin or step back past it (ssb64 4843c2c + the DKR session) */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* box_row_cached's rows averaged down the pixel's footprint too, for 2D that the output shrinks
 * in both directions: GoldenEye's folder screens are 440x330 and land on a square output, 1.83 texels
 * a pixel across and 1.375 down, and point samples down dropped every fourth row of the text
 * (MK64's menus are 320x240: only across, which box_row already filters). Colour weighted by
 * alpha x coverage as in texel_box; each texel row is still filtered across once and cached.
 * GE_TEXTV=0: rows point-sampled again (A/B). */
static int sTextV SAVESTATE_EXCLUDE = -1;
static const uint32_t *box_rows_v(uint32_t *out, int n, uint32_t texid, const void *px, bool t16, float vc, float dv,
                                  int th, int wt, int mv, int32_t uf, int32_t duf, int tw, int ws, int mu, bool premul) {
    static uint32_t sr[BOX_MAX], sg[BOX_MAX], sb[BOX_MAX], sa[BOX_MAX]; /* sized for BOX_MAX: kept off the stack */
    uint32_t tmp[BOX_MAX], sw = 0;
    int32_t v = to_fix16(vc - dv * 0.5f);
    const int32_t end = v + to_fix16(dv);
    memset(sr, 0, (size_t)n * sizeof(*sr)); memset(sg, 0, (size_t)n * sizeof(*sg));
    memset(sb, 0, (size_t)n * sizeof(*sb)); memset(sa, 0, (size_t)n * sizeof(*sa));
    while (v < end) {
        int32_t next = (int32_t)(((uint32_t)(v >> 16) + 1u) << 16);
        if (next > end) next = end;
        const uint32_t w = (uint32_t)(next - v) >> 8; /* 8.8 coverage of this texel row */
        const uint32_t *row = box_row_cached(tmp, n, texid, px, t16, wrapf(v >> 16, th, wt, mv) * tw, uf, duf, tw, ws, mu, premul);
        for (int i = 0; i < n; i++) { /* used before the next call: row may be tmp */
            const uint32_t t = row[i], a = t >> 24, k = premul ? a * w : 255u * w;
            sr[i] += ((t >> 16) & 0xff) * k; sg[i] += ((t >> 8) & 0xff) * k; sb[i] += (t & 0xff) * k;
            sa[i] += a * w;
        }
        sw += w;
        v = next;
    }
    for (int i = 0; i < n; i++) {
        const uint32_t kd = premul ? sa[i] : 255u * sw;
        out[i] = (sw == 0 || kd == 0) ? 0 : ((sa[i] / sw) << 24) | ((sr[i] / kd) << 16) | ((sg[i] / kd) << 8) | (sb[i] / kd);
    }
    return out;
}

/* texel_box in both directions, for a flipped rectangle (G_TEXRECTFLIP): its glyphs lie on
 * their side, so the squeeze to the output shrinks T across the screen and S down it, and S
 * carries the letters' upright strokes, which point samples down it can miss (the I of the
 * folder tab's PREVIOUS). Each direction whose step is over 1 texel is area-averaged over the
 * pixel's footprint (8.8 coverage per texel); the others take the texel under the centre. */
static inline uint32_t texel_box_flip(const void *px, bool t16, int tw, int th, int ws, int wt, int mu, int mv, bool premul,
                                      float uc, float du, float vc, float dv) {
    int32_t us = to_fix16(uc - du * 0.5f), ue = us + to_fix16(du), vs = to_fix16(vc - dv * 0.5f), ve = vs + to_fix16(dv);
    if (du <= 1.02f) { us = (int32_t)((uint32_t)ifloorf(uc) << 16); ue = us + 65536; }
    if (dv <= 1.02f) { vs = (int32_t)((uint32_t)ifloorf(vc) << 16); ve = vs + 65536; }
    uint64_t sr = 0, sg = 0, sb = 0, sa = 0, sw = 0;
    for (int32_t v = vs; v < ve;) {
        int32_t vn = (int32_t)(((uint32_t)(v >> 16) + 1u) << 16);
        if (vn > ve) vn = ve;
        const uint32_t wv = (uint32_t)(vn - v) >> 8;
        const int rb = wrapf(v >> 16, th, wt, mv) * tw;
        for (int32_t u = us; u < ue;) {
            int32_t un = (int32_t)(((uint32_t)(u >> 16) + 1u) << 16);
            if (un > ue) un = ue;
            const uint64_t w = (uint64_t)(((uint32_t)(un - u) >> 8) * wv);
            const uint32_t t = texel_at(px, t16, rb + wrapf(u >> 16, tw, ws, mu));
            const uint64_t a = t >> 24, k = premul ? a * w : 255u * w;
            sr += ((t >> 16) & 0xff) * k; sg += ((t >> 8) & 0xff) * k; sb += (t & 0xff) * k;
            sa += a * w; sw += w;
            u = un;
        }
        v = vn;
    }
    if (sw == 0) return 0;
    const uint64_t kd = premul ? sa : 255u * sw;
    if (kd == 0) return 0;
    return (uint32_t)(((sa / sw) << 24) | ((sr / kd) << 16) | ((sg / kd) << 8) | (sb / kd));
}

bool gfx_soft_rect_flip SAVESTATE_EXCLUDE; /* gfx_pc: the next rectangle is a G_TEXRECTFLIP (S down, T across) */
/* Magnified 2D sprites: the N64 filters texture rectangles like triangles, so a sprite drawn bigger
 * than its texture comes out smooth; the port sampled them nearest (the blood overlay, a 96x80
 * texture stretched over the screen, came out in square blocks). gfx_pc sets gfx_soft_2d_bilin
 * from the draw's G_TF_BILERP per sprite; a sprite magnified along either axis then takes the 3D
 * path's bilerp at the pixel centre. Shrunk sprites (the HUD at 240) keep the area filter, 1:1 ones
 * the copy. As Smash 872636f. GE_2DBILIN=0: nearest, as before. */
bool gfx_soft_2d_bilin SAVESTATE_EXCLUDE;
static int sBilin2DOn SAVESTATE_EXCLUDE = -1;
static int s2dSkip SAVESTATE_EXCLUDE = -1; /* GE_2DSKIP (see the sprite loop) */
void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    def_resolve_all(); /* deferred backgrounds: before anything drawn outside raster_tri */
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) tex = NULL;
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
        const bool flipr = gfx_soft_rect_flip && tex; /* x carries T (v), y carries S (u) */
        if (flipr) {
            if (x1 < x0) { int t = x0; x0 = x1; x1 = t; float tv = v0; v0 = v1; v1 = tv; }
            if (y1 < y0) { int t = y0; y0 = y1; y1 = t; float tu = u0; u0 = u1; u1 = tu; }
            /* a glyph one N64 pixel thin (the tab's I, 1 texel of S) rounds to no row at the
             * output's 0.73 scale: keep one, each glyph stands alone */
            if (x1 == x0) x1 = x0 + 1;
            if (y1 == y0) y1 = y0 + 1;
        } else {
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; float tu = u0; u0 = u1; u1 = tu; }
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; float tv = v0; v0 = v1; v1 = tv; }
        }
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
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in || sForceIn;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in || sForceIn || sForceAIn;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa;
        /* A textured sprite whose alpha is the constant alone or constant x texel (<= the constant: ta*ca/255 <= ca)
         * with the constant already under the reject writes no pixel: every pixel's fa < amin, in the flipped loop,
         * the general loop and the box loops alike (the plain and texel-is-pixel paths need ca 255 or no alpha
         * input). Skipped whole (the OoT port round 53's OOT_2DSKIP: a sprite drawn every frame at alpha 0). Exact.
         * GE_2DSKIP=0: every pixel walked, as before. */
        if (s2dSkip < 0) { const char *e = getenv("GE_2DSKIP"); s2dSkip = !(e && e[0] == '0'); }
        if (s2dFill < 0) { const char *e = getenv("GE_2DFILL"); s2dFill = !(e && e[0] == '0'); }
        if (s2dSkip && !sALerp && tex && use_alpha && (!a_tex || a_in) && ca < amin) continue;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (gfx_soft_hud_box < 0) { const char *e = getenv("GE_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
        /* shrunk 1..3x along S: area-average (see texel_box) */
        const bool boxf = tex && gfx_soft_hud_box && dudx > 1.02f && dudx <= 3.0f;
        const bool texel_is_pixel = tex && rgb_tex && a_tex && !rgb_texa && !sALerp &&
                           ((!rgb_in && !a_in) || (!sTexAdd && !sTexLerp && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
        if (sBilin2DOn < 0) { const char *e = getenv("GE_2DBILIN"); sBilin2DOn = !(e && e[0] == '0'); }
        const bool mag2 = tex && sBilin2DOn && gfx_soft_2d_bilin && !boxf &&
                          (fabsf(dudx) < 0.999f || fabsf(dvdy) < 0.999f);
        const bool plain = !boxf && !mag2 && !t16 && texel_is_pixel;
        const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        const int mu = tex ? pow2mask(tw) : -1, mv = tex ? pow2mask(th) : -1;
        const int ws2 = tex ? tex->wrap_s : 0, wt2 = tex ? tex->wrap_t : 0; /* pinned: the row[] stores are an aliasing barrier */
        const int32_t duf = tex ? to_fix16(dudx) : 0;
        const int32_t uf_row = tex ? to_fix16(u0 + (cx0 - x0 + 0.5f) * dudx) : 0; /* every row starts here */
        if (sPickX >= cx0 && sPickX < cx1 && sPickY >= cy0 && sPickY < cy1 && sPickY >= 0) /* GE_PICK: the 2D sprites over the pixel */
            port_log("  PICK2D (%d,%d) %08X: rect x %d..%d y %d..%d u %.3f..%.3f v %.3f..%.3f dudx %.4f dvdy %.4f tex %u %dx%d%s wrap %d/%d uf %d duf %d box %d boxc? plain %d alpha %d flip %d col %02X%02X%02X%02X\n",
                     sPickX, sPickY, (unsigned)sFB[sPickY * sW + sPickX], x0, x1, y0, y1, u0, u1, v0, v1, dudx, dvdy,
                     tex ? (unsigned)sBoundTex[0] : 0u, tw, th, t16 ? " 16b" : "", ws2, wt2, (int)uf_row, (int)duf, (int)boxf,
                     (int)plain, (int)use_alpha, (int)flipr, cr, cg, cb2, ca);
        if (sPickX >= cx0 && sPickX < cx1 && sPickY >= cy0 && sPickY < cy1 && tex) {
            for (int ty = (int) v0; ty < (int) v1 && ty < th; ty++) {
                char line[80]; int n = 0;
                for (int tx = (int) u0; tx < (int) u1 && tx < tw && n < 70; tx++) n += snprintf(line + n, sizeof(line) - n, "%02X ", (unsigned)(texel_at(texpx, t16, ty * tw + tx) >> 24));
                port_log("    ta row %d: %s\n", ty, line);
            }
        }
        /* area filter a row at a time, through the cross-frame row cache */
        const bool boxc = boxf && cx1 > cx0 && cx1 - cx0 <= BOX_MAX;
        if (sTextV < 0) { const char *e = getenv("GE_TEXTV"); sTextV = !(e && e[0] == '0'); }
        const bool boxcv = boxc && sTextV && ((dvdy > 1.02f && dvdy <= 3.0f) || (dvdy < -1.02f && dvdy >= -3.0f));
        if (flipr) {
            /* G_TEXRECTFLIP: T (v) across, S (u) down, area-filtered where the output shrinks
             * the texture 1..3x (texel_box_flip) */
            const float dvdx = (v1 - v0) / (float)(x1 - x0), dudy = (u1 - u0) / (float)(y1 - y0);
            const float adv = dvdx < 0.0f ? -dvdx : dvdx, adu = dudy < 0.0f ? -dudy : dudy;
            const bool boxfl = gfx_soft_hud_box && ((adv > 1.02f && adv <= 3.0f) || (adu > 1.02f && adu <= 3.0f));
            for (int y = cy0; y < cy1; y++) {
                uint32_t *row = &sFB[y * sW];
                const float uu = u0 + (y - y0 + 0.5f) * dudy;
                const int iu = wrapf(ifloorf(uu), tw, ws2, mu);
                for (int x = cx0; x < cx1; x++) {
                    const float vv = v0 + (x - x0 + 0.5f) * dvdx;
                    const uint32_t t = boxfl ? texel_box_flip(texpx, t16, tw, th, ws2, wt2, mu, mv, use_alpha, uu, adu > 3.0f ? 1.0f : adu,
                                                              vv, adv > 3.0f ? 1.0f : adv)
                                             : texel_at(texpx, t16, wrapf(ifloorf(vv), th, wt2, mv) * tw + iu);
                    const int ta = (t>>24)&0xff;
                    const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff;
                    int fr, fg, fb, fa;
                    if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }
                    else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }
                    else if (sTexLerp) { fr=sLerpR+((cr-sLerpR)*tr)/255; fg=sLerpG+((cg-sLerpG)*tg)/255; fb=sLerpB+((cb2-sLerpB)*tb)/255; }
                    else if (sTexAdd) { fr=tr+cr; fg=tg+cg; fb=tb+cb2; if (fr>255)fr=255; if (fg>255)fg=255; if (fb>255)fb=255; }
                    else              { fr=(tr*cr)/255; fg=(tg*cg)/255; fb=(tb*cb2)/255; }
                    fa = sALerp ? sALerpK + ((ca - sALerpK) * ta) / 255 : !a_tex ? ca : (!a_in ? ta : (ta*ca)/255);
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
            continue;
        }
        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = &sFB[y * sW];
            if (!tex) { /* solid rect */
                if (!use_alpha || ca >= 255) {
                    fill32(row + cx0, argb((uint8_t)cr, (uint8_t)cg, (uint8_t)cb2, 255), cx1 - cx0);
                } else if (ca >= amin) {
                    blend_solid_row(row + cx0, cx1 - cx0, cr, cg, cb2, ca); /* GE_2DFILL: NEON, the same values */
                }
                continue;
            }
            const float v = v0 + (y - y0 + 0.5f) * dvdy;
            const int32_t vf_row = mag2 ? to_fix16(v) : 0; /* bilerp: texel coordinate of the pixel centre */
            const int iv = wrapf(ifloorf(v), th, wt2, mv);
            const int trow_base = iv * tw; /* texel index of this scanline's row start */
            int32_t uf = uf_row;
            uint32_t boxtmp[BOX_MAX];
            const uint32_t *boxrow = boxcv ? box_rows_v(boxtmp, cx1 - cx0, sBoundTex[0], texpx, t16, v, dvdy < 0.0f ? -dvdy : dvdy,
                                                        th, wt2, mv, uf_row, duf, tw, ws2, mu, use_alpha)
                                   : boxc ? box_row_cached(boxtmp, cx1 - cx0, sBoundTex[0], texpx, t16, trow_base, uf_row,
                                                           duf, tw, ws2, mu, use_alpha)
                                          : NULL;
            if (boxc && texel_is_pixel) {
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
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = boxc ? boxrow[x - cx0]
                                 : boxf ? texel_box(texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha)
                                 : mag2 ? bilerp(texpx, t16, tw, th, ws2, wt2, uf, vf_row, mu, mv)
                                        : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
                const int ta = (t>>24)&0xff;
                /* alpha first: a rejected pixel skips the colour work (PD's lens flares are
                 * mostly alpha-0 texels magnified 4x). div255 is exact for these products. */
                const int fa = sALerp ? sALerpK + ((ca - sALerpK) * ta) / 255 : !a_tex ? ca : (!a_in ? ta : div255(ta*ca));
                if (use_alpha && fa < amin) continue;
                const int tr = rgb_texa ? ta : (t>>16)&0xff, tg = rgb_texa ? ta : (t>>8)&0xff, tb = rgb_texa ? ta : t&0xff;
                int fr, fg, fb;
                if (!rgb_tex)     { fr=cr; fg=cg; fb=cb2; }
                else if (!rgb_in) { fr=tr; fg=tg; fb=tb; }
                else if (sTexLerp) { fr=sLerpR+((cr-sLerpR)*tr)/255; fg=sLerpG+((cg-sLerpG)*tg)/255; fb=sLerpB+((cb2-sLerpB)*tb)/255; }
                else if (sTexAdd) { fr=tr+cr; fg=tg+cg; fb=tb+cb2; if (fr>255)fr=255; if (fg>255)fg=255; if (fb>255)fb=255; }
                else              { fr=div255(tr*cr); fg=div255(tg*cg); fb=div255(tb*cb2); }
                if (use_alpha && fa < 255) {
                    const uint32_t d = row[x];
                    const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;
                    fr = div255(fr*fa + dr*(255-fa));
                    fg = div255(fg*fa + dg*(255-fa));
                    fb = div255(fb*fa + db*(255-fa));
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
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { if (w) *w = sNatW; if (h) *h = sNatH; return sNatFB; }

/* The 3D pass's size (the PERF line): native when unified, else the internal target. */
void gfx_soft_get_internal_size(int *w, int *h) {
    *w = sUnified ? sNatW : sIntW;
    *h = sUnified ? sNatH : sIntH;
}

static void alloc_target(uint32_t **fb, uint16_t **zb, int *w, int *h, int nw, int nh) {
    if (*fb && *w == nw && *h == nh) return;
    free(*fb); free(*zb);
    *w = nw; *h = nh;
    *fb = (uint32_t *)malloc((size_t)nw * nh * 4);
    *zb = (uint16_t *)malloc((size_t)nw * nh * 2);
}

static void select_target(bool is3d) {
    def_resolve_all(); /* the pending batches belong to the current target */
    extern struct GfxDimensions gfx_current_dimensions;
    if (sPhase3D && !is3d && sUnified) tj_fill(); /* the 3D phase ends (not unified: gfx_scegu_set_phase, end_frame) */
    sPhase3D = is3d;
    if (is3d && !sUnified) { sFB = sIntFB; sZB = sIntZB; sW = sIntW; sH = sIntH; }
    else                   { sFB = sNatFB; sZB = sNatZB; sW = sNatW; sH = sNatH; }
    if (is3d) tj_prepare();
    sScX0 = 0; sScY0 = 0; sScX1 = sW; sScY1 = sH;
    sVpX = 0; sVpY = 0; sVpW = sW; sVpH = sH;
    gfx_current_dimensions.width = (uint32_t)sW;
    gfx_current_dimensions.height = (uint32_t)sH;
    gfx_current_dimensions.aspect_ratio = sH ? (float)sW / (float)sH : 1.0f;
}

/* Nearest-upscale the internal 3D image into native where the 3D wrote
 * (alpha == 255), then clear it for any later 3D batch this frame. */
static void composite_internal(void) {
    def_resolve_all();
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    PROF_BEGIN();
    nat_ready();
    if (sIntW == sNatW && sIntH == sNatH) {
        for (int i = 0; i < sNatW * sNatH; i++) if (sIntFB[i] >> 24) sNatFB[i] = sIntFB[i];
    } else if (sIntW == 2 * sNatW && sIntH == 2 * sNatH) {
        /* supersampled (gfx_soft_ss2x): the 2x2 average, the unwritten subpixels taking the native
         * pixel beneath -- coverage antialiasing against the backdrop */
        for (int y = 0; y < sNatH; y++) {
            const uint32_t *s0 = sIntFB + (2 * y) * sIntW, *s1 = s0 + sIntW;
            uint32_t *dst = sNatFB + y * sNatW;
            for (int x = 0; x < sNatW; x++) {
                const uint32_t q[4] = { s0[2 * x], s0[2 * x + 1], s1[2 * x], s1[2 * x + 1] };
                int n = 0; uint32_t r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++)
                    if (q[k] >> 24) { n++; r += (q[k] >> 16) & 0xff; g += (q[k] >> 8) & 0xff; b += q[k] & 0xff; }
                if (!n) continue;
                const uint32_t d = dst[x], m = 4 - n;
                r += m * ((d >> 16) & 0xff); g += m * ((d >> 8) & 0xff); b += m * (d & 0xff);
                dst[x] = 0xFF000000u | ((r >> 2) << 16) | ((g >> 2) << 8) | (b >> 2);
            }
        }
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
    def_resolve_all();
    PROF_BEGIN();
    nat_ready();
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
void gfx_scegu_set_sky_pass(bool on);
void gfx_scegu_set_phase(int is3d) {
    def_resolve_all();
    if ((bool)is3d == sPhase3D) return;
    if (sSkyPass) { sPhase3D = (bool)is3d; return; } /* unified: one target, stay on the sky buffer */
    if (!sUnified) { /* unified: everything is already on native, nothing to move */
        if (!is3d) tj_fill(); /* the 3D target, before it is composited */
        if (!is3d && sIntDirty) composite_internal();
        if (is3d && sNatDirty) seed_internal_from_native();
    }
    select_target((bool)is3d);
}

/* Set per frame by hh_gfx.c (hhFrontSupersample): this frame's 3D is drawn at twice the native
 * size and box-filtered down (composite_internal). The intro's GoldenEye logo is 339 thin
 * polygons, not a texture: without the RDP's coverage AA its edges stair-stepped and strokes
 * under a pixel wide fell between pixel centres (round 17). */
int gfx_soft_ss2x SAVESTATE_EXCLUDE;
static void ensure_targets(void) {
    def_resolve_all();
    extern struct GfxDimensions gfx_current_dimensions;
    if (sWantNatW <= 0) { /* no WM request yet: native = whatever gfx_pc has */
        sWantNatW = (int)gfx_current_dimensions.width; sWantNatH = (int)gfx_current_dimensions.height;
        sWantIntW = sWantNatW; sWantIntH = sWantNatH;
    }
    const int ow = sNatW, oh = sNatH, iw = sIntW, ih = sIntH;
    alloc_target(&sNatFB, &sNatZB, &sNatW, &sNatH, sWantNatW, sWantNatH);
    alloc_target(&sIntFB, &sIntZB, &sIntW, &sIntH, gfx_soft_ss2x ? sWantNatW * 2 : sWantIntW,
                 gfx_soft_ss2x ? sWantNatH * 2 : sWantIntH);
    sUnified = (sIntW == sNatW && sIntH == sNatH);
    sGateScale = (sNatW > 0 && sNatH > 0) ? (float)((double)sIntW * sIntH / ((double)sNatW * sNatH)) : 1.0f;
    if (ow != sNatW || oh != sNatH || iw != sIntW || ih != sIntH) { build_maps(); sIntClean = false; }
}

static void gfx_soft_init(void) {
    unpack_lut_init();
#if defined(HH_CM_NEON)
    { const char *e = getenv("GE_CMNEON"); if (e && e[0] == '0') sCmNeon = 0; }
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
 * frame is kept and every frame starts from it instead of black. */
static uint32_t *sPauseFB SAVESTATE_EXCLUDE;
static int sPauseW, sPauseH;
static bool sPauseActive, sPauseValid;
void gfx_soft_capture_pause_background(void) {
    def_resolve_all();
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
#if defined(MIYOO)
    { extern void gfx_miyoo_present_fence(void); gfx_miyoo_present_fence(); } /* the last frame taken (gfx_sdl12.c sPresSrc) */
#endif
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
    gfx_soft_depth_written = false; /* every target's depth is cleared here, and only here */
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        if (!sIntClean) memset(sIntFB, 0, (size_t)sIntW * sIntH * 4);
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    PROF_END(PROF_CLR);
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
/* Perfect Dark's G_COPYFB_EXT (pd_fbext.h): a w x h patch of what is drawn so far, at (x, y) in the
 * N64's 320-wide frame, as big-endian RGBA5551 (the texel format the game then samples it as). */
void gfx_soft_read_rgba16(int x, int y, int w, int h, uint8_t *dst) {
    extern int gfx_pc_frame_w(void);
    def_resolve_all();
    extern int gfx_pc_frame_h(void); /* the frame's own rows (PD: 220 in play), as gfx_pc.c maps everything else */
    const int fw = gfx_pc_frame_w() > 0 ? gfx_pc_frame_w() : 320, fh = gfx_pc_frame_h() > 0 ? gfx_pc_frame_h() : fw * 3 / 4;
    for (int j = 0; j < h; j++) {
        int sy = (y + j) * sH / fh;
        if (sy < 0) sy = 0; if (sy >= sH) sy = sH - 1;
        for (int i = 0; i < w; i++) {
            int sx = (x + i) * sW / fw;
            if (sx < 0) sx = 0; if (sx >= sW) sx = sW - 1;
            const uint32_t p = sFB ? sFB[sy * sW + sx] : 0;
            const uint16_t c = (uint16_t)((((p >> 16) & 0xff) >> 3) << 11 | (((p >> 8) & 0xff) >> 3) << 6 | ((p & 0xff) >> 3) << 1 | 1);
            dst[(j * w + i) * 2] = (uint8_t)(c >> 8);
            dst[(j * w + i) * 2 + 1] = (uint8_t)c;
        }
    }
}

/* Perfect Dark's screen copies (video_miyoo.c ids 100, 101; pd_fbext.h): the finished frame saved at
 * the target's size, and drawn back blended (the motion blur: out = dst + (saved * env - dst) * a). */
static uint32_t *sScr[2] SAVESTATE_EXCLUDE; static int sScrW[2] SAVESTATE_EXCLUDE, sScrH[2] SAVESTATE_EXCLUDE;
void gfx_soft_scr_capture(int slot) {
    int w = 0, h = 0;
    uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    if (slot < 0 || slot > 1 || !fb || w <= 0 || h <= 0) return;
    if (sScrW[slot] != w || sScrH[slot] != h) {
        free(sScr[slot]);
        sScr[slot] = (uint32_t *)malloc((size_t)w * h * 4);
        sScrW[slot] = sScr[slot] ? w : 0; sScrH[slot] = sScr[slot] ? h : 0;
    }
    if (sScr[slot]) memcpy(sScr[slot], fb, (size_t)w * h * 4);
}
/* the current target as drawn so far (G_COPYFB_EXT mid-frame: camspy's lens) */
void gfx_soft_scr_capture_now(int slot) {
    if (slot < 0 || slot > 1) return;
    def_resolve_all();
    target_ready();
    if (sScrW[slot] != sW || sScrH[slot] != sH) {
        free(sScr[slot]);
        sScr[slot] = (uint32_t *)malloc((size_t)sW * sH * 4);
        sScrW[slot] = sScr[slot] ? sW : 0; sScrH[slot] = sScr[slot] ? sH : 0;
    }
    if (sScr[slot]) memcpy(sScr[slot], sFB, (size_t)sW * sH * 4);
}
/* dest (x0,y0)-(x1,y1) and source (s0,t0)-(s1,t1) in the game's pixels (the source image iw x ih) */
void gfx_soft_scr_blend(int slot, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1,
                        int iw, int ih, int r, int g, int b, int a) {
    extern int gfx_pc_frame_w(void);
    if (slot < 0 || slot > 1 || !sScr[slot] || a <= 0 || x1 <= x0 || y1 <= y0) return;
    def_resolve_all();
    target_ready();
    if (sScrW[slot] != sW || sScrH[slot] != sH) return; /* the target changed size since: skip a frame */
    extern int gfx_pc_frame_h(void); /* the frame's own rows (PD: 220 in play), as gfx_pc.c maps everything else */
    const int fw = gfx_pc_frame_w() > 0 ? gfx_pc_frame_w() : 320, fh = gfx_pc_frame_h() > 0 ? gfx_pc_frame_h() : fw * 3 / 4;
    if (iw <= 0) iw = fw;
    if (ih <= 0) ih = fh;
    int cx0 = (int)(x0 * sW / fw), cx1 = (int)(x1 * sW / fw), cy0 = (int)(y0 * sH / fh), cy1 = (int)(y1 * sH / fh);
    if (cx1 <= cx0) cx1 = cx0 + 1;
    if (cy1 <= cy0) cy1 = cy0 + 1;
    /* target pixel -> source target pixel, linear */
    const float tx0 = x0 * sW / fw, ty0 = y0 * sH / fh;
    const float dsx = ((s1 - s0) * sW / iw) / ((x1 - x0) * sW / fw), dsy = ((t1 - t0) * sH / ih) / ((y1 - y0) * sH / fh);
    const float sx0 = s0 * sW / iw, sy0 = t0 * sH / ih;
    cx0 = imax(imax(cx0, sScX0), 0); cx1 = imin(imin(cx1, sScX1), sW);
    cy0 = imax(imax(cy0, sScY0), 0); cy1 = imin(imin(cy1, sScY1), sH);
    if (a > 255) a = 255;
    for (int y = cy0; y < cy1; y++) {
        uint32_t *row = &sFB[y * sW];
        int sy = (int)(sy0 + (y + 0.5f - ty0) * dsy);
        if (sy < 0) sy = 0; if (sy >= sH) sy = sH - 1;
        const uint32_t *srow = &sScr[slot][sy * sW];
        for (int x = cx0; x < cx1; x++) {
            int sx = (int)(sx0 + (x + 0.5f - tx0) * dsx);
            if (sx < 0) sx = 0; if (sx >= sW) sx = sW - 1;
            const uint32_t s = srow[sx], d = row[x];
            const int sr = (((s >> 16) & 0xff) * r) / 255, sg = (((s >> 8) & 0xff) * g) / 255, sb = ((s & 0xff) * b) / 255;
            const int dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            row[x] = argb((uint8_t)(dr + ((sr - dr) * a) / 255), (uint8_t)(dg + ((sg - dg) * a) / 255),
                          (uint8_t)(db + ((sb - db) * a) / 255), 255);
        }
    }
}

/* PC-port G_MODULATE_EXT fill (gfx_dp_fill_rectangle): the target multiplied by a colour, in the game's
 * pixels (night vision's green lines). */
void gfx_soft_modulate_rect(float x0, float y0, float x1, float y1, int r, int g, int b) {
    extern int gfx_pc_frame_w(void);
    def_resolve_all();
    target_ready();
    extern int gfx_pc_frame_h(void); /* the frame's own rows (PD: 220 in play), as gfx_pc.c maps everything else */
    const int fw = gfx_pc_frame_w() > 0 ? gfx_pc_frame_w() : 320, fh = gfx_pc_frame_h() > 0 ? gfx_pc_frame_h() : fw * 3 / 4;
    int cx0 = (int)(x0 * sW / fw), cx1 = (int)(x1 * sW / fw), cy0 = (int)(y0 * sH / fh), cy1 = (int)(y1 * sH / fh);
    if (cy1 <= cy0) cy1 = cy0 + 1;
    cx0 = imax(imax(cx0, sScX0), 0); cx1 = imin(imin(cx1, sScX1), sW);
    cy0 = imax(imax(cy0, sScY0), 0); cy1 = imin(imin(cy1, sScY1), sH);
    for (int y = cy0; y < cy1; y++) {
        uint32_t *row = &sFB[y * sW];
        for (int x = cx0; x < cx1; x++) {
            const uint32_t d = row[x];
            row[x] = argb((uint8_t)((((d >> 16) & 0xff) * r) / 255), (uint8_t)((((d >> 8) & 0xff) * g) / 255),
                          (uint8_t)(((d & 0xff) * b) / 255), 255);
        }
    }
}

/* A fill rectangle whose colour is the RDP's NOISE input: Perfect Dark's static (bondview.c bviewDrawStatic, the PC
 * port's (NOISE - 0) * ENV with alpha ENV, G_RM_CLD_SURF -- the Infiltration intro's bursts, the Slayer rocket out of
 * bounds). The combiner models NOISE as a constant 128 (a flat grey veil, TEST15); TEST16's per-pixel white noise was
 * far too bright and even ("super harsh and unfiltered", device). The N64 (bviewDrawStatic's PLATFORM_N64 loop,
 * bviewCopyPixels) draws each row as 320 bytes of RDRAM read as I8: fb2 = a random 256-byte-aligned address in the
 * first megabyte, the row a random one of 240 lines of 320 16-bit pixels from there, times ENV, blended by ENV alpha.
 * Its reference frames (angrylion, Infiltration intro): horizontal streaks, 40-54% of pixels near black, mean 33-57.
 * Here the same reads come from anywhere in the game's heap (memp: the loaded stage data and zeroed space, the port's RDRAM),
 * rows chosen by a counter hash so a fixed-clock run stays repeatable. PD_STATICNOISE=1: TEST16's white noise. */
void gfx_soft_noise_rect(float x0, float y0, float x1, float y1, int r, int g, int b, int a) {
    extern int gfx_pc_frame_w(void), gfx_pc_frame_h(void);
    extern unsigned char *g_MempHeap;
    extern unsigned int g_MempHeapSize;
    static uint32_t seed SAVESTATE_EXCLUDE = 0x9E3779B9u;
    static int white SAVESTATE_EXCLUDE = -1;
    if (white < 0) { const char *e = getenv("PD_STATICNOISE"); white = e && e[0] == '1'; }
    def_resolve_all();
    target_ready();
    const int fw = gfx_pc_frame_w() > 0 ? gfx_pc_frame_w() : 320, fh = gfx_pc_frame_h() > 0 ? gfx_pc_frame_h() : fw * 3 / 4;
    int cx0 = (int)(x0 * sW / fw), cx1 = (int)(x1 * sW / fw), cy0 = (int)(y0 * sH / fh), cy1 = (int)(y1 * sH / fh);
    cx0 = imax(imax(cx0, sScX0), 0); cx1 = imin(imin(cx1, sScX1), sW);
    cy0 = imax(imax(cy0, sScY0), 0); cy1 = imin(imin(cy1, sScY1), sH);
    if (a <= 0) return;
    const int ia = 255 - a;
    uint32_t h = (seed += 0x6D2B79F5u);
#define NOISE_NEXT() (h ^= h << 13, h ^= h >> 17, h ^= h << 5, h) /* xorshift32 */
    const int mem = !white && g_MempHeap && g_MempHeapSize >= 0x140000u;
    const int span = cx1 - cx0 > 0 ? cx1 - cx0 : 1;
    for (int y = cy0; y < cy1; y++) {
        uint32_t *row = &sFB[y * sW];
        const unsigned char *src = NULL;
        if (mem) {
            /* a row of mostly zero (the heap's unused space; the N64's RAM is full) is passed over, up to 8 tries: the
             * N64's frames have their black from the zero bytes inside rows of data, not from whole empty rows */
            for (int tries = 0; tries < 8; tries++) {
                const unsigned char *p = g_MempHeap + ((NOISE_NEXT() % (g_MempHeapSize - 0x30000u)) & ~0xffu) + (NOISE_NEXT() % 240u) * 640u;
                int nz = 0;
                for (int i = 0; i < 320; i += 4) nz += p[i] != 0;
                src = p;
                if (nz >= 20) break;
            }
        }
        for (int x = cx0; x < cx1; x++) {
            /* the row's 320 bytes across the rectangle (one per native pixel at 320 wide) */
            const int n = mem ? src[((x - cx0) * 320) / span] : (int)(NOISE_NEXT() & 0xff);
            const uint32_t d = row[x];
            const int sr = n * r / 255, sg = n * g / 255, sb = n * b / 255;
            row[x] = argb((uint8_t)((sr * a + (int)((d >> 16) & 0xff) * ia) / 255),
                          (uint8_t)((sg * a + (int)((d >> 8) & 0xff) * ia) / 255),
                          (uint8_t)((sb * a + (int)(d & 0xff) * ia) / 255), 255);
        }
    }
#undef NOISE_NEXT
}

/* The 3D target's depth under pixel (x, y) of the game's frame (gfx_pc_frame_w x _h), 0xFFFF where nothing
 * has drawn with depth since the clear. Perfect Dark's sun flares (G_ARTPROBE_EXT, artifact.c) test it as the
 * N64 tests its z-buffer: a flare circle shows only where the sky is uncovered. */
int gfx_soft_depth_at(int x, int y) {
    extern int gfx_pc_frame_w(void), gfx_pc_frame_h(void);
    const uint16_t *zb = sUnified ? sNatZB : sIntZB;
    const int w = sUnified ? sNatW : sIntW, h = sUnified ? sNatH : sIntH;
    const int fw = gfx_pc_frame_w(), fh = gfx_pc_frame_h();
    if (!zb || fw <= 0 || fh <= 0 || x < 0 || y < 0 || x >= fw || y >= fh) return 0xFFFF;
    const int tx = (2 * x + 1) * w / (2 * fw), ty = (2 * y + 1) * h / (2 * fh); /* the pixel's centre */
    return zb[ty * w + tx];
}

/* Perfect Dark's G_CLEAR_DEPTH_EXT mid-frame (viPrepareZbuf: once before the world, once more
 * before the first-person gun, so the gun never goes into walls). Nothing written since the
 * frame's own clear: nothing to do. Otherwise the deferred backgrounds resolve first (they rely
 * on the depth they were recorded against), then every target's depth is cleared; depth stays
 * marked written, so nothing later takes the cleared buffer for the frame-start one. */
void gfx_soft_clear_depth_mid(void) {
    if (!gfx_soft_depth_written) return;
    def_resolve_all();
    tj_fill(); /* the world's hairline cracks, while its depth is still there (GE_TJFILL) */
    PROF_BEGIN();
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    if (!sUnified) memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    PROF_END(PROF_CLR);
}
/* The folder screens' right edge (the user's "always stretch", 2026-09-29): GoldenEye's 440x330
 * menus leave their last ~6 columns black (z64: 9 of 640), a margin a TV's overscan hid; on the
 * 240 screen it is a 3-px black strip. When a menu frame ends in 1-4 fully black columns and the
 * column before them is not, the rows are stretched over the full width (linear), so the strip is
 * cropped away. Frames that are black there anyway (the logos) and the 320-wide game are left
 * alone. GE_FILLEDGE=0: the strip stays. */
static int sFillEdge SAVESTATE_EXCLUDE = -1;
static void fill_edge_strip(void) {
    extern int gfx_pc_frame_w(void);
    if (sFillEdge < 0) { const char *e = getenv("GE_FILLEDGE"); sFillEdge = !(e && e[0] == '0'); }
    if (!sFillEdge || gfx_pc_frame_w() != 440 || sNatW != 240 || !sNatFB) return;
    const int w = sNatW, h = sNatH;
    int k = 0;
    for (; k <= 4; k++) { /* fully black columns at the right edge */
        const int x = w - 1 - k;
        int y = 0;
        for (; y < h; y++) if ((sNatFB[y * w + x] & 0x00F8F8F8u) != 0) break;
        if (y < h) break;
    }
    if (k < 1 || k > 4) return;
    const int sw = w - k; /* source width: the columns with the picture */
    const uint32_t step = (uint32_t)(((uint64_t)(sw - 1) << 16) / (uint32_t)(w - 1));
    uint32_t tmp[256];
    for (int y = 0; y < h; y++) {
        uint32_t *row = &sNatFB[y * w];
        memcpy(tmp, row, (size_t)sw * sizeof(uint32_t));
        uint32_t pos = 0;
        for (int x = 0; x < w; x++, pos += step) {
            const int i = (int)(pos >> 16);
            const int f = (int)((pos >> 8) & 0xff);
            const uint32_t a = tmp[i], b = tmp[i + 1 < sw ? i + 1 : i];
            const uint32_t rb = ((a & 0x00FF00FFu) * (uint32_t)(256 - f) + (b & 0x00FF00FFu) * (uint32_t)f) >> 8;
            const uint32_t g = (((a >> 8) & 0x00FF00FFu) * (uint32_t)(256 - f) + ((b >> 8) & 0x00FF00FFu) * (uint32_t)f) >> 8;
            row[x] = (rb & 0x00FF00FFu) | ((g & 0x00FF00FFu) << 8);
        }
    }
}
static void gfx_soft_end_frame(void) {
    def_resolve_all();
    if (sDefRest > 0) sDefRest--;
    else if (sDefMarkPx && sDefDrawPx * 4u > sDefMarkPx * 3u) sDefRest = DEF_REST;
    sDefMarkPx = sDefDrawPx = 0;
    if (sSkyPass) gfx_scegu_set_sky_pass(false);
    sFpModel = false; /* a tag pair cut short never leaks into the next frame */
    if (sPhase3D && !sUnified) tj_fill(); /* the 3D target, before it is composited (unified: select_target below) */
    nat_ready(); /* nothing reached native this frame: present it cleared */
    if (!sUnified && sIntDirty) composite_internal();
    gfx_soft_last_frame_had_3d = sHad3D;
    gfx_soft_frames_rendered++;
    select_target(false); /* leave native current for present/menu/dumps */
    fill_edge_strip();
#if defined(MIYOO)
    if (miyoo_worker_ready()) split_stop(); /* the second core sleeps until the next frame's first big triangle */
#endif
}
static void gfx_soft_finish_render(void) { }

/* -------------------------------------------------------------- shims --- */
/* gfx_pc.c calls these sceGu/texman helpers directly; software equivalents. */
void gfx_scegu_set_cull_mode(uint32_t cull) { (void)cull; /* GE-only API; winding cull lives in gfx_sp_tri1 */ }
void gfx_scegu_set_texfunc_add(bool on) { sTexAdd = on; }
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; }
void gfx_scegu_set_force_in(bool on) { sForceIn = on; }
void gfx_scegu_set_force_ain(bool on) { sForceAIn = on; }
void gfx_scegu_set_alpha_lerp(bool on, int k) { sALerp = on; sALerpK = k; }
void gfx_scegu_set_n64_filter(bool on, bool tex_lod) { sN64Filter = on; sTexLod = tex_lod; }
void gfx_scegu_set_lod_level(int max_level) { sMaxLevel = max_level; }
void gfx_scegu_set_ui_mode(bool on) {
    sUiMode = on;
    if (gfx_soft_hud_box < 0) { const char *e = getenv("GE_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
}

/* S2DEX background (gfx_pc gfx_s2dex_bg): nearest blit of a 16-bit RGBA5551
 * image in host order (fmt 0: the CPU-decoded JPEG rooms) or a CI8 image with
 * a big-endian RGBA16 TLUT (fmt 1) into the current target. No depth. */
void gfx_soft_draw_bg(const void *img, int img_w, int img_h, int fmt, const uint8_t *tlut, int x0, int y0, int x1,
                      int y1, float u0, float v0, float dudx, float dvdy) {
    def_resolve_all();
    if (!img || img_w <= 0 || img_h <= 0) return;
    if (fmt == 1 && !tlut) return;
    if (x0 < sScX0) x0 = sScX0; if (y0 < sScY0) y0 = sScY0;
    if (x1 > sScX1) x1 = sScX1; if (y1 > sScY1) y1 = sScY1;
    target_ready();
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
static uint32_t *sSkyFB SAVESTATE_EXCLUDE; static uint16_t *sSkyZB SAVESTATE_EXCLUDE; static int sSkyW SAVESTATE_EXCLUDE, sSkyH SAVESTATE_EXCLUDE; /* malloc'd: live over a load */
static uint32_t *sSkySaveFB SAVESTATE_EXCLUDE; static uint16_t *sSkySaveZB SAVESTATE_EXCLUDE; static int sSkySaveW SAVESTATE_EXCLUDE, sSkySaveH SAVESTATE_EXCLUDE;
static int sSkySaveVp[4], sSkySaveSc[4];

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

static void sky_upscale(void) {
    const int W = sSkySaveW, H = sSkySaveH, sw = sSkyW, sh = sSkyH;
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
        /* sky_row_4tap writes only where source row j drew (alpha): a row of it with nothing drawn
         * (below the horizon) writes nothing (round 24: those rows were a quarter of the pass) */
        if (!(rowor[j < sh ? j : sh - 1] >> 24)) continue;
        if (!((rowand[ca] & rowand[cb]) >> 24)) { sky_row_4tap(d, W, y, sw, sh); continue; }
        const uint32_t wya = (y & 1) ? 3 : 1, wyb = 4 - wya;
        const uint32_t *ra = sSkyFB + ca * sw, *rb = sSkyFB + cb * sw;
        int i0 = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        /* NEON (round 24): the same integer sums four columns / eight pixels at a time. GE_SKYNEON=0: scalar */
        static int sSkyNeon SAVESTATE_EXCLUDE = -1;
        if (sSkyNeon < 0) { const char *e = getenv("GE_SKYNEON"); sSkyNeon = !(e && e[0] == '0'); }
        const uint32x4_t mrb = vdupq_n_u32(0xFF00FFu), mg = vdupq_n_u32(0x00FF00u);
        if (sSkyNeon)
            for (; i0 + 4 <= sw; i0 += 4) {
                const uint32x4_t a = vld1q_u32(ra + i0), b = vld1q_u32(rb + i0);
                vst1q_u32(vrb + i0 + 1, vmlaq_n_u32(vmulq_n_u32(vandq_u32(a, mrb), wya), vandq_u32(b, mrb), wyb));
                vst1q_u32(vg + i0 + 1, vmlaq_n_u32(vmulq_n_u32(vandq_u32(a, mg), wya), vandq_u32(b, mg), wyb));
            }
#endif
        for (int i = i0; i < sw; i++) {
            vrb[i + 1] = wya * (ra[i] & 0xFF00FFu) + wyb * (rb[i] & 0xFF00FFu);
            vg[i + 1] = wya * (ra[i] & 0x00FF00u) + wyb * (rb[i] & 0x00FF00u);
        }
        vrb[0] = vrb[1]; vg[0] = vg[1];                /* x = 0 takes column 0 twice */
        vrb[sw + 1] = vrb[sw]; vg[sw + 1] = vg[sw];     /* the last odd x takes column sw-1 twice */
        /* x = 2i: columns i-1, i weighted 1:3;  x = 2i+1: columns i, i+1 weighted 3:1 */
        const int pairs = W >> 1;
        int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (sSkyNeon) {
            const uint32x4_t ff = vdupq_n_u32(0xFF000000u);
            for (; i + 4 <= pairs && i + 5 <= sw + 1; i += 4) { /* pixels 2i .. 2i+7 */
                const uint32x4_t lrb = vld1q_u32(vrb + i), crb = vld1q_u32(vrb + i + 1), rrb = vld1q_u32(vrb + i + 2);
                const uint32x4_t lg = vld1q_u32(vg + i), cg = vld1q_u32(vg + i + 1), rg = vld1q_u32(vg + i + 2);
                const uint32x4_t c3rb = vmulq_n_u32(crb, 3), c3g = vmulq_n_u32(cg, 3);
                const uint32x4_t ev = vorrq_u32(ff, vorrq_u32(vandq_u32(vshrq_n_u32(vaddq_u32(lrb, c3rb), 4), mrb),
                                                              vandq_u32(vshrq_n_u32(vaddq_u32(lg, c3g), 4), mg)));
                const uint32x4_t od = vorrq_u32(ff, vorrq_u32(vandq_u32(vshrq_n_u32(vaddq_u32(c3rb, rrb), 4), mrb),
                                                              vandq_u32(vshrq_n_u32(vaddq_u32(c3g, rg), 4), mg)));
                const uint32x4x2_t z = vzipq_u32(ev, od);
                vst1q_u32(d + 2 * i, z.val[0]);
                vst1q_u32(d + 2 * i + 4, z.val[1]);
            }
        }
#endif
        for (; i < pairs; i++) {
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
    def_resolve_all();
    if (gfx_trace_frames > 0) port_log("  soft: sky pass %d (unified %d nat %d)\n", (int) on, (int) sUnified, (int) (sFB == sNatFB));
    if (on == sSkyPass) return;
    if (on) {
        /* native at 100% only (round 26: allowed on the internal target too, the user's 90% trial ran
         * slower still in the Dam -- 90% is slower than 100% either way: the composite costs more than
         * the pixels save) */
        if (!sUnified || sFB != sNatFB || getenv("GE_FULLSKY") != NULL) return;
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
 * alpha, 2 = tile-1 alpha. dq_s / dq_t: tile 1 sits that many quarter texels
 * past the whole-texel corner (non-zero only for a filtered, unshifted tile 1):
 * sampled bilinearly there, as the RDP reads it. mips: give the pre-blend mip levels
 * when tile 0's texture has them (the waters: drawn as tile 0 alone before, mipped
 * like any texture). Rewrites dst in place when its size matches (the scrolling
 * water re-blends every frame). Returns false if it cannot. */
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
/* Tile 1 at a fractional texel (u, v) (texel centres at .5), bilinear, clamped or
 * wrapped per axis as the tile says: a coarse tile-1 map under a shift (GoldenEye's
 * explosion colours, 4x4 flame texels each) interpolates as the RDP's filter does
 * instead of showing its texels as blocks. */
static uint32_t hh_tile1_bilerp(const SoftTex *t1, float u, float v, int clamp_s, int clamp_t) {
    u -= 0.5f; v -= 0.5f;
    const int iu = (int)floorf(u), iv = (int)floorf(v);
    const int fu = (int)((u - (float)iu) * 256.0f), fv = (int)((v - (float)iv) * 256.0f);
    int u0 = iu, u1 = iu + 1, v0 = iv, v1 = iv + 1;
    if (clamp_s) { u0 = u0 < 0 ? 0 : u0 >= t1->w ? t1->w - 1 : u0; u1 = u1 < 0 ? 0 : u1 >= t1->w ? t1->w - 1 : u1; }
    else { u0 = hh_wrap(u0, t1->w); u1 = hh_wrap(u1, t1->w); }
    if (clamp_t) { v0 = v0 < 0 ? 0 : v0 >= t1->h ? t1->h - 1 : v0; v1 = v1 < 0 ? 0 : v1 >= t1->h ? t1->h - 1 : v1; }
    else { v0 = hh_wrap(v0, t1->h); v1 = hh_wrap(v1, t1->h); }
    const uint32_t p00 = texel_at(t1->px, t1->is16, v0 * t1->w + u0), p10 = texel_at(t1->px, t1->is16, v0 * t1->w + u1);
    const uint32_t p01 = texel_at(t1->px, t1->is16, v1 * t1->w + u0), p11 = texel_at(t1->px, t1->is16, v1 * t1->w + u1);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const int a = (p00 >> sh) & 0xff, b = (p10 >> sh) & 0xff, c = (p01 >> sh) & 0xff, d = (p11 >> sh) & 0xff;
        const int top = a * (256 - fu) + b * fu, bot = c * (256 - fu) + d * fu;
        out |= (uint32_t)(((top * (256 - fv) + bot * fv) + 32768) >> 16) << sh;
    }
    return out;
}
static inline float hh_tile_scale(int shift) {
    return shift == 0 ? 1.0f : shift <= 10 ? 1.0f / (float)(1 << shift) : (float)(1 << (16 - shift));
}
/* A tap of tile 1 (one axis): clamped to the tile or wrapped (a power of two: m = n - 1). */
static inline int hh_tap(int i, int n, int m, int clamp) {
    if (clamp) return i < 0 ? 0 : i >= n ? n - 1 : i;
    return m >= 0 ? i & m : hh_wrap(i, n);
}
/* t0 + (t1 - t0) * K per channel, K constant (hh_blend_px's modes 2/2, constants: the waters). */
static inline uint32_t hh_lerp_px(uint32_t p0, uint32_t p1, const uint8_t krgb[3], uint8_t ka) {
    return argb((uint8_t)hh_lerp8((p0 >> 16) & 0xff, (p1 >> 16) & 0xff, krgb[0]),
                (uint8_t)hh_lerp8((p0 >> 8) & 0xff, (p1 >> 8) & 0xff, krgb[1]),
                (uint8_t)hh_lerp8(p0 & 0xff, p1 & 0xff, krgb[2]), (uint8_t)hh_lerp8(p0 >> 24, p1 >> 24, ka));
}
/* One pre-blended texel from tile 0's p0 and tile 1's p1 (the modes as below). */
static inline uint32_t hh_blend_px(uint32_t p0, uint32_t p1, int rgb_mode, int a_mode, int krgb_src, const uint8_t krgb[3],
                                   int ka_src, uint8_t ka) {
    const int a0 = p0 >> 24, a1 = p1 >> 24;
    int c[4];
    for (int ch = 0; ch < 3; ch++) {
        const int sh = 16 - ch * 8, v0 = (p0 >> sh) & 0xff, v1 = (p1 >> sh) & 0xff;
        const int k = krgb_src == 1 ? a0 : krgb_src == 2 ? a1 : krgb[ch];
        c[ch] = rgb_mode == 0 ? v0 : rgb_mode == 1 ? v1 : rgb_mode == 2 ? hh_lerp8(v0, v1, k)
              : rgb_mode == 3 ? hh_lerp8(v1, v0, k) : (v0 * v1 + 127) / 255; /* 4: t0 * t1 */
    }
    {
        const int k = ka_src == 1 ? a0 : ka_src == 2 ? a1 : ka;
        c[3] = a_mode == 0 ? a0 : a_mode == 1 ? a1 : a_mode == 2 ? hh_lerp8(a0, a1, k)
             : a_mode == 3 ? hh_lerp8(a1, a0, k) : (a0 * a1 + 127) / 255;
    }
    return argb((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], (uint8_t)c[3]);
}

/* Tile 1 sampled over a water pre-blend's texels (the fractional path below), kept from frame
 * to frame: tile 1's offset from tile 0 stays (22.5, 37.5) texels while the corners scroll and
 * the LOD fraction moves, so its four taps a texel are taken once, not every frame. Keyed by
 * the texture's generation (every texel write bumps it), the size and the offset in quarters. */
#define T1S_N 4
#define T1S_MAX (64 * 64)
static struct {
    uint32_t id, gen, used;
    int w, h, os, ot, cl;
    uint32_t px[T1S_MAX];
} sT1s[T1S_N] SAVESTATE_EXCLUDE;
static uint32_t sT1sTick SAVESTATE_EXCLUDE;

bool gfx_soft_blend_textures(uint32_t dst, uint32_t id0, uint32_t id1, int W, int H, int base_s, int base_t,
                             int uls1, int ult1, int shs1, int sht1, int rgb_mode, int a_mode,
                             int krgb_src, const uint8_t krgb[3], int ka_src, uint8_t ka, int filt1,
                             int dq_s, int dq_t, int mips) {
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
        if (sTex[dst].nlev > 1) sTex[dst].nlev = 1; /* its levels were another size's */
    }
    /* filt1: bit 0 = sample tile 1 bilinearly (the RDP filters; only worth it where a shift
     * makes tile 1 coarser than tile 0), bits 1/2 = tile 1 clamps s/t */
    if (dq_s != 0 || dq_t != 0) {
        /* tile 1 unshifted, a fixed fraction past the whole-texel corner: the RDP's bilinear
         * taps floor(s1), floor(s1) + 1 weighted by the quarter left over -- hh_tile1_bilerp's
         * arithmetic in integers (its weights are these exact multiples of 64) */
        const int ns = dq_s < 0 ? -1 : 0, fs = (dq_s - 4 * ns) * 64, nt = dq_t < 0 ? -1 : 0, ft = (dq_t - 4 * nt) * 64;
        const int ms = (t1->w & (t1->w - 1)) == 0 ? t1->w - 1 : -1, mt = (t1->h & (t1->h - 1)) == 0 ? t1->h - 1 : -1;
        const bool lerp_k = rgb_mode == 2 && a_mode == 2 && krgb_src == 0 && ka_src == 0;
        const int os = 4 * (base_s - uls1) + dq_s, ot = 4 * (base_t - ult1) + dq_t, cl = filt1 & 6;
        uint32_t *t1s = NULL;
        bool t1s_fresh = true;
        if (W * H <= T1S_MAX) {
            int e = -1, lru = 0;
            for (int i = 0; i < T1S_N && e < 0; i++) {
                if (sT1s[i].id == id1 && sT1s[i].gen == sTexGen[id1] && sT1s[i].w == W && sT1s[i].h == H && sT1s[i].os == os &&
                    sT1s[i].ot == ot && sT1s[i].cl == cl)
                    e = i;
                else if (sT1s[i].used < sT1s[lru].used)
                    lru = i;
            }
            if (e >= 0) {
                t1s_fresh = false;
            } else {
                e = lru;
                sT1s[e].id = id1; sT1s[e].gen = sTexGen[id1]; sT1s[e].w = W; sT1s[e].h = H;
                sT1s[e].os = os; sT1s[e].ot = ot; sT1s[e].cl = cl;
            }
            sT1s[e].used = ++sT1sTick;
            t1s = sT1s[e].px;
        }
        for (int y = 0; y < H; y++) {
            const int r0 = (y % t0->h) * t0->w, it = base_t + y - ult1 + nt;
            const int r1a = hh_tap(it, t1->h, mt, filt1 & 4) * t1->w, r1b = hh_tap(it + 1, t1->h, mt, filt1 & 4) * t1->w;
            int x0 = 0; /* x % t0->w */
            for (int x = 0; x < W; x++) {
                uint32_t p1 = 0;
                if (!t1s_fresh) {
                    p1 = t1s[y * W + x];
                } else {
                    const int is = base_s + x - uls1 + ns;
                    const int ua = hh_tap(is, t1->w, ms, filt1 & 2), ub = hh_tap(is + 1, t1->w, ms, filt1 & 2);
                    const uint32_t q00 = texel_at(t1->px, t1->is16, r1a + ua), q10 = texel_at(t1->px, t1->is16, r1a + ub);
                    const uint32_t q01 = texel_at(t1->px, t1->is16, r1b + ua), q11 = texel_at(t1->px, t1->is16, r1b + ub);
                    for (int sh = 0; sh < 32; sh += 8) {
                        const int a = (q00 >> sh) & 0xff, b = (q10 >> sh) & 0xff, c = (q01 >> sh) & 0xff, d = (q11 >> sh) & 0xff;
                        const int top = a * (256 - fs) + b * fs, bot = c * (256 - fs) + d * fs;
                        p1 |= (uint32_t)(((top * (256 - ft) + bot * ft) + 32768) >> 16) << sh;
                    }
                    if (t1s) t1s[y * W + x] = p1;
                }
                const uint32_t p0 = texel_at(t0->px, t0->is16, r0 + x0);
                if (++x0 == t0->w) x0 = 0;
                out[y * W + x] = lerp_k ? hh_lerp_px(p0, p1, krgb, ka)
                                        : hh_blend_px(p0, p1, rgb_mode, a_mode, krgb_src, krgb, ka_src, ka);
            }
        }
    } else {
        const bool bil1 = (filt1 & 1) && ((shs1 >= 1 && shs1 <= 10) || (sht1 >= 1 && sht1 <= 10));
        const float sc_s = hh_tile_scale(shs1), sc_t = hh_tile_scale(sht1);
        for (int y = 0; y < H; y++) {
            const int r0 = (y % t0->h) * t0->w;
            const int r1 = hh_wrap(hh_tile_coord(base_t + y, sht1) - ult1, t1->h) * t1->w;
            const float v1f = ((float)(base_t + y) + 0.5f) * sc_t - (float)ult1;
            for (int x = 0; x < W; x++) {
                const uint32_t p0 = texel_at(t0->px, t0->is16, r0 + x % t0->w);
                const uint32_t p1 = bil1 ? hh_tile1_bilerp(t1, ((float)(base_s + x) + 0.5f) * sc_s - (float)uls1, v1f, filt1 & 2, filt1 & 4)
                                         : texel_at(t1->px, t1->is16, r1 + hh_wrap(hh_tile_coord(base_s + x, shs1) - uls1, t1->w));
                out[y * W + x] = hh_blend_px(p0, p1, rgb_mode, a_mode, krgb_src, krgb, ka_src, ka);
            }
        }
    }
    if (mips && t0->nlev > 1 && W == t0->w && H == t0->h) {
        if (sTex[dst].nlev > 1 && sTex[dst].lev[0] != NULL) rebuild_mips(&sTex[dst]); /* in place: the same size */
        else { sTex[dst].nlev = 1; sTex[dst].lev[0] = out; build_mips(&sTex[dst]); }
    } else if (sTex[dst].nlev > 1) {
        sTex[dst].nlev = 1; sTex[dst].lev[0] = out; /* a slot that held a water: no levels now */
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
     * over-counted, so it flushes early). Mip levels (build_mips) add up to a third. */
    size_t need = (size_t) bytes * (tex32_expand(bytes / 2u) ? 2u : 1u);
    if (gfx_soft_mips) need += need / 3u;
    return sArenaUsed + need <= TEXARENA_SZ;
}
void texman_bind_tex(unsigned int num) { sBoundTex[sTexUploadTile] = num; }
/* The cache reset (gfx_texture_cache_reset). Ids restart at 1, so the bound ids and everything keyed by
 * an id (the alpha-row table, the tile-1 samples: id + generation) must not match the new textures that
 * take them: the bindings are dropped and every generation moves on (round 20; gfx_cache_epoch). */
void texman_clear(void) {
    sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; box_cache_clear();
    sBoundTex[0] = sBoundTex[1] = 0;
    for (int i = 0; i < MAX_TEXTURES; i++) sTexGen[i]++;
}

/* Framebuffer readback (copy_framebuffer): the race-start wipe and the
 * stadium TV screens ask for small patches of the PREVIOUS frame at N64
 * 320x240 coordinates, stored as big-endian N64 RGBA16 tiles that re-enter
 * through the normal texture import next frame. The soft renderer owns its
 * framebuffer, so unlike the PSP's deferred GPU readback this is a direct
 * nearest-neighbour copy from sFB (which still holds the last rendered frame
 * while the game builds the next display list). The stub here was why the
 * transition wipe drew see-through and the Jumbotrons were blank. */
void port_fb_copy_request(int x, int y, int w, int h, uint16_t *target) {
    def_resolve_all();
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
    def_resolve_all();
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
