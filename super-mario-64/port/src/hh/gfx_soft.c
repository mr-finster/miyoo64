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
/* unpack5551 as two table lookups, one per byte of the texel: every output
 * bit depends on one byte only (r5 and alpha sit in one byte each, b5 in the
 * other, and G's replication splits cleanly: with g5 = gl | gh<<3, the byte
 * gl<<3 | gl>>2 | gh<<6 | gh<<1 has no overlapping terms), so
 * unpack5551(c) == lut[c & 0xff] | lut[256 + (c >> 8)] for all 65536 c
 * (checked exhaustively). 5 ops instead of 11 on the raster's hottest path.
 * Filled once in gfx_soft_init. */
static uint32_t sUnpackLut[512] SAVESTATE_EXCLUDE;
static void unpack_lut_init(void) {
    for (int x = 0; x < 256; x++) {
        sUnpackLut[x] = unpack5551((uint16_t)x);
        sUnpackLut[256 + x] = unpack5551((uint16_t)(x << 8));
    }
}
/* TEX32 (device A/B): expand 16-bit textures to 8888 at upload, so the pixel
 * loops never unpack (bit-identical: the same unpack5551, done once per
 * texel) at the cost of twice the arena bytes and cache footprint. 1 = every
 * 16-bit texture, 2 = only small ones (<= TEX32_SMALL texels: the course and
 * kart textures most pixels sample, while menus/backdrops stay 16-bit). Build
 * default PORT_TEX32 (0 unless set); SM64_TEX32=0/1/2 overrides at run time. */
#ifndef PORT_TEX32
#define PORT_TEX32 0
#endif
#define TEX32_SMALL 4096
static int tex32_mode(void) {
    static int mode SAVESTATE_EXCLUDE = -1;
    if (mode < 0) { const char *e = getenv("SM64_TEX32"); mode = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : PORT_TEX32; }
    return mode;
}
/* Whether a 16-bit texture of this many texels is stored expanded. */
static bool tex32_expand(size_t texels) {
    const int m = tex32_mode();
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
static bool sEdgeOpaque SAVESTATE_EXCLUDE; /* gfx_soft_set_edge_opaque */
/* A textured draw whose bound texture isn't there comes out as a flat block of the combiner's
 * colour (OoT's v35 sunset bar). Said on the console, 4 times a session at most (every time under
 * SM64_TEXSTRESS, for the stress sweeps). */
static void untex_log(const char *where, uint32_t id) {
    static int sN SAVESTATE_EXCLUDE, sAll = -1;
    if (sAll < 0) sAll = getenv("SM64_TEXSTRESS") != NULL;
    if (sN < 4 || sAll) { sN++; port_log("gfx: untextured draw (%s): tex %u not valid\n", where, (unsigned)id); }
}
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
    } else if (src16) {
        uint32_t *d = (uint32_t *)dst; const uint16_t *s16 = (const uint16_t *)buf;
        for (size_t i = 0; i < n; i++) d[i] = unpack5551(s16[i]);
    } else { /* GU_PSM_8888: RGBA bytes -> ARGB8888 */
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < n; i++) d[i] = argb(buf[i*4+0], buf[i*4+1], buf[i*4+2], buf[i*4+3]);
    }
    sTex[id].px = dst; sTex[id].is16 = is16; sTex[id].w = width; sTex[id].h = height; sTex[id].valid = true;
    { /* SM64_TEXDUMP=<dir>: every upload as RGB PPM + alpha PGM (debug). */
        static const char *sTd; static int sTdInit;
        if (!sTdInit) { sTdInit = 1; sTd = getenv("SM64_TEXDUMP"); }
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

extern int gfx_soft_hud_box;                     /* the 2D sprites' area-filter switch (below) */
static bool sPhase3D SAVESTATE_EXCLUDE;          /* false: the 2D pass (see the frame section) */

/* One pixel of a UI quad squeezed 4:3 in the 2D pass (text, icons): the area
 * average of the texels its footprint covers along the texture axis screen x
 * follows -- S (ax 1), or T (ax 2) for SM64's dialog and menu fonts, whose
 * glyphs are stored sideways -- and the nearest texel across that axis. The
 * 2D sprites' filter (box_row), per pixel, for triangles: nearest dropped every
 * fourth texel column of the font, bilinear faded 1-texel strokes in and out.
 * OoT's FETCH_BOX averages along S only; an S-only box smeared SM64's sideways
 * glyphs. step: texels a pixel along x (16.16); premultiplied alpha as box_row. */
static inline uint32_t texel_box_ax(const void *px, bool t16, int tw, int th, int ws, int wt, int mu, int mv,
                                    int32_t uf, int32_t vf, int32_t step, int ax, bool premul) {
    const int size = ax == 1 ? tw : th, wm = ax == 1 ? ws : wt, mk = ax == 1 ? mu : mv;
    const int across = ax == 1 ? wrapf(vf >> 16, th, wt, mv) : wrapf(uf >> 16, tw, ws, mu);
    int32_t c = (ax == 1 ? uf : vf) - step / 2;
    const int32_t end = c + step;
    int ti = c >> 16;
    uint32_t sr = 0, sg = 0, sb = 0, sa = 0, sw = 0;
    for (;;) {
        const int wi = wrapf(ti, size, wm, mk);
        const uint32_t t = texel_at(px, t16, ax == 1 ? across * tw + wi : wi * tw + across);
        int32_t next = (int32_t)(((uint32_t)ti + 1u) << 16);
        if (next > end) next = end;
        const uint32_t w = (uint32_t)(next - c) >> 8;
        const uint32_t a = t >> 24, k = premul ? a * w : 255u * w;
        sr += ((t >> 16) & 0xff) * k; sg += ((t >> 8) & 0xff) * k; sb += (t & 0xff) * k;
        sa += a * w; sw += w;
        c = next;
        if (c >= end) break;
        ti++;
    }
    const uint32_t kd = premul ? sa : 255u * sw;
    return (sw == 0 || kd == 0) ? 0 : ((sa / sw) << 24) | ((sr / kd) << 16) | ((sg / kd) << 8) | (sb / kd);
}
static inline int pow2mask(int size) { return (size > 0 && (size & (size - 1)) == 0) ? size - 1 : -1; }
/* float texel coordinate -> 16.16, clamped so the shift never overflows */
static inline int32_t to_fix16(float f) {
    if (f > 16000.0f) f = 16000.0f; else if (f < -16000.0f) f = -16000.0f;
    return (int32_t)(f * 65536.0f);
}
/* float -> int32 as the device's VFP converts (vcvt saturates, NaN gives 0), so the
 * x86 proxy agrees with it where a plain cast is undefined: a near-degenerate
 * triangle's colour step reaches ~1e10 in 16.16, and x86's cvttss2si gives
 * INT_MIN there (MK64 b2dff40; `make PLAT=pc UBSAN=1` found it in all 12 sweep
 * levels here). */
static inline int32_t sat_i32(float f) {
    return f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
}

/* 3D bilinear filtering (menu "3D FILTER: SMOOTH"; off by default). uf/vf are
 * 16.16 texel coordinates at the pixel centre; sample the 2x2 texel
 * neighbourhood offset by half a texel and blend by the fractions. */
int gfx_soft_bilinear SAVESTATE_EXCLUDE = 1; /* default SMOOTH (bilinear); 0 = SHARP nearest */
/* The M5 round 1 NEON paths (texel alone, untextured, fog with alpha, span tails): 1 = on; the
 * device bench (hh_main.c) flips it per PERF window to time them against round 2's. */
int gfx_soft_neon_new SAVESTATE_EXCLUDE = 1;
/* M5 round 4: at each row, a prefetch of the next row's framebuffer and depth lines (the blended
 * water reads both for every pixel; qemu can't time memory). 1 = on; SM64_PF=0 off; the device
 * bench (hh_main.c) flips it per PERF window. Same pixels either way. */
int gfx_soft_pf SAVESTATE_EXCLUDE = -1;
/* SM64_ALPHASKIP (2026-10-05; gfx_soft_span_neon.h span_grp): a NEON group whose texels all fail
 * the alpha test ends before the shade, blend and stores. 1 = on; SM64_ALPHASKIP=0 off; the device
 * bench flips it per PERF window. Same pixels either way. */
int gfx_soft_alphaskip SAVESTATE_EXCLUDE = -1;
/* SM64_NEONNOZA (2026-10-05; the user: the star, Mario and Bowser wipes "have a slight pause when they
 * expand"): blended texture x shade with no depth test on the NEON span too -- the wipe's shape quad
 * (screen_transition.c render_textured_transition: the shape's IA8 texture, bilinear, blended, alpha =
 * the texel's), up to the whole screen every frame of a wipe. It ran the scalar loop at ~80
 * instructions a pixel: qemu, BoB's star wipe, +11.5M instructions a frame over the 13.3M of the level
 * alone. The NEON group is the z-tested one with its depth work compiled out, as the skybox's (NOZ);
 * groups wholly inside the shape (texel alpha under the floor) end early (SM64_ALPHASKIP). 1 = on;
 * SM64_NEONNOZA=0 off; the device bench flips it per PERF window. Same pixels either way. */
int gfx_soft_neonnoza SAVESTATE_EXCLUDE = -1;
/* T-junction cracks (IMG_0202, 2026-09-29). SM64's meshes have T-junctions: a vertex on a neighbour's
 * edge, rounded to whole model units, so up to ~0.1 px off it. The sliver between them missed every
 * pixel centre on a shallow edge, one pixel a row, and the sky showed through as a dotted line (Whomp's
 * Fortress; a census over the 12-level sweep found them in every level). The N64 hides them: its
 * coverage anti-aliasing blends the partly covered pixels. Here every depth-tested 3D span marks a
 * coverage map; when the 3D pass ends, a pixel no such span touched whose left and right (or upper and
 * lower) neighbours were touched is a crack, and takes their average. Silhouettes, holes in cutout
 * textures (their spans still cover them) and the sky (no depth test) are untouched. Growing every
 * edge instead (0.125 or 0.25 px) closed them too, but scored further from the real game in 9 of 10
 * levels (fatter silhouettes). SM64_CRACKFILL=0: off, 1: the sky's gaps only, 2 (default): also the seams showing a farther surface. */
int gfx_soft_crackfill SAVESTATE_EXCLUDE = -1;
int gfx_soft_edge_exact SAVESTATE_EXCLUDE = -1; /* raster_tri: shared edges solved once (SM64_EDGEEXACT) */
static uint8_t sCov[320 * 240] SAVESTATE_EXCLUDE; /* the 3D target's coverage this pass (<= 320x240) */
/* T-junctions that show a farther surface instead of the sky (IMG_0205: the stone wall through the
 * seam of a grass block): the pixel's depth came from a third, farther triangle than both neighbours',
 * which came from two different triangles. A 1-px gap between two fence posts looks the same, so the
 * gap's own width decides: from the pixel centre to each neighbour triangle's nearest edge line (their
 * screen corners, kept per pass below), a T-junction's sliver is a fraction of a pixel wide, a real
 * gap about a whole one. */
static uint16_t sTid[320 * 240] SAVESTATE_EXCLUDE;   /* which triangle wrote each pixel's depth (0: none) */
static uint16_t sTidSerial SAVESTATE_EXCLUDE;
#define TID_MAX 8192
static float sTidV[TID_MAX][6] SAVESTATE_EXCLUDE;     /* ids 1..TID_MAX-1: their screen corners (x, y) x 3 */
static bool sCovAny SAVESTATE_EXCLUDE;
/* SM64_CRACKBOX (2026-10-06, default 1): the box around every triangle that marked coverage this pass. Outside it,
 * one pixel either side, no pixel is covered and none has a covered neighbour, so the fill can't touch it: the scan
 * and the clears keep to the box (the sky above the level costs nothing). 0: the whole target, as before. Same
 * pixels either way. Four ints: the static data has little room left before the asset region moves to the next
 * page, and a region on a 64 KB boundary does not run (2026-10-06; see tools/build_miyoo.sh gate 2c). */
int gfx_soft_crackbox SAVESTATE_EXCLUDE = -1;
static int sCovX0 SAVESTATE_EXCLUDE, sCovX1 SAVESTATE_EXCLUDE, sCovY0 SAVESTATE_EXCLUDE, sCovY1 SAVESTATE_EXCLUDE; /* Y1 0: empty */
/* SM64_CRACKLEAN (2026-10-06, default 1): the NEON candidate test with the same conditions in fewer steps (no
 * inversions, one min for the two zero tests, no trip through the stack), and only the candidate lanes of a group
 * go to the per-pixel test, not all eight. Same pixels tested the same way. 0: as before. */
int gfx_soft_cracklean SAVESTATE_EXCLUDE = -1;
/* SM64_FASTROWS (2026-10-06, branch sm64-speed-2026-10-06; default 1): raster_tri's row loop, for the triangles whose
 * rows go to a NEON row (span_row), in a small function of its own (tri_rows): raster_tri kept ~45 values live across
 * the row and reloaded or spilled most of them every row (~210 instructions a row, ~22% of every level's instructions).
 * Same float operations in the same order; the row's start values (depth, shade, u/w, v/w, 1/w: 24 multiplies and 16
 * adds) as NEON lanes, multiply then add, each rounded as the VFP rounds them. 0: raster_tri's own loop. Same pixels. */
int gfx_soft_fastrows SAVESTATE_EXCLUDE = -1;
int gfx_soft_spanvec SAVESTATE_EXCLUDE = -1; /* SM64_SPANVEC: gfx_soft_span_neon.h span_row_v */
/* SM64_SHADEPX (2026-10-06, default 1; needs SM64_SPANVEC): a smooth-shaded NEON group's shade as one vector per pixel,
 * { b, g, r, a } in 16.16, the group's first one stepped four pixels at a time, where the rows kept four vectors (one per
 * channel, four pixels each) and zipped them into pixel order: three fewer vectors live in the group loop (GCC spilled
 * them, ~22 instructions a group for their steps), no zips. The same integers lane for lane. Same pixels. */
int gfx_soft_shadepx SAVESTATE_EXCLUDE = -1;
/* SM64_ZSKIP (2026-10-06, default 1; in SM64_SPANVEC's rows): a depth-tested span whose nearest depth is further than the
 * farthest depth stored under it (plus the decal window) fails the test at every pixel: it is skipped whole, before its
 * texture setup and groups (gfx_soft_span_neon.h span_row_v). Same pixels. */
int gfx_soft_zskip SAVESTATE_EXCLUDE = -1;
int gfx_soft_neonnoto SAVESTATE_EXCLUDE = -1; /* SM64_NEONNOTO: raster_tri's neon_noto */
/* SM64_WRAPRR (2026-10-06, default 1; with SM64_SPANVEC): rows of a texture that repeats on both axes, their texel
 * wrap the AND alone (the clamp to the int16 lane range before it changes no lane there). Same pixels. */
int gfx_soft_wraprr SAVESTATE_EXCLUDE = -1;
/* SM64_SKYCLR (2026-10-07, speed round 2; default 1): the sky opening the frame's 3D clears the target's alpha (the
 * coverage marks the deferred sky reads) with a pass over the whole framebuffer, a read and a write of every pixel. On
 * every frame of a level with a sky the target is still waiting for its lazy clear there, which writes 0 to every pixel
 * just before: the pass then changed nothing, so it is left out. 0: the pass every time, as before. Same pixels. */
int gfx_soft_skyclr SAVESTATE_EXCLUDE = -1;
/* SM64_LEAN (2026-10-07, round 2; default 1): the NEON rows of the z-tested textured kinds, for a triangle with the
 * settings nearly every one has (the paired bilinear taps, no lower decal side; blended: blends and keeps the depth,
 * opaque: writes the depth and the crack fill's ids), in a variant with those settings known when compiled
 * (gfx_soft_span_neon.h span_grp, PV 8): no runtime test or load of them in the group loop. Any other triangle
 * takes the rows as before. Same pixels. */
int gfx_soft_lean SAVESTATE_EXCLUDE = -1;
/* SM64_FULLST (2026-10-07, round 2; default 1; in SM64_LEAN's opaque rows): a group whose four pixels all pass the
 * depth test stores its colours, depths and ids without loading the old ones first: the select it made with them
 * keeps none. The framebuffer's and the ids' lines are then written without a load waiting on them. Same pixels. */
int gfx_soft_fullst SAVESTATE_EXCLUDE = -1;
int gfx_soft_magbilin SAVESTATE_EXCLUDE = 0; /* 0 = every textured pixel bilinear (kit k1.2, the default); 1 = bilinear only
                                                * when magnified (SM64_MAGBILIN=1, A/B): the gate flipped a floor between smooth
                                                * and sharp along triangle edges as the camera moved (DKR's floor flashing) */
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
static bool sSkyFlushing SAVESTATE_EXCLUDE; /* drawing the deferred sky: only pixels at alpha 0 (see sky_flush) */
static int sVpX, sVpY, sVpW, sVpH;
static int sScX0, sScY0, sScX1, sScY1;
static bool sTexAdd;       /* GE ADD: texel + shade */
/* (A - V) * TEXEL0 + V, A constant: the texel lerps the vertex colour V toward
 * A per channel (G_CC_HILITERGBA: Mario's face shine on the title, A = the
 * light colour). gfx_pc sends V as the vertex colour and A here. Bit 0 = the
 * RGB combiner has the form, bit 1 = the alpha combiner. */
static int sHilite;
static uint8_t sHiliteC[4];
/* Per-pixel fog (the fog render modes' blender cycle 1: colour -> lerp toward
 * the fog colour by the shade alpha, which the RSP set to the fog factor). gfx_pc
 * folds fog into the vertex colour where that is exact and turns this on for
 * the rest; the vertex alpha then carries the fog factor, and sFogA >= 0 is the
 * combiner's constant alpha (ENV/PRIM) that the fog factor displaced. */
static bool sFog;
static int sFogR, sFogG, sFogB, sFogA = -1;
/* G_CC_TRILERP, (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0: the texel is the
 * blend of tile 0's and tile 1's textures by the LOD fraction gfx_pc works out
 * per triangle (0..255). SM64's castle window: Peach far, Bowser near. */
static bool sTriLerp;
static int sTriF;

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
/* a -> b by f/255, per ARGB channel (the TRILERP texel blend). */
static inline uint32_t lerp_argb(uint32_t a, uint32_t b, int f) {
    uint32_t o = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        o |= (uint32_t) div255(ca * (255 - f) + cb * f) << sh;
    }
    return o;
}
/* Round 27: the z-tested texture x shade span four pixels at a time in NEON
 * (see gfx_soft_span_neon.h; bit-identical, 8888 power-of-two textures).
 * SM64_NEON=0 keeps the scalar loop (A/B). */
