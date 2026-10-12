/* The race's hot pixel loop, four pixels at a time (round 27).
 *
 * Covers SPAN_LOOP_CM with TEX=1, ZTEST=1, CM=1 (z-tested texture x shade,
 * ~62% of DK's span pixels at 100% 3D RES), and CM=2 blended (the particle
 * lerp: div255(t*s + y*(255 - t)), y's alpha byte 0 so byte 3 stays
 * div255(ta*sa); t*s + y*(255-t) <= 65025, exact in u16), and CM=3 opaque (texture x shade,
 * then fogged by the shade alpha: div255(p*(255 - f) + fog*f)) for the 8888 power-of-two textures
 * every race samples (TEX32=2), nearest or bilinear, flat or smooth shade,
 * with or without alpha. Bit-identical to the scalar loop in gfx_soft.c:
 *  - z: clamp16_sh8 is a saturating narrow (vqshrun #8); the scalar's pass,
 *    uz <= zb + zbias, is (uz -sat zbias) <= zb in u16 lanes.
 *  - shade: clamp8_sh16 = vqshrun #16, then a saturating narrow to u8.
 *  - bilinear: lerp2 is floor((a*(256-f) + b*f) / 256) per 8-bit channel; the
 *    sums stay <= 255*256, so 16-bit lanes do it exactly, in the same order
 *    (u, then v), per pixel with its weights as a lane (round 28: was spread
 *    across q registers, which spilled). Round 28 also loads each row's two
 *    taps as one 8-byte pair when x0 + 1 neither wraps nor clamps, on all four
 *    pixels (most groups; else lane loads).
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
 * The cross-port speed round (2026-10-07, from the SM64 port e914b1a; gfx_soft.c MK64_FASTROWS): the same groups
 * (span_grp4) also run whole rows (span_row / span_row_v at the end), a span's last n & 3 pixels as one masked
 * group; MK64_SPANVEC, MK64_SHADEPX, MK64_ZSKIP and MK64_WRAPRR live in those rows. Same pixels. */
#pragma once
#include <stdint.h>
#include <arm_neon.h>

typedef struct {
    uint32_t *fbuf; uint16_t *zb;
    const uint32_t *tex; int lw;          /* 8888 texels, width 1 << lw */
    int32_t ulo, uhi, uand, vlo, vhi, vand;
    uint32_t zbias; int dmask;
    int32_t duf, dvf, zdq, rdq, gdq, bdq, adq;
    uint32_t fs;                          /* flat shade as a pixel: b | g<<8 | r<<16 | a<<24 */
    uint32_t amin;
    uint32_t ablend;                      /* 0: TEX_EDGE past amin written unblended (X12): fa taken as 255 */
    int amode;                            /* fa = 0: shade a, 1: texel a, 2: div255(ta*sa) */
    uint32_t ly;                          /* CM=2 lerp colour at texel 0: b | g<<8 | r<<16 (alpha byte 0) */
    uint32_t fogc;                        /* CM=3 fog colour: b | g<<8 | r<<16 (alpha byte 0) */
    int zskip;                            /* MK64_ZSKIP: span_row_v tests spans against the depth buffer while > 0 */
    const AlphaRows *arows; int ws, wt;   /* MK64_FASTROWS' rows: the sprite span skip (gfx_soft.c span_clear), or NULL */
} SpanCM;

static inline __attribute__((always_inline)) int32_t span_wrap(int32_t c, int32_t lo, int32_t hi, int32_t m) {
    c = c < lo ? lo : (c > hi ? hi : c);
    return c & m;
}
static inline __attribute__((always_inline)) uint16x8_t span_div255(uint16x8_t x) {
    return vshrq_n_u16(vaddq_u16(vsraq_n_u16(x, x, 8), vdupq_n_u16(1)), 8);
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
#define SPAN_LD(dst, ix, L, D) dst = vld1_lane_u32(tex + vget_lane_u16(ix, L), dst, D);
/* Bilinear, one pixel P of the group from its two texel pairs: AB = the top row's
 * [a | b] (x0, x0 + 1), CD = the bottom row's [c | d]. lerp2 along u (the pixel's
 * fu/gu = 256 - fu, lane P), then v: the scalar bilerp's order. u16 lanes hold
 * every product (x * (256 - f) + y * f <= 255 * 256). */
#define SPAN_BIL_PX(OUT, AB, CD, P)                                                                              \
    {   const uint16x8_t tw_ = vmovl_u8(vreinterpret_u8_u32(AB)), bw_ = vmovl_u8(vreinterpret_u8_u32(CD));      \
        const uint16x4_t t_ = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(tw_), gu4, P), vget_high_u16(tw_), fu4, P), 8); \
        const uint16x4_t b_ = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(vget_low_u16(bw_), gu4, P), vget_high_u16(bw_), fu4, P), 8); \
        OUT = vshr_n_u16(vmla_lane_u16(vmul_lane_u16(t_, gv4, P), b_, fv4, P), 8); }
/* a pair from one 8-byte load (x0 + 1 is the next texel of the row) or from two */
#define SPAN_PAIR2(AB, CD, P) AB = vld1_u32(tex + vget_lane_u16(i00, P)); CD = vld1_u32(tex + vget_lane_u16(i10, P));
#define SPAN_PAIR4(AB, CD, P)                                                                                    \
    AB = vld1_lane_u32(tex + vget_lane_u16(i01, P), vld1_dup_u32(tex + vget_lane_u16(i00, P)), 1);              \
    CD = vld1_lane_u32(tex + vget_lane_u16(i11, P), vld1_dup_u32(tex + vget_lane_u16(i10, P)), 1);

