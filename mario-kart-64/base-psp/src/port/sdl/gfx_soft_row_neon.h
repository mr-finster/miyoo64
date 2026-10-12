/* Per-row NEON for the z-tested texture x shade path (MK64_NEON=2): ported
 * as-is from the OoT port c935b73 (cm_row_neon), same kit lineage -- raster_tri's
 * span setup and SPAN_LOOP_CM are float-for-float and bit-for-bit the same in
 * both ports. Included by gfx_soft.c after its helpers and PERSP_SPAN; the
 * device A/B against round 27's four-pixel span (MK64_NEON=1) decides. */
#pragma once
#include <arm_neon.h>
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
 * so it is bit-identical (proved on ARM by bx/qsweep.sh, MK64_NEON=0 vs 2). In raster_tri
 * the span setup spilled to the stack, paid an FPSCR transfer per clamp and
 * re-converted the texture size every span, and each span paid a 76-instruction
 * call; flat triangles come in with zero colour steps. MK64_NEON=0 keeps the
 * scalar loops. */
typedef struct {
    const void *px; int w, h, ws, wt, mu, mv; bool t16; /* texture */
    float swdx, uwdx, vwdx, zdx, crdx, cgdx, cbdx, cadx;  /* triangle x-steps */
    float ftw, fth;                                       /* (float)tw, (float)th */
    int32x4_t dacc;                                       /* B G R A 16.16 x-steps */
    int32_t zdq; int zbias, amode, amin, ablend; /* ablend 0: TEX_EDGE written unblended (X12) */
    bool persp, dmask, alpha, bilin;
} CmTri;
static inline uint16x4_t cm_div255(uint16x4_t m) {
    return vshr_n_u16(vadd_u16(vadd_u16(m, vshr_n_u16(m, 8)), vdup_n_u16(1)), 8);
}
/* The pixel loop of one span: Tx (the texel, B G R A in u16 lanes) from the fetch (...). */
#define CM_PIXELS(ALPHA, ...)                                                                            \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz + zbias) continue;                                                                 \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        const uint16x4_t S = vmin_u16(vqmovun_s32(vshrq_n_s32(acc, 16)), vdup_n_u16(255));              \
        uint16x4_t M = cm_div255(vmul_u16(Tx, S));                                                      \
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
#define CM_ROW(ALPHA, ...)                                                                               \
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
        CM_PIXELS(ALPHA, __VA_ARGS__)                                                                   \
        zf += zdx * fspan;                                                                              \
        cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;                 \
        uw += uwdx * fspan;                                                                             \
        vw += vwdx * fspan;                                                                             \
        sw = sw_b; rw = rw_b;                                                                           \
        fb = end; zp += span; n -= span;                                                                \
    }
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
    (void)th; (void)ws; (void)wt; (void)px32;
#define CM_VARIANTS(KIND, ALPHA)                                                                        \
    if (rr) { if (T->t16) { CM_ROW(ALPHA, KIND(CM_TAP16, CM_WRAP_RRU, CM_WRAP_RRV)) }                  \
              else        { CM_ROW(ALPHA, KIND(CM_TAP32, CM_WRAP_RRU, CM_WRAP_RRV)) } }                \
    else    { if (T->t16) { CM_ROW(ALPHA, KIND(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) }                    \
              else        { CM_ROW(ALPHA, KIND(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) } }
    if (T->bilin) { if (T->alpha) { CM_VARIANTS(CM_BILIN, 1) } else { CM_VARIANTS(CM_BILIN, 0) } }
    else          { if (T->alpha) { CM_VARIANTS(CM_NEAR, 1) }  else { CM_VARIANTS(CM_NEAR, 0) } }
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
#endif
