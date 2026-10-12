/* Paper Mario's audio microcode (n_aspMain) in C, for the native port (M2). Host side.
 *
 * The game builds a command list per audio frame (alAudioFrame, src/audio/core/syn_driver.c) and the N64's RSP ran
 * it: 184-sample chunks decoded from ADPCM, resampled, mixed through volume ramps into dry and wet (reverb) buses,
 * the reverb's delay lines and pole filter, and the final stereo interleave into the output buffer. Here the list
 * runs at once over a 4 KB DMEM image. Everything is in the host's byte order: the instrument banks' codebooks and
 * loop states are swapped when they load (port_fix_audio.c) and the compressed samples are bytes.
 *
 * The n_ commands address DMEM explicitly (unlike aspMain's SETBUFF) but some work on fixed buffers (the game's
 * N_AL_* layout, src/audio.h): the decoder reads at 0, the envelope mixer reads 184 samples at 0 and adds to the dry
 * L/R and wet L/R buses at 0x4E0/0x650/0x7C0/0x930, INTERLEAVE takes the dry pair to 0, MIXER works on 184 samples.
 * A resampler's or pole filter's buffer is given as an index (the game passes dmem >> 8): n * 0x170.
 *
 * The algorithms (ADPCM with the 8-tap codebook prediction, the 4-tap resampler table, the saturating mixer) are
 * the sm64 PC port's mixer.c (super-mario-64's port/src/pc/mixer.c, scalar paths); the n_ envelope mixer ramps
 * linearly (the game's _getRate: a 16.16 step per 8 samples toward the target) and the pole filter is the libultra
 * reverb's 2-pole low-pass. Addresses the game packs into 24 bits (ADPCM and resampler state in w0, the pole
 * filter's state) are host pointers inside the audio heap: its top byte is put back (port_mixer_set_heap). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define MIX_NEON 1
/* PM64_MIXNEON=0: the scalar loops (the A/B; the NEON ones give the same samples, PM64_WAV compares) */
static int mix_neon(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PM64_MIXNEON"); on = !(e && e[0] == '0'); }
    return on;
}
#endif

#define DMEM_SIZE 0x1000
#define NSAMP 184               /* AUDIO_SAMPLES */
#define NBYTES (NSAMP * 2)      /* 0x170 */
#define DRY_L 0x4E0
#define DRY_R 0x650
#define WET_L 0x7C0
#define WET_R 0x930

enum { A_SPNOOP, A_ADPCM, A_CLEARBUFF, A_ENVMIXER, A_LOADBUFF, A_RESAMPLE, A_SAVEBUFF, A_SEGMENT, A_SETBUFF, A_SETVOL,
       A_DMEMMOVE, A_LOADADPCM, A_MIXER, A_INTERLEAVE, A_POLEF, A_SETLOOP };
#define F_INIT 0x01
#define F_LOOP 0x02
#define F_LEFT 0x02
#define F_VOL 0x04

static union { int16_t s16[DMEM_SIZE / 2 + 32]; uint8_t u8[DMEM_SIZE + 64]; } sDmem;
static int16_t sBook[8][2][8];      /* the ADPCM codebook / pole filter coefficients */
static const int16_t *sLoopState;   /* SETLOOP: 16 samples */
static int16_t sVol[2], sTarget[2], sDry, sWet;
static int32_t sRate[2];
static uint32_t sHeapLo, sHeapHi;   /* the audio heap: where 24-bit addresses point */
static int sBadAddr;

#define D8(a) (sDmem.u8 + ((a) & (DMEM_SIZE - 1)))
#define D16(a) ((int16_t *) D8(a))

static inline int16_t clamp16(int32_t v) { return (int16_t) (v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v); }

void port_mixer_set_heap(void *base, uint32_t size) {
    sHeapLo = (uint32_t) (uintptr_t) base;
    sHeapHi = sHeapLo + size;
    if ((sHeapLo >> 24) != ((sHeapHi - 1) >> 24))
        port_log("mixer: the audio heap %08X..%08X crosses a 16 MB line: 24-bit state addresses are ambiguous\n",
                 (unsigned) sHeapLo, (unsigned) sHeapHi);
}