/* shade bytes [b0..b3 g0..g3 | r0..r3 a0..a3] -> pixels [b g r a] x 4 */
static const uint8_t kSpanIl[16] = { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
/* each pixel's byte 3 (its alpha) on all four of its bytes */
static const uint8_t kSpanA3[8] = { 3, 3, 3, 3, 7, 7, 7, 7 };
/* One group of four pixels at idx (round 27's loop body). TAIL: only the lanes lm names (MK64_FASTROWS' rows: a
 * span's last n & 3 pixels as the group ending at the span's end; the other lanes were drawn already, or lie
 * before the row, and keep their pixel and depth). PV (MK64_SPANVEC's rows): 1 = MK64_SHADEPX (rv: the group's
 * first pixel { b, g, r, a } in 16.16, gv: one pixel's step; bv, av unused), 4 = MK64_WRAPRR (both axes repeat:
 * the wrap is the AND alone). PV 0, TAIL 0: round 27's group, as it was. */
static inline __attribute__((always_inline)) void
span_grp4(const SpanCM *k, uint32_t *const fbuf, uint16_t *const zb, const uint32_t *const tex, int idx,
          int32x4_t uv, int32x4_t vv, int32x4_t zv, int32x4_t rv, int32x4_t gv, int32x4_t bv, int32x4_t av, uint16x4_t lm,
          int16x8_t lo, int16x8_t hi, int16x8_t am, int16x4_t lw, uint16x4_t zbias, int dmask, uint32_t fs32,
          uint8x16_t y8, uint8x16_t fc8, uint8_t amin8,
          const int FLAT, const int BILIN, const int ALPHA, const int AMODE, const int LERP, const int FOG,
          const int TAIL, const int PV) {
    /* z: uz <= oz + zbias, as (uz -sat zbias) <= oz in u16 (uz < zbias passes either way) */
    const uint16x4_t uz = vqshrun_n_s32(zv, 8);
    const uint16x4_t oz = vld1_u16(&zb[idx]);
    uint16x4_t zpass = vcle_u16(vqsub_u16(uz, zbias), oz);
    if (TAIL) zpass = vand_u16(zpass, lm);
    if (vget_lane_u64(vreinterpret_u64_u16(zpass), 0) == 0) return; /* the whole group behind the z-buffer */
    const int16x8_t cxy = vcombine_s16(vshrn_n_s32(uv, 16), vshrn_n_s32(vv, 16));
    uint8x16_t t8;
    if (!BILIN) {
        /* PV 4 (MK64_WRAPRR): both axes repeat, where the clamp to the int16 range changes no lane: the AND alone */
        const int16x8_t w = (PV & 4) ? vandq_s16(cxy, am) : span_wrap16(cxy, lo, hi, am);
        const uint16x4_t ix = span_index(w, w, lw);
        uint32x2_t t01 = vdup_n_u32(0), t23 = t01;
        SPAN_LD(t01, ix, 0, 0) SPAN_LD(t01, ix, 1, 1) SPAN_LD(t23, ix, 2, 0) SPAN_LD(t23, ix, 3, 1)
        t8 = vreinterpretq_u8_u32(vcombine_u32(t01, t23));
    } else {
        const int16x8_t c1 = vaddq_s16(cxy, vdupq_n_s16(1));
        const int16x8_t w0 = (PV & 4) ? vandq_s16(cxy, am) : span_wrap16(cxy, lo, hi, am);
        const int16x8_t w1 = (PV & 4) ? vandq_s16(c1, am) : span_wrap16(c1, lo, hi, am);
        const int16x4_t y0 = vshl_s16(vget_high_s16(w0), lw), y1 = vshl_s16(vget_high_s16(w1), lw);
        const uint16x4_t i00 = vreinterpret_u16_s16(vadd_s16(y0, vget_low_s16(w0)));
        const uint16x4_t i10 = vreinterpret_u16_s16(vadd_s16(y1, vget_low_s16(w0)));
        /* weights: (s >> 8) & 0xff per pixel, and 256 - that */
        const uint16x4_t fu4 = vand_u16(vreinterpret_u16_s16(vshrn_n_s32(uv, 8)), vdup_n_u16(0xff));
        const uint16x4_t fv4 = vand_u16(vreinterpret_u16_s16(vshrn_n_s32(vv, 8)), vdup_n_u16(0xff));
        const uint16x4_t gu4 = vsub_u16(vdup_n_u16(256), fu4), gv4 = vsub_u16(vdup_n_u16(256), fv4);
        uint32x2_t ab0, cd0, ab1, cd1, ab2, cd2, ab3, cd3;
        /* x0 + 1 unwrapped and unclamped on all four: each row's pair is one load */
        const uint16x4_t adj = vceq_s16(vget_low_s16(w1), vadd_s16(vget_low_s16(w0), vdup_n_s16(1)));
        if (vget_lane_u64(vreinterpret_u64_u16(adj), 0) == ~0ull) {
            SPAN_PAIR2(ab0, cd0, 0) SPAN_PAIR2(ab1, cd1, 1) SPAN_PAIR2(ab2, cd2, 2) SPAN_PAIR2(ab3, cd3, 3)
        } else {
            const uint16x4_t i01 = vreinterpret_u16_s16(vadd_s16(y0, vget_low_s16(w1)));
            const uint16x4_t i11 = vreinterpret_u16_s16(vadd_s16(y1, vget_low_s16(w1)));
            SPAN_PAIR4(ab0, cd0, 0) SPAN_PAIR4(ab1, cd1, 1) SPAN_PAIR4(ab2, cd2, 2) SPAN_PAIR4(ab3, cd3, 3)
        }
        uint16x4_t p0, p1, p2, p3;
        SPAN_BIL_PX(p0, ab0, cd0, 0) SPAN_BIL_PX(p1, ab1, cd1, 1) SPAN_BIL_PX(p2, ab2, cd2, 2) SPAN_BIL_PX(p3, ab3, cd3, 3)
        t8 = vcombine_u8(vmovn_u16(vcombine_u16(p0, p1)), vmovn_u16(vcombine_u16(p2, p3)));
    }
    /* shade as pixels, same byte order as the texels: clamp8_sh16 = vqshrun #16 then vqmovn (min 255) */
    uint8x16_t s8;
    if (FLAT) s8 = vreinterpretq_u8_u32(vdupq_n_u32(fs32));
    else if (PV & 1) { /* MK64_SHADEPX: each pixel's { b, g, r, a } as the per-channel lanes make them, already in
                        * pixel byte order (no table lookup) */
        const int32x4_t q1 = vaddq_s32(rv, gv), q2 = vaddq_s32(q1, gv), q3 = vaddq_s32(q2, gv);
        const uint8x8_t s01 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(rv, 16), vqshrun_n_s32(q1, 16)));
        const uint8x8_t s23 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(q2, 16), vqshrun_n_s32(q3, 16)));
        s8 = vcombine_u8(s01, s23);
    } else {
        const uint8x8x2_t sp = { { vqmovn_u16(vcombine_u16(vqshrun_n_s32(bv, 16), vqshrun_n_s32(gv, 16))),
                                   vqmovn_u16(vcombine_u16(vqshrun_n_s32(rv, 16), vqshrun_n_s32(av, 16))) } };
        s8 = vcombine_u8(vtbl2_u8(sp, vld1_u8(kSpanIl)), vtbl2_u8(sp, vld1_u8(kSpanIl + 8)));
    }
    /* texture x shade, all four bytes (byte 3 = div255(ta*sa)); LERP adds y*(255 - t) */
    uint8x16_t p8;
    if (!LERP) {
        p8 = vcombine_u8(vmovn_u16(span_div255(vmull_u8(vget_low_u8(t8), vget_low_u8(s8)))),
                         vmovn_u16(span_div255(vmull_u8(vget_high_u8(t8), vget_high_u8(s8)))));
    } else {
        const uint8x16_t it8 = vsubq_u8(vdupq_n_u8(255), t8);
        p8 = vcombine_u8(vmovn_u16(span_div255(vmlal_u8(vmull_u8(vget_low_u8(t8), vget_low_u8(s8)), vget_low_u8(y8), vget_low_u8(it8)))),
                         vmovn_u16(span_div255(vmlal_u8(vmull_u8(vget_high_u8(t8), vget_high_u8(s8)), vget_high_u8(y8), vget_high_u8(it8)))));
    }
    if (FOG) { /* CM=3: the shade alpha is the fog factor (opaque draws only: the alpha byte is dropped) */
        const uint8x8_t a3 = vld1_u8(kSpanA3);
        const uint8x16_t f8 = vcombine_u8(vtbl1_u8(vget_low_u8(s8), a3), vtbl1_u8(vget_high_u8(s8), a3));
        const uint8x16_t if8 = vsubq_u8(vdupq_n_u8(255), f8);
        p8 = vcombine_u8(vmovn_u16(span_div255(vmlal_u8(vmull_u8(vget_low_u8(p8), vget_low_u8(if8)), vget_low_u8(fc8), vget_low_u8(f8)))),
                         vmovn_u16(span_div255(vmlal_u8(vmull_u8(vget_high_u8(p8), vget_high_u8(if8)), vget_high_u8(fc8), vget_high_u8(f8)))));
    }
    const uint32x4_t old = vld1q_u32(&fbuf[idx]);
    uint32x4_t out, wmask = vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u16(zpass)));
    if (ALPHA) {
        const uint8x16_t src = AMODE == 2 ? p8 : AMODE == 1 ? t8 : s8;
        const uint8x8_t a3 = vld1_u8(kSpanA3);
        const uint8x16_t fa8 = vcombine_u8(vtbl1_u8(vget_low_u8(src), a3), vtbl1_u8(vget_high_u8(src), a3));
        wmask = vandq_u32(wmask, vreinterpretq_u32_u8(vcgeq_u8(fa8, vdupq_n_u8(amin8))));
        const uint8x16_t fb8 = k->ablend ? fa8 : vdupq_n_u8(255); /* 255: div255(p*255) == p, the unblended colour */
        const uint8x16_t ia8 = vsubq_u8(vdupq_n_u8(255), fb8);
        const uint8x16_t o8 = vreinterpretq_u8_u32(old);
        const uint16x8_t lo16 = vmlal_u8(vmull_u8(vget_low_u8(p8), vget_low_u8(fb8)), vget_low_u8(o8), vget_low_u8(ia8));
        const uint16x8_t hi16 = vmlal_u8(vmull_u8(vget_high_u8(p8), vget_high_u8(fb8)), vget_high_u8(o8), vget_high_u8(ia8));
        out = vorrq_u32(vreinterpretq_u32_u8(vcombine_u8(vmovn_u16(span_div255(lo16)), vmovn_u16(span_div255(hi16)))), vdupq_n_u32(0xFF000000u));
    } else {
        out = vorrq_u32(vreinterpretq_u32_u8(p8), vdupq_n_u32(0xFF000000u));
    }
    vst1q_u32(&fbuf[idx], vbslq_u32(wmask, out, old));
    if (dmask) vst1_u16(&zb[idx], vbsl_u16(vmovn_u32(wmask), uz, oz));
}
/* the span's constants, as round 27's span_cm4 builds them per span */
#define SPAN_CONSTS                                                                                                   \
    const uint32_t *const tex = k->tex;                                                                               \
    const int16x8_t lo = vcombine_s16(vdup_n_s16((int16_t)(k->ulo < -32768 ? -32768 : k->ulo)), vdup_n_s16((int16_t)(k->vlo < -32768 ? -32768 : k->vlo))); \
    const int16x8_t hi = vcombine_s16(vdup_n_s16((int16_t)(k->uhi > 32767 ? 32767 : k->uhi)), vdup_n_s16((int16_t)(k->vhi > 32767 ? 32767 : k->vhi))); \
    const int16x8_t am = vcombine_s16(vdup_n_s16((int16_t)k->uand), vdup_n_s16((int16_t)k->vand));                    \
    const int16x4_t lw = vdup_n_s16((int16_t)k->lw);                                                                  \
    const int dmask = k->dmask;                                                                                       \
    const uint16x4_t zbias = vdup_n_u16((uint16_t)k->zbias);                                                          \
    const uint32_t fs32 = k->fs;                                                                                      \
    const uint8x16_t y8 = LERP ? vreinterpretq_u8_u32(vdupq_n_u32(k->ly)) : vdupq_n_u8(0);                            \
    const uint8x16_t fc8 = FOG ? vreinterpretq_u8_u32(vdupq_n_u32(k->fogc)) : vdupq_n_u8(0);                         \
    const uint8_t amin8 = (uint8_t)k->amin;

