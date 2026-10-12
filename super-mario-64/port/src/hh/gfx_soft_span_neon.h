/* The race's hot pixel loop, four pixels at a time (round 27).
 *
 * Covers SPAN_LOOP_CM with TEX=1, ZTEST=1, CM=1 (z-tested texture x shade,
 * ~62% of DK's span pixels at 100% 3D RES) for the 8888 power-of-two textures
 * every race samples (TEX32=2), nearest or bilinear, flat or smooth shade,
 * with or without alpha. Bit-identical to the scalar loop in gfx_soft.c:
 *  - z: clamp16_sh8 is a saturating narrow (vqshrun #8); the test keeps the
 *    scalar's uz > zb + zbias || uz + zlo < zb (zlo: a decal's lower side).
 *  - shade: clamp8_sh16 = vqshrun #16, then min 255.
 *  - bilinear: lerp2 is floor((a*(256-f) + b*f) / 256) per 8-bit channel; the
 *    sums stay <= 255*256, so 16-bit lanes do it exactly, in the same order
 *    (u, then v).
 *  - texture x shade and the alpha blend: div255(x) = (x + 1 + (x >> 8)) >> 8
 *    in 16-bit lanes (x <= 65025 + 256 < 65536). The blend runs on every lane:
 *    at fa = 255 it returns the colour unchanged (div255(255c) == c), which is
 *    what the scalar's "fa < 255" test skips.
 * Lanes that fail z or the alpha reject keep their old pixel and depth; the
 * scalar's `continue` has no other side effect. Texel addresses of failed
 * lanes are still wrapped/clamped into the texture, so every load is in range.
 * Wrap is clamp-then-AND: REPEAT clamps to the whole int range and ANDs with
 * size-1 (== c & mask), CLAMP clamps to [0, size-1] and ANDs with -1.
 *
 * M5 round 1 (the user: "improve speed"; JRB's water and the span tails ran the
 * scalar loops at ~4x a NEON pixel's instructions):
 *  - TO: the texel alone as the colour (SPAN_LOOP's !rgb_in branch), any alpha
 *    source, fogged or not (JRB's water: texel alpha, blended, fogged);
 *  - UT: untextured, opaque, z-tested (the colour is the shade);
 *  - fog with alpha: the pixel's alpha is taken before the fog step and a
 *    constant alpha (foga >= 0) after it, as the scalar loop does;
 *  - the tail: a span of n pixels ends with one group at n - 4, the lanes the
 *    groups before it drew masked off (for n < 4, the lanes before the span),
 *    so no pixel goes back to the scalar loop. Masked lanes behave as failed
 *    z lanes: old pixel and depth kept, texel addresses still wrapped in range. */
#pragma once
#include <stdint.h>
#include <arm_neon.h>

typedef struct {
    uint32_t *fbuf; uint16_t *zb;
    const uint32_t *tex; int lw;          /* 8888 texels, width 1 << lw */
    int32_t ulo, uhi, uand, vlo, vhi, vand;
    uint32_t zbias, zlo; int dmask;
    int32_t duf, dvf, zdq, rdq, gdq, bdq, adq;
    uint32_t fs;                          /* flat shade as a pixel: b | g<<8 | r<<16 | a<<24 */
    uint32_t amin;
    int amode;                            /* fa = 0: shade a, 1: texel a, 2: div255(ta*sa) */
    int fog; uint32_t fogpx;              /* opaque fog: blend to fogpx (b | g<<8 | r<<16) by the shade alpha */
    int noz;                              /* no depth test or write: opaque (the skybox), or blended (the wipes) */
    int to, ut;                           /* the texel alone as the colour; untextured (see the top) */
    int foga;                             /* fogged: the constant alpha after the fog step (-1: none) */
    int pair;                             /* bilinear: 1 the paired texel loads (SPAN_BIL_HALF2), 2 also the row below (HALF4) */
    int skym;                             /* no-z: only pixels nothing has written, alpha 0 (the deferred sky) */
    int trow;                             /* texels per texture row: 1 << lw */
    int noblend;                          /* a cutout (TEX_EDGE without FORCE_BL): written unblended past amin */
    void *kc; int kc_ok;                  /* span_row: the triangle's SpanK, built by its first row (raster_tri's) */
    uint16_t *tidb; uint16_t tid;         /* the crack fill's depth-writer ids (gfx_soft.c crack_fill); NULL: none */
    int askip;                            /* SM64_ALPHASKIP: a group whose texels all fail the alpha test ends early */
    int zskip;                            /* SM64_ZSKIP: span_row_v skips a span wholly behind the depth buffer */
} SpanCM;

static inline __attribute__((always_inline)) int32_t span_wrap(int32_t c, int32_t lo, int32_t hi, int32_t m) {
    c = c < lo ? lo : (c > hi ? hi : c);
    return c & m;
}
/* the same, narrowed: the add-high-narrow takes the sum's high byte (x <= 65025: no carry out) */
static inline __attribute__((always_inline)) uint8x8_t span_div255n(uint16x8_t x) {
    return vaddhn_u16(x, vsraq_n_u16(vdupq_n_u16(1), x, 8));
}
static inline __attribute__((always_inline)) uint16x8_t span_div255(uint16x8_t x) {
    return vshrq_n_u16(vaddq_u16(vsraq_n_u16(x, x, 8), vdupq_n_u16(1)), 8);
}
/* floor((x*(256-f) + y*f) / 256) per 16-bit lane */
static inline __attribute__((always_inline)) uint16x8_t span_lerp(uint16x8_t x, uint16x8_t y, uint16x8_t f, uint16x8_t g) {
    return vshrq_n_u16(vmlaq_u16(vmulq_u16(x, g), y, f), 8);
}
/* each u32 lane's top byte (its alpha) in all four of its bytes */
static inline __attribute__((always_inline)) uint8x16_t span_bcast_a(uint8x16_t x) {
    const uint8x8_t ix = { 3, 3, 3, 3, 7, 7, 7, 7 };
    return vcombine_u8(vtbl1_u8(vget_low_u8(x), ix), vtbl1_u8(vget_high_u8(x), ix));
}
/* channel = clamp8_sh16(q) in u32 lanes */
static inline __attribute__((always_inline)) uint32x4_t span_ch(int32x4_t q) {
    return vminq_u32(vmovl_u16(vqshrun_n_s32(q, 16)), vdupq_n_u32(255));
}

/* Coordinates travel as int16 lanes [x0 x1 x2 x3 | y0 y1 y2 y3]: a texel
 * coordinate is at most +-16000 (to_fix16 clamps), a wrapped one 0..255, and a
 * row start y << lw at most 255 << 8, so the texel index (<= 65535) fits too.
 * Wrap = clamp then AND on both axes at once. */
static inline __attribute__((always_inline)) int16x8_t span_wrap16(int16x8_t c, int16x8_t lo, int16x8_t hi, int16x8_t m) {
    return vandq_s16(vminq_s16(vmaxq_s16(c, lo), hi), m);
}
/* texel index (y << lw) + x per lane, from a wrapped [x | y] pair */
static inline __attribute__((always_inline)) uint16x4_t span_index(int16x8_t xs, int16x8_t ys, int16x4_t lw) {
    return vreinterpret_u16_s16(vadd_s16(vshl_s16(vget_high_s16(ys), lw), vget_low_s16(xs)));
}
/* [f0 f1 f2 f3] -> {f0 x4, f1 x4} (lo) / {f2 x4, f3 x4} (hi): a pixel's weight on its four channels */
static inline __attribute__((always_inline)) uint16x8_t span_spread_lo(uint16x4_t d) {
    return vcombine_u16(vdup_lane_u16(d, 0), vdup_lane_u16(d, 1)); /* (vzip needed copies: in place) */
}
static inline __attribute__((always_inline)) uint16x8_t span_spread_hi(uint16x4_t d) {
    return vcombine_u16(vdup_lane_u16(d, 2), vdup_lane_u16(d, 3));
}
/* every lane of a 64-bit mask set (a pairwise min to one word: one transfer, no stack slot) */
static inline __attribute__((always_inline)) int span_all(uint16x4_t m) {
    const uint32x2_t w = vreinterpret_u32_u16(m);
    return vget_lane_u32(vpmin_u32(w, w), 0) == 0xFFFFFFFFu;
}
static inline __attribute__((always_inline)) int span_none(uint16x4_t m) {
    const uint32x2_t w = vreinterpret_u32_u16(m);
    return vget_lane_u32(vpmax_u32(w, w), 0) == 0;
}
#define SPAN_LD(dst, ix, L, D) dst = vld1_lane_u32(tex + vget_lane_u16(ix, L), dst, D);
/* the four taps of pixels L0, L1 ({L0, L1} in each: ta top left, tb top right, tc, td below) -> 8 bytes.
 * span_lerp's floor((x*(256-f) + y*f) / 256) as floor((x*256 + (y-x)*f) / 256): the sum is at most
 * 255*256, so the wrapping 16-bit arithmetic gives it exactly, with one multiply instead of two. */
#define SPAN_BIL_MIX(OUT, ta, tb, tc, td, FU, FV)                                                                 \
    {   const uint8x8_t a8 = vreinterpret_u8_u32(ta), b8 = vreinterpret_u8_u32(tb);                            \
        const uint8x8_t c8 = vreinterpret_u8_u32(tc), d8 = vreinterpret_u8_u32(td);                            \
        const uint16x8_t top = vshrq_n_u16(vmlaq_u16(vshll_n_u8(a8, 8), vsubl_u8(b8, a8), FU), 8);            \
        const uint16x8_t bot = vshrq_n_u16(vmlaq_u16(vshll_n_u8(c8, 8), vsubl_u8(d8, c8), FU), 8);            \
        OUT = vshrn_n_u16(vmlaq_u16(vshlq_n_u16(top, 8), vsubq_u16(bot, top), FV), 8); }