/* a 24-bit address the game packed (osVirtualToPhysical is the identity) -> the host pointer in the audio heap */
static void *addr24(uint32_t a) {
    uint32_t p = (sHeapLo & 0xFF000000u) | (a & 0xFFFFFFu);
    if (p < sHeapLo || p >= sHeapHi) {
        p = ((sHeapHi - 1) & 0xFF000000u) | (a & 0xFFFFFFu);
        if ((p < sHeapLo || p >= sHeapHi) && sBadAddr++ < 8) port_log("mixer: state address %06X is outside the audio heap\n", (unsigned) a);
    }
    return (void *) (uintptr_t) p;
}

/* ---------------------------------------------------------------- ADPCM */
/* c bytes of samples to (d): first the 16 samples of history (the state, the loop state or zeros), then 16 per
 * 9-byte frame read from DMEM a (the load's misalignment). The last 16 go back to the state. */
static void adpcm(uint8_t flags, uint32_t c, uint32_t a, uint32_t d, int16_t *state) {
    const uint8_t *in = D8(a);
    int16_t *out = D16(d);
    int n = (int) ((c + 31) & ~31u);
    if (flags & F_INIT) memset(out, 0, 32);
    else if ((flags & F_LOOP) && sLoopState) memcpy(out, sLoopState, 32);
    else memcpy(out, state, 32);
    out += 16;
    while (n > 0) {
        const int shift = *in >> 4, idx = *in++ & 7;
        int16_t (*tbl)[8] = sBook[idx];
        for (int half = 0; half < 2; half++) {
            int16_t ins[8];
            const int16_t prev1 = out[-1], prev2 = out[-2];
            for (int j = 0; j < 4; j++) {
                ins[j * 2] = (int16_t) (((int32_t) ((*in >> 4) << 28) >> 28) << shift);
                ins[j * 2 + 1] = (int16_t) (((int32_t) ((*in++ & 0xF) << 28) >> 28) << shift);
            }
            for (int j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
                for (int k = 0; k < j; k++) acc += tbl[1][(j - k) - 1] * ins[k];
                *out++ = clamp16(acc >> 11);
            }
        }
        n -= 32;
    }
    memcpy(state, out - 16, 32);
}

/* ---------------------------------------------------------------- resampler */
static const int16_t kResample[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8}, {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6}, {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a}, {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56}, {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e}, {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8}, {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd}, {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab}, {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0}, {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f}, {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac}, {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae}, {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a}, {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435}, {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4}, {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44}, {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}};

/* 184 samples out at (o), pitch in UQ1.15, from (i) with 4 samples of history before it; the state keeps the history,
 * the fraction and the input's alignment (the sm64 port's aResampleImpl) */
static void resample(uint8_t flags, uint16_t pitch, uint32_t i, uint32_t o, int16_t *state) {
    int16_t tmp[16];
    int16_t *in_initial = D16(i), *in = in_initial, *out = D16(o);
    int n = NBYTES;
    uint32_t acc;
    if (flags & F_INIT) memset(tmp, 0, 5 * sizeof(int16_t));
    else memcpy(tmp, state, 16 * sizeof(int16_t));
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / (int) sizeof(int16_t);
    }
    in -= 4;
    acc = (uint16_t) tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));
    do {
        for (int k = 0; k < 8; k++) {
            const int16_t *t = kResample[acc * 64 >> 16];
            const int32_t s = ((in[0] * t[0] + 0x4000) >> 15) + ((in[1] * t[1] + 0x4000) >> 15) +
                              ((in[2] * t[2] + 0x4000) >> 15) + ((in[3] * t[3] + 0x4000) >> 15);
            *out++ = clamp16(s);
            acc += (uint32_t) pitch << 1;
            in += acc >> 16;
            acc &= 0xFFFF;
        }
        n -= 16;
    } while (n > 0);
    state[4] = (int16_t) acc;
    memcpy(state, in, 4 * sizeof(int16_t));
    int al = (int) (in - in_initial + 4) & 7;
    in -= al;
    if (al != 0) al = -8 - al;
    state[5] = (int16_t) al;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}

