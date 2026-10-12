/* MK64's envelope mixer (mixer.c's NEW_AUDIO_UCODE aEnvMixerImpl) eight samples a
 * step in NEON -- the scalar loop's arithmetic lane for lane:
 *   (s16)(((s32)in * vol) >> 16) ^ neg   widen, vmulq_n_s32 (|in * vol| < 2^31),
 *                                        shift-narrow 16 (keeps bits 16..31 = the s16
 *                                        cast), then XOR on the s16 (the scalar XORs the
 *                                        s32 first: same low 16 bits)
 *   (wet_in * vol_wet) >> 16             the same, it already fits s16
 *   clamp16(buf + x)                     vqaddq_s16 (a sum of two s16)
 * Samples are independent, so one group per buffer at a time gives the scalar's
 * per-sample result also when two buffers are the same buffer (the stores keep its
 * dry left, dry right, wet left, wet right order); the caller keeps partial overlaps
 * on the scalar loop. The volumes step once per 8 samples, as there.
 * Tested against the scalar loop by bx/mixertest (random buffers under qemu-arm). */
#pragma once
#include <stdint.h>
#include <arm_neon.h>

static inline __attribute__((always_inline)) int16x8_t env_mulhi8(int16x8_t x, int32_t v) {
    return vcombine_s16(vshrn_n_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(x)), v), 16),
                        vshrn_n_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(x)), v), 16));
}

/* n: samples, a multiple of 8 (> 0). vols/vol_wet are stepped in place. */
static inline void env_mix_neon(const int16_t *in, int16_t *dl, int16_t *dr, int16_t *wl, int16_t *wr, int n,
                                uint16_t vols[2], const uint16_t rates[2], uint16_t *vol_wet, uint16_t rate_wet,
                                int16_t neg_l, int16_t neg_r, int swap) {
    const int16x8_t nl = vdupq_n_s16(neg_l), nr = vdupq_n_s16(neg_r);
    do {
        const int16x8_t x = vld1q_s16(in);
        const int16x8_t sl = veorq_s16(env_mulhi8(x, vols[0]), nl);
        const int16x8_t sr = veorq_s16(env_mulhi8(x, vols[1]), nr);
        const int16x8_t wetl = env_mulhi8(swap ? sr : sl, *vol_wet);
        const int16x8_t wetr = env_mulhi8(swap ? sl : sr, *vol_wet);
        vst1q_s16(dl, vqaddq_s16(vld1q_s16(dl), sl));
        vst1q_s16(dr, vqaddq_s16(vld1q_s16(dr), sr));
        vst1q_s16(wl, vqaddq_s16(vld1q_s16(wl), wetl));
        vst1q_s16(wr, vqaddq_s16(vld1q_s16(wr), wetr));
        in += 8; dl += 8; dr += 8; wl += 8; wr += 8;
        vols[0] += rates[0]; vols[1] += rates[1]; *vol_wet += rate_wet;
        n -= 8;
    } while (n > 0);
}

/* 1 when [a, a + bytes) and [b, b + bytes) overlap without being the same buffer */
static inline int env_partial_overlap(const void *a, const void *b, int bytes) {
    const uintptr_t x = (uintptr_t) a, y = (uintptr_t) b;
    return x != y && x < y + (uintptr_t) bytes && y < x + (uintptr_t) bytes;
}