/* one half: pixels L0, L1 of the group -> 8 bytes (two BGRA texels), bilinear */
#define SPAN_BIL_HALF(OUT, L0, L1, FU, FV)                                                                        \
    {   uint32x2_t ta = vdup_n_u32(0), tb = ta, tc = ta, td = ta;                                                \
        SPAN_LD(ta, i00, L0, 0) SPAN_LD(ta, i00, L1, 1) SPAN_LD(tb, i01, L0, 0) SPAN_LD(tb, i01, L1, 1)          \
        SPAN_LD(tc, i10, L0, 0) SPAN_LD(tc, i10, L1, 1) SPAN_LD(td, i11, L0, 0) SPAN_LD(td, i11, L1, 1)          \
        SPAN_BIL_MIX(OUT, ta, tb, tc, td, FU, FV) }
/* The same when each pixel's right-hand taps are the texels after its left-hand ones (no wrap
 * or clamp between them): one 64-bit load per row fetches both, and vtrn sorts the two pixels'
 * pairs into the same {L0, L1} vectors. Same texels, same result. */
#define SPAN_BIL_HALF2(OUT, L0, L1, FU, FV)                                                                       \
    {   const uint32x2x2_t ab = vtrn_u32(vld1_u32(tex + vget_lane_u16(i00, L0)), vld1_u32(tex + vget_lane_u16(i00, L1))); \
        const uint32x2x2_t cd = vtrn_u32(vld1_u32(tex + vget_lane_u16(i10, L0)), vld1_u32(tex + vget_lane_u16(i10, L1))); \
        SPAN_BIL_MIX(OUT, ab.val[0], ab.val[1], cd.val[0], cd.val[1], FU, FV) }

/* And when each pixel's lower taps are also the texels one texel row below its upper ones (no
 * wrap or clamp between the rows either): one texel address per pixel, the lower pair at
 * address + the texture's row length. Same texels, same result. */
#define SPAN_BIL_HALF4(OUT, L0, L1, FU, FV)                                                                       \
    {   const uint32_t *const p0 = tex + vget_lane_u16(i00, L0), *const p1 = tex + vget_lane_u16(i00, L1);       \
        const uint32x2x2_t ab = vtrn_u32(vld1_u32(p0), vld1_u32(p1));                                            \
        const uint32x2x2_t cd = vtrn_u32(vld1_u32(p0 + trow), vld1_u32(p1 + trow));                              \
        SPAN_BIL_MIX(OUT, ab.val[0], ab.val[1], cd.val[0], cd.val[1], FU, FV) }

/* One group of four pixels at idx. TAIL: the lanes lm names only (the span's last group, see the
 * top); the main loop's groups (TAIL = 0) take no mask at all, so round 2's loop is unchanged. */
