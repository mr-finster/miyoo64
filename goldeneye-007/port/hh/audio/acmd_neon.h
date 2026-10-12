/* ADPCM decode and resample as plain kernels, C and NEON, so tools/mixer_test.c
 * (run under qemu-arm) can prove them equal on random buffers, and
 * acmd_mixer.c can use the NEON ones. The C kernels are acmd_mixer.c's loops
 * verbatim; the NEON ones are the same integer arithmetic (the OoT port 2b629ab,
 * hh_mixer_kernels.h, adapted to DKR's 4-bit-only decoder and do-while
 * resampler). The port builds with -fwrapv: s32 sums wrap, in any order. */
#ifndef ACMD_NEON_H
#define ACMD_NEON_H

#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#define ACMD_NEON 1
#endif

static inline int16_t acmd_clamp16(int32_t v) {
    return (int16_t) (v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

/* One ADPCM frame (1 header byte + 8 data bytes -> 16 samples) from `in` to
 * out[0..15], reading out[-2], out[-1]; tables[8][2][8]. Returns in + 9. */
static inline const uint8_t *acmd_adpcm_frame_c(const uint8_t *in, int16_t *out, const int16_t (*tables)[2][8]) {
    int shift = *in >> 4;
    int table_index = *in++ & 0xf;
    const int16_t (*tbl)[8] = tables[table_index];
    int i;

    for (i = 0; i < 2; i++) {
        int16_t ins[8];
        int16_t prev1 = out[-1];
        int16_t prev2 = out[-2];
        int j, k;

        for (j = 0; j < 4; j++) {
            int32_t hi = *in >> 4;
            int32_t lo = *in++ & 0xF;

            if (hi & 8) hi -= 16;
            if (lo & 8) lo -= 16;
            ins[j * 2] = (int16_t) (hi << shift);
            ins[j * 2 + 1] = (int16_t) (lo << shift);
        }
        for (j = 0; j < 8; j++) {
            int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);

            for (k = 0; k < j; k++) {
                acc += tbl[1][((j - k) - 1)] * ins[k];
            }
            acc >>= 11;
            *out++ = acmd_clamp16(acc);
        }
    }
    return in;
}

/* `n` outputs (a multiple of 8, or the do-while's 8 when nbytes <= 0).
 * Returns the input pointer after the last step. */
static inline const int16_t *acmd_resample_c(const int16_t *in, int16_t *out, int n, const int16_t (*table)[4],
                                             uint32_t *pacc, uint32_t step) {
    uint32_t pitch_accumulator = *pacc;
    int i;
    for (i = 0; i < n; i++) {
        const int16_t *tbl = table[pitch_accumulator * 64 >> 16];
        int32_t sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                         ((in[1] * tbl[1] + 0x4000) >> 15) +
                         ((in[2] * tbl[2] + 0x4000) >> 15) +
                         ((in[3] * tbl[3] + 0x4000) >> 15);

        *out++ = acmd_clamp16(sample);

        pitch_accumulator += step;
        in += pitch_accumulator >> 16;
        pitch_accumulator %= 0x10000;
    }
    *pacc = pitch_accumulator;
    return in;
}

#if ACMD_NEON
#ifdef ACMD_MUTATE /* tools/mixer_test.c: a planted bug the test must catch */
#define ACMD_ONE11 2047
#else
#define ACMD_ONE11 2048
#endif
/* ADPCM: the decoded values (a signed nibble << shift, kept to 16 bits as the
 * C's (s16) cast does -- vshl in s16 lanes drops the same high bits) and the
 * previous samples are 16-bit, so a half-frame of 8 outputs is an 8x10 matrix
 * product: output k is
 *   t0[k]*prev2 + t1[k]*prev1 + ins[k]*2048 + sum_{j<k} t1[k-1-j]*ins[j]
 * i.e. the column of ins[j] is P[8-j .. 15-j] of P = {0 x8, 2048, t1[0..6]}.
 * clamp16(acc >> 11) is vqshrn #11 (truncating shift, saturating narrow). */
typedef struct { int16x8_t c[10]; const int16_t *key; } AcmdAdpcmCols;
static inline void acmd_adpcm_cols(AcmdAdpcmCols *k, const int16_t (*tbl)[8]) {
    int16_t P[16];
    int j;
    memset(P, 0, sizeof(P));
    P[8] = ACMD_ONE11;
    memcpy(&P[9], tbl[1], 7 * sizeof(int16_t));
    k->c[0] = vld1q_s16(tbl[0]);
    k->c[1] = vld1q_s16(tbl[1]);
    for (j = 0; j < 8; j++) k->c[2 + j] = vld1q_s16(&P[8 - j]);
    k->key = &tbl[0][0];
}
/* 8 inputs from 4 bytes, high nibble first */
static inline int16x8_t acmd_adpcm_ins(const uint8_t *in, int shift) {
    uint32_t w;
    memcpy(&w, in, 4);
    const uint8x8_t b = vreinterpret_u8_u32(vdup_n_u32(w));
    const uint8x8x2_t z = vzip_u8(vshr_n_u8(b, 4), vand_u8(b, vdup_n_u8(0xF))); /* hi0 lo0 hi1 lo1 ... */
    const int8x8_t v = vshr_n_s8(vreinterpret_s8_u8(vshl_n_u8(z.val[0], 4)), 4); /* sign-extend 4 bits */
    return vshlq_s16(vmovl_s8(v), vdupq_n_s16((int16_t) shift));
}
/* one half: out[0..7] from out[-2], out[-1] and ins */
static inline void acmd_adpcm_half(int16_t *out, const AcmdAdpcmCols *k, int16x8_t ins) {
    const int16x4_t il = vget_low_s16(ins), ih = vget_high_s16(ins);
    const int16_t p2 = out[-2], p1 = out[-1];
    int32x4_t lo = vmull_n_s16(vget_low_s16(k->c[0]), p2), hi = vmull_n_s16(vget_high_s16(k->c[0]), p2);
    lo = vmlal_n_s16(lo, vget_low_s16(k->c[1]), p1); hi = vmlal_n_s16(hi, vget_high_s16(k->c[1]), p1);
#define ACMD_AD(J, V, L) lo = vmlal_lane_s16(lo, vget_low_s16(k->c[2 + J]), V, L); hi = vmlal_lane_s16(hi, vget_high_s16(k->c[2 + J]), V, L);
    ACMD_AD(0, il, 0) ACMD_AD(1, il, 1) ACMD_AD(2, il, 2) ACMD_AD(3, il, 3)
#undef ACMD_AD
#define ACMD_AD(J, V, L) hi = vmlal_lane_s16(hi, vget_high_s16(k->c[2 + J]), V, L); /* columns 4-7 are 0 for outputs 0-3 */
    ACMD_AD(4, ih, 0) ACMD_AD(5, ih, 1) ACMD_AD(6, ih, 2) ACMD_AD(7, ih, 3)
#undef ACMD_AD
    vst1q_s16(out, vcombine_s16(vqshrn_n_s32(lo, 11), vqshrn_n_s32(hi, 11)));
}
static inline const uint8_t *acmd_adpcm_frame_neon(const uint8_t *in, int16_t *out, const int16_t (*tables)[2][8],
                                                   AcmdAdpcmCols *cols) {
    const int shift = *in >> 4, idx = *in & 0xf;
    if (idx >= 8) return acmd_adpcm_frame_c(in, out, tables); /* past the 8-entry book: whatever memory follows,
                                                                 read when the C reads it (never in valid data) */
    const int16_t (*tbl)[8] = tables[idx];
    in++;
    if (cols->key != &tbl[0][0]) acmd_adpcm_cols(cols, tbl);
    acmd_adpcm_half(out, cols, acmd_adpcm_ins(in, shift));
    acmd_adpcm_half(out + 8, cols, acmd_adpcm_ins(in + 4, shift));
    return in + 8;
}

/* Resample: per output the four taps times one table row, each product
 * rounded ((x*c + 0x4000) >> 15 == vrshr #15; no s32 overflow), the four
 * summed with two vpadd (exact in s32), clamp16 = vqmovn. Two outputs a pass;
 * `n` is even. */
static inline const int16_t *acmd_resample_neon(const int16_t *in, int16_t *out, int n, const int16_t (*table)[4],
                                                uint32_t *pacc, uint32_t step) {
    uint32_t pa = *pacc;
    int i;
    for (i = 0; i < n; i += 2) {
#ifdef ACMD_MUTATE
        const int32x4_t pA = vshrq_n_s32(vmull_s16(vld1_s16(in), vld1_s16(table[(pa * 64) >> 16])), 15);
#else
        const int32x4_t pA = vrshrq_n_s32(vmull_s16(vld1_s16(in), vld1_s16(table[(pa * 64) >> 16])), 15);
#endif
        pa += step; in += pa >> 16; pa &= 0xFFFF;
        const int32x4_t pB = vrshrq_n_s32(vmull_s16(vld1_s16(in), vld1_s16(table[(pa * 64) >> 16])), 15);
        pa += step; in += pa >> 16; pa &= 0xFFFF;
        const int32x2_t a2 = vpadd_s32(vget_low_s32(pA), vget_high_s32(pA)); /* [p0+p1, p2+p3] */
        const int32x2_t b2 = vpadd_s32(vget_low_s32(pB), vget_high_s32(pB));
        const int16x4_t r = vqmovn_s32(vcombine_s32(vpadd_s32(a2, b2), vdup_n_s32(0)));
        vst1_lane_s32((int32_t *) (out + i), vreinterpret_s32_s16(r), 0);
    }
    *pacc = pa;
    return in;
}
#endif

#endif