static inline __attribute__((always_inline)) void
span_cm4(const SpanCM *k, uint32_t *const fbuf, uint16_t *const zb, int idx, int n, int32_t uf, int32_t vf,
         const int32_t duf, const int32_t dvf, int32_t zq, int32_t rq, int32_t gq, int32_t bq, int32_t aq,
         const int FLAT, const int BILIN, const int ALPHA, const int AMODE, const int LERP, const int FOG) {
    const int32x4_t lane = { 0, 1, 2, 3 };
    SPAN_CONSTS
    int32x4_t uv = vmlaq_n_s32(vdupq_n_s32(BILIN ? uf - 0x8000 : uf), lane, duf);
    int32x4_t vv = vmlaq_n_s32(vdupq_n_s32(BILIN ? vf - 0x8000 : vf), lane, dvf);
    const int32_t ust = 4 * duf, vst = 4 * dvf;
    int32x4_t zv = vmlaq_n_s32(vdupq_n_s32(zq), lane, k->zdq);
    const int32_t zst = 4 * k->zdq;
    int32x4_t rv = vdupq_n_s32(0), gv = rv, bv = rv, av = rv;
    if (!FLAT) {
        rv = vmlaq_n_s32(vdupq_n_s32(rq), lane, k->rdq); gv = vmlaq_n_s32(vdupq_n_s32(gq), lane, k->gdq);
        bv = vmlaq_n_s32(vdupq_n_s32(bq), lane, k->bdq); av = vmlaq_n_s32(vdupq_n_s32(aq), lane, k->adq);
    }
    const int32_t rst = 4 * k->rdq, gst = 4 * k->gdq, bst = 4 * k->bdq, ast = 4 * k->adq;
    for (int g = 0; g < n; g += 4, idx += 4, uv = vaddq_s32(uv, vdupq_n_s32(ust)), vv = vaddq_s32(vv, vdupq_n_s32(vst)),
                         zv = vaddq_s32(zv, vdupq_n_s32(zst))) {
        if (!FLAT && g) {
            rv = vaddq_s32(rv, vdupq_n_s32(rst)); gv = vaddq_s32(gv, vdupq_n_s32(gst));
            bv = vaddq_s32(bv, vdupq_n_s32(bst)); av = vaddq_s32(av, vdupq_n_s32(ast));
        }
        span_grp4(k, fbuf, zb, tex, idx, uv, vv, zv, rv, gv, bv, av, vdup_n_u16(0), lo, hi, am, lw, zbias, dmask, fs32,
                  y8, fc8, amin8, FLAT, BILIN, ALPHA, AMODE, LERP, FOG, 0, 0);
    }
}
/* MK64_FASTROWS' rows: n >= 1 pixels from idx, the groups and then the span's last n & 3 pixels as the masked
 * group ending at the span's end (the caller checks that group starts inside the framebuffer: n >= 4, or
 * idx >= 4 - n). Every lane drawn holds the values round 27's span would give that pixel (the same integer steps),
 * so the same pixels; the scalar loop drew those last pixels before (raster_tri). */