#if defined(__ARM_NEON)
#include "gfx_soft_span_neon.h"
#define GFX_SOFT_NEON_SPAN 1
#ifndef PORT_NEON_MODE
#define PORT_NEON_MODE 1
#endif
/* SM64_NEON=0 scalar, 1 four-pixel spans (round 27), 2 per-pixel rows (ocarina-of-time c935b73) */
static int sNeonMode SAVESTATE_EXCLUDE = -1;
#endif
static inline float edge(const SoftVtx *a, const SoftVtx *b, float px, float py) {
    return (px - a->x) * (b->y - a->y) - (py - a->y) * (b->x - a->x);
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
#if defined(GFX_SOFT_NEON_SPAN) && PERSP_SPAN != SPAN_ROW_PERSP
#error "span_row (gfx_soft_span_neon.h) steps SPAN_ROW_PERSP pixels a span: keep it equal to PERSP_SPAN"
#endif
#if defined(GFX_SOFT_NEON_SPAN)
#include "gfx_soft_row_neon.h"
#endif

/* A span's coverage bytes: memset was a library call per span (~4% of the fill's cost). With SM64_CRACKLEAN, NEON
 * stores of ones, the last one overlapping back so nothing past the span is written. The same bytes. */
static inline void cov_mark(uint8_t *p, int n) {
#if defined(GFX_SOFT_NEON_SPAN)
    if (gfx_soft_cracklean > 0) {
        const uint8x16_t one = vdupq_n_u8(1);
        if (n >= 16) {
            int i = 0;
            for (; i + 16 <= n; i += 16) vst1q_u8(p + i, one);
            if (i < n) vst1q_u8(p + n - 16, one);
        } else if (n >= 8) {
            vst1_u8(p, vget_low_u8(one)); vst1_u8(p + n - 8, vget_low_u8(one));
        } else if (n >= 4) { /* (GCC made the byte loop a memset call, with spills around it in tri_rows) */
            const uint32_t w = 0x01010101u;
            memcpy(p, &w, 4); memcpy(p + n - 4, &w, 4);
        } else if (n > 0) {
            p[0] = 1; p[n >> 1] = 1; p[n - 1] = 1;
        }
        return;
    }
#endif
    memset(p, 1, (size_t) n);
}
#if defined(GFX_SOFT_NEON_SPAN)
/* SM64_FASTROWS (above): a triangle's row constants and the NEON row it draws with. */
typedef struct {
    float32x4_t va0, va1, va2, vb0, vb1, vb2; /* each vertex's {z, r, g, b} and {a, u/w, v/w, 1/w} */
    float32x4_t fa, fb;                       /* flat: {-, r0, g0, b0}, {a0, -, -, -} in place of the sums (tri_rows_t) */
    float e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx;
    int minx, maxx, sw, persp, pf, flat;
    int ek[3];                                /* each edge's w_i_dx: 0 > 0, 1 < 0, 2 == 0 */
    uint8_t *cov;                             /* the crack fill's coverage map, or NULL */
    uint32_t *fbuf; uint16_t *zb;
    SpanRowFn fn; const SpanCM *k; const SpanRowT *t;
#if defined(MIYOO)
    int late; /* the Miyoo's split (TR_LATE in raster_tri): 1 rows at the row start's edge left out, 2 only those */
#endif
} TriRows;
#if defined(MIYOO)
typedef struct { SpanRowF f; int x, xe, idx, y; } TrRef; /* a row a split share handed back (SM64_SPLITREF) */
typedef struct { SpanRowF f; int x, xe, idx; uint32_t px; int late; TrRef *ref; int nref, refcap; } TriRowsOut; /* late: an edge row was left out */
#else
typedef struct { SpanRowF f; int x, xe, idx; uint32_t px; } TriRowsOut;
#endif
/* Rows y.. of the triangle, each exactly as raster_tri's loop draws it (the same bases, edge solve, snap and start
 * values; then the row's NEON function). Returns maxy when every row is done, else the row whose NEON function
 * refused a span: o holds that row's state there (raster_tri's own loop finishes it). o->px: the span pixels. */
/* x < 0.0f for a float that is not NaN, from its bits (no VFP compare: each waited on a vmrs). No value here is NaN:
 * the vertices are finite and the area is not 0. */
static inline int f_neg(float f) { uint32_t u; memcpy(&u, &f, 4); return u > 0x80000000u; }
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
static inline __attribute__((always_inline)) int
tri_rows_t(const TriRows *T, int y, const int maxy, TriRowsOut *o, const int FLAT TR_STEP_PARAM) {
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
        /* raster_tri's solve, as it is there, its ds tests made once per triangle (ek: 0 ds > 0, 1 ds < 0, 2 ds == 0) */
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
        } else if (f_neg(BS)) continue; /* the row outside this edge: nothing (xs = xe there) */
        TRI_EDGE(ek0, b0, w0dx)
        TRI_EDGE(ek1, b1, w1dx)
        TRI_EDGE(ek2, b2, w2dx)
#undef TRI_EDGE
        if (xs >= xe) continue;
        /* the snap: b_i + (xs - minx) * w_i_dx >= 0 for all three, in three lanes (vmla is not fused: the VFP's sums) */
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
#if defined(MIYOO)
        if (T->late) { /* TR_LATE (raster_tri): the span's masked tail group would start before the row */
            const int edge = xe - xs < 4 && xs < 4 - (xe - xs);
            if (T->late == 1 && edge) { o->late = 1; continue; }
            if (T->late == 2 && !edge) continue;
        }
#endif
        const int n = xe - xs;
        px += (uint32_t)n;
        if (T->cov) cov_mark(T->cov + y * T->sw + xs, n);
        /* w0*z0 + w1*z1 + w2*z2 for all eight (W: w0, w1, w2 as raster_tri makes them) */
        float32x4_t A = vmulq_lane_f32(T->va0, vget_low_f32(W), 0), B = vmulq_lane_f32(T->vb0, vget_low_f32(W), 0);
        A = vmlaq_lane_f32(A, T->va1, vget_low_f32(W), 1); B = vmlaq_lane_f32(B, T->vb1, vget_low_f32(W), 1);
        A = vmlaq_lane_f32(A, T->va2, vget_high_f32(W), 0); B = vmlaq_lane_f32(B, T->vb2, vget_high_f32(W), 0);
        if (FLAT) { /* the shade is the vertex colour */
            static const uint32_t ma[4] = { 0, ~0u, ~0u, ~0u }, mb[4] = { ~0u, 0, 0, 0 };
            A = vbslq_f32(vld1q_u32(ma), T->fa, A); B = vbslq_f32(vld1q_u32(mb), T->fb, B);
        }
        SpanRowF rf;
        vst1q_f32(&rf.zf, A); vst1q_f32(&rf.ca, B);
        const float sw = rf.sw;
        rf.rw = T->persp ? ((sw > 1e-12f) ? 1.0f / sw : 0.0f) : 0.0f;
        const int idx = y * T->sw + xs;
        if (T->pf) { __builtin_prefetch(T->fbuf + idx + T->sw); __builtin_prefetch(T->zb + idx + T->sw); }
        const int d = T->fn(T->k, T->t, &rf, idx, n);
#if defined(MIYOO)
        if (d != n && o->nref < o->refcap) { /* SM64_SPLITREF: noted, finished after both shares (raster_tri) */
            TrRef *const r = &o->ref[o->nref++];
            r->f = rf; r->x = xs + d; r->xe = xe; r->idx = idx + d; r->y = y;
            continue;
        }
#endif
        if (d != n) { o->f = rf; o->x = xs + d; o->xe = xe; o->idx = idx + d; o->px = px; return y; }
    }
    o->px = px;
    return maxy;
}
static int __attribute__((noinline)) tri_rows_f(const TriRows *T, int y, const int maxy, TriRowsOut *o TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, o, 1 TR_STEP_ARG(ystep)); }
static int __attribute__((noinline)) tri_rows_s(const TriRows *T, int y, const int maxy, TriRowsOut *o TR_STEP_PARAM) { return tri_rows_t(T, y, maxy, o, 0 TR_STEP_ARG(ystep)); }
static inline int tri_rows(const TriRows *T, int y, const int maxy, TriRowsOut *o TR_STEP_PARAM) {
    return T->flat ? tri_rows_f(T, y, maxy, o TR_STEP_ARG(ystep)) : tri_rows_s(T, y, maxy, o TR_STEP_ARG(ystep));
}
#if defined(MIYOO)
/* The Mini's second core (kit/platform/miyoo_worker.c, the shared Miyoo kit KM2): a 3D triangle whose rows go to
 * the NEON rows (SM64_FASTROWS' tri_rows) and that has enough rows is drawn by both cores at once, this one its even
 * rows from miny, the worker the odd ones. Exact:
 *  - tri_rows computes each row from the triangle's constants and its own y alone (the bases, the edge solve, the snap
 *    and the row's start values are recomputed per row; nothing steps from one row to the next), so a row comes out
 *    the same whichever core draws it;
 *  - the shares write disjoint rows of colour, depth, the crack fill's coverage and its ids;
 *  - all of the setup (TriRows, SpanCM, SpanRowT, the lazily read switches) is done by this core before the handoff,
 *    and the worker only reads it. What a row writes besides pixels is per share: the worker has its own copy of
 *    the SpanCM and of its SpanK (span_row builds the SpanK at its first row: the same values from the same inputs;
 *    SM64_ZSKIP's count of misses, carried row to row, only decides which spans are tested, and a tested span is
 *    skipped only when every pixel fails the depth test), and its own pixel count, added after the wait;
 *  - a row the NEON row hands back (span_ok refused a span: raster_tri's own loop draws the rest of it) stops the
 *    worker's share there; once this core's share is done and the worker has finished, this core finishes that
 *    row and the worker's remaining rows itself, with the same code as a one-core run;
 *  - the triangle is finished (miyoo_worker_wait) before the next one starts.
 * SM64_THREADS=1 (or threads=1 in sm64_display.cfg): one core, as before (gfx_sdl12.c starts the worker).
 * SM64_SPLIT=<px>: the smallest triangle split, in pixels of area (default TR_SPLIT_PX). */
#include "miyoo_worker.h"
extern volatile int gMiyooJobOut; /* gfx_sdl12.c: a present or audio job still on the worker */
extern void gfx_miyoo_job_fence(void);
extern int gfx_miyoo_job_try(void);
#define TR_SPLIT_PX   64 /* below this, the handoff costs about what it saves */
#define TR_SPLIT_ROWS 4
/* TR_LATE (SM64_SPLITEDGE, default 1): a span of n < 4 pixels starting less than 4 - n pixels into its row ends in a
 * masked tail group that starts before the row (gfx_soft_span_neon.h: the group ending at the span's end): it reads
 * the previous row's last pixels and writes them back unchanged. On one core that is harmless; with the rows split
 * that previous row is the other core's, being drawn at the same time, and a write-back could undo its pixel. So
 * while a triangle is split both shares leave such rows out (TriRows.late 1), and once both are done this core draws
 * exactly those rows (late 2), in order, as one core would: every row once, with the same code. The MK64 port's
 * agent found the same hazard in its NEON groups. 0: as before (A/B). */
typedef struct { TriRows trr; SpanCM nk; SpanK nkc; int y, maxy, yr; TriRowsOut tro; } TrJob;
static int sTrLate SAVESTATE_EXCLUDE = -1;
/* SM64_SPLITREF (2026-10-12, default 1): a row whose NEON row hands a span back (span_ok: texel coordinates out of the
 * lanes' range) used to end the worker's share there, and this core drew the rest of the share after the wait: one
 * such row early in a big triangle gave this core most of the triangle. Now each share notes such a row (its state
 * where the NEON row stopped, as the hand-back always kept it) and goes on with its next rows; once both are done this
 * core finishes the noted rows with raster_tri's own loop, as it always finished a handed-back row. Disjoint rows, the
 * same code for every pixel. Over TR_REFS rows in one share: that share stops there, as before. 0: as before. */
