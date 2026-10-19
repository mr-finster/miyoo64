/* The audio mixer's two hottest loops as plain kernels, C and NEON, so a unit
 * test (tools/handheld/mixer_test.c, run under qemu-arm) can prove them equal
 * on random buffers: the envelope mixer (the new-ABI aEnvMixer: 16-sample
 * blocks, one volume set for each 8-sample half) and the mix (aMix).
 * The NEON versions are the C arithmetic lane by lane (DKR 0db1de8 did the
 * same for its older-ABI mixer):
 *   (s16)((s32)x * vol >> 16)   widen, vmulq_n_s32 (|x * vol| < 2^31), vshrq 16, vmovn
 *   clamp16(dst + x)            vqaddq_s16
 *   clamp16((out*0x7FFF + in*gain + 0x4000) >> 15)
 *                               vmull/vmlal (the sum stays inside s32), vqrshrn 15 */
#ifndef HH_MIXER_KERNELS_H
#define HH_MIXER_KERNELS_H

#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HH_MIXER_NEON 1
#endif

typedef struct {
    uint16_t volLeft0, volRight0, reverb0; /* samples 0-7 of each block */
    uint16_t volLeft1, volRight1, reverb1; /* samples 8-15 */
    int16_t rampLeft, rampRight, rampReverb; /* per block (already doubled) */
    int16_t dryLeftMask, dryRightMask, wetLeftMask, wetRightMask;
    int32_t swapLR;
} HhEnvMix;