static inline __attribute__((always_inline)) void
span_grp(const SpanCM *__restrict k, const uint32_t *tex, int idx, int32x4_t uv, int32x4_t vv, int32x4_t zv,
         int32x4_t rv, int32x4_t gv, int32x4_t bv, int32x4_t av, uint32x4_t lm,
         int16x8_t lo, int16x8_t hi, int16x8_t am, int16x4_t lw, uint32_t zbias, uint32_t zlo, uint32_t fs32,
         uint32_t amin32, int amode, int use_foga, uint8x16_t fogav8, int askip,
         const int FLAT, const int BILIN, const int ALPHA, const int FOG, const int NOZ, const int TO, const int UT,
         const int TAIL, const int SKIP, const int PV) {
    uint32_t *const fbuf = k->fbuf; uint16_t *const zb = k->zb;
    /* PV 8 (SM64_LEAN): the triangle's settings this row kind nearly always has, known when compiled (span_row_pick
     * picks these rows only for triangles that have them): the paired bilinear taps (pair 2), no lower decal side (zlo
     * none), and blended rows that blend and keep their depth (XLU; with PV 32 write it and the ids), opaque ones
     * that write depth and the crack fill's ids. The runtime tests and loads they replace come out of every group;
     * the same operations otherwise.
     * PV 16 (SM64_FULLST, in LEAN's opaque rows): a group whose four lanes all pass writes its pixels, depths and ids
     * without first reading the old ones it would keep none of (vbsl with an all-ones mask is the new value). */
    const int LEAN = PV & 8, FULLST = (PV & 16) && LEAN && !ALPHA && !NOZ && !TAIL;
    const int LZ = LEAN && (!ALPHA || (PV & 32)); /* PV 32: a blended row that writes depth and ids (JRB's water) */
    /* (each setting read where it is used, as before: read up front they lengthened the generic rows' live ranges) */
#define SPAN_PAIR (LEAN ? 2 : k->pair)
#define SPAN_NOBLEND ((LEAN && ALPHA) ? 0 : k->noblend)
#define SPAN_DMASK (LEAN ? LZ : k->dmask)
#define SPAN_TIDOK (LZ || k->tidb)
    uint16x4_t uz = vdup_n_u16(0), oz = uz, zp4 = uz;
    uint32x4_t zpass = TAIL ? lm : vdupq_n_u32(0xFFFFFFFFu);
    if (!NOZ) {
        uz = vqshrun_n_s32(zv, 8);
        oz = vld1_u16(&zb[idx]);
        /* uz <= oz + zbias && uz + zlo >= oz in u16 lanes: a saturated sum decides the same (zbias
         * <= 32768; zlo 0x10000, "none", as 65535: still always true) */
        const uint16x4_t zb16 = vdup_n_u16((uint16_t)zbias), zl16 = vdup_n_u16((uint16_t)(zlo > 65535 ? 65535 : zlo));
        uint16x4_t zp = LEAN ? vcle_u16(uz, vqadd_u16(oz, zb16))
                             : vand_u16(vcle_u16(uz, vqadd_u16(oz, zb16)), vcge_u16(vqadd_u16(uz, zl16), oz));
        if (TAIL) zp = vand_u16(zp, vmovn_u32(lm));
        /* the whole group behind the z-buffer: nothing to fetch */
        if (span_none(zp)) return;
        if (LEAN) zp4 = zp; /* (LEAN: the mask kept narrow until it is used, a d register where the q one was spilled) */
        else zpass = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(zp)));
    } else if (k->skym) { /* the deferred sky (gfx_soft.c sky_flush): only pixels still at alpha 0 */
        zpass = vandq_u32(zpass, vceqq_u32(vshrq_n_u32(vld1q_u32(&fbuf[idx]), 24), vdupq_n_u32(0)));
        if (span_none(vmovn_u32(zpass))) return;
    }
    uint8x16_t t8 = vdupq_n_u8(0);
    if (!UT) {
        const int16x8_t cxy = vcombine_s16(vshrn_n_s32(uv, 16), vshrn_n_s32(vv, 16));
        if (!BILIN) {
            /* PV 4 (SM64_WRAPRR): both axes repeat, where the clamp to the int16 range changes no lane: the AND alone */
            const int16x8_t w = (PV & 4) ? vandq_s16(cxy, am) : span_wrap16(cxy, lo, hi, am);
            const uint16x4_t ix = span_index(w, w, lw);
            uint32x2_t t01 = vdup_n_u32(0), t23 = t01;
            SPAN_LD(t01, ix, 0, 0) SPAN_LD(t01, ix, 1, 1) SPAN_LD(t23, ix, 2, 0) SPAN_LD(t23, ix, 3, 1)
            t8 = vreinterpretq_u8_u32(vcombine_u32(t01, t23));
        } else {
            const int16x8_t c1 = vaddq_s16(cxy, vdupq_n_s16(1));
            const int16x8_t w0 = (PV & 4) ? vandq_s16(cxy, am) : span_wrap16(cxy, lo, hi, am);
            const int16x8_t w1 = (PV & 4) ? vandq_s16(c1, am) : span_wrap16(c1, lo, hi, am);
            const uint16x4_t i00 = span_index(w0, w0, lw), i01 = span_index(w1, w0, lw);
            const uint16x4_t i10 = span_index(w0, w1, lw), i11 = span_index(w1, w1, lw);
            /* weights: (s >> 8) & 0xff, as u16 lanes */
            const uint16x4_t fu = vand_u16(vreinterpret_u16_s16(vshrn_n_s32(uv, 8)), vdup_n_u16(0xff));
            const uint16x4_t fv = vand_u16(vreinterpret_u16_s16(vshrn_n_s32(vv, 8)), vdup_n_u16(0xff));
            uint8x8_t h01, h23;
            /* every lane's right-hand x the one after its left-hand x: the paired loads; and its lower
             * y the one after its upper y: the row below at a fixed distance */
            const uint16x8_t adj8 = vceqq_s16(w1, vaddq_s16(w0, vdupq_n_s16(1)));
            if (PV & 64) { /* SM64_LEAN: the span's lanes all adjacent (span_adj), so the test is known to pass */
                const int trow = k->trow;
                SPAN_BIL_HALF4(h01, 0, 1, span_spread_lo(fu), span_spread_lo(fv))
                SPAN_BIL_HALF4(h23, 2, 3, span_spread_hi(fu), span_spread_hi(fv))
            } else if (SPAN_PAIR == 2 && span_all(vand_u16(vget_low_u16(adj8), vget_high_u16(adj8)))) {
                const int trow = k->trow;
                SPAN_BIL_HALF4(h01, 0, 1, span_spread_lo(fu), span_spread_lo(fv))
                SPAN_BIL_HALF4(h23, 2, 3, span_spread_hi(fu), span_spread_hi(fv))
            } else if (SPAN_PAIR && span_all(vget_low_u16(adj8))) {
                SPAN_BIL_HALF2(h01, 0, 1, span_spread_lo(fu), span_spread_lo(fv))
                SPAN_BIL_HALF2(h23, 2, 3, span_spread_hi(fu), span_spread_hi(fv))
            } else {
                SPAN_BIL_HALF(h01, 0, 1, span_spread_lo(fu), span_spread_lo(fv))
                SPAN_BIL_HALF(h23, 2, 3, span_spread_hi(fu), span_spread_hi(fv))
            }
            t8 = vcombine_u8(h01, h23);
        }
        /* SM64_ALPHASKIP (2026-10-05, ocarina-of-time's OOT_ALPHASKIP for these groups): when the pixel's
         * alpha is its texel's (askip: amode 1, fa = ta, no fog; see span_consts), a group whose four
         * texels' alphas are all under amin fails the alpha test in every lane, and such a lane writes
         * nothing (its old pixel, depth and id stored back): the group ends here, before the shade,
         * the blend and the stores. A lane's alpha byte is under amin exactly when its word is under
         * amin << 24. Bilinear: the lerped texel, the one the alpha test reads. */
        if (SKIP && askip && span_none(vmovn_u32(vcgeq_u32(vreinterpretq_u32_u8(t8), vdupq_n_u32(amin32 << 24))))) return;
    }
    /* shade as pixels, same byte order as the texels */
    uint32x4_t s;
    if (FLAT) s = vdupq_n_u32(fs32);
    else if (PV & 1) { /* SM64_SHADEPX: rv = the first pixel's { b, g, r, a } in 16.16, gv = the per-pixel step: each
                    * pixel's channels as span_ch makes them, already in pixel byte order (no zips) */
        const int32x4_t p1 = vaddq_s32(rv, gv), p2 = vaddq_s32(p1, gv), p3 = vaddq_s32(p2, gv);
        const uint8x8_t s01 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(rv, 16), vqshrun_n_s32(p1, 16)));
        const uint8x8_t s23 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(p2, 16), vqshrun_n_s32(p3, 16)));
        s = vreinterpretq_u32_u8(vcombine_u8(s01, s23));
    } else { /* span_ch per channel: the u16 saturating shift, then the u8 saturating narrow (min 255) */
        const uint8x8_t bg = vqmovn_u16(vcombine_u16(vqshrun_n_s32(bv, 16), vqshrun_n_s32(gv, 16)));
        const uint8x8_t ra = vqmovn_u16(vcombine_u16(vqshrun_n_s32(rv, 16), vqshrun_n_s32(av, 16)));
        const uint8x8x2_t z1 = vzip_u8(bg, ra);             /* b0 r0 b1 r1 .. | g0 a0 g1 a1 .. */
        const uint8x8x2_t z2 = vzip_u8(z1.val[0], z1.val[1]); /* b0 g0 r0 a0 b1 g1 r1 a1 | pixels 2, 3 */
        s = vreinterpretq_u32_u8(vcombine_u8(z2.val[0], z2.val[1]));
    }
    const uint8x16_t s8 = vreinterpretq_u8_u32(s);
    /* the colour: texture x shade, all four bytes (byte 3 = div255(ta*sa)); the texel alone; or the shade */
    uint8x16_t p8;
    if (UT) p8 = s8;
    else if (TO && !(ALPHA && amode == 2)) p8 = t8;
    else p8 = vcombine_u8(span_div255n(vmull_u8(vget_low_u8(t8), vget_low_u8(s8))),
                          span_div255n(vmull_u8(vget_high_u8(t8), vget_high_u8(s8))));
    /* the pixel's alpha, taken before the fog step (the scalar's order), in all four bytes of its lane */
    uint8x16_t fa8 = vdupq_n_u8(255);
    if (ALPHA) {
        fa8 = span_bcast_a(amode == 2 ? p8 : amode == 1 ? t8 : s8);
        if (TO && amode == 2) p8 = t8; /* the colour is still the texel alone */
    }
    if (FOG) { /* div255(fog*sa + c*(255-sa)) per channel, the scalar's sum */
        const uint8x16_t sa8 = span_bcast_a(s8);
        const uint8x16_t ia8 = vsubq_u8(vdupq_n_u8(255), sa8);
        const uint8x16_t f8 = vreinterpretq_u8_u32(vdupq_n_u32(k->fogpx));
        const uint16x8_t flo = vmlal_u8(vmull_u8(vget_low_u8(f8), vget_low_u8(sa8)), vget_low_u8(p8), vget_low_u8(ia8));
        const uint16x8_t fhi = vmlal_u8(vmull_u8(vget_high_u8(f8), vget_high_u8(sa8)), vget_high_u8(p8), vget_high_u8(ia8));
        p8 = vcombine_u8(span_div255n(flo), span_div255n(fhi));
        if (ALPHA && use_foga) fa8 = fogav8; /* a constant alpha, displaced by the fog factor */
    }
    const uint32x4_t p = vreinterpretq_u32_u8(p8);
    if (LEAN && !NOZ) zpass = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(zp4)));
    if (FULLST && span_all(zp4)) { /* (no alpha test in these rows: the z test is the whole mask) */
        vst1q_u32(&fbuf[idx], vorrq_u32(p, vdupq_n_u32(0xFF000000u)));
        vst1_u16(&zb[idx], uz);
        vst1_u16(&k->tidb[idx], vdup_n_u16(k->tid));
        return;
    }
    const uint32x4_t old = vld1q_u32(&fbuf[idx]);
    uint32x4_t out, wmask = zpass;
    if (ALPHA) wmask = vandq_u32(wmask, vreinterpretq_u32_u8(vcgeq_u8(fa8, vdupq_n_u8((uint8_t)amin32)))); /* amin <= 255 */
    if (ALPHA && !SPAN_NOBLEND) {
        const uint8x16_t ia8 = vsubq_u8(vdupq_n_u8(255), fa8);
        const uint8x16_t o8 = vreinterpretq_u8_u32(old);
        const uint16x8_t lo16 = vmlal_u8(vmull_u8(vget_low_u8(p8), vget_low_u8(fa8)), vget_low_u8(o8), vget_low_u8(ia8));
        const uint16x8_t hi16 = vmlal_u8(vmull_u8(vget_high_u8(p8), vget_high_u8(fa8)), vget_high_u8(o8), vget_high_u8(ia8));
        out = vorrq_u32(vreinterpretq_u32_u8(vcombine_u8(span_div255n(lo16), span_div255n(hi16))), vdupq_n_u32(0xFF000000u));
    } else {
        out = vorrq_u32(p, vdupq_n_u32(0xFF000000u));
    }
    vst1q_u32(&fbuf[idx], vbslq_u32(wmask, out, old));
    if (!NOZ && SPAN_DMASK) {
        vst1_u16(&zb[idx], vbsl_u16(vmovn_u32(wmask), uz, oz));
        if (SPAN_TIDOK) vst1_u16(&k->tidb[idx], vbsl_u16(vmovn_u32(wmask), vdup_n_u16(k->tid), vld1_u16(&k->tidb[idx])));
    }
#undef SPAN_PAIR
#undef SPAN_NOBLEND
#undef SPAN_DMASK
#undef SPAN_TIDOK
}