/* ---------------------------------------------------------------- envelope mixer */
/* 184 samples at 0 into the dry and wet buses, each channel's volume stepping linearly toward its target (16.16,
 * the game's rate per 8 samples) and scaled by the dry and wet amounts. The state (the game's 80-byte ENVMIX_STATE)
 * keeps the ramps and amounts between frames. */
typedef struct { int32_t value, target, step; } Ramp;
typedef struct { Ramp ramp[2]; int16_t dry, wet; } EnvState; /* 28 bytes of the game's 80 */

static inline int16_t ramp_step(Ramp *r) {
    r->value += r->step;
    if (r->step <= 0 ? r->value <= r->target : r->value >= r->target) { r->value = r->target; r->step = 0; }
    return (int16_t) (r->value >> 16);
}

#ifdef MIX_NEON
/* the NEON loop's range: volumes >= 0, and value + 8 x step inside the int range from either end of the ramp */
static int ramp_fits(const Ramp *r) {
    const int64_t hi = r->value > r->target ? r->value : r->target, st = r->step < 0 ? -(int64_t) r->step : r->step;
    return r->value >= 0 && r->target >= 0 && hi + 8 * st <= INT32_MAX;
}
#endif
static void envmixer(uint8_t flags, EnvState *st) {
    EnvState s;
    if (flags & F_INIT) {
        for (int c = 0; c < 2; c++) {
            s.ramp[c].value = (int32_t) sVol[c] << 16;
            s.ramp[c].target = (int32_t) sTarget[c] << 16;
            s.ramp[c].step = sRate[c] / 8;
        }
        s.dry = sDry;
        s.wet = sWet;
    } else {
        s = *st;
    }
    const int16_t *in = D16(0);
    int16_t *dl = D16(DRY_L), *dr = D16(DRY_R), *wl = D16(WET_L), *wr = D16(WET_R);
#ifdef MIX_NEON
    /* 8 samples a step (the heavy effects' round: the mixer was 8% of the fire breath's frame). A ramp's 8 values are
     * value + j x step, held at the target (max/min: once there it stays there, as ramp_step). With every volume and
     * amount >= 0 and no sum past the int range (ramp_fits) each product fits: vqrdmulh is (a x b + 0x4000) >> 15
     * exactly and never saturates, vqadd is clamp16(a + b). Otherwise the scalar loop below. */
    if (mix_neon() && s.dry >= 0 && s.wet >= 0 && ramp_fits(&s.ramp[0]) && ramp_fits(&s.ramp[1])) {
        static const int32_t kJ[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        const int32x4_t j0 = vld1q_s32(kJ), j1 = vld1q_s32(kJ + 4);
        const int16x8_t vdry = vdupq_n_s16(s.dry), vwet = vdupq_n_s16(s.wet);
        for (int k = 0; k < NSAMP; k += 8) {
            int16x8_t g[2];
            for (int c = 0; c < 2; c++) {
                Ramp *r = &s.ramp[c];
                const int32x4_t v0 = vdupq_n_s32(r->value), t = vdupq_n_s32(r->target);
                int32x4_t a = vqaddq_s32(v0, vmulq_n_s32(j0, r->step)), b = vqaddq_s32(v0, vmulq_n_s32(j1, r->step));
                if (r->step <= 0) { a = vmaxq_s32(a, t); b = vmaxq_s32(b, t); }
                else { a = vminq_s32(a, t); b = vminq_s32(b, t); }
                r->value = vgetq_lane_s32(b, 3);
                if (r->value == r->target) r->step = 0;
                g[c] = vcombine_s16(vshrn_n_s32(a, 16), vshrn_n_s32(b, 16));
            }
            const int16x8_t x = vld1q_s16(in + k);
            const int16x8_t gdl = vqrdmulhq_s16(g[0], vdry), gdr = vqrdmulhq_s16(g[1], vdry);
            const int16x8_t gwl = vqrdmulhq_s16(g[0], vwet), gwr = vqrdmulhq_s16(g[1], vwet);
            vst1q_s16(dl + k, vqaddq_s16(vld1q_s16(dl + k), vqrdmulhq_s16(x, gdl)));
            vst1q_s16(dr + k, vqaddq_s16(vld1q_s16(dr + k), vqrdmulhq_s16(x, gdr)));
            vst1q_s16(wl + k, vqaddq_s16(vld1q_s16(wl + k), vqrdmulhq_s16(x, gwl)));
            vst1q_s16(wr + k, vqaddq_s16(vld1q_s16(wr + k), vqrdmulhq_s16(x, gwr)));
        }
        *st = s;
        return;
    }
#endif
    for (int k = 0; k < NSAMP; k++) {
        const int32_t l = ramp_step(&s.ramp[0]), r = ramp_step(&s.ramp[1]), x = in[k];
        const int32_t gdl = clamp16((l * s.dry + 0x4000) >> 15), gdr = clamp16((r * s.dry + 0x4000) >> 15);
        const int32_t gwl = clamp16((l * s.wet + 0x4000) >> 15), gwr = clamp16((r * s.wet + 0x4000) >> 15);
        dl[k] = clamp16(dl[k] + ((x * gdl + 0x4000) >> 15));
        dr[k] = clamp16(dr[k] + ((x * gdr + 0x4000) >> 15));
        wl[k] = clamp16(wl[k] + ((x * gwl + 0x4000) >> 15));
        wr[k] = clamp16(wr[k] + ((x * gwr + 0x4000) >> 15));
    }
    *st = s;
}

/* ---------------------------------------------------------------- pole filter */
/* 184 samples at (d) in place: the reverb's 2-pole low-pass with the coefficients LOADADPCM put in the codebook's
 * first 16 entries (h1 = [0], h2 = [1], h2 scaled by the gain), two samples of history in the state */
static void polef(uint8_t flags, int16_t gain, uint32_t d, int16_t *state) {
    int16_t *dst = D16(d);
    const int16_t *h1 = sBook[0][0];
    int16_t h2[8], h2_before[8];
    int16_t l1 = (flags & F_INIT) ? 0 : state[2], l2 = (flags & F_INIT) ? 0 : state[3];
    for (int k = 0; k < 8; k++) {
        h2_before[k] = sBook[0][1][k];
        h2[k] = (int16_t) (((int32_t) sBook[0][1][k] * gain) >> 14);
    }
    for (int n = 0; n < NSAMP; n += 8, dst += 8) {
        int16_t frame[8];
        memcpy(frame, dst, sizeof frame);
        for (int k = 0; k < 8; k++) {
            int32_t acc = frame[k] * gain + h1[k] * l1 + h2_before[k] * l2;
            for (int j = 0; j < k; j++) acc += h2[j] * frame[k - 1 - j];
            dst[k] = clamp16(acc >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
    }
    memcpy(state, dst - 4, 8);
}

/* ---------------------------------------------------------------- the list */
void port_mixer_run(const uint32_t *cmds, int count) {
    for (int n = 0; n < count; n++, cmds += 2) {
        const uint32_t w0 = cmds[0], w1 = cmds[1];
        switch (w0 >> 24) {
            case A_SPNOOP:
            case A_SEGMENT:
            case A_SETBUFF:
                break;
            case A_ADPCM: /* w0: state (24 bits); w1: flags << 28 | count << 16 | align << 12 | out */
                adpcm((uint8_t) (w1 >> 28), (w1 >> 16) & 0xFFF, (w1 >> 12) & 0xF, w1 & 0xFFF, addr24(w0));
                break;
            case A_CLEARBUFF:
                memset(D8(w0 & 0xFFF), 0, (w1 + 15) & 0xFFF0);
                break;
            case A_ENVMIXER: /* w0: flags << 16 | right volume; w1: state */
                sVol[1] = (int16_t) w0;
                envmixer((uint8_t) (w0 >> 16), (EnvState *) (uintptr_t) w1);
                break;
            case A_LOADBUFF: /* w0: count << 12 | dmem; w1: address */
                memcpy(D8(w0 & 0xFFF), (const void *) (uintptr_t) w1, ((w0 >> 12) & 0xFFF));
                break;
            case A_RESAMPLE: /* w0: state (24 bits); w1: flags << 30 | pitch << 14 | in << 2 | out buffer index */
                resample((uint8_t) (w1 >> 30), (uint16_t) (w1 >> 14), (w1 >> 2) & 0xFFF, (w1 & 3) * NBYTES, addr24(w0));
                break;
            case A_SAVEBUFF:
                memcpy((void *) (uintptr_t) w1, D8(w0 & 0xFFF), ((w0 >> 12) & 0xFFF));
                break;
            case A_SETVOL: { /* A_RATE: left target and rate; A_VOL|A_LEFT: left volume, dry, wet; A_VOL: right target, rate */
                const uint8_t f = (uint8_t) (w0 >> 16);
                if (f & F_VOL) {
                    if (f & F_LEFT) { sVol[0] = (int16_t) w0; sDry = (int16_t) (w1 >> 16); sWet = (int16_t) w1; }
                    else { sTarget[1] = (int16_t) w0; sRate[1] = (int32_t) w1; }
                } else {
                    sTarget[0] = (int16_t) w0;
                    sRate[0] = (int32_t) w1;
                }
                break;
            }
            case A_DMEMMOVE: /* w0: in; w1: out << 16 | count */
                memmove(D8(w1 >> 16), D8(w0 & 0xFFF), ((w1 & 0xFFFF) + 15) & ~15u);
                break;
            case A_LOADADPCM: /* w0: bytes; w1: codebook */
                memcpy(sBook, (const void *) (uintptr_t) w1, (w0 & 0xFFFF) > sizeof sBook ? sizeof sBook : (w0 & 0xFFFF));
                break;
            case A_MIXER: { /* w0: gain; w1: in << 16 | out; 184 samples */
                const int16_t gain = (int16_t) w0;
                const int16_t *in = D16(w1 >> 16);
                int16_t *out = D16(w1 & 0xFFFF);
#ifdef MIX_NEON
                if (mix_neon()) { /* the same 32-bit sum (it wraps alike), then vqshrn = clamp16(sum >> 15) */
                    for (int k = 0; k < NSAMP; k += 8) {
                        const int16x8_t o = vld1q_s16(out + k), x = vld1q_s16(in + k);
                        int32x4_t lo = vmlal_n_s16(vmull_n_s16(vget_low_s16(o), 0x7FFF), vget_low_s16(x), gain);
                        int32x4_t hi = vmlal_n_s16(vmull_n_s16(vget_high_s16(o), 0x7FFF), vget_high_s16(x), gain);
                        lo = vaddq_s32(lo, vdupq_n_s32(0x4000)); hi = vaddq_s32(hi, vdupq_n_s32(0x4000));
                        vst1q_s16(out + k, vcombine_s16(vqshrn_n_s32(lo, 15), vqshrn_n_s32(hi, 15)));
                    }
                    break;
                }
#endif
                for (int k = 0; k < NSAMP; k++) out[k] = clamp16((out[k] * 0x7FFF + in[k] * gain + 0x4000) >> 15);
                break;
            }
            case A_INTERLEAVE: { /* the dry pair -> 0, 184 frames */
                int16_t tmp[NSAMP * 2];
                const int16_t *l = D16(DRY_L), *r = D16(DRY_R);
                for (int k = 0; k < NSAMP; k++) { tmp[k * 2] = l[k]; tmp[k * 2 + 1] = r[k]; }
                memcpy(D16(0), tmp, sizeof tmp);
                break;
            }
            case A_POLEF: /* w0: flags << 16 | gain; w1: buffer index << 24 | state (24 bits) */
                polef((uint8_t) (w0 >> 16), (int16_t) w0, (w1 >> 24) * NBYTES, addr24(w1));
                break;
            case A_SETLOOP:
                sLoopState = (const int16_t *) (uintptr_t) w1;
                break;
            default:
                break;
        }
    }
}