static inline int16_t hh_mul_hi_su(int16_t x, uint16_t v) {
    return (int16_t)(((int32_t)x * v) >> 16);
}
static inline int16_t hh_clamp16(int32_t v) {
    return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

/* `samples` is a multiple of 16. */
static inline void hh_env_mix_c(const int16_t* in, int16_t* dryL, int16_t* dryR, int16_t* wetL, int16_t* wetR,
                                int32_t samples, HhEnvMix e) {
    const int32_t masks = e.dryLeftMask | e.dryRightMask | e.wetLeftMask | e.wetRightMask;
    for (int32_t done = 0; done < samples; done += 16) {
        for (int b = 0; b < 16; b++) {
            const int16_t s = *in++;
            const uint16_t vl = b < 8 ? e.volLeft0 : e.volLeft1, vr = b < 8 ? e.volRight0 : e.volRight1;
            const uint16_t rv = b < 8 ? e.reverb0 : e.reverb1;
            int16_t l = hh_mul_hi_su(s, vl), r = hh_mul_hi_su(s, vr), wl, wr;
            if (masks) {
                l ^= e.dryLeftMask; r ^= e.dryRightMask;
                wl = (int16_t)(hh_mul_hi_su(l, rv) ^ e.wetLeftMask);
                wr = (int16_t)(hh_mul_hi_su(r, rv) ^ e.wetRightMask);
            } else {
                wl = hh_mul_hi_su(l, rv); wr = hh_mul_hi_su(r, rv);
            }
            *dryL = hh_clamp16(*dryL + l); dryL++;
            *dryR = hh_clamp16(*dryR + r); dryR++;
            if (e.swapLR) { *wetL = hh_clamp16(*wetL + wr); *wetR = hh_clamp16(*wetR + wl); }
            else          { *wetL = hh_clamp16(*wetL + wl); *wetR = hh_clamp16(*wetR + wr); }
            wetL++; wetR++;
        }
        e.volLeft0 = (uint16_t)(e.volLeft0 + e.rampLeft);   e.volLeft1 = (uint16_t)(e.volLeft1 + e.rampLeft);
        e.volRight0 = (uint16_t)(e.volRight0 + e.rampRight); e.volRight1 = (uint16_t)(e.volRight1 + e.rampRight);
        e.reverb0 = (uint16_t)(e.reverb0 + e.rampReverb);    e.reverb1 = (uint16_t)(e.reverb1 + e.rampReverb);
    }
}

/* `samples` is a multiple of 8. */
static inline void hh_mix_c(const int16_t* in, int16_t* out, int32_t samples, int16_t gain) {
    for (int32_t i = 0; i < samples; i++) {
        out[i] = hh_clamp16((((int32_t)out[i] * 0x7FFF) + ((int32_t)in[i] * gain) + 0x4000) >> 15);
    }
}

#if HH_MIXER_NEON
static inline int16x8_t hh_mul_hi_su8(int16x8_t x, uint16_t v) {
    const int32x4_t lo = vshrq_n_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(x)), (int32_t)v), 16);
    const int32x4_t hi = vshrq_n_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(x)), (int32_t)v), 16);
    return vcombine_s16(vmovn_s32(lo), vmovn_s32(hi));
}
static inline void hh_env_mix_half(const int16_t* in, int16_t* dryL, int16_t* dryR, int16_t* wetL, int16_t* wetR,
                                   uint16_t vl, uint16_t vr, uint16_t rv, const HhEnvMix* e, int masks) {
    const int16x8_t s = vld1q_s16(in);
    int16x8_t l = hh_mul_hi_su8(s, vl), r = hh_mul_hi_su8(s, vr), wl, wr;
    if (masks) {
        l = veorq_s16(l, vdupq_n_s16(e->dryLeftMask)); r = veorq_s16(r, vdupq_n_s16(e->dryRightMask));
        wl = veorq_s16(hh_mul_hi_su8(l, rv), vdupq_n_s16(e->wetLeftMask));
        wr = veorq_s16(hh_mul_hi_su8(r, rv), vdupq_n_s16(e->wetRightMask));
    } else {
        wl = hh_mul_hi_su8(l, rv); wr = hh_mul_hi_su8(r, rv);
    }
    vst1q_s16(dryL, vqaddq_s16(vld1q_s16(dryL), l));
    vst1q_s16(dryR, vqaddq_s16(vld1q_s16(dryR), r));
    if (e->swapLR) { const int16x8_t t = wl; wl = wr; wr = t; }
    vst1q_s16(wetL, vqaddq_s16(vld1q_s16(wetL), wl));
    vst1q_s16(wetR, vqaddq_s16(vld1q_s16(wetR), wr));
}
static inline void hh_env_mix_neon(const int16_t* in, int16_t* dryL, int16_t* dryR, int16_t* wetL, int16_t* wetR,
                                   int32_t samples, HhEnvMix e) {
    const int masks = (e.dryLeftMask | e.dryRightMask | e.wetLeftMask | e.wetRightMask) != 0;
    for (int32_t done = 0; done < samples; done += 16) {
        hh_env_mix_half(in, dryL, dryR, wetL, wetR, e.volLeft0, e.volRight0, e.reverb0, &e, masks);
        hh_env_mix_half(in + 8, dryL + 8, dryR + 8, wetL + 8, wetR + 8, e.volLeft1, e.volRight1, e.reverb1, &e, masks);
        in += 16; dryL += 16; dryR += 16; wetL += 16; wetR += 16;
        e.volLeft0 = (uint16_t)(e.volLeft0 + e.rampLeft);   e.volLeft1 = (uint16_t)(e.volLeft1 + e.rampLeft);
        e.volRight0 = (uint16_t)(e.volRight0 + e.rampRight); e.volRight1 = (uint16_t)(e.volRight1 + e.rampRight);
        e.reverb0 = (uint16_t)(e.reverb0 + e.rampReverb);    e.reverb1 = (uint16_t)(e.reverb1 + e.rampReverb);
    }
}
static inline void hh_mix_neon(const int16_t* in, int16_t* out, int32_t samples, int16_t gain) {
    const int16x4_t k = vdup_n_s16(0x7FFF), g = vdup_n_s16(gain);
    for (int32_t i = 0; i < samples; i += 8) {
        const int16x8_t o = vld1q_s16(out + i), x = vld1q_s16(in + i);
        const int32x4_t lo = vmlal_s16(vmull_s16(vget_low_s16(o), k), vget_low_s16(x), g);
        const int32x4_t hi = vmlal_s16(vmull_s16(vget_high_s16(o), k), vget_high_s16(x), g);
        vst1q_s16(out + i, vcombine_s16(vqrshrn_n_s32(lo, 15), vqrshrn_n_s32(hi, 15)));
    }
}
/* aFilter (ABI2, OotPspMixer_Filter): per block of 8 samples, output o is the
 * sum of the 8 products in[k] * taps[t] listed for it here (k 0-7 = the previous
 * block, 8-15 = this block; the RSP works on byte-swapped sample pairs, hence
 * the pattern), then clamp16((sum + 0x4000) >> 15). The sums wrap in 32 bits
 * (the port builds with -fwrapv) and a wrapping sum is the same in any order,
 * so vmlal accumulation is exact. Previous-block sample 1 is never used. */