/* A span's per-triangle constants in NEON form (what span_cm4 built on every call). */
typedef struct {
    int16x8_t lo, hi, am; int16x4_t lw;
    uint32_t zbias, zlo, fs32, amin32; int amode, use_foga, askip;
    uint8x16_t fogav8; uint32x4_t all;
    int32x4_t zl, rl, gl, bl, al;          /* lane * the triangle's z and colour steps */
    int32x4_t zs, rs, gs, bs, as;          /* four times them: one group's step */
    int32x4_t dc, dc4;                     /* SM64_SHADEPX: { bdq, gdq, rdq, adq } (a pixel's step), and four times it */
} SpanK;
static inline __attribute__((always_inline)) void
span_consts(const SpanCM *k, SpanK *K, const int ALPHA, const int AMODE, const int FOG) {
    K->lo = vcombine_s16(vdup_n_s16((int16_t)(k->ulo < -32768 ? -32768 : k->ulo)), vdup_n_s16((int16_t)(k->vlo < -32768 ? -32768 : k->vlo)));
    K->hi = vcombine_s16(vdup_n_s16((int16_t)(k->uhi > 32767 ? 32767 : k->uhi)), vdup_n_s16((int16_t)(k->vhi > 32767 ? 32767 : k->vhi)));
    K->am = vcombine_s16(vdup_n_s16((int16_t)k->uand), vdup_n_s16((int16_t)k->vand));
    K->lw = vdup_n_s16((int16_t)k->lw);
    K->zbias = k->zbias; K->zlo = k->zlo; K->fs32 = k->fs; K->amin32 = k->amin;
    K->amode = AMODE == 3 ? k->amode : AMODE; /* 3: from k (the new variants) */
    K->use_foga = FOG && ALPHA && k->foga >= 0;
    K->fogav8 = vdupq_n_u8(K->use_foga ? (uint8_t)k->foga : 0);
    /* (span_grp) The skip is exact for amode 2 too (fa = div255(ta * sa) <= ta) and under fog without a
     * constant alpha, but qemu priced those rows (JRB's water and blended layers, King Whomp's mist: groups
     * that rarely fall under amin all at once) at +1.6-2.3% for the test, and the texel-alpha rows without
     * fog (cut-out trees, fences, sprites) at -2.8 to -3.4%: so only those. */
    K->askip = ALPHA && k->askip && K->amode == 1 && !FOG;
    K->all = vdupq_n_u32(0xFFFFFFFFu);
    const int32x4_t lane = { 0, 1, 2, 3 };
    K->zl = vmulq_s32(lane, vdupq_n_s32(k->zdq));
    K->rl = vmulq_s32(lane, vdupq_n_s32(k->rdq)); K->gl = vmulq_s32(lane, vdupq_n_s32(k->gdq));
    K->bl = vmulq_s32(lane, vdupq_n_s32(k->bdq)); K->al = vmulq_s32(lane, vdupq_n_s32(k->adq));
    K->zs = vdupq_n_s32(4 * k->zdq);
    K->rs = vdupq_n_s32(4 * k->rdq); K->gs = vdupq_n_s32(4 * k->gdq);
    K->bs = vdupq_n_s32(4 * k->bdq); K->as = vdupq_n_s32(4 * k->adq);
    K->dc = (int32x4_t){ k->bdq, k->gdq, k->rdq, k->adq };
    K->dc4 = vshlq_n_s32(K->dc, 2);
}
/* n pixels from idx: the groups, then the masked tail group (n & 3). duf/dvf: the span's texel steps. */
static inline __attribute__((always_inline)) void
span_draw_g(const SpanCM *__restrict k, const SpanK *__restrict K, int idx, int n, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int32_t zq,
          int32_t rq, int32_t gq, int32_t bq, int32_t aq, const int FLAT, const int BILIN, const int ALPHA,
          const int FOG, const int NOZ, const int TO, const int UT, const int AMODE, const int PV) {
    const int32x4_t lane = { 0, 1, 2, 3 };
    const int SKIP = ALPHA && !FOG && (AMODE == 1 || AMODE == 3); /* SM64_ALPHASKIP's rows (span_consts) */
    const uint32_t *const tex = k->tex;
    const int16x8_t lo = K->lo, hi = K->hi, am = K->am; const int16x4_t lw = K->lw;
    const uint32_t zbias = K->zbias, zlo = K->zlo, fs32 = K->fs32, amin32 = K->amin32;
    const int amode = (PV & 8) && AMODE != 3 ? AMODE : K->amode, use_foga = K->use_foga, askip = K->askip; /* (LEAN: known) */
    const uint8x16_t fogav8 = K->fogav8; const uint32x4_t all = K->all;
    /* (vmlaq_s32 with a vdup, not vmlaq_n_s32: GCC takes the _n forms' scalar through a stack slot) */
    int32x4_t uv = vmlaq_s32(vdupq_n_s32(BILIN ? uf - 0x8000 : uf), lane, vdupq_n_s32(duf));
    int32x4_t vv = vmlaq_s32(vdupq_n_s32(BILIN ? vf - 0x8000 : vf), lane, vdupq_n_s32(dvf));
    const int32_t ust = 4 * duf, vst = 4 * dvf;
    int32x4_t zv = vaddq_s32(vdupq_n_s32(zq), K->zl);
    int32x4_t rv = vdupq_n_s32(0), gv = rv, bv = rv, av = rv;
    const int n4 = n & ~3, rem = n & 3;
    if ((PV & 1) && !FLAT) {
        /* SM64_SHADEPX: one vector, the group's first pixel { b, g, r, a } (rv), stepped by four pixels a group; gv: the
         * pixel step. The same 16.16 values lane for lane (integer sums, in any order the same) */
        const int32x4_t dc = K->dc;
        rv = vcombine_s32(vset_lane_s32(gq, vdup_n_s32(bq), 1), vset_lane_s32(aq, vdup_n_s32(rq), 1));
        for (int g = 0; g < n4; g += 4, idx += 4, uv = vaddq_s32(uv, vdupq_n_s32(ust)), vv = vaddq_s32(vv, vdupq_n_s32(vst)),
                             zv = vaddq_s32(zv, K->zs), rv = vaddq_s32(rv, K->dc4)) {
            span_grp(k, tex, idx, uv, vv, zv, rv, dc, bv, av, all, lo, hi, am, lw, zbias, zlo, fs32, amin32, amode, use_foga, fogav8, askip,
                     FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, 0, SKIP, PV);
        }
        if (rem) { /* the tail group from n - 4 (see below): its first pixel's shade */
            const int back = 4 - rem;
            uv = vsubq_s32(uv, vdupq_n_s32(back * duf)); vv = vsubq_s32(vv, vdupq_n_s32(back * dvf));
            zv = vsubq_s32(zv, vdupq_n_s32(back * k->zdq));
            rv = vsubq_s32(rv, vmulq_s32(dc, vdupq_n_s32(back)));
            const uint32x4_t lm = vcgeq_u32(vreinterpretq_u32_s32(lane), vdupq_n_u32((uint32_t)back));
            span_grp(k, tex, idx - back, uv, vv, zv, rv, dc, bv, av, lm, lo, hi, am, lw, zbias, zlo, fs32, amin32, amode, use_foga, fogav8, askip,
                     FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, 1, SKIP, PV);
        }
        return;
    }
    if (!FLAT) {
        rv = vaddq_s32(vdupq_n_s32(rq), K->rl); gv = vaddq_s32(vdupq_n_s32(gq), K->gl);
        bv = vaddq_s32(vdupq_n_s32(bq), K->bl); av = vaddq_s32(vdupq_n_s32(aq), K->al);
    }

    /* round 2's loop, as it was */
    for (int g = 0; g < n4; g += 4, idx += 4, uv = vaddq_s32(uv, vdupq_n_s32(ust)), vv = vaddq_s32(vv, vdupq_n_s32(vst)),
                         zv = vaddq_s32(zv, K->zs)) {
        if (!FLAT && g) {
            rv = vaddq_s32(rv, K->rs); gv = vaddq_s32(gv, K->gs);
            bv = vaddq_s32(bv, K->bs); av = vaddq_s32(av, K->as);
        }
        span_grp(k, tex, idx, uv, vv, zv, rv, gv, bv, av, all, lo, hi, am, lw, zbias, zlo, fs32, amin32, amode, use_foga, fogav8, askip,
                 FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, 0, SKIP, PV & ~1);
    }
    if (rem) {
        /* the tail: the group ending at the span's end, its first 4 - rem lanes drawn already (or,
         * for n < 4, before the span). After the loop uv/vv/zv sit at n4; the colours sit at the
         * last group (n4 - 4) when there was one, else at 0. */
        const int back = 4 - rem;
        uv = vsubq_s32(uv, vdupq_n_s32(back * duf)); vv = vsubq_s32(vv, vdupq_n_s32(back * dvf));
        zv = vsubq_s32(zv, vdupq_n_s32(back * k->zdq));
        if (!FLAT) {
            const int32_t fwd = (n4 ? 4 : 0) - back; /* from the colours' position to n - 4 */
            rv = vaddq_s32(rv, vdupq_n_s32(fwd * k->rdq)); gv = vaddq_s32(gv, vdupq_n_s32(fwd * k->gdq));
            bv = vaddq_s32(bv, vdupq_n_s32(fwd * k->bdq)); av = vaddq_s32(av, vdupq_n_s32(fwd * k->adq));
        }
        const uint32x4_t lm = vcgeq_u32(vreinterpretq_u32_s32(lane), vdupq_n_u32((uint32_t)back));
        span_grp(k, tex, idx - back, uv, vv, zv, rv, gv, bv, av, lm, lo, hi, am, lw, zbias, zlo, fs32, amin32, amode, use_foga, fogav8, askip,
                 FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, 1, SKIP, PV & ~1);
    }
}
/* SM64_LEAN, bilinear: every lane of the span's groups (the masked tail group's too) has its right-hand taps the texels
 * after its left-hand ones and its lower taps the row below, as span_grp's per-group test asks (w0 + 1 == w1 on both
 * axes): then span_grp skips the test. A lane's texel coordinate (int16) is (uf - 0.5 + i * duf) >> 16, linear in the
 * lane's place i, so the span's are the integers between its first lane's and its last's. A repeating axis (AND with
 * size - 1) fails only at a coordinate whose low bits are all ones (the next one at or after a is a | m); a clamped
 * axis (clamp to 0 .. size - 1) only below 0 or at size - 1 and over. Only speed depends on the answer. */
static inline __attribute__((always_inline)) int
span_adj(const SpanCM *k, int n, int32_t uf, int32_t vf, int32_t duf, int32_t dvf) {
    const int p0 = n < 4 ? n - 4 : 0; /* the first lane: the tail group's, when it starts before the span */
    const int32_t u0 = uf - 0x8000 + p0 * duf, u1 = uf - 0x8000 + (n - 1) * duf;
    const int32_t v0 = vf - 0x8000 + p0 * dvf, v1 = vf - 0x8000 + (n - 1) * dvf;
    const int ua = (u0 < u1 ? u0 : u1) >> 16, ub = (u0 < u1 ? u1 : u0) >> 16;
    const int va = (v0 < v1 ? v0 : v1) >> 16, vb = (v0 < v1 ? v1 : v0) >> 16;
    const int uok = k->uand >= 0 ? (ua | k->uand) > ub : ua >= 0 && ub < k->uhi;
    const int vok = k->vand >= 0 ? (va | k->vand) > vb : va >= 0 && vb < k->vhi;
    return uok && vok;
}
static inline __attribute__((always_inline)) void
span_draw(const SpanCM *__restrict k, const SpanK *__restrict K, int idx, int n, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int32_t zq,
          int32_t rq, int32_t gq, int32_t bq, int32_t aq, const int FLAT, const int BILIN, const int ALPHA,
          const int FOG, const int NOZ, const int TO, const int UT, const int AMODE, const int PV) {
    if ((PV & 8) && BILIN && ALPHA && !UT && span_adj(k, n, uf, vf, duf, dvf)) /* (blended rows: qemu priced the opaque ones' at +2-5%) */
        span_draw_g(k, K, idx, n, uf, vf, duf, dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, AMODE, PV | 64);
    else
        span_draw_g(k, K, idx, n, uf, vf, duf, dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, AMODE, PV);
}
static inline __attribute__((always_inline)) void
span_cm4(const SpanCM *k, int idx, int n, int32_t uf, int32_t vf, int32_t zq,
         int32_t rq, int32_t gq, int32_t bq, int32_t aq, const int FLAT, const int BILIN, const int ALPHA, const int AMODE,
         const int FOG, const int NOZ, const int TO, const int UT) {
    SpanK K;
    span_consts(k, &K, ALPHA, AMODE, FOG);
    span_draw(k, &K, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, AMODE, 0);
}
/* The span's texel coordinates stay inside the int16 lanes (see span_cm_neon), and a short span's
 * tail group starts inside the framebuffer. */