#define TR_REFS 64
int gMiyooSplitRef SAVESTATE_EXCLUDE = -1;
extern int gMiyooMt; /* gfx_sdl12.c: the second core's work at all (the bench's mt-off windows: 0) */
static TrRef sTrRefs[2][TR_REFS] SAVESTATE_EXCLUDE; /* [0] this core's share, [1] the worker's */
static uint32_t sTrHandback SAVESTATE_EXCLUDE, sTrHandRows SAVESTATE_EXCLUDE; /* the PERF line: shares stopped, rows noted */
static void tr_job(void *arg) {
    TrJob *const j = (TrJob *)arg;
    j->yr = tri_rows(&j->trr, j->y, j->maxy, &j->tro, 2);
}
static int sTrMinPx SAVESTATE_EXCLUDE = -1;
static uint32_t sTrTris SAVESTATE_EXCLUDE, sTrSplits SAVESTATE_EXCLUDE; /* the PERF line's split counts */
static uint64_t sTrPx SAVESTATE_EXCLUDE, sTrSplitPx SAVESTATE_EXCLUDE;
/* hh_main.c's PERF line: this window's split triangles, the split share of the 3D pixels, the worker's jobs and sleeps */
int gfx_soft_mt_line(char *buf, int n) {
    unsigned jobs = 0, sleeps = 0;
    unsigned long long spins = 0;
    miyoo_worker_stats(&jobs, &sleeps, &spins);
    const int r = snprintf(buf, n, " | mt %s split %u/%u tris %.0f%% px, jobs %u, sleeps %u, refs %u/%u", miyoo_worker_ready() ? "on" : "off",
                           (unsigned)sTrSplits, (unsigned)sTrTris, sTrPx ? 100.0 * (double)sTrSplitPx / (double)sTrPx : 0.0, jobs, sleeps,
                           (unsigned)sTrHandRows, (unsigned)sTrHandback);
    sTrSplits = sTrTris = 0; sTrHandRows = sTrHandback = 0;
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
    if (tex && !tex->valid) { untex_log("3d", sBoundTex[0]); tex = NULL; }
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
    bool use_bilin = gfx_soft_bilinear && tex;
    if (use_bilin && gfx_soft_magbilin) {
        const float uvcross = (v1->u - v0->u) * (v2->v - v0->v) - (v2->u - v0->u) * (v1->v - v0->v);
        const float scr = area < 0 ? -area : area;
        const float uvc = (uvcross < 0 ? -uvcross : uvcross) * (float)tex->w * (float)tex->h * sGateScale; /* texel^2, in native-px terms */
        use_bilin = scr * 2.0f >= uvc; /* bilinear until ~2x minified */
    }
    const int amin = tex_edge ? 0x55 : 8; /* alpha reject: texture-edge or near-zero (only under sUseAlpha) */
    const int ablend = !(tex_edge && sEdgeOpaque); /* 0: a cutout, the pixel written unblended past amin */
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
    const int32_t zdq = (int32_t)((zdx < -1.0f ? -1.0f : zdx > 1.0f ? 1.0f : zdx) * (65535.0f * 256.0f));
    const int32_t rdq = sat_i32(crdx * 65536.0f), gdq = sat_i32(cgdx * 65536.0f), bdq = sat_i32(cbdx * 65536.0f), adq = sat_i32(cadx * 65536.0f);

    /* Flat triangles: the shade colour the pixel loop would clamp every pixel
     * (rq = (int32_t)(cr * 65536.0f) with cr == r0 on every row and span, since
     * every colour step is 0), clamped once. */
    const int fsr = clamp8_sh16((int32_t)(r0 * 65536.0f)), fsg = clamp8_sh16((int32_t)(g0 * 65536.0f));
    const int fsb = clamp8_sh16((int32_t)(bl0 * 65536.0f)), fsa = clamp8_sh16((int32_t)(a0 * 65536.0f));

    /* Per-pixel loop reloads these globals/fields otherwise (-fno-strict-aliasing
     * forces a reload after every framebuffer store): pin them to locals. */
    uint32_t *const fbuf = sFB; uint16_t *const zb = sZB;
    const bool hil_rgb = tex && (sHilite & 1), hil_a = tex && (sHilite & 2);
    const int hr = sHiliteC[0], hg = sHiliteC[1], hb = sHiliteC[2], ha = sHiliteC[3];
    /* A fogged draw's triangle with no fog at any corner (the vertex alpha is the fog factor, 0 near
     * the camera) takes the unfogged paths (MK64 c400d1b): its fog step, div255(fog*0 + c*255), is c
     * bit for bit, and its pixel alpha comes from the same shade alpha either way. Not when a
     * constant alpha displaced the fog factor (foga >= 0: the fog path puts it back; SF64's guard).
     * SM64_FOGSKIP=0: every triangle of a fogged draw fogs per pixel (A/B). */
    static int sFogSkip SAVESTATE_EXCLUDE = -1;
    if (sFogSkip < 0) { const char *e = getenv("SM64_FOGSKIP"); sFogSkip = !(e && e[0] == '0'); }
    const bool fog = sFog && (!sFogSkip || (v0->a | v1->a | v2->a) != 0 || sFogA >= 0);
    const int fogr = sFogR, fogg = sFogG, fogb = sFogB, foga = sFogA;
    const SoftTex *tex1 = (sTriLerp && tex) ? &sTex[sBoundTex[1]] : NULL;
    if (tex1 && (!tex1->valid || tex1->w != tex->w || tex1->h != tex->h)) tex1 = NULL; /* same-size levels only */
    const bool trl = tex1 != NULL; const int trf = sTriF;
    const void *tex1px = trl ? tex1->px : NULL; const bool t1_16 = trl && tex1->is16;
    /* Decal depth (ZMODE_DEC): the RDP passes a decal pixel within its depth
     * range of the stored depth, so a coplanar decal holds however steeply the
     * surface recedes. The fixed 4 lost Mario's shadow with the camera close (the
     * intro's pipe landing: the ground's depth changes by hundreds of units a
     * pixel there, more than the two triangulations' rounding apart). The window
     * here is a quarter of the decal's depth change over a pixel (|dz/dx| +
     * |dz/dy|), rounded down to a power of two, at least 4: scored against the
     * real game (m64ref z64) on every pixel it changes, a quarter fixes the
     * shadow as fully as the whole slope does, without the whole slope's shadow
     * over the foot of WF's trees. SM64_DECALDZ=0: the fixed 4 (A/B); 2: also
     * the RDP's lower side (a decal in front of the surface fails; it scored
     * worse on SM64's stairs and ledges, so off); SM64_DECALSHIFT=n: the slope
     * over 2^n (default 2). zlo: the lower side (0x10000 = none). */
    static int sDecalDz SAVESTATE_EXCLUDE = -1, sDecalShift SAVESTATE_EXCLUDE = -1;
    if (sDecalDz < 0) {
        const char *e = getenv("SM64_DECALDZ"), *k = getenv("SM64_DECALSHIFT");
        sDecalDz = e ? atoi(e) : 1;
        sDecalShift = k ? atoi(k) : 2;
    }
    int zbias = 0, zlo = 0x10000;
    if (sZDecal) {
        zbias = 4;
        if (sDecalDz) {
            const float zdy = (v1->x - v2->x) * inv_area * z0 + (v2->x - v0->x) * inv_area * z1 + (v0->x - v1->x) * inv_area * z2;
            const float dz = (fabsf(zdx) + fabsf(zdy)) * 65535.0f / (float) (1 << sDecalShift);
            while (zbias < 32768 && (float) (zbias << 1) <= dz) zbias <<= 1;
            if (sDecalDz == 2) zlo = zbias;
        }
    }
    const bool dmask = sDepthMask, texmix = sTexAdd || rgb_decal || hil_rgb || hil_a || fog || trl; /* the rare combines share one test */
    const bool cm_mod = rgb_tex && rgb_in && !texmix && !rgb_texa; /* texture x shade: see SPAN_LOOP_CM */
    const int tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
    const int ws = tex ? tex->wrap_s : 0, wt = tex ? tex->wrap_t : 0;
    /* UI quads squeezed 4:3 (the 2D pass: flat on the screen, 1/w equal at the
     * corners): 1.02..3 texels a pixel along x on one texture axis and none on
     * the other -> the area filter along it (texel_box_ax). SM64_HUDFILTER=0
     * turns it off with the sprites' (A/B). */
    if (gfx_soft_hud_box < 0) { const char *e = getenv("SM64_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
    int box_ax = 0; int32_t box_step = 0;
    if (tex && !sPhase3D && gfx_soft_hud_box > 0 && iw0 == iw1 && iw1 == iw2) {
        const float k = persp ? 1.0f / iw0 : 1.0f;
        const float su = fabsf(uwdx * k * (float)tw), sv = fabsf(vwdx * k * (float)th);
        if (su > 1.02f && su <= 3.0f && sv < 1.0f / 64.0f)      { box_ax = 1; box_step = to_fix16(su); }
        else if (sv > 1.02f && sv <= 3.0f && su < 1.0f / 64.0f) { box_ax = 2; box_step = to_fix16(sv); }
        /* at 1:1 (the unsquished HUD, the act select, the pause screen): one texel a pixel, so the
         * texel itself -- the N64 samples its fonts G_TF_POINT; bilinear, half a texel off, spread
         * every 1-texel stroke over two pixels at half strength ("text looks not pixel perfect",
         * the user, 2026-09-28). gfx_pc.c places these on whole pixels (hud_snap). */
        else if (gfx_soft_hud_box > 0 && ((fabsf(su - 1.0f) <= 0.02f && sv < 1.0f / 64.0f) || (fabsf(sv - 1.0f) <= 0.02f && su < 1.0f / 64.0f)))
            use_bilin = false;
    }
    if (gfx_soft_crackfill < 0) { const char *e = getenv("SM64_CRACKFILL"); gfx_soft_crackfill = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : 2; }
    const bool cov = gfx_soft_crackfill && sPhase3D && ztest && sW * sH <= (int) sizeof(sCov);
    if (cov) { /* every span lies in [minx, maxx) x [miny, maxy): grow the pass's box (SM64_CRACKBOX) */
        if (gfx_soft_cracklean < 0) { const char *e = getenv("SM64_CRACKLEAN"); gfx_soft_cracklean = !(e && e[0] == '0'); }
        if (sCovY1 == 0) { sCovX0 = minx; sCovX1 = maxx; sCovY0 = miny; sCovY1 = maxy; }
        else {
            if (minx < sCovX0) sCovX0 = minx;
            if (maxx > sCovX1) sCovX1 = maxx;
            if (miny < sCovY0) sCovY0 = miny;
            if (maxy > sCovY1) sCovY1 = maxy;
        }
        sCovAny = true;
    }
    uint16_t *const tidb = cov && gfx_soft_crackfill >= 2 && sTidSerial < TID_MAX - 1 ? sTid : NULL; /* 1: the sky's gaps only */
    uint16_t tid = 0;
    if (tidb) {
        tid = ++sTidSerial;
        float *tv = sTidV[tid];
        tv[0] = v0->x; tv[1] = v0->y; tv[2] = v1->x; tv[3] = v1->y; tv[4] = v2->x; tv[5] = v2->y;
    }
#if defined(GFX_SOFT_NEON_SPAN)
    if (sNeonMode < 0) { const char *e = getenv("SM64_NEON"); sNeonMode = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : PORT_NEON_MODE; }
    const bool neon_cm = sNeonMode == 1 && tex && ztest && cm_mod && !t16 && tmask_u >= 0 && tmask_v >= 0 && !box_ax;
    /* Opaque fogged texture x shade -- the fog render modes' terrain, a third of
     * BoB's pixels -- on the same four-pixel span, the fog blend added there
     * (bit-identical to the scalar's). SM64_NEONFOG=0: the scalar loop (A/B). */
    static int sNeonFog SAVESTATE_EXCLUDE = -1;
    if (sNeonFog < 0) { const char *e = getenv("SM64_NEONFOG"); sNeonFog = !(e && e[0] == '0'); }
    /* M5 round 1: fogged draws may blend too (the alpha taken before the fog step, a constant one
     * after it). SM64_NEONFOGA=0: opaque only, as before (A/B). */
    static int sNeonFogA SAVESTATE_EXCLUDE = -1;
    if (sNeonFogA < 0) { const char *e = getenv("SM64_NEONFOGA"); sNeonFogA = !(e && e[0] == '0'); }
    const bool neon2 = gfx_soft_neon_new != 0; /* the round's new paths (the device bench flips them) */
    const bool neon_fog = sNeonFog && sNeonMode == 1 && tex && ztest && fog && rgb_tex && rgb_in && !rgb_texa && !sTexAdd &&
                          !rgb_decal && !hil_rgb && !hil_a && !trl && ((neon2 && sNeonFogA) || !sUseAlpha) && !t16 && tmask_u >= 0 &&
                          tmask_v >= 0 && !box_ax;
    /* The texel alone as the colour (the scalar's !rgb_in branch; rgb_decal, hilite and the
     * texture add all read the vertex colour, so they can't be set here), z-tested, any alpha,
     * fogged or not: JRB's water, ~17k scalar px a frame. SM64_NEONTO=0: the scalar loop (A/B). */
    static int sNeonTo SAVESTATE_EXCLUDE = -1;
    if (sNeonTo < 0) { const char *e = getenv("SM64_NEONTO"); sNeonTo = !(e && e[0] == '0'); }
    const bool neon_to = neon2 && sNeonTo && sNeonMode == 1 && tex && ztest && rgb_tex && !rgb_in && !rgb_texa && !hil_a && !trl &&
                         (sNeonFogA || !fog || !sUseAlpha) && !t16 && tmask_u >= 0 && tmask_v >= 0 && !box_ax;
    /* Untextured, opaque, z-tested: the colour is the shade. SM64_NEONUT=0: the scalar loop (A/B). */
    static int sNeonUt SAVESTATE_EXCLUDE = -1;
    if (sNeonUt < 0) { const char *e = getenv("SM64_NEONUT"); sNeonUt = !(e && e[0] == '0'); }
    const bool neon_ut = neon2 && sNeonUt && sNeonMode == 1 && !tex && ztest && !sUseAlpha && !fog;
    /* Opaque texture x shade with no depth test -- the skybox, ~40% of the pixels
     * of every level -- on the same span, its depth work compiled out. SM64_NEONNOZ=0:
     * the scalar loop (A/B). */
    static int sNeonNoz SAVESTATE_EXCLUDE = -1;
    if (sNeonNoz < 0) { const char *e = getenv("SM64_NEONNOZ"); sNeonNoz = !(e && e[0] == '0'); }
    const bool neon_noz = sNeonNoz && sNeonMode == 1 && tex && !ztest && cm_mod && !sUseAlpha && !t16 && tmask_u >= 0 &&
                          tmask_v >= 0 && !box_ax;
    /* The same blended (SM64_NEONNOZA, above): the screen wipes' shape quad. */
    if (gfx_soft_neonnoza < 0) { const char *e = getenv("SM64_NEONNOZA"); gfx_soft_neonnoza = !(e && e[0] == '0'); }
    const bool neon_noza = gfx_soft_neonnoza > 0 && neon2 && sNeonNoz && sNeonMode == 1 && tex && !ztest && cm_mod && sUseAlpha &&
                           !t16 && tmask_u >= 0 && tmask_v >= 0 && !box_ax;
    /* SM64_NEONNOTO (2026-10-06): the texel alone as the colour, no depth test, opaque or blended -- the power meter
     * (shown while Mario is in the water: Dire Dire Docks', Wet-Dry World's starts) ran the scalar loop, ~1.3 ms a frame on
     * the device (their r2d). The NEON group's TO and NOZ parts together, each as already proved. Same pixels. */
    if (gfx_soft_neonnoto < 0) { const char *e = getenv("SM64_NEONNOTO"); gfx_soft_neonnoto = !(e && e[0] == '0'); }
    const bool neon_noto = gfx_soft_neonnoto > 0 && neon2 && sNeonNoz && sNeonMode == 1 && tex && !ztest && rgb_tex && !rgb_in &&
                           !rgb_texa && !hil_a && !trl && !fog && !t16 && tmask_u >= 0 && tmask_v >= 0 && !box_ax;
    const bool cm_rows = sNeonMode == 2 && tex && ztest && cm_mod && !box_ax;
    CmTri cmt;
    if (cm_rows) {
        const int32_t d[4] = { bdq, gdq, rdq, adq };
        cmt.px = texpx; cmt.w = tw; cmt.h = th; cmt.ws = ws; cmt.wt = wt; cmt.mu = tmask_u; cmt.mv = tmask_v; cmt.t16 = t16;
        cmt.swdx = swdx; cmt.uwdx = uwdx; cmt.vwdx = vwdx; cmt.zdx = zdx;
        cmt.crdx = crdx; cmt.cgdx = cgdx; cmt.cbdx = cbdx; cmt.cadx = cadx;
        cmt.ftw = (float)tw; cmt.fth = (float)th;
        cmt.dacc = vld1q_s32(d); cmt.zdq = zdq; cmt.zbias = zbias; cmt.zlo = zlo;
        cmt.amode = !a_tex ? 2 : !a_in ? 1 : 0; /* OoT numbering: fa = !a_tex ? sa : !a_in ? ta : div255(ta*sa) */
        cmt.amin = amin; cmt.ablend = ablend; cmt.persp = persp; cmt.dmask = dmask; cmt.alpha = sUseAlpha; cmt.bilin = use_bilin;
    }
    /* The span's last n & 3 pixels as one masked NEON group, when there are at least
     * SM64_NEONTAIL of them (1-3; 4: never, the scalar loop draws them). */
    static int sNeonTail SAVESTATE_EXCLUDE = -1;
    if (sNeonTail < 0) { const char *e = getenv("SM64_NEONTAIL"); sNeonTail = (e && e[0] >= '1' && e[0] <= '4') ? e[0] - '0' : 2; }
    const bool neon_any = neon_cm || neon_fog || neon_noz || neon_noza || neon_to || neon_ut || neon_noto;
    SpanCM nk;
    SpanK nkc; /* span_row's per-triangle constants (nk.kc) */
    if (neon_any && !tex) { /* untextured: the texel fields are never read */
        nk.fbuf = fbuf; nk.zb = zb; nk.tex = NULL; nk.lw = 0; nk.trow = 0;
        nk.ulo = nk.vlo = 0; nk.uhi = nk.vhi = 0; nk.uand = nk.vand = 0;
    } else if (neon_any) {
        nk.fbuf = fbuf; nk.zb = zb; nk.tex = (const uint32_t *)texpx; nk.lw = __builtin_ctz((unsigned)tw); nk.trow = tw;
        if (ws == WRAP_REPEAT) { nk.ulo = INT32_MIN; nk.uhi = INT32_MAX; nk.uand = tw - 1; } else { nk.ulo = 0; nk.uhi = tw - 1; nk.uand = -1; }
        if (wt == WRAP_REPEAT) { nk.vlo = INT32_MIN; nk.vhi = INT32_MAX; nk.vand = th - 1; } else { nk.vlo = 0; nk.vhi = th - 1; nk.vand = -1; }
    }
    if (neon_any) {
        nk.zbias = (uint32_t)zbias; nk.zlo = (uint32_t)zlo; nk.dmask = dmask;
        nk.tidb = tidb; nk.tid = tid;
        nk.duf = nk.dvf = 0; nk.zdq = zdq; nk.rdq = rdq; nk.gdq = gdq; nk.bdq = bdq; nk.adq = adq;
        nk.fs = (uint32_t)fsb | (uint32_t)fsg << 8 | (uint32_t)fsr << 16 | (uint32_t)fsa << 24;
        nk.amin = (uint32_t)amin; nk.amode = !a_tex ? 0 : (!a_in ? 1 : 2);
        nk.noz = neon_noz || neon_noza || neon_noto; nk.fog = neon_fog || (neon_to && fog); nk.fogpx = (uint32_t)fogb | (uint32_t)fogg << 8 | (uint32_t)fogr << 16;
        nk.to = neon_to || neon_noto; nk.ut = neon_ut; nk.foga = fog ? foga : -1;
        /* the bilinear taps two at a time (gfx_soft_span_neon.h SPAN_BIL_HALF2); SM64_NEONPAIR=0: one
         * at a time (A/B), and the device bench's round-2 windows (neon2 0) go without them too */
        static int sNeonPair SAVESTATE_EXCLUDE = -1;
        /* SM64_NEONPAIR=0: four loads per pixel; 1: the paired horizontal taps; 2 (default): also the row below */
        if (sNeonPair < 0) { const char *e = getenv("SM64_NEONPAIR"); sNeonPair = (e && e[0] >= '0' && e[0] <= '2') ? e[0] - '0' : 2; }
        nk.pair = neon2 ? sNeonPair : 0;
        nk.skym = sSkyFlushing;
        nk.noblend = !ablend;
        if (gfx_soft_alphaskip < 0) { const char *e = getenv("SM64_ALPHASKIP"); gfx_soft_alphaskip = !(e && e[0] == '0'); }
        nk.askip = gfx_soft_alphaskip > 0;
        if (gfx_soft_zskip < 0) { const char *e = getenv("SM64_ZSKIP"); gfx_soft_zskip = !(e && e[0] == '0'); }
        nk.zskip = gfx_soft_zskip > 0 ? 4 : 0;

        nk.kc = &nkc; nk.kc_ok = 0;
    }
    /* A whole row per call (gfx_soft_span_neon.h span_row), the variant picked here once.
     * SM64_NEONROW=0: a call per span, as before (A/B); the device bench's round-2 windows
     * (neon2 0) go per span too. */
    static int sNeonRow SAVESTATE_EXCLUDE = -1;
    if (sNeonRow < 0) { const char *e = getenv("SM64_NEONROW"); sNeonRow = !(e && e[0] == '0'); }
    SpanRowFn row_fn = NULL;
    SpanRowT rt;
    if (neon_any && neon2 && sNeonRow) {
        if (gfx_soft_spanvec < 0) { const char *e = getenv("SM64_SPANVEC"); gfx_soft_spanvec = !(e && e[0] == '0'); }
        if (gfx_soft_shadepx < 0) { const char *e = getenv("SM64_SHADEPX"); gfx_soft_shadepx = !(e && e[0] == '0'); }
        if (gfx_soft_wraprr < 0) { const char *e = getenv("SM64_WRAPRR"); gfx_soft_wraprr = !(e && e[0] == '0'); }
        if (gfx_soft_lean < 0) { const char *e = getenv("SM64_LEAN"); gfx_soft_lean = !(e && e[0] == '0'); }
        if (gfx_soft_fullst < 0) { const char *e = getenv("SM64_FULLST"); gfx_soft_fullst = !(e && e[0] == '0'); }
        row_fn = span_row_pick(&nk, flat, use_bilin, sUseAlpha, gfx_soft_spanvec > 0 ? (gfx_soft_shadepx > 0 ? 2 : 1) : 0,
                               gfx_soft_wraprr > 0 && tex && ws == WRAP_REPEAT && wt == WRAP_REPEAT,
                               gfx_soft_lean > 0 ? (gfx_soft_fullst > 0 ? 2 : 1) : 0);
        rt.da = (float32x4_t){ zdx, crdx, cgdx, cbdx }; rt.db = (float32x4_t){ cadx, uwdx, vwdx, swdx };
        rt.swdx = swdx; rt.uwdx = uwdx; rt.vwdx = vwdx; rt.zdx = zdx;
        rt.crdx = crdx; rt.cgdx = cgdx; rt.cbdx = cbdx; rt.cadx = cadx;
        rt.ftw = (float)tw; rt.fth = (float)th; rt.persp = persp;
        rt.m = vcombine_f32(vdup_n_f32(rt.ftw), vdup_n_f32(rt.fth));
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
    /* Shared edges without cracks (IMG_0205, the grass top and the dirt side of Whomp's Fortress's
     * blocks): each triangle solved an edge's row crossing from its own vertex order and origin, so two
     * triangles sharing an edge rounded it differently by up to ~1e-3 px, against a 1e-5 px bias. Where
     * the edge passed a pixel centre (a near-vertical edge at x + 0.5 does for several rows running)
     * both could leave the pixel out: dashes of whatever lies behind. Now an edge's crossing is solved
     * from its endpoints in one fixed order (the lower y first, then the lower x) and the row's own
     * y, so both triangles get the same float; a pixel whose centre is right of it or on it belongs to
     * the triangle inside to the right, the others to the one inside to the left: each pixel exactly
     * once. (The barycentrics stay the triangle's own; the first pixel's can be a hair below 0.)
     * SM64_EDGEEXACT=1: on (default off, see below). */
    /* OFF by default (2026-09-30, the user's rule "slower but correct visuals"; speed changes must be exact):
     * it changed 10 px in 3 WF frames, closed no gaps (SM64's are T-junctions) and cost +1.5% (qemu). */
    if (gfx_soft_edge_exact < 0) { const char *e = getenv("SM64_EDGEEXACT"); gfx_soft_edge_exact = e && e[0] == '1'; }
    const bool eexact = gfx_soft_edge_exact > 0;
    float eax[3], eay[3], esl[3];
    if (eexact) {
        const SoftVtx *const ep[3][2] = { { v1, v2 }, { v2, v0 }, { v0, v1 } };
        for (int i = 0; i < 3; i++) {
            const SoftVtx *a = ep[i][0], *b = ep[i][1];
            if (b->y < a->y || (b->y == a->y && b->x < a->x)) { const SoftVtx *t = a; a = b; b = t; }
            eax[i] = a->x; eay[i] = a->y;
            esl[i] = b->y != a->y ? (b->x - a->x) / (b->y - a->y) : 0.0f;
        }
    }

    if (gfx_soft_pf < 0) { const char *e = getenv("SM64_PF"); gfx_soft_pf = !(e && e[0] == '0'); }
    const int pf = gfx_soft_pf;
    int ystart = miny;
#if defined(MIYOO) && defined(GFX_SOFT_NEON_SPAN)
    int ystep = 1;   /* 2 while the worker draws the odd rows (below) */
    TrJob job;
    bool split = false, was_split = false;
    int yend = maxy; /* the row loop's end: one row at a time while the noted rows are finished (SM64_SPLITREF) */
    int refi = 0, refg = 0, refw = 0;
#define TR_YEND yend
#else
#define TR_YEND maxy
#endif
#if defined(GFX_SOFT_NEON_SPAN)
    /* SM64_FASTROWS: the rows in tri_rows; a row it hands back (a span its NEON row refused) is finished below from
     * the state it reached, and tri_rows takes the next rows again */
    if (gfx_soft_fastrows < 0) { const char *e = getenv("SM64_FASTROWS"); gfx_soft_fastrows = !(e && e[0] == '0'); }
    const bool fastrows = row_fn && !eexact && gfx_soft_fastrows > 0;
    TriRows trr;
    TriRowsOut tro;
    bool resume = false;
    if (fastrows) {
        trr.va0 = (float32x4_t){ z0, r0, g0, bl0 }; trr.vb0 = (float32x4_t){ a0, uw0, vw0, iw0 };
        trr.va1 = (float32x4_t){ z1, r1, g1, bl1 }; trr.vb1 = (float32x4_t){ a1, uw1, vw1, iw1 };
        trr.va2 = (float32x4_t){ z2, r2, g2, bl2 }; trr.vb2 = (float32x4_t){ a2, uw2, vw2, iw2 };
        trr.fa = (float32x4_t){ 0.0f, r0, g0, bl0 }; trr.fb = (float32x4_t){ a0, 0.0f, 0.0f, 0.0f };
        trr.e0c = e0c; trr.e0x = e0x; trr.e0y = e0y; trr.e1c = e1c; trr.e1x = e1x; trr.e1y = e1y;
        trr.e2c = e2c; trr.e2x = e2x; trr.e2y = e2y; trr.inv_area = inv_area;
        trr.w0dx = w0dx; trr.w1dx = w1dx; trr.w2dx = w2dx;
        trr.minx = minx; trr.maxx = maxx; trr.sw = sW; trr.persp = persp; trr.pf = pf; trr.flat = flat;
        trr.ek[0] = w0dx > 0.0f ? 0 : w0dx < 0.0f ? 1 : 2;
        trr.ek[1] = w1dx > 0.0f ? 0 : w1dx < 0.0f ? 1 : 2;
        trr.ek[2] = w2dx > 0.0f ? 0 : w2dx < 0.0f ? 1 : 2;
        trr.cov = cov ? sCov : NULL; trr.fbuf = fbuf; trr.zb = zb;
        trr.fn = row_fn; trr.k = &nk; trr.t = &rt;
#if defined(MIYOO)
        if (sTrMinPx < 0) { const char *e = getenv("SM64_SPLIT"); sTrMinPx = e && e[0] ? atoi(e) : TR_SPLIT_PX; }
        if (sTrLate < 0) { const char *e = getenv("SM64_SPLITEDGE"); sTrLate = !(e && e[0] == '0'); }
        sTrTris++;
        trr.late = 0; tro.late = 0; tro.ref = NULL; tro.nref = tro.refcap = 0;
        if (gMiyooSplitRef < 0) { const char *e = getenv("SM64_SPLITREF"); gMiyooSplitRef = !(e && e[0] == '0'); }
        if (gMiyooMt && miyoo_worker_ready() && maxy - miny >= TR_SPLIT_ROWS && (area < 0.0f ? -area : area) >= 2.0f * (float)sTrMinPx &&
            gfx_miyoo_job_try()) { /* (the audio or present job still on the worker: this triangle on this core alone, no wait) */
            /* the worker's share: the odd rows, with its own SpanCM and SpanK (see TrJob); edge rows later (TR_LATE) */
            trr.late = sTrLate;
            job.trr = trr; job.nk = nk; job.nk.kc = &job.nkc; job.nk.kc_ok = 0; job.trr.k = &job.nk;
            job.y = miny + 1; job.maxy = maxy; job.yr = maxy; job.tro.px = 0; job.tro.late = 0;
            job.tro.ref = sTrRefs[1]; job.tro.nref = 0; job.tro.refcap = gMiyooSplitRef > 0 ? TR_REFS : 0;
            tro.ref = sTrRefs[0]; tro.refcap = gMiyooSplitRef > 0 ? TR_REFS : 0;
            miyoo_worker_run(tr_job, &job);
            split = true; was_split = true; ystep = 2;
            sTrSplits++;
        }
        ystart = tri_rows(&trr, miny, maxy, &tro, ystep);
#else
        ystart = tri_rows(&trr, miny, maxy, &tro);
#endif
        tri_px += tro.px;
        resume = ystart < maxy;
    }
#endif
#if defined(MIYOO) && defined(GFX_SOFT_NEON_SPAN)
rows_again: /* (a split's worker share handed back a row: this core draws it and the share's rest, below) */
#endif
    for (int y = ystart; y < TR_YEND; y++) {
        int x, xe, idx;
        float zf, cr, cg, cb, ca, uw, vw, sw, rw;
#if defined(GFX_SOFT_NEON_SPAN)
        if (resume) { /* the row tri_rows handed back, from where its NEON row stopped */
            resume = false;
            x = tro.x; xe = tro.xe; idx = tro.idx;
            zf = tro.f.zf; cr = tro.f.cr; cg = tro.f.cg; cb = tro.f.cb; ca = tro.f.ca;
            uw = tro.f.uw; vw = tro.f.vw; sw = tro.f.sw; rw = tro.f.rw;
        } else
#endif
        {
        const float py = y + 0.5f;
        /* Row bases recomputed exactly each row (no row-to-row drift). */
        const float b0 = (e0c - (py - e0y) * e0x) * inv_area;
        const float b1 = (e1c - (py - e1y) * e1x) * inv_area;
        const float b2 = (e2c - (py - e2y) * e2x) * inv_area;

        /* Solve b_i + (x - minx) * w_i_dx >= 0 for the pixel interval. */
        int xs = minx;
        xe = maxx;
        {
            const float bs[3] = { b0, b1, b2 }, ds[3] = { w0dx, w1dx, w2dx };
            if (eexact) {
                for (int i = 0; i < 3; i++) {
                    if (ds[i] == 0.0f) { if (bs[i] < 0.0f) { xs = xe; break; } continue; } /* horizontal: as before */
                    float xc = eax[i] + (py - eay[i]) * esl[i] - 0.5f;
                    if (xc > 1e6f) xc = 1e6f; else if (xc < -1e6f) xc = -1e6f;
                    const int a = iceilf(xc); /* the first pixel whose centre is at or right of the edge */
                    if (ds[i] > 0.0f) { if (a > xs) xs = a; }
                    else if (a < xe) xe = a;
                }
            } else
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
        /* Snap against rounding from the divisions above so the first pixel
         * really is inside per the linear edge functions. */
        while (!eexact && xs < xe) {
            const float o = (float)(xs - minx);
            if (b0 + o * w0dx >= 0 && b1 + o * w1dx >= 0 && b2 + o * w2dx >= 0) break;
            xs++;
        }
        if (xs >= xe) continue;
        tri_px += (uint32_t)(xe - xs); /* fill/overdraw proxy (before z-reject) */
        if (cov) cov_mark(sCov + y * sW + xs, xe - xs); /* the crack fill's coverage */

        /* Barycentrics at the span start. */
        const float o = (float)(xs - minx);
        float w0 = b0 + o * w0dx;
        float w1 = b1 + o * w1dx;
        float w2 = b2 + o * w2dx;

        zf = w0*z0 + w1*z1 + w2*z2;
        if (flat) { cr = r0; cg = g0; cb = bl0; ca = a0; } /* constant: no barycentric build */
        else { cr = w0*r0 + w1*r1 + w2*r2; cg = w0*g0 + w1*g1 + w2*g2; cb = w0*bl0 + w1*bl1 + w2*bl2; ca = w0*a0 + w1*a1 + w2*a2; }
        uw = w0*uw0 + w1*uw1 + w2*uw2;
        vw = w0*vw0 + w1*vw1 + w2*vw2;
        sw = w0*iw0 + w1*iw1 + w2*iw2;

        idx = y * sW + xs;
        if (pf) { __builtin_prefetch(fbuf + idx + sW); __builtin_prefetch(zb + idx + sW); }

        /* 1/w at the span start. The old loop recomputed it every span, but
         * the next span's start w IS this span's end w (same float op), so
         * carry the reciprocal forward: one divide per span instead of two.
         * Only perspective spans need it -- round 12 had hoisted the divides
         * out of that test, so untextured/affine spans paid a vdiv for nothing. */
        rw = 0.0f;
        if (persp) rw = (sw > 1e-12f) ? 1.0f / sw : 0.0f;
#if defined(GFX_SOFT_NEON_SPAN)
        if (cm_rows) { /* the whole row; flat triangles have zero colour steps, so rq.. clamp to the fs* */
            cm_row_neon(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            continue;
        }
#endif
        x = xs;
#if defined(GFX_SOFT_NEON_SPAN)
        if (row_fn) { /* the whole row; a span it refuses and the rest go on below, from its state there */
            SpanRowF rf = { zf, cr, cg, cb, ca, uw, vw, sw, rw };
            const int d = row_fn(&nk, &rt, &rf, idx, xe - xs);
            if (d == xe - xs) continue;
            x = xs + d; idx += d;
            zf = rf.zf; cr = rf.cr; cg = rf.cg; cb = rf.cb; ca = rf.ca; uw = rf.uw; vw = rf.vw; sw = rf.sw; rw = rf.rw;
        }
#endif
        }
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
#if defined(GFX_SOFT_NEON_SPAN)
            /* M5 round 1: the span's fixed-point starts and steps four at a time, the same values
             * as to_fix16 and the casts below: vcvt truncates and saturates like the VFP's scalar
             * vcvt (a NaN gives 0 either way), vmin/vmax keep the clamps, the multiplies are the
             * same IEEE singles. The scalar form's compares each waited on a vmrs (the A7 drains
             * its VFP pipe for it): ~6% of JRB's instructions, lines 263 and 801. SM64_SPANSETUP=0:
             * the scalar form (A/B). */
            static int sSpanSetup SAVESTATE_EXCLUDE = -1;
            if (sSpanSetup < 0) { const char *e = getenv("SM64_SPANSETUP"); sSpanSetup = !(e && e[0] == '0'); }
            int32_t zq, rq, gq, bq, aq;
            if (sSpanSetup) {
                if (tex) {
                    const float32x4_t a = { u_a, du, v_a, dv }, m = { (float)tw, (float)tw, (float)th, (float)th };
                    const float32x4_t c = vmaxq_f32(vminq_f32(vmulq_f32(a, m), vdupq_n_f32(16000.0f)), vdupq_n_f32(-16000.0f));
                    int32_t o[4]; vst1q_s32(o, vcvtq_s32_f32(vmulq_n_f32(c, 65536.0f)));
                    uf = o[0]; duf = o[1]; vf = o[2]; dvf = o[3];
                }
                const float32x4_t cc = { cr, cg, cb, ca };
                int32_t oc[4]; vst1q_s32(oc, vcvtq_s32_f32(vmulq_n_f32(cc, 65536.0f)));
                rq = oc[0]; gq = oc[1]; bq = oc[2]; aq = oc[3];
                const float32x2_t zc = vmax_f32(vmin_f32(vdup_n_f32(zf), vdup_n_f32(2.0f)), vdup_n_f32(-1.0f));
                zq = vget_lane_s32(vcvt_s32_f32(vmul_n_f32(zc, 65535.0f * 256.0f)), 0);
            } else {
                if (tex) {
                    uf = to_fix16(u_a * (float)tw); duf = to_fix16(du * (float)tw);
                    vf = to_fix16(v_a * (float)th); dvf = to_fix16(dv * (float)th);
                }
                zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
                rq = (int32_t)(cr * 65536.0f); gq = (int32_t)(cg * 65536.0f); bq = (int32_t)(cb * 65536.0f); aq = (int32_t)(ca * 65536.0f);
            }
#else
            if (tex) {
                uf = to_fix16(u_a * (float)tw); duf = to_fix16(du * (float)tw);
                vf = to_fix16(v_a * (float)th); dvf = to_fix16(dv * (float)th);
            }
            int32_t zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            int32_t rq = (int32_t)(cr * 65536.0f), gq = (int32_t)(cg * 65536.0f), bq = (int32_t)(cb * 65536.0f), aq = (int32_t)(ca * 65536.0f);
#endif

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST(CMV)                                                                             \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    uint32_t t = texel_at(texpx, t16, iv * tw + iu);                                    \
                    if (!(CMV) && trl) t = lerp_argb(t, texel_at(tex1px, t1_16, iv * tw + iu), trf);
#define FETCH_BOX(CMV)  uint32_t t = texel_box_ax(texpx, t16, tw, th, ws, wt, tmask_u, tmask_v, uf, vf, box_step, box_ax, sUseAlpha);
#define FETCH_BILINEAR(CMV)  uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);      \
                    if (!(CMV) && trl) t = lerp_argb(t, bilerp(tex1px, t1_16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v), trf);
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once per
 * triangle above) -- the four colour accumulators and their steps drop out of
 * the pixel loop, which is what made it spill to the stack (13 ARM registers,
 * ~12 live accumulators + steps). Same values: flat => every step is 0. */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
/* CM=1: the combiner is plain texture x shade ("modulate": rgb_tex && rgb_in
 * && !texmix && !rgb_texa -- over 90% of textured pixels in a race), so the
 * per-pixel walk of the combine flags (reloaded from the stack, three
 * conditional selects) drops out. CM=0 keeps the general combine. */
#define SPAN_LOOP(TEX, ZTEST, ALPHA, BILIN, FLAT) SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, 0)
#define SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, CM)                                               \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq COLSTEP_##FLAT) {     \
                if (!ZTEST && sSkyFlushing && (fbuf[idx] >> 24)) continue; /* the deferred sky */      \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias || uz + zlo < zb[idx]) continue; }                                      \
                const int sr = FLAT ? fsr : clamp8_sh16(rq), sg = FLAT ? fsg : clamp8_sh16(gq);         \
                const int sb = FLAT ? fsb : clamp8_sh16(bq), sa = FLAT ? fsa : clamp8_sh16(aq);         \
                int fr, fg, fb, fa;                                                                     \
                if (TEX) {                                                                              \
                    FETCH_##BILIN(CM)                                                                   \
                    const int ta = (t>>24)&0xff;                                                        \
                    const int tr = (!CM && rgb_texa) ? ta : (t>>16)&0xff, tg = (!CM && rgb_texa) ? ta : (t>>8)&0xff, tb = (!CM && rgb_texa) ? ta : t&0xff; \
                    if (CM)             { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (texmix) {                                                          \
                        if (rgb_decal)  { fr=div255(tr*ta + sr*(255-ta)); fg=div255(tg*ta + sg*(255-ta)); \
                                          fb=div255(tb*ta + sb*(255-ta)); }                             \
                        else if (hil_rgb) { fr=div255(hr*tr + sr*(255-tr)); fg=div255(hg*tg + sg*(255-tg)); \
                                          fb=div255(hb*tb + sb*(255-tb)); }                             \
                        else if (sTexAdd) { fr=tr+sr; fg=tg+sg; fb=tb+sb;                               \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                        else            { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    }                                                                                   \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = hil_a ? div255(ha*ta + sa*(255-ta)) : !a_tex ? sa : (!a_in ? ta : div255(ta*sa)); \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (!CM && fog) {                                                                       \
                    fr = div255(fogr*sa + fr*(255-sa)); fg = div255(fogg*sa + fg*(255-sa));             \
                    fb = div255(fogb*sa + fb*(255-sa)); if (foga >= 0) fa = foga;                       \
                }                                                                                       \
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
                if (ZTEST && dmask) { zb[idx] = (uint16_t)uz; if (tidb) tidb[idx] = tid; }              \
            }
            int span_left = span;
#if defined(GFX_SOFT_NEON_SPAN)
            const int n4 = neon_any ? ((span & 3) >= (neon2 ? sNeonTail : 4) ? span : span & ~3) : 0; /* the tail: see sNeonTail */
            if (n4) {
                nk.duf = duf; nk.dvf = dvf;
                if (span_cm_neon(&nk, flat, use_bilin, sUseAlpha, idx, n4, uf, vf, zq, rq, gq, bq, aq)) {
                    idx += n4; uf += n4 * duf; vf += n4 * dvf; zq += n4 * zdq;
                    if (!flat) { rq += n4 * rdq; gq += n4 * gdq; bq += n4 * bdq; aq += n4 * adq; }
                    span_left = span - n4;
                }
            }
#endif
            { const int span = span_left; /* the macros below run the rest (all of it off the NEON path) */
            if (box_ax) { /* squeezed UI text and icons: the area filter (never on the NEON paths) */
                if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BOX, 0) else SPAN_LOOP(1, 1, 0, BOX, 0) }
                else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BOX, 0) else SPAN_LOOP(1, 0, 0, BOX, 0) }
            } else if (tex && ztest && cm_mod) {
                /* the race's hot path: z-tested texture x shade */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                }
            } else if (tex) {
                if (flat && ztest) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR, 1) else SPAN_LOOP(1, 1, 0, BILINEAR, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEAREST, 1) else SPAN_LOOP(1, 1, 0, NEAREST, 1) }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR, 0) else SPAN_LOOP(1, 1, 0, BILINEAR, 0) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BILINEAR, 0) else SPAN_LOOP(1, 0, 0, BILINEAR, 0) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEAREST, 0) else SPAN_LOOP(1, 1, 0, NEAREST, 0) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEAREST, 0) else SPAN_LOOP(1, 0, 0, NEAREST, 0) }
                }
            } else if (!ztest && !sUseAlpha && !fog && rdq == 0 && gdq == 0 && bdq == 0) {
                /* Constant-colour run (the sky/ground gradient quads: colour varies
                 * only down the screen, so each span is one colour): a plain fill.
                 * Same pixels -- with no z, no alpha and zero steps every pixel of
                 * the span is argb(clamp(rq), clamp(gq), clamp(bq), 255). */
                const uint32_t c = argb((uint8_t)clamp8_sh16(rq), (uint8_t)clamp8_sh16(gq), (uint8_t)clamp8_sh16(bq), 255);
                uint32_t *const row = fbuf + idx;
                if (sSkyFlushing) { for (int i = 0; i < span; i++) if (!(row[i] >> 24)) row[i] = c; } /* the deferred sky */
                else for (int i = 0; i < span; i++) row[i] = c;
                idx += span;
            } else {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(0, 1, 1, NEAREST, 0) else SPAN_LOOP(0, 1, 0, NEAREST, 0) }
                else       { if (sUseAlpha) SPAN_LOOP(0, 0, 1, NEAREST, 0) else SPAN_LOOP(0, 0, 0, NEAREST, 0) }
            }
            }