static const uint8_t kHhFilterTab[64][3] = { /* output, input, tap */
    {0, 3, 6}, {0, 2, 7}, {0, 5, 4}, {0, 4, 5}, {0, 7, 2}, {0, 6, 3}, {0, 9, 0}, {0, 8, 1},
    {1, 0, 6}, {1, 3, 7}, {1, 2, 4}, {1, 5, 5}, {1, 4, 2}, {1, 7, 3}, {1, 6, 0}, {1, 9, 1},
    {2, 5, 6}, {2, 4, 7}, {2, 7, 4}, {2, 6, 5}, {2, 9, 2}, {2, 8, 3}, {2, 11, 0}, {2, 10, 1},
    {3, 2, 6}, {3, 5, 7}, {3, 4, 4}, {3, 7, 5}, {3, 6, 2}, {3, 9, 3}, {3, 8, 0}, {3, 11, 1},
    {4, 7, 6}, {4, 6, 7}, {4, 9, 4}, {4, 8, 5}, {4, 11, 2}, {4, 10, 3}, {4, 13, 0}, {4, 12, 1},
    {5, 4, 6}, {5, 7, 7}, {5, 6, 4}, {5, 9, 5}, {5, 8, 2}, {5, 11, 3}, {5, 10, 0}, {5, 13, 1},
    {6, 9, 6}, {6, 8, 7}, {6, 11, 4}, {6, 10, 5}, {6, 13, 2}, {6, 12, 3}, {6, 15, 0}, {6, 14, 1},
    {7, 6, 6}, {7, 9, 7}, {7, 8, 4}, {7, 11, 5}, {7, 10, 2}, {7, 13, 3}, {7, 12, 0}, {7, 15, 1},
};
/* `blocks` blocks from in to out; hist = the previous block (updated). */
static inline void hh_filter_neon(const int16_t* in, int16_t* out, int32_t blocks, const int16_t taps[8],
                                  int16_t hist[8]) {
    int16_t cols[16][8]; /* cols[k][o]: the tap on input k for output o */
    int16x8_t c[16];
    memset(cols, 0, sizeof(cols));
    for (int e = 0; e < 64; e++) {
        cols[kHhFilterTab[e][1]][kHhFilterTab[e][0]] = taps[kHhFilterTab[e][2]];
    }
    for (int k = 0; k < 16; k++) {
        c[k] = vld1q_s16(cols[k]);
    }
    int16x8_t h = vld1q_s16(hist);
    for (int32_t n = 0; n < blocks; n++, in += 8, out += 8) {
        const int16x8_t b = vld1q_s16(in);
        const int16x4_t hl = vget_low_s16(h), hh = vget_high_s16(h), bl = vget_low_s16(b), bh = vget_high_s16(b);
        int32x4_t lo = vdupq_n_s32(0x4000), hi = vdupq_n_s32(0x4000); /* the rounding term, added first */
#define HH_FT(K, V, L) lo = vmlal_lane_s16(lo, vget_low_s16(c[K]), V, L); hi = vmlal_lane_s16(hi, vget_high_s16(c[K]), V, L);
        HH_FT(0, hl, 0) HH_FT(2, hl, 2) HH_FT(3, hl, 3) HH_FT(4, hh, 0) HH_FT(5, hh, 1) HH_FT(6, hh, 2) HH_FT(7, hh, 3)
        HH_FT(8, bl, 0) HH_FT(9, bl, 1) HH_FT(10, bl, 2) HH_FT(11, bl, 3)
        HH_FT(12, bh, 0) HH_FT(13, bh, 1) HH_FT(14, bh, 2) HH_FT(15, bh, 3)
#undef HH_FT
        vst1q_s16(out, vcombine_s16(vqmovn_s32(vshrq_n_s32(lo, 15)), vqmovn_s32(vshrq_n_s32(hi, 15))));
        h = b;
    }
    vst1q_s16(hist, h);
}
/* aResample (OotPspMixer_Resample's loop): per output sample, the four input
 * samples at the pitch position times one row of the 64-row table, each
 * product rounded ((x*c + 0x4000) >> 15 == vrshr #15, no overflow in s32),
 * (p0 + p1) + (p2 + p3) (vpadd, exact in s32), then clamp16 (vqmovn). Two
 * outputs a pass; `samples` is even. Returns the input pointer after the
 * last step; *pacc is the 16-bit pitch accumulator. */