static inline __attribute__((always_inline)) int
span_ok(const SpanCM *k, int idx, int n, int32_t uf, int32_t vf, int32_t duf, int32_t dvf) {
    if (n < 4 && idx < 4 - n) return 0;
    /* the usual case without 64-bit maths: a start within +-16000 texels (span_row's clamp) and a
     * step of at most 1000 keep lanes -3..15 within +-31000 (only speed depends on this answer:
     * a refused span is drawn by the scalar loop, the same pixels) */
    if (n <= 16 && (uint32_t)(duf + (1000 << 16)) <= (2000u << 16) && (uint32_t)(dvf + (1000 << 16)) <= (2000u << 16) &&
        (uint32_t)(uf + (16001 << 16)) <= (32002u << 16) && (uint32_t)(vf + (16001 << 16)) <= (32002u << 16)) return 1;
    if (!k->ut) {
        const int64_t first = n & 3 ? n - 4 : 0; /* the tail group's first lane, if it starts before the span */
        const int64_t f0 = first < 0 ? first : 0;
        const int64_t u0 = (int64_t)uf - 0x8000 + f0 * duf, u1 = (int64_t)uf - 0x8000 + (int64_t)(n - 1) * duf;
        const int64_t v0 = (int64_t)vf - 0x8000 + f0 * dvf, v1 = (int64_t)vf - 0x8000 + (int64_t)(n - 1) * dvf;
        const int64_t L = -((int64_t)32000 << 16), H = (int64_t)32000 << 16;
        if (u0 < L || u0 > H || u1 < L || u1 > H || v0 < L || v0 > H || v1 < L || v1 > H) return 0;
    }
    return 1;
}
#undef SPAN_LD
#undef SPAN_BIL_HALF

#define SPAN4(F, B, A, M, FG, NZ, T, U) span_cm4(k, idx, n, uf, vf, zq, rq, gq, bq, aq, F, B, A, M, FG, NZ, T, U)
/* the four flat/bilinear combinations of one variant */
#define SPAN_FB(A, M, FG, NZ, T, U)                                                                  \
    { if (flat) { if (bilin) SPAN4(1, 1, A, M, FG, NZ, T, U); else SPAN4(1, 0, A, M, FG, NZ, T, U); }    \
      else      { if (bilin) SPAN4(0, 1, A, M, FG, NZ, T, U); else SPAN4(0, 0, A, M, FG, NZ, T, U); } }
#define SPAN_CM_ALPHA(F, B)                                                                            \
    switch (k->amode) {                                                                                \
        case 0:  SPAN4(F, B, 1, 0, 0, 0, 0, 0); break;                                                 \
        case 1:  SPAN4(F, B, 1, 1, 0, 0, 0, 0); break;                                                 \
        default: SPAN4(F, B, 1, 2, 0, 0, 0, 0); break;                                                 \
    }
/* n: any length (n & 3 pixels end in a masked group, see the top). The caller advances its own
 * idx/uf/vf/zq/colour by n. Returns 0 (nothing drawn) when a texel coordinate of the span could
 * leave the int16 lanes: the span start is always within +-16000 texels (to_fix16), but a
 * degenerate triangle's step can carry it further; the scalar loop draws those. (Also keeps every
 * uf + i*duf inside int32, as the scalar needs.) Also 0 when a short span's group would start
 * before the framebuffer. */
static int __attribute__((noinline))
span_cm_neon(const SpanCM *k, int flat, int bilin, int alpha, int idx, int n,
             int32_t uf, int32_t vf, int32_t zq, int32_t rq, int32_t gq, int32_t bq, int32_t aq) {
    if (!span_ok(k, idx, n, uf, vf, k->duf, k->dvf)) return 0;
    if (k->ut) { SPAN_FB(0, 0, 0, 0, 0, 1) return 1; } /* untextured, opaque, z-tested (the caller's rule) */
    if (k->noz) { /* no depth: opaque, the skybox; blended (amode from k), the screen wipes (the caller's rules) */
        if (k->to) { if (alpha) SPAN_FB(1, 3, 0, 1, 1, 0) else SPAN_FB(0, 0, 0, 1, 1, 0) } /* the texel alone (SM64_NEONNOTO) */
        else if (alpha) SPAN_FB(1, 3, 0, 1, 0, 0) else SPAN_FB(0, 0, 0, 1, 0, 0)
        return 1;
    }
    if (k->to) { /* the texel alone: any alpha (amode from k), fogged or not */
        if (k->fog) { if (alpha) SPAN_FB(1, 3, 1, 0, 1, 0) else SPAN_FB(0, 0, 1, 0, 1, 0) }
        else        { if (alpha) SPAN_FB(1, 3, 0, 0, 1, 0) else SPAN_FB(0, 0, 0, 0, 1, 0) }
        return 1;
    }
    if (k->fog) { if (alpha) SPAN_FB(1, 3, 1, 0, 0, 0) else SPAN_FB(0, 0, 1, 0, 0, 0) return 1; }
    if (flat) {
        if (bilin) { if (alpha) { SPAN_CM_ALPHA(1, 1) } else SPAN4(1, 1, 0, 0, 0, 0, 0, 0); }
        else       { if (alpha) { SPAN_CM_ALPHA(1, 0) } else SPAN4(1, 0, 0, 0, 0, 0, 0, 0); }
    } else {
        if (bilin) { if (alpha) { SPAN_CM_ALPHA(0, 1) } else SPAN4(0, 1, 0, 0, 0, 0, 0, 0); }
        else       { if (alpha) { SPAN_CM_ALPHA(0, 0) } else SPAN4(0, 0, 0, 0, 0, 0, 0, 0); }
    }
    return 1;
}
#undef SPAN_CM_ALPHA
#undef SPAN_FB
#undef SPAN4

/* M5 round 1: a whole row per call. raster_tri's span loop called span_cm_neon for every span
 * (most rows are one span: ~11 px on a square output), and each call paid the variant dispatch, the range
 * check and the constants, with the span's setup spilled around it: ~17% of JRB's instructions
 * outside the pixels. span_row runs raster_tri's span loop itself -- the same float steps in the
 * same order (-ffp-contract=off), the same NEON conversions as its span setup -- with the
 * variant picked once per triangle (span_row_pick) and the constants built once per row. Every
 * pixel goes to NEON: a span's last n & 3 pixels as the masked tail group, 1 included (the
 * caller's tail threshold drew a lone pixel with the scalar loop; same pixel either way). A span
 * span_ok refuses returns to the caller at its start, with the row's float state there, and
 * raster_tri's own loop carries on from it. */
#define SPAN_ROW_PERSP 16 /* raster_tri's PERSP_SPAN (checked there) */
typedef struct {
    float32x4_t m;                        /* { ftw, ftw, fth, fth } */
    float32x4_t da, db;                   /* SM64_SPANVEC: { zdx, crdx, cgdx, cbdx }, { cadx, uwdx, vwdx, swdx } */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx, ftw, fth;
    int persp;
} SpanRowT;
typedef struct { float zf, cr, cg, cb, ca, uw, vw, sw, rw; } SpanRowF;
typedef int (*SpanRowFn)(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n);