static inline __attribute__((always_inline)) void
span_draw(const SpanCM *__restrict k, int idx, int n, int32_t uf, int32_t vf, const int32_t duf, const int32_t dvf, int32_t zq,
          int32_t rq, int32_t gq, int32_t bq, int32_t aq,
          const int FLAT, const int BILIN, const int ALPHA, const int AMODE, const int LERP, const int FOG, const int PV) {
    const int32x4_t lane = { 0, 1, 2, 3 };
    uint32_t *const fbuf = k->fbuf; uint16_t *const zb = k->zb;
    SPAN_CONSTS
    int32x4_t uv = vmlaq_n_s32(vdupq_n_s32(BILIN ? uf - 0x8000 : uf), lane, duf);
    int32x4_t vv = vmlaq_n_s32(vdupq_n_s32(BILIN ? vf - 0x8000 : vf), lane, dvf);
    const int32_t ust = 4 * duf, vst = 4 * dvf;
    int32x4_t zv = vmlaq_n_s32(vdupq_n_s32(zq), lane, k->zdq);
    const int32_t zst = 4 * k->zdq;
    const int SPX = (PV & 1) && !FLAT;
    int32x4_t rv = vdupq_n_s32(0), gv = rv, bv = rv, av = rv, rs = rv, gs = rv, bs = rv, as = rv;
    if (SPX) { /* MK64_SHADEPX: rv = the group's first pixel { b, g, r, a }, gv = one pixel's step, rs = four pixels' */
        rv = (int32x4_t){ bq, gq, rq, aq };
        gv = (int32x4_t){ k->bdq, k->gdq, k->rdq, k->adq };
        rs = vshlq_n_s32(gv, 2);
    } else if (!FLAT) {
        rv = vmlaq_n_s32(vdupq_n_s32(rq), lane, k->rdq); gv = vmlaq_n_s32(vdupq_n_s32(gq), lane, k->gdq);
        bv = vmlaq_n_s32(vdupq_n_s32(bq), lane, k->bdq); av = vmlaq_n_s32(vdupq_n_s32(aq), lane, k->adq);
        rs = vdupq_n_s32(4 * k->rdq); gs = vdupq_n_s32(4 * k->gdq); bs = vdupq_n_s32(4 * k->bdq); as = vdupq_n_s32(4 * k->adq);
    }
    const int n4 = n & ~3, rem = n & 3;
    for (int g = 0; g < n4; g += 4, idx += 4, uv = vaddq_s32(uv, vdupq_n_s32(ust)), vv = vaddq_s32(vv, vdupq_n_s32(vst)),
                          zv = vaddq_s32(zv, vdupq_n_s32(zst))) {
        span_grp4(k, fbuf, zb, tex, idx, uv, vv, zv, rv, gv, bv, av, vdup_n_u16(0), lo, hi, am, lw, zbias, dmask, fs32,
                  y8, fc8, amin8, FLAT, BILIN, ALPHA, AMODE, LERP, FOG, 0, PV);
        if (SPX) rv = vaddq_s32(rv, rs);
        else if (!FLAT) { rv = vaddq_s32(rv, rs); gv = vaddq_s32(gv, gs); bv = vaddq_s32(bv, bs); av = vaddq_s32(av, as); }
    }
    if (rem) { /* the group ending at the span's end: its first 4 - rem lanes drawn already (or before the row) */
        const int back = 4 - rem;
        uv = vsubq_s32(uv, vdupq_n_s32(back * duf)); vv = vsubq_s32(vv, vdupq_n_s32(back * dvf));
        zv = vsubq_s32(zv, vdupq_n_s32(back * k->zdq));
        if (SPX) rv = vsubq_s32(rv, vmulq_n_s32(gv, back));
        else if (!FLAT) {
            rv = vsubq_s32(rv, vdupq_n_s32(back * k->rdq)); gv = vsubq_s32(gv, vdupq_n_s32(back * k->gdq));
            bv = vsubq_s32(bv, vdupq_n_s32(back * k->bdq)); av = vsubq_s32(av, vdupq_n_s32(back * k->adq));
        }
        const uint16x4_t lm = vcge_u16(vcreate_u16(0x0003000200010000ull), vdup_n_u16((uint16_t)back));
        span_grp4(k, fbuf, zb, tex, idx - back, uv, vv, zv, rv, gv, bv, av, lm, lo, hi, am, lw, zbias, dmask, fs32,
                  y8, fc8, amin8, FLAT, BILIN, ALPHA, AMODE, LERP, FOG, 1, PV);
    }
}
#undef SPAN_CONSTS
#undef SPAN_LD
#undef SPAN_BIL_PX
#undef SPAN_PAIR2
#undef SPAN_PAIR4