static inline const int16_t* hh_resample_neon(const int16_t* in, int16_t* out, int32_t samples,
                                             const int16_t (*table)[4], uint32_t* pacc, uint32_t step) {
    uint32_t pa = *pacc;
    for (int32_t i = 0; i < samples; i += 2) {
        const int32x4_t pA = vrshrq_n_s32(vmull_s16(vld1_s16(in), vld1_s16(table[(pa * 64) >> 16])), 15);
        pa += step; in += pa >> 16; pa &= 0xFFFF;
        const int32x4_t pB = vrshrq_n_s32(vmull_s16(vld1_s16(in), vld1_s16(table[(pa * 64) >> 16])), 15);
        pa += step; in += pa >> 16; pa &= 0xFFFF;
        const int32x2_t a2 = vpadd_s32(vget_low_s32(pA), vget_high_s32(pA)); /* [p0+p1, p2+p3] */
        const int32x2_t b2 = vpadd_s32(vget_low_s32(pB), vget_high_s32(pB));
        const int16x4_t r = vqmovn_s32(vcombine_s32(vpadd_s32(a2, b2), vdup_n_s32(0)));
        vst1_lane_s32((int32_t*)(out + i), vreinterpret_s32_s16(r), 0);
    }
    *pacc = pa;
    return in;
}
/* ADPCM (OotPspMixer_ADPCMdec + OotPspMixer_DecodeAdpcmHalfReference). The
 * decoded values -- a 4-bit (or 2-bit) signed nibble << shift, shift <= 12
 * (14) -- and the previous samples all fit in 16 bits, so a half-frame of 8
 * outputs is an 8x10 matrix product in vmull/vmlal: output k is
 *   t0[k]*prev2 + t1[k]*prev1 + (ins[k] << 11) + sum_{j<k} t1[k-1-j]*ins[j]
 * i.e. the column of ins[j] is P[8-j .. 15-j] of P = {0 x8, 2048, t1[0..6]};
 * the s32 sums wrap like the C (-fwrapv) in any order, and clamp16(acc >> 11)
 * is vqshrn #11. Columns come from one frame table; `c` holds them. */
typedef struct { int16x8_t c[10]; const int16_t* key; } HhAdpcmCols;
static inline void hh_adpcm_cols(HhAdpcmCols* k, const int16_t tbl[2][8]) {
    int16_t P[16];
    memset(P, 0, 8 * sizeof(int16_t));
    P[8] = 2048;
    memcpy(&P[9], tbl[1], 7 * sizeof(int16_t));
    k->c[0] = vld1q_s16(tbl[0]);
    k->c[1] = vld1q_s16(tbl[1]);
    for (int j = 0; j < 8; j++) k->c[2 + j] = vld1q_s16(&P[8 - j]);
    k->key = &tbl[0][0];
}
/* 8 decoded inputs from 4 bytes (4-bit: high nibble first) or 2 bytes (2-bit: top pair first) */
static inline int16x8_t hh_adpcm_ins(const uint8_t* in, int shift, int twoBit) {
    int8x8_t v;
    if (!twoBit) {
        uint32_t w;
        memcpy(&w, in, 4);
        const uint8x8_t b = vreinterpret_u8_u32(vdup_n_u32(w));
        const uint8x8x2_t z = vzip_u8(vshr_n_u8(b, 4), vand_u8(b, vdup_n_u8(0xF))); /* hi0 lo0 hi1 lo1 ... */
        v = vshr_n_s8(vreinterpret_s8_u8(vshl_n_u8(z.val[0], 4)), 4);                    /* sign-extend 4 bits */
    } else {
        const uint8x8_t b = vcreate_u8(((uint64_t)in[0] * 0x01010101ull) | ((uint64_t)in[1] * 0x0101010100000000ull));
        const int8x8_t sh = vcreate_s8(0x0604020006040200ull);                            /* bit pairs 7-6, 5-4, 3-2, 1-0 */
        v = vshr_n_s8(vreinterpret_s8_u8(vshl_u8(b, sh)), 6);                             /* sign-extend 2 bits */
    }
    return vshlq_s16(vmovl_s8(v), vdupq_n_s16((int16_t)shift));
}
/* one half: out[0..7] from out[-2], out[-1] and ins */
static inline void hh_adpcm_half(int16_t* out, const HhAdpcmCols* k, int16x8_t ins) {
    const int16x4_t il = vget_low_s16(ins), ih = vget_high_s16(ins);
    const int16_t p2 = out[-2], p1 = out[-1];
    int32x4_t lo = vmull_n_s16(vget_low_s16(k->c[0]), p2), hi = vmull_n_s16(vget_high_s16(k->c[0]), p2);
    lo = vmlal_n_s16(lo, vget_low_s16(k->c[1]), p1); hi = vmlal_n_s16(hi, vget_high_s16(k->c[1]), p1);
#define HH_AD(J, V, L) lo = vmlal_lane_s16(lo, vget_low_s16(k->c[2 + J]), V, L); hi = vmlal_lane_s16(hi, vget_high_s16(k->c[2 + J]), V, L);
    HH_AD(0, il, 0) HH_AD(1, il, 1) HH_AD(2, il, 2) HH_AD(3, il, 3)
#undef HH_AD
#define HH_AD(J, V, L) hi = vmlal_lane_s16(hi, vget_high_s16(k->c[2 + J]), V, L); /* columns 4-7 are 0 for outputs 0-3 */
    HH_AD(4, ih, 0) HH_AD(5, ih, 1) HH_AD(6, ih, 2) HH_AD(7, ih, 3)
#undef HH_AD
    vst1q_s16(out, vcombine_s16(vqshrn_n_s32(lo, 11), vqshrn_n_s32(hi, 11)));
}
#endif

#endif