static inline __attribute__((always_inline)) int
span_row(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n, const int FLAT, const int BILIN,
         const int ALPHA, const int AMODE, const int FOG, const int NOZ, const int TO, const int UT) {
    /* the constants once per triangle (M5 round 5): each row rebuilt and spilled them, ~70
     * instructions a row. The variant, and so ALPHA/AMODE/FOG, is fixed for the triangle. */
    SpanK *const K = (SpanK *)k->kc;
    if (!k->kc_ok) { span_consts(k, K, ALPHA, AMODE, FOG); ((SpanCM *)k)->kc_ok = 1; }
    float zf = f->zf, cr = f->cr, cg = f->cg, cb = f->cb, ca = f->ca, uw = f->uw, vw = f->vw, sw = f->sw, rw = f->rw;
    int x = 0, rw_stale = 0;
    while (x < n) {
        int span = n - x;
        if (span > SPAN_ROW_PERSP) span = SPAN_ROW_PERSP;
        const float fspan = (float)span;
        const float sw_b = sw + t->swdx * fspan;
        if (NOZ && k->skym && span >= 4) {
            /* the deferred sky: a span whose pixels are all drawn already (alpha != 0) draws nothing,
             * so skip its setup; the row state steps on as below, and 1/w (the next span's start
             * reciprocal, carried) is taken fresh from sw when a span needs it -- the same divide */
            const uint32_t *const fb = k->fbuf + idx;
            uint32x4_t m = vld1q_u32(fb + span - 4);
            for (int i = 0; i < span - 4; i += 4) m = vminq_u32(m, vld1q_u32(fb + i));
            const uint32x2_t m2 = vmin_u32(vget_low_u32(m), vget_high_u32(m));
            if (vget_lane_u32(vpmin_u32(m2, m2), 0) >> 24) {
                zf += t->zdx * fspan;
                cr += t->crdx * fspan; cg += t->cgdx * fspan; cb += t->cbdx * fspan; ca += t->cadx * fspan;
                uw += t->uwdx * fspan;
                vw += t->vwdx * fspan;
                sw = sw_b; rw_stale = 1;
                x += span; idx += span;
                continue;
            }
        }
        if (NOZ && rw_stale) { rw = t->persp && sw > 1e-12f ? 1.0f / sw : 0.0f; rw_stale = 0; }
        float rw_b = 0.0f;
        int32_t uf = 0, vf = 0, duf = 0, dvf = 0;
        if (!UT) { /* textured: raster_tri's u, v at the span's ends, then its fixed-point setup */
            float u_a, v_a, du, dv;
            if (t->persp) {
                rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
                u_a = uw * rw; v_a = vw * rw;
                const float nu = (uw + t->uwdx * fspan) * rw_b - u_a;
                const float nv = (vw + t->vwdx * fspan) * rw_b - v_a;
                if (span == SPAN_ROW_PERSP) { du = nu * (1.0f / SPAN_ROW_PERSP); dv = nv * (1.0f / SPAN_ROW_PERSP); }
                else                        { du = nu / fspan;                   dv = nv / fspan; }
            } else {
                u_a = uw; v_a = vw; du = t->uwdx; dv = t->vwdx;
            }
            /* (lanes set in registers: an initializer list went through the stack) */
            const float32x4_t a = vcombine_f32(vset_lane_f32(du, vdup_n_f32(u_a), 1), vset_lane_f32(dv, vdup_n_f32(v_a), 1));
            const float32x4_t m = t->m;
            const float32x4_t c = vmaxq_f32(vminq_f32(vmulq_f32(a, m), vdupq_n_f32(16000.0f)), vdupq_n_f32(-16000.0f));
            const int32x4_t o = vcvtq_s32_f32(vmulq_f32(c, vdupq_n_f32(65536.0f)));
            uf = vgetq_lane_s32(o, 0); duf = vgetq_lane_s32(o, 1); vf = vgetq_lane_s32(o, 2); dvf = vgetq_lane_s32(o, 3);
        }
        if (!span_ok(k, idx, span, uf, vf, duf, dvf)) break; /* raster_tri draws it (and the rest) */
        int32_t rq = 0, gq = 0, bq = 0, aq = 0;
        if (!FLAT) {
            const float32x4_t cc = vcombine_f32(vset_lane_f32(cg, vdup_n_f32(cr), 1), vset_lane_f32(ca, vdup_n_f32(cb), 1));
            const int32x4_t oc = vcvtq_s32_f32(vmulq_f32(cc, vdupq_n_f32(65536.0f)));
            rq = vgetq_lane_s32(oc, 0); gq = vgetq_lane_s32(oc, 1); bq = vgetq_lane_s32(oc, 2); aq = vgetq_lane_s32(oc, 3);
        }
        const float32x2_t zc = vmax_f32(vmin_f32(vdup_n_f32(zf), vdup_n_f32(2.0f)), vdup_n_f32(-1.0f));
        const int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_f32(zc, vdup_n_f32(65535.0f * 256.0f))), 0);
        span_draw(k, K, idx, span, uf, vf, duf, dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, AMODE, 0);
        zf += t->zdx * fspan;
        cr += t->crdx * fspan; cg += t->cgdx * fspan; cb += t->cbdx * fspan; ca += t->cadx * fspan;
        uw += t->uwdx * fspan;
        vw += t->vwdx * fspan;
        sw = sw_b; rw = rw_b;
        x += span; idx += span;
    }
    if (x < n) { f->zf = zf; f->cr = cr; f->cg = cg; f->cb = cb; f->ca = ca; f->uw = uw; f->vw = vw; f->sw = sw; f->rw = rw; }
    return x;
}
/* SM64_SPANVEC (2026-10-06, branch sm64-speed-2026-10-06; default 1): span_row with the row's float state as two NEON
 * vectors, A = { z, r, g, b } and B = { a, u/w, v/w, 1/w }, as SpanRowF lays them out. The scalar row kept nine floats
 * and nine steps that GCC spilled around every span (~40 loads, stores and moves a span, beside the 18 multiply-adds).
 * The same operations in the same order, lane by lane: the next span's start is A + dA * span and B + dB * span
 * (vmla: the product rounded, then the sum, as the VFP's scalar multiply and add round them); the perspective ends,
 * u/w * (1/w) and ((u/w)' * (1/w)' - u), and the 1/16 step, as two-lane multiplies and a subtract; the divides (the
 * span's 1/w and a short span's step) stay scalar VFP divides. NEON flushes denormals to zero where the VFP keeps
 * them: no value here comes near one (screen and texture coordinates, 1/w, colours 0..255, depth 0..1 and their
 * per-pixel steps are each 0 or far above 1e-30). Same pixels: the sweeps and the bench views, ARM == x86. */
static inline __attribute__((always_inline)) int
span_row_v(const SpanCM *__restrict k, const SpanRowT *__restrict t, SpanRowF *__restrict f, int idx, int n, const int FLAT, const int BILIN,
           const int ALPHA, const int AMODE, const int FOG, const int NOZ, const int TO, const int UT, const int PV) {
    SpanK *const K = (SpanK *)k->kc;
    if (!k->kc_ok) { span_consts(k, K, ALPHA, AMODE, FOG); ((SpanCM *)k)->kc_ok = 1; }
    float32x4_t A = vld1q_f32(&f->zf), B = vld1q_f32(&f->ca);
    const float32x4_t DA = t->da, DB = t->db;
    float rw = f->rw;
    int x = 0, rw_stale = 0;
    while (x < n) {
        int span = n - x;
        if (span > SPAN_ROW_PERSP) span = SPAN_ROW_PERSP;
        const float fspan = (float)span;
        const float32x4_t fs = vdupq_n_f32(fspan);
        const float32x4_t An = vmlaq_f32(A, DA, fs), Bn = vmlaq_f32(B, DB, fs); /* the next span's start */
        const float sw_b = vgetq_lane_f32(Bn, 3);
        if (NOZ && k->skym && span >= 4) { /* the deferred sky: as span_row */
            const uint32_t *const fb = k->fbuf + idx;
            uint32x4_t m = vld1q_u32(fb + span - 4);
            for (int i = 0; i < span - 4; i += 4) m = vminq_u32(m, vld1q_u32(fb + i));
            const uint32x2_t m2 = vmin_u32(vget_low_u32(m), vget_high_u32(m));
            if (vget_lane_u32(vpmin_u32(m2, m2), 0) >> 24) {
                A = An; B = Bn; rw_stale = 1;
                x += span; idx += span;
                continue;
            }
        }
        if (!NOZ && k->zskip > 0 && span >= 4) {
            const float32x2_t zc = vmax_f32(vmin_f32(vdup_lane_f32(vget_low_f32(A), 0), vdup_n_f32(2.0f)), vdup_n_f32(-1.0f));
            const int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_f32(zc, vdup_n_f32(65535.0f * 256.0f))), 0); /* as below */
            /* SM64_ZSKIP: the span behind the depth buffer all along -- its nearest depth (z is linear along the span,
             * so at one of its ends) further than its farthest stored depth plus the decal window -- fails the depth
             * test at every pixel, so every group of it writes nothing: skip its setup and its groups. A sufficient
             * test, never a guess: any other span is drawn as before. */
            const uint16_t *const zp = k->zb + idx;
            const int uz0 = clamp16_sh8(zq), uz1 = clamp16_sh8(zq + (span - 1) * k->zdq);
            uint16x4_t m4;
            if (span >= 8) { const uint16x8_t m8 = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8)); m4 = vmax_u16(vget_low_u16(m8), vget_high_u16(m8)); }
            else m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));
            m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);
            const int ozb = (int)vget_lane_u16(m4, 0) + (int)k->zbias;
            if ((uz0 < uz1 ? uz0 : uz1) > (ozb > 65535 ? 65535 : ozb)) {
                ((SpanCM *)k)->zskip = 4;
                A = An; B = Bn; rw = t->persp && sw_b > 1e-12f ? 1.0f / sw_b : 0.0f; /* the next span's start 1/w, as drawn */
                x += span; idx += span;
                continue;
            }
            ((SpanCM *)k)->zskip--; /* the triangle's spans in front: after four misses running it stops testing (speed only) */
        }
        if (NOZ && rw_stale) { const float sw = vgetq_lane_f32(B, 3); rw = t->persp && sw > 1e-12f ? 1.0f / sw : 0.0f; rw_stale = 0; }
        float rw_b = 0.0f;
        int32_t uf = 0, vf = 0, duf = 0, dvf = 0;
        if (!UT) {
            const float32x2_t uv = vget_low_f32(vextq_f32(B, B, 1)); /* { u/w, v/w } */
            float32x2_t ua, du; /* { u_a, v_a }, { du, dv } */
            if (t->persp) {
                rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
                ua = vmul_f32(uv, vdup_n_f32(rw));
                const float32x2_t nuv = vsub_f32(vmul_f32(vget_low_f32(vextq_f32(Bn, Bn, 1)), vdup_n_f32(rw_b)), ua);
                if (span == SPAN_ROW_PERSP) du = vmul_f32(nuv, vdup_n_f32(1.0f / SPAN_ROW_PERSP));
                else du = vset_lane_f32(vget_lane_f32(nuv, 1) / fspan, vdup_n_f32(vget_lane_f32(nuv, 0) / fspan), 1);
            } else {
                ua = uv; du = vget_low_f32(vextq_f32(DB, DB, 1));
            }
            const float32x2x2_t z = vzip_f32(ua, du);
            const float32x4_t a = vcombine_f32(z.val[0], z.val[1]); /* { u_a, du, v_a, dv } */
            const float32x4_t c = vmaxq_f32(vminq_f32(vmulq_f32(a, t->m), vdupq_n_f32(16000.0f)), vdupq_n_f32(-16000.0f));
            /* (int32_t)(c * 65536.0f) as one fixed-point conversion: the scaling by 2^16 is exact, then the same
             * truncation and saturation */
            const int32x4_t o = vcvtq_n_s32_f32(c, 16);
            uf = vgetq_lane_s32(o, 0); duf = vgetq_lane_s32(o, 1); vf = vgetq_lane_s32(o, 2); dvf = vgetq_lane_s32(o, 3);
            /* span_ok's usual case for the four at once: (uint32_t)(x + B) <= 2B per lane (B = 16001 << 16 for u, v;
             * 1000 << 16 for their steps); else span_ok itself decides */
            static const uint32_t okb[4] = { 16001u << 16, 1000u << 16, 16001u << 16, 1000u << 16 };
            static const uint32_t okl[4] = { 32002u << 16, 2000u << 16, 32002u << 16, 2000u << 16 };
            const uint32x4_t okv = vcleq_u32(vaddq_u32(vreinterpretq_u32_s32(o), vld1q_u32(okb)), vld1q_u32(okl));
            const uint32x2_t ok2 = vpmin_u32(vget_low_u32(okv), vget_high_u32(okv));
            if ((span < 4 && idx < 4 - span) || (vget_lane_u32(vpmin_u32(ok2, ok2), 0) != 0xFFFFFFFFu && !span_ok(k, idx, span, uf, vf, duf, dvf)))
                break; /* raster_tri draws it (and the rest) */
        } else if (!span_ok(k, idx, span, uf, vf, duf, dvf)) break;
        int32_t rq = 0, gq = 0, bq = 0, aq = 0;
        if (!FLAT) {
            const int32x4_t oc = vcvtq_n_s32_f32(vextq_f32(A, B, 1), 16); /* { r, g, b, a }: (int32_t)(c * 65536.0f) */
            rq = vgetq_lane_s32(oc, 0); gq = vgetq_lane_s32(oc, 1); bq = vgetq_lane_s32(oc, 2); aq = vgetq_lane_s32(oc, 3);
        }
        const float32x2_t zc = vmax_f32(vmin_f32(vdup_lane_f32(vget_low_f32(A), 0), vdup_n_f32(2.0f)), vdup_n_f32(-1.0f));
        const int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_f32(zc, vdup_n_f32(65535.0f * 256.0f))), 0);
        span_draw(k, K, idx, span, uf, vf, duf, dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, FOG, NOZ, TO, UT, AMODE, PV);
        A = An; B = Bn; rw = rw_b;
        x += span; idx += span;
    }
    if (x < n) { vst1q_f32(&f->zf, A); vst1q_f32(&f->ca, B); f->rw = rw; }
    return x;
}
#ifdef SPAN_ROWS_LEAN_TU
/* (gfx_soft_rows_lean.c: SM64_LEAN's rows in a file of their own, so neither compile needs all the row variants at once) */
/* SM64_LEAN's rows (PV 8; with SM64_FULLST, PV 16 too), for the default variants only: flat _v/_vr, smooth _p/_pr
 * (l: lean and fullst; lo: lean alone, which the opaque kinds need for SM64_FULLST=0) */