/* 0 when a texel coordinate of an n-pixel run could leave the int16 lanes: the
 * run start is always within +-16000 texels (to_fix16), but a degenerate
 * triangle's step can carry it further; the scalar loop draws those. (Also
 * keeps every uf + i*duf inside int32, as the scalar needs.) */
static inline __attribute__((always_inline)) int span_fits16(int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int n) {
    const int64_t u0 = (int64_t)uf - 0x8000, u1 = u0 + (int64_t)(n - 1) * duf;
    const int64_t v0 = (int64_t)vf - 0x8000, v1 = v0 + (int64_t)(n - 1) * dvf;
    const int64_t L = -((int64_t)32000 << 16), H = (int64_t)32000 << 16;
    return !(u0 < L || u0 > H || u1 < L || u1 > H || v0 < L || v0 > H || v1 < L || v1 > H);
}
/* n: a multiple of 4. The caller advances its own idx/uf/vf/zq/colour by n. */
#define SPAN_CM_ALPHA(F, B)                                                                            \
    switch (k->amode) {                                                                                \
        case 0:  span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, F, B, 1, 0, 0, 0); break;               \
        case 1:  span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, F, B, 1, 1, 0, 0); break;               \
        default: span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, F, B, 1, 2, 0, 0); break;               \
    }
/* Returns 0 (nothing drawn) when span_fits16 says no. */
static int __attribute__((noinline))
span_cm_neon(const SpanCM *k, int flat, int bilin, int alpha, int idx, int n,
             int32_t uf, int32_t vf, int32_t zq, int32_t rq, int32_t gq, int32_t bq, int32_t aq) {
    if (!span_fits16(uf, vf, k->duf, k->dvf, n)) return 0;
    if (flat) {
        if (bilin) { if (alpha) { SPAN_CM_ALPHA(1, 1) } else span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 1, 1, 0, 0, 0, 0); }
        else       { if (alpha) { SPAN_CM_ALPHA(1, 0) } else span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 1, 0, 0, 0, 0, 0); }
    } else {
        if (bilin) { if (alpha) { SPAN_CM_ALPHA(0, 1) } else span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 0, 1, 0, 0, 0, 0); }
        else       { if (alpha) { SPAN_CM_ALPHA(0, 0) } else span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 0, 0, 0, 0, 0, 0); }
    }
    return 1;
}
#undef SPAN_CM_ALPHA
/* CM=2, the particle lerp, which the caller only sends blended with amode 2 (alpha TEXEL0 * PRIM).
 * A function of its own: inside span_cm_neon it made the plain spans dearer (register allocation). */