#undef SPAN_LOOP
#undef SPAN_LOOP_CM
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
#if defined(GFX_SOFT_NEON_SPAN)
        if (fastrows) { /* the handed-back row is done: the next rows in tri_rows again */
#if defined(MIYOO)
            const int yr = tri_rows(&trr, y + ystep, yend, &tro, ystep);
#else
            const int yr = tri_rows(&trr, y + 1, maxy, &tro);
#endif
            tri_px += tro.px;
            if (yr >= TR_YEND) break;
            resume = true;
            y = yr - 1;
        }
#endif
    }
#if defined(MIYOO) && defined(GFX_SOFT_NEON_SPAN)
    if (split) {
        split = false;
        miyoo_worker_wait();
        tri_px += job.tro.px;
        refg = tro.nref; refw = job.tro.nref; /* the rows noted (SM64_SPLITREF), finished below */
        sTrHandRows += (uint32_t)(refg + refw);
        tro.ref = NULL; tro.nref = tro.refcap = 0; /* from here on a handed-back row is finished at once, as on one core */
        if (job.yr < maxy) { /* the worker's share stopped at a row its NEON row handed back: finish the share here */
            const int late = tro.late | job.tro.late;
            sTrHandback++;
            tro = job.tro; tro.px = 0; tro.late = late; tro.ref = NULL; tro.nref = tro.refcap = 0; resume = true; ystart = job.yr;
            goto rows_again;
        }
        tro.late |= job.tro.late;
    }
    if (refi < refg + refw) { /* SM64_SPLITREF: a noted row, from where its NEON row stopped, alone */
        const TrRef *const r = refi < refg ? &sTrRefs[0][refi] : &sTrRefs[1][refi - refg];
        refi++;
        tro.f = r->f; tro.x = r->x; tro.xe = r->xe; tro.idx = r->idx; tro.ref = NULL; tro.nref = tro.refcap = 0;
        resume = true; ystart = r->y; yend = r->y + 1;
        goto rows_again;
    }
    yend = maxy;
    if (fastrows && trr.late == 1 && tro.late) { /* TR_LATE: the edge rows both shares left out, now, in order */
        trr.late = 2; ystep = 1;
        ystart = tri_rows(&trr, miny, maxy, &tro, 1);
        tri_px += tro.px;
        resume = ystart < maxy;
        goto rows_again;
    }
    if (fastrows) sTrPx += tri_px;
    if (was_split) sTrSplitPx += tri_px;