/* (in three files, gfx_soft_rows_lean{1,2,3}.c, one part each (SPAN_ROWS_LEAN_TU): each declares the others' rows) */
#define SPAN_ROW_FNL_DEF(NAME, F, B, A, M, FG, NZ, T, U, PV, SFX)                                      \
    int __attribute__((noinline)) NAME##SFX(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) { \
        return span_row_v(k, t, f, idx, n, F, B, A, M, FG, NZ, T, U, PV); }
#define SPAN_ROW_FNL_DECL(NAME, F, B, A, M, FG, NZ, T, U, PV, SFX)                                     \
    int NAME##SFX(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n);
#define SPAN_ROW_FNL(NAME, F, B, A, M, FG, NZ, T, U, PV, SFX) SPAN_ROW_FNL_PART(NAME, F, B, A, M, FG, NZ, T, U, PV, SFX)
#if SPAN_ROWS_LEAN_TU == 1
#define SPAN_ROW_FNL_PART SPAN_ROW_FNL_DEF
#else
#define SPAN_ROW_FNL_PART SPAN_ROW_FNL_DECL
#endif
#define SPAN_ROW_FBL(P, A, M, FG, NZ, T, U, X)                                                         \
    SPAN_ROW_FNL(P##_f1b1, 1, 1, A, M, FG, NZ, T, U, 0 | X, _vl) SPAN_ROW_FNL(P##_f1b1, 1, 1, A, M, FG, NZ, T, U, 4 | X, _vrl) \
    SPAN_ROW_FNL(P##_f1b0, 1, 0, A, M, FG, NZ, T, U, 0 | X, _vl) SPAN_ROW_FNL(P##_f1b0, 1, 0, A, M, FG, NZ, T, U, 4 | X, _vrl) \
    SPAN_ROW_FNL(P##_f0b1, 0, 1, A, M, FG, NZ, T, U, 1 | X, _pl) SPAN_ROW_FNL(P##_f0b1, 0, 1, A, M, FG, NZ, T, U, 5 | X, _prl) \
    SPAN_ROW_FNL(P##_f0b0, 0, 0, A, M, FG, NZ, T, U, 1 | X, _pl) SPAN_ROW_FNL(P##_f0b0, 0, 0, A, M, FG, NZ, T, U, 5 | X, _prl)
#define SPAN_ROW_FBLO(P, A, M, FG, NZ, T, U)                                                           \
    SPAN_ROW_FNL(P##_f1b1, 1, 1, A, M, FG, NZ, T, U, 8, _vlo) SPAN_ROW_FNL(P##_f1b1, 1, 1, A, M, FG, NZ, T, U, 12, _vrlo) \
    SPAN_ROW_FNL(P##_f1b0, 1, 0, A, M, FG, NZ, T, U, 8, _vlo) SPAN_ROW_FNL(P##_f1b0, 1, 0, A, M, FG, NZ, T, U, 12, _vrlo) \
    SPAN_ROW_FNL(P##_f0b1, 0, 1, A, M, FG, NZ, T, U, 9, _plo) SPAN_ROW_FNL(P##_f0b1, 0, 1, A, M, FG, NZ, T, U, 13, _prlo) \
    SPAN_ROW_FNL(P##_f0b0, 0, 0, A, M, FG, NZ, T, U, 9, _plo) SPAN_ROW_FNL(P##_f0b0, 0, 0, A, M, FG, NZ, T, U, 13, _prlo)
#define SPAN_ROW_LEAN_A(P, A, M, FG, NZ, T, U) SPAN_ROW_FBL(P, A, M, FG, NZ, T, U, 8)
#define SPAN_ROW_LEAN_O(P, A, M, FG, NZ, T, U) SPAN_ROW_FBL(P, A, M, FG, NZ, T, U, 24) SPAN_ROW_FBLO(P, A, M, FG, NZ, T, U)
/* the z-tested textured kinds: blended (A) and opaque (O) */
SPAN_ROW_LEAN_A(srow_to_fog_a, 1, 3, 1, 0, 1, 0) SPAN_ROW_LEAN_A(srow_to_a, 1, 3, 0, 0, 1, 0)
SPAN_ROW_LEAN_A(srow_fog_a, 1, 3, 1, 0, 0, 0) SPAN_ROW_LEAN_A(srow_cm_a0, 1, 0, 0, 0, 0, 0)
SPAN_ROW_LEAN_A(srow_cm_a1, 1, 1, 0, 0, 0, 0) SPAN_ROW_LEAN_A(srow_cm_a2, 1, 2, 0, 0, 0, 0)
#undef SPAN_ROW_FNL_PART
#if SPAN_ROWS_LEAN_TU == 2
#define SPAN_ROW_FNL_PART SPAN_ROW_FNL_DEF
#else
#define SPAN_ROW_FNL_PART SPAN_ROW_FNL_DECL
#endif
SPAN_ROW_LEAN_O(srow_to_fog, 0, 0, 1, 0, 1, 0) SPAN_ROW_LEAN_O(srow_to, 0, 0, 0, 0, 1, 0)
SPAN_ROW_LEAN_O(srow_fog, 0, 0, 1, 0, 0, 0) SPAN_ROW_LEAN_O(srow_cm, 0, 0, 0, 0, 0, 0)
#undef SPAN_ROW_FNL_PART
#if SPAN_ROWS_LEAN_TU == 3
#define SPAN_ROW_FNL_PART SPAN_ROW_FNL_DEF
#else
#define SPAN_ROW_FNL_PART SPAN_ROW_FNL_DECL
#endif
/* blended rows that write depth (lz): the texel alone, fogged or not (JRB's water, the dust's cutouts) */
#define SPAN_ROW_LEAN_Z(P, A, M, FG, NZ, T, U)                                                         \
    SPAN_ROW_FNL(P##_f1b1, 1, 1, A, M, FG, NZ, T, U, 40, _vlz) SPAN_ROW_FNL(P##_f1b1, 1, 1, A, M, FG, NZ, T, U, 44, _vrlz) \
    SPAN_ROW_FNL(P##_f1b0, 1, 0, A, M, FG, NZ, T, U, 40, _vlz) SPAN_ROW_FNL(P##_f1b0, 1, 0, A, M, FG, NZ, T, U, 44, _vrlz) \
    SPAN_ROW_FNL(P##_f0b1, 0, 1, A, M, FG, NZ, T, U, 41, _plz) SPAN_ROW_FNL(P##_f0b1, 0, 1, A, M, FG, NZ, T, U, 45, _prlz) \
    SPAN_ROW_FNL(P##_f0b0, 0, 0, A, M, FG, NZ, T, U, 41, _plz) SPAN_ROW_FNL(P##_f0b0, 0, 0, A, M, FG, NZ, T, U, 45, _prlz)
SPAN_ROW_LEAN_Z(srow_to_fog_a, 1, 3, 1, 0, 1, 0) SPAN_ROW_LEAN_Z(srow_to_a, 1, 3, 0, 0, 1, 0)
#undef SPAN_ROW_LEAN_Z
#define SPAN_ROW_SELL(P, S) (rr ? (flat ? (bilin ? P##_f1b1_vr##S : P##_f1b0_vr##S) : (bilin ? P##_f0b1_pr##S : P##_f0b0_pr##S)) \
                             : (flat ? (bilin ? P##_f1b1_v##S : P##_f1b0_v##S) : (bilin ? P##_f0b1_p##S : P##_f0b0_p##S)))
#define SPAN_ROW_SELO(P) (lean == 2 ? SPAN_ROW_SELL(P, l) : SPAN_ROW_SELL(P, lo))
#undef SPAN_ROW_FNL_PART
#if SPAN_ROWS_LEAN_TU == 1
/* span_row_pick's choice for a triangle it found to have the settings these rows assume (lean 2: with SM64_FULLST) */
SpanRowFn span_row_pick_lean(const SpanCM *k, int flat, int bilin, int alpha, int rr, int lean) {
    if (alpha && k->dmask) return !k->to ? NULL : k->fog ? SPAN_ROW_SELL(srow_to_fog_a, lz) : SPAN_ROW_SELL(srow_to_a, lz);
    if (k->to) return k->fog ? (alpha ? SPAN_ROW_SELL(srow_to_fog_a, l) : SPAN_ROW_SELO(srow_to_fog))
                             : (alpha ? SPAN_ROW_SELL(srow_to_a, l) : SPAN_ROW_SELO(srow_to));
    if (k->fog) return alpha ? SPAN_ROW_SELL(srow_fog_a, l) : SPAN_ROW_SELO(srow_fog);
    if (!alpha) return SPAN_ROW_SELO(srow_cm);
    return k->amode == 0 ? SPAN_ROW_SELL(srow_cm_a0, l) : k->amode == 1 ? SPAN_ROW_SELL(srow_cm_a1, l) : SPAN_ROW_SELL(srow_cm_a2, l);
}
#endif
#undef SPAN_ROW_FNL_DEF
#undef SPAN_ROW_FNL_DECL
#undef SPAN_ROW_SELL
#undef SPAN_ROW_SELO
#undef SPAN_ROW_LEAN_A
#undef SPAN_ROW_LEAN_O
#undef SPAN_ROW_FBL
#undef SPAN_ROW_FBLO
#undef SPAN_ROW_FNL
#else
SpanRowFn span_row_pick_lean(const SpanCM *k, int flat, int bilin, int alpha, int rr, int lean); /* gfx_soft_rows_lean.c */
/* one noinline function per variant, named by its flags (NAME##_v: span_row_v) */
#define SPAN_ROW_FN(NAME, F, B, A, M, FG, NZ, T, U)                                                    \
    static int __attribute__((noinline)) NAME(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) { \
        return span_row(k, t, f, idx, n, F, B, A, M, FG, NZ, T, U); }                                  \
    static int __attribute__((noinline)) NAME##_v(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) { \
        return span_row_v(k, t, f, idx, n, F, B, A, M, FG, NZ, T, U, 0); }                            \
    static int __attribute__((noinline)) NAME##_p(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) { \
        return span_row_v(k, t, f, idx, n, F, B, A, M, FG, NZ, T, U, 1); }                            \
    static int __attribute__((noinline)) NAME##_vr(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) { \
        return span_row_v(k, t, f, idx, n, F, B, A, M, FG, NZ, T, U, 4); }                            \
    static int __attribute__((noinline)) NAME##_pr(const SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) { \
        return span_row_v(k, t, f, idx, n, F, B, A, M, FG, NZ, T, U, 5); }
#define SPAN_ROW_FB(P, A, M, FG, NZ, T, U) \
    SPAN_ROW_FN(P##_f1b1, 1, 1, A, M, FG, NZ, T, U) SPAN_ROW_FN(P##_f1b0, 1, 0, A, M, FG, NZ, T, U) \
    SPAN_ROW_FN(P##_f0b1, 0, 1, A, M, FG, NZ, T, U) SPAN_ROW_FN(P##_f0b0, 0, 0, A, M, FG, NZ, T, U)
/* the row kinds: name, ALPHA, AMODE, FOG, NOZ, TO, UT */
#define SPAN_ROW_KINDS(X)                                                                                  \
    X(srow_ut, 0, 0, 0, 0, 0, 1) X(srow_noz, 0, 0, 0, 1, 0, 0) X(srow_noz_a, 1, 3, 0, 1, 0, 0)             \
    X(srow_noz_to, 0, 0, 0, 1, 1, 0) X(srow_noz_to_a, 1, 3, 0, 1, 1, 0)                                    \
    X(srow_to_fog_a, 1, 3, 1, 0, 1, 0) X(srow_to_fog, 0, 0, 1, 0, 1, 0) X(srow_to_a, 1, 3, 0, 0, 1, 0)     \
    X(srow_to, 0, 0, 0, 0, 1, 0) X(srow_fog_a, 1, 3, 1, 0, 0, 0) X(srow_fog, 0, 0, 1, 0, 0, 0)             \
    X(srow_cm_a0, 1, 0, 0, 0, 0, 0) X(srow_cm_a1, 1, 1, 0, 0, 0, 0) X(srow_cm_a2, 1, 2, 0, 0, 0, 0)        \
    X(srow_cm, 0, 0, 0, 0, 0, 0)
SPAN_ROW_KINDS(SPAN_ROW_FB)
#define SPAN_ROW_SEL(P) (rr ? (vec == 2 ? (flat ? (bilin ? P##_f1b1_vr : P##_f1b0_vr) : (bilin ? P##_f0b1_pr : P##_f0b0_pr)) \
                                     : (flat ? (bilin ? P##_f1b1_vr : P##_f1b0_vr) : (bilin ? P##_f0b1_vr : P##_f0b0_vr))) \
                     : vec == 2 ? (flat ? (bilin ? P##_f1b1_v : P##_f1b0_v) : (bilin ? P##_f0b1_p : P##_f0b0_p)) \
                     : vec ? (flat ? (bilin ? P##_f1b1_v : P##_f1b0_v) : (bilin ? P##_f0b1_v : P##_f0b0_v)) \
                             : (flat ? (bilin ? P##_f1b1 : P##_f1b0) : (bilin ? P##_f0b1 : P##_f0b0)))
/* span_cm_neon's dispatch, once per triangle (vec: SM64_SPANVEC's span_row_v; 2: and SM64_SHADEPX's smooth shade;
 * rr: SM64_WRAPRR, a texture repeating on both axes, with vec only; lean: SM64_LEAN's rows, 2: with SM64_FULLST,
 * for the default variants of the z-tested textured kinds when the triangle has the settings they assume) */
static SpanRowFn span_row_pick(const SpanCM *k, int flat, int bilin, int alpha, int vec, int rr, int lean) {
    rr = rr && vec && !k->ut;
    if (lean && !k->ut && !k->noz && (vec == 2 || (vec && flat)) && k->pair == 2 && k->zlo >= 0x10000u &&
        (alpha ? !k->noblend && (!k->dmask || (k->to && k->tidb != NULL)) : k->dmask && k->tidb != NULL))
        return span_row_pick_lean(k, flat, bilin, alpha, rr, lean);
    if (k->ut) return SPAN_ROW_SEL(srow_ut);
    if (k->noz) return k->to ? (alpha ? SPAN_ROW_SEL(srow_noz_to_a) : SPAN_ROW_SEL(srow_noz_to))
                             : (alpha ? SPAN_ROW_SEL(srow_noz_a) : SPAN_ROW_SEL(srow_noz));
    if (k->to) return k->fog ? (alpha ? SPAN_ROW_SEL(srow_to_fog_a) : SPAN_ROW_SEL(srow_to_fog))
                             : (alpha ? SPAN_ROW_SEL(srow_to_a) : SPAN_ROW_SEL(srow_to));
    if (k->fog) return alpha ? SPAN_ROW_SEL(srow_fog_a) : SPAN_ROW_SEL(srow_fog);
    if (!alpha) return SPAN_ROW_SEL(srow_cm);
    return k->amode == 0 ? SPAN_ROW_SEL(srow_cm_a0) : k->amode == 1 ? SPAN_ROW_SEL(srow_cm_a1) : SPAN_ROW_SEL(srow_cm_a2);
}
#undef SPAN_ROW_SEL
#undef SPAN_ROW_FB
#undef SPAN_ROW_FN
#undef SPAN_ROW_KINDS
#endif /* SPAN_ROWS_LEAN_TU */