static int __attribute__((noinline))
span_cm_lerp_neon(const SpanCM *k, int flat, int bilin, int idx, int n,
                  int32_t uf, int32_t vf, int32_t zq, int32_t rq, int32_t gq, int32_t bq, int32_t aq) {
    if (!span_fits16(uf, vf, k->duf, k->dvf, n)) return 0;
    if (flat) { if (bilin) span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 1, 1, 1, 2, 1, 0);
                else       span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 1, 0, 1, 2, 1, 0); }
    else      { if (bilin) span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 0, 1, 1, 2, 1, 0);
                else       span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 0, 0, 1, 2, 1, 0); }
    return 1;
}
/* CM=3, texture x shade fogged per pixel (gfx_pc fog_px), opaque only. A function of its own, like
 * the lerp: Choco Mountain's white fog reaches most of its course geometry. */
static int __attribute__((noinline))
span_cm_fog_neon(const SpanCM *k, int flat, int bilin, int idx, int n,
                 int32_t uf, int32_t vf, int32_t zq, int32_t rq, int32_t gq, int32_t bq, int32_t aq) {
    if (!span_fits16(uf, vf, k->duf, k->dvf, n)) return 0;
    if (flat) { if (bilin) span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 1, 1, 0, 0, 0, 1);
                else       span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 1, 0, 0, 0, 0, 1); }
    else      { if (bilin) span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 0, 1, 0, 0, 0, 1);
                else       span_cm4(k, k->fbuf, k->zb, idx, n, uf, vf, k->duf, k->dvf, zq, rq, gq, bq, aq, 0, 0, 0, 0, 0, 1); }
    return 1;
}

/* ---- MK64_FASTROWS' rows (2026-10-07, the cross-port speed round from the SM64 port e914b1a): one call a row ---- */
#define SPAN_ROW_PERSP 16 /* raster_tri's PERSP_SPAN (checked there) */
typedef struct {
    float32x4_t m;                        /* { ftw, ftw, fth, fth } */
    float32x4_t da, db;                   /* MK64_SPANVEC: { zdx, crdx, cgdx, cbdx }, { cadx, uwdx, vwdx, swdx } */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx, ftw, fth;
    int persp;
} SpanRowT;
/* a row's float state: A = { z, r, g, b }, B = { a, u/w, v/w, 1/w } (MK64_SPANVEC loads them as two vectors), 1/w's
 * reciprocal */
typedef struct { float zf, cr, cg, cb, ca, uw, vw, sw, rw; } SpanRowF;
typedef int (*SpanRowFn)(SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n);
/* the sprite span skip, as raster_tri's span loop takes it (before the span is drawn; every tap alpha 0: nothing) */
static inline __attribute__((always_inline)) int
span_row_skip(const SpanCM *k, int32_t uf, int32_t vf, int32_t duf, int32_t dvf, int span) {
    if (!k->arows) return 0;
    gfx_soft_spanskip_tried++;
    if (!span_clear(k->arows, uf, vf, duf, dvf, span, k->ws, k->wt)) return 0;
    gfx_soft_spanskip_spans++; gfx_soft_spanskip_px += (uint64_t)span;
    return 1;
}
/* n * s as raster_tri's int arithmetic gives it (the lanes wrap as its int32 sums do) */
static inline __attribute__((always_inline)) int32_t span_imul(int n, int32_t s) { return (int32_t)((uint32_t)n * (uint32_t)s); }
/* The row as raster_tri's span loop draws a NEON triangle's row (scalar VFP, the same float steps in the same order),
 * each span by span_draw. Returns n, or the pixel where a span needs raster_tri (the scalar fallback span_fits16
 * sends there, or a short group that would start before the framebuffer): f then holds the state there. */