#endif
    return (int)tri_px;
}
static bool sPhase3D SAVESTATE_EXCLUDE, sIntDirty SAVESTATE_EXCLUDE, sNatDirty SAVESTATE_EXCLUDE; /* see the frame section */
static bool sHad3D SAVESTATE_EXCLUDE;         /* any 3D batch this frame */
/* Lazy colour clears: a race frame's opening full-screen fill and the
 * composite each overwrite every pixel of their target, so clearing both
 * targets up front was ~400 KB of stores a frame for nothing. A target is now
 * cleared just before its first write (target_ready), unless that write is a
 * full-target opaque fill. */
static bool sNatStale SAVESTATE_EXCLUDE;      /* native colour not cleared yet this frame */
static bool sIntStale SAVESTATE_EXCLUDE;      /* internal colour must be cleared (alpha 0) before its next write */
static bool sIntFull SAVESTATE_EXCLUDE;       /* every internal pixel is opaque: a full-target opaque fill since the last clear */
static void target_ready(void);
static void target_covered(void);
static bool sUnified SAVESTATE_EXCLUDE;       /* internal res == native: 3D draws straight to native (see frame section) */
int gfx_soft_last_frame_had_3d = 1;           /* read by the WM's res ladder */

/* ------------------------------------------------------- the sky last --- */
/* M5 round 2 (the user: "we need to see why", other ports run faster): SM64 draws its skybox
 * first, every frame, over the whole screen (57.6k px, no depth test, no depth write, opaque),
 * and the level then paints over most of it: at the first blended draw, 64% (JRB) to 90% (the
 * castle grounds) of the sky is already hidden (census, 9 levels). So the sky is recorded
 * instead of drawn (vertices, render state, shader and the two textures' entries: the texel
 * memory never changes within a frame, the arena only grows until texman_clear) and drawn only
 * where nothing covers it, just before the first draw that could need it beneath: blended,
 * written without depth, drawn without a depth test, 2D, a phase switch, a texture reset, the
 * frame's end. Until then every draw is opaque and writes each pixel whole, so a pixel it wrote
 * shows it whatever was under it, and one it didn't shows the sky: as before.
 * "Written" is the pixel's alpha byte. Every raster path writes 255 and the composite already
 * reads it as coverage (the internal target clears to 0); so the recording clears the target's
 * alpha to 0 (unless it's the freshly cleared internal target) and the flush draws the sky only
 * where it is still 0 -- whatever the depth holds (a far-plane pixel can be written with the
 * cleared depth, 0xFFFF). The picture is the same bit for bit. SM64_SKYLAST=0: the sky first, as
 * before; the device bench's round-2 windows (neon2 0) draw it first too. */