static inline __attribute__((always_inline)) int
span_row(SpanCM *__restrict k, const SpanRowT *__restrict t, SpanRowF *__restrict f, int idx, int n,
         const int FLAT, const int BILIN, const int ALPHA, const int AMODE, const int LERP, const int FOG) {
    float zf = f->zf, cr = f->cr, cg = f->cg, cb = f->cb, ca = f->ca, uw = f->uw, vw = f->vw, sw = f->sw, rw = f->rw;
    const float swdx = t->swdx, uwdx = t->uwdx, vwdx = t->vwdx, ftw = t->ftw, fth = t->fth;
    int x = 0;
    while (x < n) {
        int span = n - x;
        if (span > SPAN_ROW_PERSP) span = SPAN_ROW_PERSP;
        const float fspan = (float)span;
        const float sw_b = sw + swdx * fspan;
        float rw_b = 0.0f, u_a, v_a, du, dv;
        if (t->persp) {
            rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
            u_a = uw * rw; v_a = vw * rw;
            const float nu = (uw + uwdx * fspan) * rw_b - u_a;
            const float nv = (vw + vwdx * fspan) * rw_b - v_a;
            if (span == SPAN_ROW_PERSP) { du = nu * (1.0f / SPAN_ROW_PERSP); dv = nv * (1.0f / SPAN_ROW_PERSP); }
            else                        { du = nu / fspan;                   dv = nv / fspan; }
        } else {
            u_a = uw; v_a = vw; du = uwdx; dv = vwdx;
        }
        const int32_t uf = to_fix16(u_a * ftw), duf = to_fix16(du * ftw);
        const int32_t vf = to_fix16(v_a * fth), dvf = to_fix16(dv * fth);
        const int32_t zq = (int32_t)((zf < -1.0f ? -1.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
        const int32_t rq = (int32_t)(cr * 65536.0f), gq = (int32_t)(cg * 65536.0f), bq = (int32_t)(cb * 65536.0f), aq = (int32_t)(ca * 65536.0f);
        if (!span_row_skip(k, uf, vf, duf, dvf, span)) {
            if ((span < 4 && idx < 4 - span) || !span_fits16(uf, vf, duf, dvf, span)) break;
            span_draw(k, idx, span, uf, vf, duf, dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, AMODE, LERP, FOG, 0);
        }
        zf += t->zdx * fspan;
        cr += t->crdx * fspan; cg += t->cgdx * fspan; cb += t->cbdx * fspan; ca += t->cadx * fspan;
        uw += uwdx * fspan;
        vw += vwdx * fspan;
        sw = sw_b; rw = rw_b;
        x += span; idx += span;
    }
    if (x < n) { f->zf = zf; f->cr = cr; f->cg = cg; f->cb = cb; f->ca = ca; f->uw = uw; f->vw = vw; f->sw = sw; f->rw = rw; }
    return x;
}
/* MK64_SPANVEC (2026-10-07; the SM64 port SM64_SPANVEC; default 1, inside MK64_FASTROWS' rows): span_row with the row's
 * float state as two NEON vectors, A = { z, r, g, b } and B = { a, u/w, v/w, 1/w }, as SpanRowF lays them out, each
 * span stepped by two multiply-adds. The same operations in the same order, lane by lane: the next span's start is
 * A + dA * span and B + dB * span (vmla rounds the product, then the sum, as the VFP's multiply and add do); the
 * perspective ends u/w * (1/w), ((u/w)' * (1/w)' - u) and the 1/16 step as two-lane multiplies and a subtract; the
 * divides (the span's 1/w, a short span's step) stay scalar VFP divides; to_fix16's clamp and scaling as vmin/vmax and
 * one vcvt #16 (the scaling by 2^16 is exact, then the same truncation), the colours' (int32_t)(c * 65536.0f) as one
 * vcvt #16 (both saturate, NaN 0); span_fits16's usual case for all four values in one NEON test. NEON flushes
 * denormals to zero where the VFP keeps them: no value here comes near one (screen and texture coordinates, 1/w,
 * colours 0..255, depth 0..1 and their per-pixel steps are each 0 or far above 1e-30), and a flushed one converts
 * to 0 as the VFP's does. PV: MK64_SHADEPX (1), MK64_WRAPRR (4).
 * MK64_ZSKIP (default 1, in these rows): a depth-tested span whose nearest depth (z is linear along the span, so at
 * one of its ends) is further than the farthest depth stored under it plus the decal window fails the depth test at
 * every pixel, so it writes nothing: skipped before its texture setup and groups. A sufficient test, never a guess;
 * a triangle stops testing after four misses running (speed only). Same pixels. */
static inline __attribute__((always_inline)) int
span_row_v(SpanCM *__restrict k, const SpanRowT *__restrict t, SpanRowF *__restrict f, int idx, int n,
           const int FLAT, const int BILIN, const int ALPHA, const int AMODE, const int LERP, const int FOG, const int PV) {
    float32x4_t A = vld1q_f32(&f->zf), B = vld1q_f32(&f->ca);
    const float32x4_t DA = t->da, DB = t->db;
    float rw = f->rw;
    int x = 0;
    while (x < n) {
        int span = n - x;
        if (span > SPAN_ROW_PERSP) span = SPAN_ROW_PERSP;
        const float fspan = (float)span;
        const float32x4_t fs = vdupq_n_f32(fspan);
        const float32x4_t An = vmlaq_f32(A, DA, fs), Bn = vmlaq_f32(B, DB, fs); /* the next span's start */
        const float sw_b = vgetq_lane_f32(Bn, 3);
        const float32x2_t zc = vmax_f32(vmin_f32(vdup_lane_f32(vget_low_f32(A), 0), vdup_n_f32(2.0f)), vdup_n_f32(-1.0f));
        const int32_t zq = vget_lane_s32(vcvt_s32_f32(vmul_f32(zc, vdup_n_f32(65535.0f * 256.0f))), 0);
        if (k->zskip > 0 && span >= 4) { /* MK64_ZSKIP (above) */
            const uint16_t *const zp = k->zb + idx;
            const int uz0 = clamp16_sh8(zq), uz1 = clamp16_sh8(zq + span_imul(span - 1, k->zdq));
            uint16x4_t m4;
            if (span >= 8) { const uint16x8_t m8 = vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8)); m4 = vmax_u16(vget_low_u16(m8), vget_high_u16(m8)); }
            else m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));
            m4 = vpmax_u16(m4, m4); m4 = vpmax_u16(m4, m4);
            const int ozb = (int)vget_lane_u16(m4, 0) + (int)k->zbias;
            if ((uz0 < uz1 ? uz0 : uz1) > (ozb > 65535 ? 65535 : ozb)) {
                k->zskip = 4;
                A = An; B = Bn; rw = t->persp && sw_b > 1e-12f ? 1.0f / sw_b : 0.0f; /* the next span's start 1/w, as drawn */
                x += span; idx += span;
                continue;
            }
            k->zskip--; /* the triangle's spans in front: after four misses running it stops testing (speed only) */
        }
        float rw_b = 0.0f;
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
        const int32x4_t o = vcvtq_n_s32_f32(c, 16); /* to_fix16: (int32_t)(c * 65536.0f) */
        const int32_t uf = vgetq_lane_s32(o, 0), duf = vgetq_lane_s32(o, 1), vf = vgetq_lane_s32(o, 2), dvf = vgetq_lane_s32(o, 3);
        /* span_fits16's usual case for the four at once: (uint32_t)(x + B) <= 2B per lane (B = 16001 << 16 for u, v,
         * 1000 << 16 for their steps: then every pixel's coordinate, less the bilinear half texel, is within 32000
         * texels for spans up to 16); else span_fits16 itself decides */
        static const uint32_t okb[4] = { 16001u << 16, 1000u << 16, 16001u << 16, 1000u << 16 };
        static const uint32_t okl[4] = { 32002u << 16, 2000u << 16, 32002u << 16, 2000u << 16 };
        const uint32x4_t okv = vcleq_u32(vaddq_u32(vreinterpretq_u32_s32(o), vld1q_u32(okb)), vld1q_u32(okl));
        const uint32x2_t ok2 = vpmin_u32(vget_low_u32(okv), vget_high_u32(okv));
        if (span_row_skip(k, uf, vf, duf, dvf, span)) { A = An; B = Bn; rw = rw_b; x += span; idx += span; continue; }
        if ((span < 4 && idx < 4 - span) || (vget_lane_u32(vpmin_u32(ok2, ok2), 0) != 0xFFFFFFFFu && !span_fits16(uf, vf, duf, dvf, span)))
            break; /* raster_tri draws it (and the rest) */
        int32_t rq = 0, gq = 0, bq = 0, aq = 0;
        if (!FLAT) {
            const int32x4_t oc = vcvtq_n_s32_f32(vextq_f32(A, B, 1), 16); /* { r, g, b, a }: (int32_t)(c * 65536.0f) */
            rq = vgetq_lane_s32(oc, 0); gq = vgetq_lane_s32(oc, 1); bq = vgetq_lane_s32(oc, 2); aq = vgetq_lane_s32(oc, 3);
        }
        span_draw(k, idx, span, uf, vf, duf, dvf, zq, rq, gq, bq, aq, FLAT, BILIN, ALPHA, AMODE, LERP, FOG, PV);
        A = An; B = Bn; rw = rw_b;
        x += span; idx += span;
    }
    if (x < n) { vst1q_f32(&f->zf, A); vst1q_f32(&f->ca, B); f->rw = rw; }
    return x;
}
/* one noinline function per variant, named by its flags: NAME (span_row), NAME_v (span_row_v), NAME_p (and
 * MK64_SHADEPX: smooth shade only), NAME_vr / NAME_pr (and MK64_WRAPRR) */
#define SPAN_ROW_FN0(NAME, F, B, A, M, L, G)                                                                          \
    static int __attribute__((noinline)) NAME(SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) {           \
        return span_row(k, t, f, idx, n, F, B, A, M, L, G); }                                                          \
    static int __attribute__((noinline)) NAME##_v(SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) {       \
        return span_row_v(k, t, f, idx, n, F, B, A, M, L, G, 0); }                                                     \
    static int __attribute__((noinline)) NAME##_vr(SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) {      \
        return span_row_v(k, t, f, idx, n, F, B, A, M, L, G, 4); }
#define SPAN_ROW_FN1(NAME, F, B, A, M, L, G)                                                                          \
    SPAN_ROW_FN0(NAME, F, B, A, M, L, G)                                                                              \
    static int __attribute__((noinline)) NAME##_p(SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) {       \
        return span_row_v(k, t, f, idx, n, F, B, A, M, L, G, 1); }                                                     \
    static int __attribute__((noinline)) NAME##_pr(SpanCM *k, const SpanRowT *t, SpanRowF *f, int idx, int n) {      \
        return span_row_v(k, t, f, idx, n, F, B, A, M, L, G, 5); }
#define SPAN_ROW_FB(P, A, M, L, G) \
    SPAN_ROW_FN0(P##_f1b1, 1, 1, A, M, L, G) SPAN_ROW_FN0(P##_f1b0, 1, 0, A, M, L, G) \
    SPAN_ROW_FN1(P##_f0b1, 0, 1, A, M, L, G) SPAN_ROW_FN1(P##_f0b0, 0, 0, A, M, L, G)
/* the row kinds (raster_tri's NEON spans): name, ALPHA, AMODE, LERP, FOG */
SPAN_ROW_FB(srow_cm, 0, 0, 0, 0)
SPAN_ROW_FB(srow_cm_a0, 1, 0, 0, 0)
SPAN_ROW_FB(srow_cm_a1, 1, 1, 0, 0)
SPAN_ROW_FB(srow_cm_a2, 1, 2, 0, 0)
SPAN_ROW_FB(srow_lerp, 1, 2, 1, 0)
SPAN_ROW_FB(srow_fog, 0, 0, 0, 1)
#define SPAN_ROW_SEL(P) (!vec ? (flat ? (bilin ? P##_f1b1 : P##_f1b0) : (bilin ? P##_f0b1 : P##_f0b0))                 \
                       : rr ? (flat ? (bilin ? P##_f1b1_vr : P##_f1b0_vr)                                               \
                                    : vec == 2 ? (bilin ? P##_f0b1_pr : P##_f0b0_pr) : (bilin ? P##_f0b1_vr : P##_f0b0_vr)) \
                            : (flat ? (bilin ? P##_f1b1_v : P##_f1b0_v)                                                 \
                                    : vec == 2 ? (bilin ? P##_f0b1_p : P##_f0b0_p) : (bilin ? P##_f0b1_v : P##_f0b0_v)))
/* raster_tri's NEON span dispatch (cm_mod: span_cm_neon by k->amode; cm_lerp; else the fog), once per triangle.
 * vec: 0 span_row, 1 MK64_SPANVEC's span_row_v, 2 and MK64_SHADEPX; rr: MK64_WRAPRR (with vec only). */
static SpanRowFn span_row_pick(const SpanCM *k, int kind, int flat, int bilin, int alpha, int vec, int rr) {
    rr = rr && vec;
    if (kind == 0) {
        if (!alpha) return SPAN_ROW_SEL(srow_cm);
        return k->amode == 0 ? SPAN_ROW_SEL(srow_cm_a0) : k->amode == 1 ? SPAN_ROW_SEL(srow_cm_a1) : SPAN_ROW_SEL(srow_cm_a2);
    }
    return kind == 1 ? SPAN_ROW_SEL(srow_lerp) : SPAN_ROW_SEL(srow_fog);
}
#undef SPAN_ROW_SEL
#undef SPAN_ROW_FB
#undef SPAN_ROW_FN0
#undef SPAN_ROW_FN1