typedef struct {
    bool useAlpha, depthTest, depthMask, zDecal, texAdd, fog, triLerp;
    int hilite, fogR, fogG, fogB, fogA, triF;
    uint8_t hiliteC[4];
    int vpX, vpY, vpW, vpH, scX0, scY0, scX1, scY1;
    bool hasShader;
    struct ShaderProgram shader;
    uint32_t bound[2];
    SoftTex tex[2];
    uint32_t v0, ntris;
} SkyBatch;
static SkyBatch *sSkyB SAVESTATE_EXCLUDE;
static SoftVtx *sSkyV SAVESTATE_EXCLUDE;
static uint32_t sSkyBN SAVESTATE_EXCLUDE, sSkyBCap SAVESTATE_EXCLUDE, sSkyVN SAVESTATE_EXCLUDE, sSkyVCap SAVESTATE_EXCLUDE;
/* 0: the frame's 3D hasn't started; 1: recording the sky; 2: the sky pending; 3: done this frame */
static int sSkyState SAVESTATE_EXCLUDE;
static int sSkyLast SAVESTATE_EXCLUDE = -1;
static bool sky_save(SkyBatch *b) {
    b->useAlpha = sUseAlpha; b->depthTest = sDepthTest; b->depthMask = sDepthMask; b->zDecal = sZDecal;
    b->texAdd = sTexAdd; b->fog = sFog; b->triLerp = sTriLerp;
    b->hilite = sHilite; memcpy(b->hiliteC, sHiliteC, 4);
    b->fogR = sFogR; b->fogG = sFogG; b->fogB = sFogB; b->fogA = sFogA; b->triF = sTriF;
    b->vpX = sVpX; b->vpY = sVpY; b->vpW = sVpW; b->vpH = sVpH;
    b->scX0 = sScX0; b->scY0 = sScY0; b->scX1 = sScX1; b->scY1 = sScY1;
    b->hasShader = sCurShader != NULL;
    if (sCurShader) b->shader = *sCurShader;
    for (int t = 0; t < 2; t++) {
        b->bound[t] = sBoundTex[t];
        if (sBoundTex[t] < MAX_TEXTURES) b->tex[t] = sTex[sBoundTex[t]];
    }
    return true;
}
static void sky_load(SkyBatch *b) { /* the recorded state as current (sky_flush saves and restores around it) */
    sUseAlpha = b->useAlpha; sDepthTest = b->depthTest; sDepthMask = b->depthMask; sZDecal = b->zDecal;
    sTexAdd = b->texAdd; sFog = b->fog; sTriLerp = b->triLerp;
    sHilite = b->hilite; memcpy(sHiliteC, b->hiliteC, 4);
    sFogR = b->fogR; sFogG = b->fogG; sFogB = b->fogB; sFogA = b->fogA; sTriF = b->triF;
    sVpX = b->vpX; sVpY = b->vpY; sVpW = b->vpW; sVpH = b->vpH;
    sScX0 = b->scX0; sScY0 = b->scY0; sScX1 = b->scX1; sScY1 = b->scY1;
    sCurShader = b->hasShader ? &b->shader : NULL;
    for (int t = 0; t < 2; t++) {
        sBoundTex[t] = b->bound[t];
        if (b->bound[t] < MAX_TEXTURES) sTex[b->bound[t]] = b->tex[t];
    }
}
/* Keep a sky batch; false = draw it now (out of memory). */
static bool sky_record(const SoftVtx *v, size_t ntris) {
    if (sSkyBN + 1 > sSkyBCap) {
        const uint32_t n = sSkyBCap ? sSkyBCap * 2 : 64;
        SkyBatch *q = realloc(sSkyB, (size_t)n * sizeof(*q));
        if (!q) return false;
        sSkyB = q; sSkyBCap = n;
    }
    if (sSkyVN + (uint32_t)ntris * 3 > sSkyVCap) {
        uint32_t n = sSkyVCap ? sSkyVCap : 3 * 256;
        while (n < sSkyVN + (uint32_t)ntris * 3) n *= 2;
        SoftVtx *q = realloc(sSkyV, (size_t)n * sizeof(*q));
        if (!q) return false;
        sSkyV = q; sSkyVCap = n;
    }
    SkyBatch *b = &sSkyB[sSkyBN++];
    sky_save(b);
    b->v0 = sSkyVN; b->ntris = (uint32_t)ntris;
    memcpy(sSkyV + sSkyVN, v, ntris * 3 * sizeof(*v));
    sSkyVN += (uint32_t)ntris * 3;
    return true;
}
static void select_target(bool is3d);
/* Draw the recorded sky where nothing has been written (alpha 0), on the 3D target. */
static void sky_flush(void) {
    if (sSkyState != 1 && sSkyState != 2) return;
    sSkyState = 3;
    if (sSkyBN == 0) return;
    PROF_BEGIN();
    SkyBatch cur;
    struct ShaderProgram *const curShader = sCurShader;
    sky_save(&cur);
    const bool was3d = sPhase3D;
    if (!was3d) select_target(true);
    target_ready(); /* the lazy clear, if the sky is the frame's first 3D write */
    sSkyFlushing = true;
    for (uint32_t i = 0; i < sSkyBN; i++) {
        SkyBatch *b = &sSkyB[i];
        SoftTex keep[2];
        for (int t = 0; t < 2; t++) if (b->bound[t] < MAX_TEXTURES) keep[t] = sTex[b->bound[t]];
        sky_load(b);
        const SoftVtx *v = sSkyV + b->v0;
        for (uint32_t j = 0; j < b->ntris; j++) {
            const int px = raster_tri(&v[j*3+0], &v[j*3+1], &v[j*3+2], true);
            if (px >= 0) { gfx_soft_tris++; gfx_soft_span_px += (uint32_t)px; }
        }
        for (int t = 0; t < 2; t++) if (b->bound[t] < MAX_TEXTURES) sTex[b->bound[t]] = keep[t];
    }
    sSkyFlushing = false;
    sky_load(&cur);
    sCurShader = curShader; /* the shader itself, not sky_load's copy */
    if (!was3d) select_target(false);
    sSkyBN = 0; sSkyVN = 0;
    PROF_END(PROF_R3D);
}
/* gfx_soft_draw_triangles, before drawing: true = the batch was kept (the sky). */
static bool sky_draw_hook(const SoftVtx *v, size_t ntris) {
    if (sSkyLast < 0) { const char *e = getenv("SM64_SKYLAST"); sSkyLast = !(e && e[0] == '0'); }
    if (!sSkyLast || !gfx_soft_neon_new || sSkyState == 3) return false;
    const bool skylike = sPhase3D && !sDepthTest && !sDepthMask && !sUseAlpha;
    if (sSkyState == 0) {
        if (!sPhase3D) return false;           /* 2D before the frame's 3D: nothing to do */
        if (!skylike) { sSkyState = 3; return false; } /* the frame doesn't open with a sky */
        /* The sky opens the 3D: clear the target's alpha (the coverage marks) unless it is the
         * internal target nothing has touched since its clear (all 0 already). */
        if (sUnified || sIntDirty || !sIntStale) {
            /* SM64_SKYCLR: a target still waiting for its lazy clear is cleared to 0 here, so its alpha is 0 already */
            if (gfx_soft_skyclr < 0) { const char *e = getenv("SM64_SKYCLR"); gfx_soft_skyclr = !(e && e[0] == '0'); }
            const bool fresh = gfx_soft_skyclr > 0 && (sFB == sNatFB ? sNatStale : sIntStale);
            target_ready();
            const int n = fresh ? 0 : sW * sH;
            for (int i = 0; i < n; i++) sFB[i] &= 0x00FFFFFFu;
        }
        sSkyState = 1;
    }
    if (sSkyState == 1) {
        if (skylike) return sky_record(v, ntris) ? true : (sky_flush(), false);
        sSkyState = 2;
    }
    /* pending: anything that could need the sky beneath it draws it first */
    if (!sPhase3D || sUseAlpha || !sDepthMask || !sDepthTest) sky_flush();
    return false;
}

static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t num_tris) {
    (void)buf_vbo_len;
    if (sky_draw_hook((const SoftVtx *)buf_vbo, num_tris)) { if (!sUnified && sPhase3D) sIntDirty = true; return; }
    PROF_BEGIN();
    const SoftVtx *v = (const SoftVtx *)buf_vbo;
    target_ready();
    if (!sUnified) { if (sPhase3D) sIntDirty = true; else sNatDirty = true; }
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
/* Nothing drawn on either target since start_frame (both uniformly clear). */
int gfx_soft_frame_blank(void) { return !sUnified && !sNatDirty && !sIntDirty; }

/* 2D path (HUD, menus, logo): gfx_pc.c emits PSP GU_SPRITES — TWO corners per
 * rect in a VertexColor {s16 u,v; RGBA; u16 x,y,z}: screen pixels + TEXEL UVs
 * (tile-origin already subtracted), combiner colour pre-folded by
 * gfx_eval_vertex_color. GU takes the sprite colour from the second vertex. */
typedef struct __attribute__((aligned(16))) {
    int16_t u, v; /* signed, as VertexColor: a sprite starting at s = -1 is texel -1, not 65535 (diddy-kong-racing 061ee77) */
    uint8_t r, g, b, a;
    uint16_t x, y, z;
} Vtx2D; /* must match gfx_pc.c's VertexColor layout/stride (16 bytes) */

/* HUD/menu sprites are laid out for the N64's 320-wide screen and land on 240
 * columns in CONVERT mode: 4 texels to 3 pixels. Nearest drops every fourth
 * texel column (strokes vanish or double: jaggy, low-res text and digits), so a
 * shrunk sprite takes the area average of exactly the texels each pixel covers -
 * every column counts, edges antialias. Alpha is premultiplied so transparent
 * texels do not darken the average. SM64_HUDFILTER=0 restores nearest (A/B).
 * (From f-zero-x ca8ef33.) */
int gfx_soft_hud_box SAVESTATE_EXCLUDE = -1;
/* A sprite row at a time: pixel i covers texels [u0 + i*duf, u0 + (i+1)*duf),
 * u0 = uf - duf/2 (uf is the first pixel's centre). Walked once left to right,
 * each texel is fetched and unpacked once -- not once per pixel it touches --
 * and the wrap is stepped instead of recomputed (menu backdrops repeat at
 * non-power-of-two widths: a divide per texel). Coverage is 8.8 per texel. */
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

/* Boxed rows are kept across frames. A HUD or menu sprite row is the same
 * texels at the same scale and phase frame after frame (desktop proxy, square output:
 * 97-99% of boxed rows repeat the previous frame's; menus box ~100k px a frame,
 * a race ~7k), so after its first frame a row costs one hash probe instead of
 * the walk -- less than the nearest fetch it replaced. The key is everything
 * box_row reads: texture data pointer, row, start, step, width, wrap, formats.
 * Texture data never changes in place (the arena only bump-allocates and
 * texman_clear, its one reset, empties this cache too), so a hit is always
 * exactly the row box_row would compute. When the store fills, rows not used
 * this frame or the last are dropped (then all but this frame's, then all).
 * SM64_BOXCACHE=0 walks every row every frame (A/B). */
#define BOXC_PX   (192u * 1024u) /* filtered texels: 768 KB */
#define BOXC_ENTS 4096u
#define BOXC_HASH 8192u          /* power of two, 2x BOXC_ENTS */
typedef struct { uintptr_t px; int32_t rowbase, uf, duf; uint32_t misc, off, frame; } BoxEnt;
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
static inline uint32_t box_hash(uintptr_t px, int32_t rowbase, int32_t uf, int32_t duf, uint32_t misc) {
    uint32_t h = (uint32_t)px * 0x9E3779B1u ^ (uint32_t)rowbase * 0x85EBCA77u ^ (uint32_t)uf * 0xC2B2AE3Du ^
                 (uint32_t)duf * 0x27D4EB2Fu ^ misc;
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
        uint32_t h = box_hash(b->px, b->rowbase, b->uf, b->duf, b->misc);
        while (sBoxHash[h]) h = (h + 1) & (BOXC_HASH - 1);
        sBoxHash[h] = (uint16_t)(i + 1);
    }
}
/* box_row through the cache: the row's texels (into tmp only when the cache is off). */
static const uint32_t *box_row_cached(uint32_t *tmp, int n, const void *px, bool t16, int rowbase, int32_t uf, int32_t duf,
                                      int tw, int ws, int mu, bool premul) {
    if (sBoxState == 0) {
        const char *e = getenv("SM64_BOXCACHE");
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
    const uint32_t misc = (uint32_t)n | (uint32_t)ws << 10 | (uint32_t)premul << 12 | (uint32_t)t16 << 13 | (uint32_t)tw << 16;
    const uint32_t now = gfx_soft_frames_rendered;
    uint32_t h = box_hash(key, rowbase, uf, duf, misc);
    for (uint32_t e; (e = sBoxHash[h]) != 0; h = (h + 1) & (BOXC_HASH - 1)) {
        BoxEnt *b = &sBoxEnt[e - 1];
        if (b->px == key && b->rowbase == rowbase && b->uf == uf && b->duf == duf && b->misc == misc) {
            b->frame = now;
            return &sBoxPx[b->off];
        }
    }
    if (sBoxUsed + (uint32_t)n > BOXC_PX || sBoxN >= BOXC_ENTS) {
        box_cache_compact(now, 1);
        if (sBoxUsed + (uint32_t)n > BOXC_PX || sBoxN >= BOXC_ENTS) box_cache_compact(now, 0);
        if (sBoxUsed + (uint32_t)n > BOXC_PX || sBoxN >= BOXC_ENTS) box_cache_clear();
        h = box_hash(key, rowbase, uf, duf, misc);
        while (sBoxHash[h]) h = (h + 1) & (BOXC_HASH - 1);
    }
    BoxEnt *b = &sBoxEnt[sBoxN];
    b->px = key; b->rowbase = rowbase; b->uf = uf; b->duf = duf; b->misc = misc; b->off = sBoxUsed; b->frame = now;
    sBoxHash[h] = (uint16_t)++sBoxN;
    uint32_t *out = &sBoxPx[sBoxUsed];
    sBoxUsed += (uint32_t)n;
    box_row(out, n, px, t16, rowbase, uf, duf, tw, ws, mu, premul);
    return out;
}

void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t num_sprites) {
    sky_flush(); /* a 2D draw may blend over the sky */
    (void)buf_vbo_len;
    const Vtx2D *q = (const Vtx2D *)buf_vbo;
    const SoftTex *tex = (sCurShader && sCurShader->cc.used_textures[0]) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) { untex_log("2d", sBoundTex[0]); tex = NULL; }
    const bool modulate = !sCurShader || sCurShader->cc.num_inputs > 0;
    /* Normally the 2D phase (native); the opening full-screen fill of a race
     * frame lands on the internal target instead (gfx_pc gfx_rect_on_3d). */
    if (!sUnified && sPhase3D) sIntDirty = true; else sNatDirty = true;
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
        if (cx0 >= cx1 || cy0 >= cy1) continue; /* wholly clipped (e.g. menu strips past the right edge) */
        const int cr = b->r, cg = b->g, cb2 = b->b, ca = b->a; /* GU: colour of 2nd vertex */
        /* Per-sprite constants hoisted out of the pixel loop. amin folds the
         * texture-edge (0x55) and near-zero (8) alpha rejects. "plain" = the
         * texel is the final colour (no combiner input, or a white opaque
         * prim, which is what nearly every HUD sprite and menu tile uses). */
        const bool use_alpha = sUseAlpha;
        const int amin = use_alpha ? ((sCurShader && sCurShader->cc.opt_texture_edge) ? 0x55 : 8) : 0;
        if (!tex && (!use_alpha || ca >= 255) && cx0 == 0 && cy0 == 0 && cx1 == sW && cy1 == sH) {
            target_covered(); /* this solid rect is about to overwrite the whole target */
        } else {
            target_ready();
        }
        const bool rgb_tex = tex && sCurShader && sCurShader->rgb_tex, rgb_in = !sCurShader || sCurShader->rgb_in;
        const bool a_tex = tex && sCurShader && sCurShader->a_tex, a_in = !sCurShader || sCurShader->a_in;
        const bool rgb_texa = sCurShader && sCurShader->rgb_texa, rgb_decal = sCurShader && sCurShader->rgb_decal;
        /* plain: the texel IS the pixel (both channels from the texture, no
         * input, or a white opaque input that changes nothing). */
        const bool t16 = tex && tex->is16; const void *texpx = tex ? tex->px : NULL;
        /* plain raw-copy fast path applies only to 8888 storage (16-bit needs
         * per-texel unpack, so those fall to the general loop below). */
        if (gfx_soft_hud_box < 0) { const char *e = getenv("SM64_HUDFILTER"); gfx_soft_hud_box = !(e && e[0] == '0'); }
        /* shrunk 1..3x along S: area-average (see box_row) */
        const bool boxf = tex && gfx_soft_hud_box && dudx > 1.02f && dudx <= 3.0f && cx1 - cx0 <= BOX_MAX;
        const bool plain = !boxf && tex && !t16 && rgb_tex && a_tex && !rgb_texa &&
                           ((!rgb_in && !a_in) || (!sTexAdd && cr == 255 && cg == 255 && cb2 == 255 && ca == 255));
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
            uint32_t boxtmp[BOX_MAX];
            /* a sprite clipped to nothing never reaches the row cache: a negative n
             * rewound its row store (diddy-kong-racing 061ee77, the cross-port lead's S4) */
            const uint32_t *boxrow = (boxf && cx1 > cx0) ? box_row_cached(boxtmp, cx1 - cx0, texpx, t16, trow_base, uf, duf, tw, ws2, mu, use_alpha) : NULL;
            for (int x = cx0; x < cx1; x++, uf += duf) {
                const uint32_t t = boxf ? boxrow[x - cx0] : texel_at(texpx, t16, trow_base + wrapf(uf >> 16, tw, ws2, mu));
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
static void nat_ready(void) {
    if (sNatStale) { memset(sNatFB, 0, (size_t)sNatW * sNatH * 4); sNatStale = false; }
}
static void int_ready(void) {
    if (sIntStale) { memset(sIntFB, 0, (size_t)sIntW * sIntH * 4); sIntStale = false; }
}
/* Before writing the current target (sFB aliases one of the two). */
static void target_ready(void) { if (sFB == sNatFB) nat_ready(); else int_ready(); }
/* The next write is an opaque fill of the whole current target. */
static void target_covered(void) {
    if (sFB == sNatFB) sNatStale = false;
    else { sIntStale = false; sIntFull = true; }
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

/* From (px, py) outside triangle `id`, the distance to the nearest of its edge lines that has the point
 * outside: how far the triangle's edge is from the pixel centre. */
static float tid_gap(uint16_t id, float px, float py) {
    const float *v = sTidV[id];
    const float area = (v[2] - v[0]) * (v[5] - v[1]) - (v[3] - v[1]) * (v[4] - v[0]);
    float best = 1e9f;
    for (int i = 0; i < 3; i++) {
        const float ax = v[2 * i], ay = v[2 * i + 1], bx = v[(2 * i + 2) % 6], by = v[(2 * i + 3) % 6];
        const float ex = bx - ax, ey = by - ay, len = sqrtf(ex * ex + ey * ey);
        if (len < 1e-6f) continue;
        const float e = ex * (py - ay) - ey * (px - ax); /* > 0 on the inside when area > 0 */
        if ((area > 0.0f) ? e >= 0.0f : e <= 0.0f) continue; /* this edge has the point inside */
        const float d = fabsf(e) / len;
        if (d < best) best = d;
    }
    return best;
}
#define CRACK_DZ 64      /* depth steps (of 65535) the surface in the gap lies behind both neighbours */
#define CRACK_GAP 0.35f  /* px: a gap narrower than this between the two neighbours is a crack */
/* One pixel of the fill: a bare pixel between two covered ones, or a covered one in a T-junction's sliver (its
 * depth from a farther, third triangle between two others), takes the per-channel mean of those two. */
static inline void crack_px(const uint8_t *c, const uint16_t *t, const uint16_t *z, uint32_t *f, int x, int y, int w) {
    uint32_t a, b;
    if (c[x]) { /* covered: a farther surface in a T-junction's sliver between two triangles */
        const uint16_t id = t[x];
        const float pxc = x + 0.5f, pyc = y + 0.5f;
        if (id != t[x - 1] && id != t[x + 1] && t[x - 1] && t[x + 1] && t[x - 1] != t[x + 1] &&
            z[x] > z[x - 1] + CRACK_DZ && z[x] > z[x + 1] + CRACK_DZ &&
            tid_gap(t[x - 1], pxc, pyc) + tid_gap(t[x + 1], pxc, pyc) < CRACK_GAP) { a = f[x - 1]; b = f[x + 1]; }
        else if (id != t[x - w] && id != t[x + w] && t[x - w] && t[x + w] && t[x - w] != t[x + w] &&
                 z[x] > z[x - w] + CRACK_DZ && z[x] > z[x + w] + CRACK_DZ &&
                 tid_gap(t[x - w], pxc, pyc) + tid_gap(t[x + w], pxc, pyc) < CRACK_GAP) { a = f[x - w]; b = f[x + w]; }
        else return;
    } else if (c[x - 1] && c[x + 1]) { a = f[x - 1]; b = f[x + 1]; }
    else if (c[x - w] && c[x + w]) { a = f[x - w]; b = f[x + w]; }
    else return;
    f[x] = (a & b) + (((a ^ b) & 0xFEFEFEFEu) >> 1); /* the per-channel mean */
}
#if defined(MIYOO) && defined(GFX_SOFT_NEON_SPAN)
/* SM64_CRACKSPLIT (2026-10-12, default 1; the Miyoo with the worker on, SM64_CRACKLEAN's scan): the fill in two steps.
 * The scan -- SM64_CRACKLEAN's NEON test over every pixel of the box, nearly all of the fill's time -- reads only the
 * coverage, the ids and the depths, which the fill never changes, so each core scans half the rows at once (the worker
 * the lower half) into a list of the pixels it would have passed to crack_px. Then this core runs crack_px on both lists
 * in raster order (the upper half's, then the lower's): exactly the calls, in exactly the order, of the one-pass fill,
 * so each pixel reads the same already-filled neighbours. 0: one pass, as before. */
int gMiyooCrackSplit SAVESTATE_EXCLUDE = -1;
static uint32_t *sCrackList[2] SAVESTATE_EXCLUDE; /* malloc'd, w * h entries each */
static int sCrackListCap SAVESTATE_EXCLUDE;
typedef struct { int y0, y1, x0, x1, w, n; uint32_t *out; } CrackScan;
static void crack_scan(void *arg) {
    CrackScan *const j = (CrackScan *)arg;
    const int w = j->w, x0 = j->x0, x1 = j->x1;
    int n = 0;
    for (int y = j->y0; y < j->y1; y++) {
        const uint8_t *c = sCov + y * w;
        const uint16_t *t = sTid + y * w;
        for (int x = x0; x < x1; x++) {
            if (x + 8 <= x1) { /* crack_fill's SM64_CRACKLEAN test, as it is there */
                const uint16x8_t t0 = vld1q_u16(t + x), tl = vld1q_u16(t + x - 1), tr = vld1q_u16(t + x + 1);
                const uint16x8_t tu = vld1q_u16(t + x - w), td = vld1q_u16(t + x + w);
                const uint16x8_t z16 = vdupq_n_u16(0);
                const uint16x8_t qh = vorrq_u16(vorrq_u16(vceqq_u16(t0, tl), vceqq_u16(t0, tr)),
                                                vorrq_u16(vceqq_u16(tl, tr), vceqq_u16(vminq_u16(tl, tr), z16)));
                const uint16x8_t qv = vorrq_u16(vorrq_u16(vceqq_u16(t0, tu), vceqq_u16(t0, td)),
                                                vorrq_u16(vceqq_u16(tu, td), vceqq_u16(vminq_u16(tu, td), z16)));
                const uint8x8_t quiet = vmovn_u16(vandq_u16(qh, qv));
                const uint8x8_t nb = vorr_u8(vand_u8(vld1_u8(c + x - 1), vld1_u8(c + x + 1)), vand_u8(vld1_u8(c + x - w), vld1_u8(c + x + w)));
                const uint8x8_t hole = vbic_u8(nb, vld1_u8(c + x));
                const uint8x8_t bad = vorn_u8(vtst_u8(hole, hole), quiet);
                const uint32x2_t r = vreinterpret_u32_u8(bad);
                if (vget_lane_u32(vpmax_u32(r, r), 0) != 0) {
                    uint8x8_t m8 = vand_u8(bad, vcreate_u8(0x8040201008040201ull));
                    m8 = vpadd_u8(m8, m8); m8 = vpadd_u8(m8, m8); m8 = vpadd_u8(m8, m8);
                    for (unsigned m = vget_lane_u8(m8, 0); m; m &= m - 1) j->out[n++] = (uint32_t)(y * w + x + __builtin_ctz(m));
                }
                x += 7;
                continue;
            }
            j->out[n++] = (uint32_t)(y * w + x); /* the row's last pixels: crack_px tests each */
        }
    }
    j->n = n;
}
#endif
/* The end of a 3D pass: fill its cracks (see gfx_soft_crackfill), then clear the coverage. Pixels are visited in
 * raster order and a pixel's test reads only coverage, ids and depth, which the fill doesn't change, so skipping
 * any pixel the test would leave alone gives the same picture. */
static void crack_fill(void) {
    if (!sCovAny) return;
    sCovAny = false;
    if (gfx_soft_crackbox < 0) { const char *e = getenv("SM64_CRACKBOX"); gfx_soft_crackbox = !(e && e[0] == '0'); }
    if (gfx_soft_cracklean < 0) { const char *e = getenv("SM64_CRACKLEAN"); gfx_soft_cracklean = !(e && e[0] == '0'); }
    const int w = sW, h = sH;
    const bool box = gfx_soft_crackbox > 0 && sCovY1 > 0;
    /* the pixels tested: x0 .. x1 - 1 on rows y0 .. y1 - 1 */
    int x0 = 1, x1 = w - 1, y0 = 1, y1 = h - 1;
    if (box) {
        if (sCovX0 - 1 > x0) x0 = sCovX0 - 1;
        if (sCovX1 + 1 < x1) x1 = sCovX1 + 1;
        if (sCovY0 - 1 > y0) y0 = sCovY0 - 1;
        if (sCovY1 + 1 < y1) y1 = sCovY1 + 1;
    }
#if defined(GFX_SOFT_NEON_SPAN)
    const bool lean = gfx_soft_cracklean > 0;
#endif
#if defined(MIYOO) && defined(GFX_SOFT_NEON_SPAN)
    if (gMiyooCrackSplit < 0) { const char *e = getenv("SM64_CRACKSPLIT"); gMiyooCrackSplit = !(e && e[0] == '0'); }
    if (gMiyooCrackSplit > 0 && gMiyooMt && lean && miyoo_worker_ready() && y1 - y0 >= 16 && x1 > x0) {
        if (sCrackListCap < w * h) {
            free(sCrackList[0]); free(sCrackList[1]);
            sCrackList[0] = malloc((size_t)w * h * sizeof(uint32_t)); sCrackList[1] = malloc((size_t)w * h * sizeof(uint32_t));
            sCrackListCap = sCrackList[0] && sCrackList[1] ? w * h : 0;
        }
        if (sCrackListCap >= w * h) {
            const int ym = (y0 + y1) / 2;
            CrackScan top = { y0, ym, x0, x1, w, 0, sCrackList[0] }, bot = { ym, y1, x0, x1, w, 0, sCrackList[1] };
            if (gMiyooJobOut) gfx_miyoo_job_fence();
            miyoo_worker_run(crack_scan, &bot);
            crack_scan(&top);
            miyoo_worker_wait();
            const CrackScan *const part[2] = { &top, &bot };
            for (int k = 0; k < 2; k++)
                for (int i = 0; i < part[k]->n; i++) {
                    const uint32_t p = part[k]->out[i];
                    const int y = (int)(p / (uint32_t)w), x = (int)(p - (uint32_t)y * (uint32_t)w);
                    crack_px(sCov + y * w, sTid + y * w, sZB + y * w, sFB + y * w, x, y, w);
                }
            y1 = y0; /* (the one-pass loop below has nothing left to do) */
        }
    }
#endif
    for (int y = y0; y < y1; y++) {
        const uint8_t *c = sCov + y * w;
        const uint16_t *t = sTid + y * w, *z = sZB + y * w;
        uint32_t *f = sFB + y * w;
        int scalar_to = 0; /* the NEON test found a candidate among x .. scalar_to - 1: test them one by one */
        for (int x = x0; x < x1; x++) {
#if defined(GFX_SOFT_NEON_SPAN)
            if (lean && x + 8 <= x1) {
                /* SM64_CRACKLEAN. A seam lane: its id differs from both neighbours' on a line, the two differ, and
                 * neither is 0; a hole lane: bare, with both neighbours on a line covered. A bare lane passing the
                 * seam test is a hole anyway (an id implies coverage), so the bare test can be left out there. */
                const uint16x8_t t0 = vld1q_u16(t + x), tl = vld1q_u16(t + x - 1), tr = vld1q_u16(t + x + 1);
                const uint16x8_t tu = vld1q_u16(t + x - w), td = vld1q_u16(t + x + w);
                const uint16x8_t z16 = vdupq_n_u16(0);
                const uint16x8_t qh = vorrq_u16(vorrq_u16(vceqq_u16(t0, tl), vceqq_u16(t0, tr)),
                                                vorrq_u16(vceqq_u16(tl, tr), vceqq_u16(vminq_u16(tl, tr), z16)));
                const uint16x8_t qv = vorrq_u16(vorrq_u16(vceqq_u16(t0, tu), vceqq_u16(t0, td)),
                                                vorrq_u16(vceqq_u16(tu, td), vceqq_u16(vminq_u16(tu, td), z16)));
                const uint8x8_t quiet = vmovn_u16(vandq_u16(qh, qv)); /* 0xFF: no seam here */
                const uint8x8_t nb = vorr_u8(vand_u8(vld1_u8(c + x - 1), vld1_u8(c + x + 1)), vand_u8(vld1_u8(c + x - w), vld1_u8(c + x + w)));
                const uint8x8_t hole = vbic_u8(nb, vld1_u8(c + x)); /* coverage bytes are 0 or 1: a hole lane is 1 */
                const uint8x8_t bad = vorn_u8(vtst_u8(hole, hole), quiet); /* 0xFF in every candidate lane */
                const uint32x2_t r = vreinterpret_u32_u8(bad);
                if (vget_lane_u32(vpmax_u32(r, r), 0) != 0) {
                    uint8x8_t m8 = vand_u8(bad, vcreate_u8(0x8040201008040201ull));
                    m8 = vpadd_u8(m8, m8); m8 = vpadd_u8(m8, m8); m8 = vpadd_u8(m8, m8);
                    for (unsigned m = vget_lane_u8(m8, 0); m; m &= m - 1) crack_px(c, t, z, f, x + __builtin_ctz(m), y, w);
                }
                x += 7;
                continue;
            }
            if (!lean && x >= scalar_to && x + 8 <= x1) {
                /* eight at a time (the fill was 14% of a WF frame one pixel at a time): any pixel bare
                 * between two covered ones, or covered with its depth from a third triangle between two
                 * others (left/right or above/below)? None: skip the eight. */
                const uint8x8_t c0 = vld1_u8(c + x), cl = vld1_u8(c + x - 1), cr = vld1_u8(c + x + 1);
                const uint8x8_t cu = vld1_u8(c + x - w), cd = vld1_u8(c + x + w);
                const uint8x8_t bare = vceq_u8(c0, vdup_n_u8(0));
                const uint8x8_t hole = vand_u8(bare, vorr_u8(vand_u8(cl, cr), vand_u8(cu, cd)));
                const uint16x8_t t0 = vld1q_u16(t + x), tl = vld1q_u16(t + x - 1), tr = vld1q_u16(t + x + 1);
                const uint16x8_t tu = vld1q_u16(t + x - w), td = vld1q_u16(t + x + w);
                const uint16x8_t z16 = vdupq_n_u16(0);
                const uint16x8_t sh = vandq_u16(vandq_u16(vmvnq_u16(vceqq_u16(t0, tl)), vmvnq_u16(vceqq_u16(t0, tr))),
                                                vandq_u16(vmvnq_u16(vceqq_u16(tl, tr)),
                                                          vandq_u16(vmvnq_u16(vceqq_u16(tl, z16)), vmvnq_u16(vceqq_u16(tr, z16)))));
                const uint16x8_t sv = vandq_u16(vandq_u16(vmvnq_u16(vceqq_u16(t0, tu)), vmvnq_u16(vceqq_u16(t0, td))),
                                                vandq_u16(vmvnq_u16(vceqq_u16(tu, td)),
                                                          vandq_u16(vmvnq_u16(vceqq_u16(tu, z16)), vmvnq_u16(vceqq_u16(td, z16)))));
                const uint8x8_t seam = vand_u8(vmvn_u8(bare), vmovn_u16(vorrq_u16(sh, sv)));
                const uint8x8_t any = vorr_u8(hole, seam);
                if (vget_lane_u64(vreinterpret_u64_u8(any), 0) == 0) { x += 7; continue; }
                scalar_to = x + 8;
            }
#else
            if (x + 8 <= x1) { /* eight at a time: all bare with no pixel covered above and below; or all
                                    * covered, each depth from the same triangle as its left and upper
                                    * neighbours' (no seam between triangles there) */
                uint64_t v, u, d;
                memcpy(&v, c + x, 8);
                if (v == 0) { memcpy(&u, c + x - w, 8); memcpy(&d, c + x + w, 8); if ((u & d) == 0) { x += 7; continue; } }
                else if (v == 0x0101010101010101ull) {
                    uint64_t a0, a1, l0, l1, u0, u1;
                    memcpy(&a0, t + x, 8); memcpy(&a1, t + x + 4, 8);
                    memcpy(&l0, t + x - 1, 8); memcpy(&l1, t + x + 3, 8);
                    memcpy(&u0, t + x - w, 8); memcpy(&u1, t + x + 4 - w, 8);
                    if (a0 == l0 && a1 == l1 && a0 == u0 && a1 == u1) { x += 7; continue; }
                }
            }
#endif
            crack_px(c, t, z, f, x, y, w);
        }
    }
    if (box) { /* from the box's first pixel to its last, one run: nothing outside the box was marked (ids are
                * written only inside covered spans), so the rows' ends in between are zero already */
        const int a = sCovY0 * w + sCovX0, n = (sCovY1 - 1) * w + sCovX1 - a;
        memset(sCov + a, 0, (size_t) n);
        if (gfx_soft_crackfill >= 2) memset(sTid + a, 0, (size_t) n * sizeof(sTid[0]));
    } else {
        memset(sCov, 0, (size_t) w * h);
        if (gfx_soft_crackfill >= 2) memset(sTid, 0, (size_t) w * h * sizeof(sTid[0]));
    }
    sCovY1 = 0;
    sTidSerial = 0;
}
static void select_target(bool is3d) {
    extern struct GfxDimensions gfx_current_dimensions;
    if (sPhase3D && !is3d) crack_fill();
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
static void composite_internal(void) {
    if (!sIntFB || !sNatFB || sIntW <= 0 || sIntH <= 0) return;
    PROF_BEGIN();
    /* Every internal pixel opaque (a race frame opens with a full-screen fill
     * on this target): the alpha test would pass everywhere, so it is a plain
     * nearest upscale that overwrites all of native -- no clear of its own. */
    const bool full = sIntFull && !sIntStale;
    if (!full) nat_ready();
    if (full) {
        for (int y = 0; y < sNatH; y++) {
            const uint32_t *src = sIntFB + sN2I_y[y] * sIntW;
            uint32_t *dst = sNatFB + y * sNatW;
            for (int x = 0; x < sNatW; x++) dst[x] = src[sN2I_x[x]];
        }
        sNatStale = false;
    } else if (sIntW == sNatW && sIntH == sNatH) {
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
    sIntStale = true; sIntFull = false; /* cleared before any later 3D write this frame */
    sIntDirty = false;
    sNatDirty = true;
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
    nat_ready();
    sIntStale = false; sIntFull = false; /* every internal pixel is rewritten, alpha 0 */
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
    sky_flush(); /* before the composite reads the 3D target */
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
    if (ow != sNatW || oh != sNatH || iw != sIntW || ih != sIntH) { build_maps(); sNatStale = sIntStale = true; sIntFull = false; }
}

static void gfx_soft_init(void) {
    unpack_lut_init();
    if (!sTexArena) {
        sTexArena = aligned_alloc(16, TEXARENA_SZ);
        if (!sTexArena) { fprintf(stderr, "gfx_soft: cannot allocate the %u MB texture arena\n", (unsigned) (TEXARENA_SZ >> 20)); abort(); }
    }
    ensure_targets(); select_target(false);
}
static void gfx_soft_on_resize(void) { ensure_targets(); select_target(sPhase3D); }
u32 gfx_soft_frames_rendered SAVESTATE_EXCLUDE; /* frames that reached end_frame (frameskip excluded) */
#if defined(MIYOO)
/* SM64_PRESJOB's job also clears the next frame's depth buffer (gfx_sdl12.c pres_job), on the worker: nothing reads or
 * writes it between the frame's end (the present) and the next frame's start, which then finds it cleared to the same
 * 0xFFFF its own memset writes (if the target is still that buffer, at that size). */
static const uint16_t *sZPre SAVESTATE_EXCLUDE;
static int sZPreN SAVESTATE_EXCLUDE;
void gfx_soft_preclear_z(void) {
    if (!sNatZB || sNatW <= 0 || sNatH <= 0) return;
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
    sZPreN = sNatW * sNatH;
    __atomic_store_n(&sZPre, sNatZB, __ATOMIC_RELEASE);
}
#endif
static void gfx_soft_start_frame(void) {
#if defined(MIYOO)
    { extern void gfx_miyoo_present_fence(void); gfx_miyoo_present_fence(); } /* the last frame's present job is done with it */
#endif
    sky_flush(); /* never left pending (the end of frame flushes) */
    sSkyState = 0;
    ensure_targets();
    PROF_BEGIN();
#if defined(MIYOO)
    { const uint16_t *const pre = __atomic_load_n(&sZPre, __ATOMIC_ACQUIRE); sZPre = NULL;
      if (!(pre && pre == sNatZB && sZPreN == sNatW * sNatH)) memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2); }
#else
    memset(sNatZB, 0xFF, (size_t)sNatW * sNatH * 2);
#endif
    sNatStale = true; /* colour: cleared lazily (target_ready) */
    if (!sUnified) { /* the internal buffer is unused in unified mode */
        sIntStale = true;
        memset(sIntZB, 0xFF, (size_t)sIntW * sIntH * 2);
    }
    sIntFull = false;
    PROF_END(PROF_CLR);
    sIntDirty = false; sNatDirty = false; sHad3D = false;
    select_target(true); /* frames open in the 3D phase; gfx_pc flips as needed */
}
static void gfx_soft_end_frame(void) {
    sky_flush();
    if (sPhase3D) crack_fill();
    if (!sUnified && sIntDirty) composite_internal();
    nat_ready(); /* nothing reached native this frame: present it cleared */
    gfx_soft_last_frame_had_3d = sHad3D;
    gfx_soft_frames_rendered++;
    select_target(false); /* leave native current for present/menu/dumps */
}
static void gfx_soft_finish_render(void) { }

/* -------------------------------------------------------------- shims --- */
/* gfx_pc.c calls these sceGu/texman helpers directly; software equivalents. */
void gfx_scegu_set_cull_mode(uint32_t cull) { (void)cull; /* GE-only API; winding cull lives in gfx_sp_tri1 */ }
void gfx_scegu_set_texfunc_add(bool on) { sTexAdd = on; }
/* Cutout triangles without FORCE_BL (G_RM_AA_ZB_TEX_EDGE; gfx_pc, SM64_EDGEOPAQUE): the RDP takes
 * coverage from the texel's alpha and never blends such a pixel with memory. */
void gfx_soft_set_edge_opaque(bool on) { sEdgeOpaque = on; }
void gfx_scegu_reset_texfunc_add(void) { sTexAdd = false; sHilite = 0; sFog = false; sTriLerp = false; }
void gfx_scegu_set_texfunc_hilite(int mode, const uint8_t c[4]) { sHilite = mode; memcpy(sHiliteC, c, 4); }
void gfx_scegu_set_trilerp(bool on, int f) { sTriLerp = on; sTriF = f; }
void gfx_scegu_set_fog(bool on, uint8_t r, uint8_t g, uint8_t b, int const_a) {
    sFog = on; sFogR = r; sFogG = g; sFogB = b; sFogA = const_a;
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
/* The bound ids go too: ids restart at 1, so a kept one would name the next texture imported
 * (another image) instead of none. gfx_pc re-imports and re-binds whatever the next draw uses. */
void texman_clear(void) { sky_flush(); /* its textures go */ sArenaUsed = 0; memset(sTex, 0, sizeof(sTex)); sNextTexId = 1; sBoundTex[0] = sBoundTex[1] = 0; box_cache_clear(); }

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
