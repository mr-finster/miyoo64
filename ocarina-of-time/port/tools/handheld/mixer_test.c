/* C vs NEON audio mixer kernels (src/port/hh/hh_mixer_kernels.h) on random
 * buffers, edge values forced in: every output sample must match.
 *   ./dev.sh 'sh /work/port/tools/handheld/mixer_test.sh'
 * (builds with the handheld toolchain and runs under qemu-arm). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/port/hh/hh_mixer_kernels.h"

#ifndef HH_MIXER_NEON
#error "build for ARM with NEON"
#endif

static uint32_t sSeed = 12345;
static uint32_t rnd(void) { sSeed = sSeed * 1664525u + 1013904223u; return sSeed >> 8; }
static int16_t rs16(void) {
    static const int16_t edge[] = { -32768, -32767, -1, 0, 1, 32766, 32767, 0x4000, -0x4000 };
    return (rnd() % 8 == 0) ? edge[rnd() % 9] : (int16_t)rnd();
}
static uint16_t ru16(void) {
    static const uint16_t edge[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF };
    return (rnd() % 6 == 0) ? edge[rnd() % 6] : (uint16_t)rnd();
}


/* OotPspMixer_Filter's block loop, verbatim (hh_mixer.c), as the reference for
 * hh_filter_neon. Built with -fwrapv like the port, so a wrapping sum is defined. */
static void filter_ref(const int16_t* input, int16_t* out, int32_t blocks, const int16_t taps[8], int16_t history[8]) {
    for (int32_t n = 0; n < blocks; n++) {
        int16_t blockInput[8];
        int32_t out1[8];
        memcpy(blockInput, input, sizeof(blockInput));
        out1[1] = history[0] * taps[6]; out1[1] += history[3] * taps[7]; out1[1] += history[2] * taps[4];
        out1[1] += history[5] * taps[5]; out1[1] += history[4] * taps[2]; out1[1] += history[7] * taps[3];
        out1[1] += history[6] * taps[0]; out1[1] += blockInput[1] * taps[1];
        out1[0] = history[3] * taps[6]; out1[0] += history[2] * taps[7]; out1[0] += history[5] * taps[4];
        out1[0] += history[4] * taps[5]; out1[0] += history[7] * taps[2]; out1[0] += history[6] * taps[3];
        out1[0] += blockInput[1] * taps[0]; out1[0] += blockInput[0] * taps[1];
        out1[3] = history[2] * taps[6]; out1[3] += history[5] * taps[7]; out1[3] += history[4] * taps[4];
        out1[3] += history[7] * taps[5]; out1[3] += history[6] * taps[2]; out1[3] += blockInput[1] * taps[3];
        out1[3] += blockInput[0] * taps[0]; out1[3] += blockInput[3] * taps[1];
        out1[2] = history[5] * taps[6]; out1[2] += history[4] * taps[7]; out1[2] += history[7] * taps[4];
        out1[2] += history[6] * taps[5]; out1[2] += blockInput[1] * taps[2]; out1[2] += blockInput[0] * taps[3];
        out1[2] += blockInput[3] * taps[0]; out1[2] += blockInput[2] * taps[1];
        out1[5] = history[4] * taps[6]; out1[5] += history[7] * taps[7]; out1[5] += history[6] * taps[4];
        out1[5] += blockInput[1] * taps[5]; out1[5] += blockInput[0] * taps[2]; out1[5] += blockInput[3] * taps[3];
        out1[5] += blockInput[2] * taps[0]; out1[5] += blockInput[5] * taps[1];
        out1[4] = history[7] * taps[6]; out1[4] += history[6] * taps[7]; out1[4] += blockInput[1] * taps[4];
        out1[4] += blockInput[0] * taps[5]; out1[4] += blockInput[3] * taps[2]; out1[4] += blockInput[2] * taps[3];
        out1[4] += blockInput[5] * taps[0]; out1[4] += blockInput[4] * taps[1];
        out1[7] = history[6] * taps[6]; out1[7] += blockInput[1] * taps[7]; out1[7] += blockInput[0] * taps[4];
        out1[7] += blockInput[3] * taps[5]; out1[7] += blockInput[2] * taps[2]; out1[7] += blockInput[5] * taps[3];
        out1[7] += blockInput[4] * taps[0]; out1[7] += blockInput[7] * taps[1];
        out1[6] = blockInput[1] * taps[6]; out1[6] += blockInput[0] * taps[7]; out1[6] += blockInput[3] * taps[4];
        out1[6] += blockInput[2] * taps[5]; out1[6] += blockInput[5] * taps[2]; out1[6] += blockInput[4] * taps[3];
        out1[6] += blockInput[7] * taps[0]; out1[6] += blockInput[6] * taps[1];
        for (int i = 0; i < 8; i++) out[i] = hh_clamp16((out1[i] + 0x4000) >> 15);
        memcpy(history, blockInput, sizeof(history[0]) * 8);
        input += 8;
        out += 8;
    }
}


/* OotPspMixer_Resample's loop, verbatim (hh_mixer.c), for hh_resample_neon. */
static int16_t rs_table[64][4];
static int32_t rs_product(int16_t sample, int16_t coefficient) { return (((int32_t)sample * coefficient) + 0x4000) >> 15; }
static const int16_t* resample_ref(const int16_t* in, int16_t* out, int32_t samples, uint32_t* pacc, uint16_t pitch) {
    uint32_t pitchAccumulator = *pacc;
    for (int32_t i = 0; i < samples; i++) {
        const int16_t* tbl = rs_table[(pitchAccumulator * 64) >> 16];
        int32_t product01 = rs_product(in[0], tbl[0]) + rs_product(in[1], tbl[1]);
        int32_t product23 = rs_product(in[2], tbl[2]) + rs_product(in[3], tbl[3]);
        *out++ = hh_clamp16(product01 + product23);
        pitchAccumulator += pitch << 1;
        in += pitchAccumulator >> 16;
        pitchAccumulator &= 0xFFFF;
    }
    *pacc = pitchAccumulator;
    return in;
}


/* OotPspMixer_ADPCMdec + DecodeAdpcmHalfReference, verbatim, for hh_adpcm_*. */
static int32_t sx2(uint8_t value, int32_t shift) { return (int32_t)((uint32_t)(value & 3) << 30) >> (30 - shift); }
static int32_t sx4(uint8_t value, int32_t shift) { return (int32_t)((uint32_t)(value & 0xF) << 28) >> (28 - shift); }
static void adpcm_half_ref(int16_t** outPtr, const int16_t table[2][8], const int32_t* ins) {
    int16_t* out = *outPtr;
    int16_t prev1 = out[-1], prev2 = out[-2];
    const int16_t* table0 = table[0];
    const int16_t* table1 = table[1];
    for (int k = 0; k < 8; k++) {
        int32_t acc = (table0[k] * prev2) + (table1[k] * prev1) + (ins[k] << 11);
        for (int j = 0; j < k; j++) acc += table1[k - 1 - j] * ins[j];
        *out++ = hh_clamp16(acc >> 11);
    }
    *outPtr = out;
}
static void adpcm_ref(const uint8_t* in, int16_t* out, int32_t nbytes, int flags, int16_t tables[8][2][8]) {
    while (nbytes > 0) {
        int32_t shift = *in >> 4;
        int32_t tableIndex = *in++ & 0xF;
        int16_t (*tbl)[8] = tables[tableIndex & 7];
        if (flags & 4) { if (shift > 14) shift = 14; } else if (shift > 12) shift = 12;
        for (int half = 0; half < 2; half++) {
            int32_t ins[8];
            if (flags & 4) {
                for (int j = 0; j < 2; j++) {
                    uint8_t packed = *in++;
                    ins[(j * 4) + 0] = sx2(packed >> 6, shift); ins[(j * 4) + 1] = sx2(packed >> 4, shift);
                    ins[(j * 4) + 2] = sx2(packed >> 2, shift); ins[(j * 4) + 3] = sx2(packed, shift);
                }
            } else {
                for (int j = 0; j < 4; j++) {
                    uint8_t packed = *in++;
                    ins[j * 2] = sx4(packed >> 4, shift); ins[(j * 2) + 1] = sx4(packed, shift);
                }
            }
            adpcm_half_ref(&out, (const int16_t (*)[8])tbl, ins);
        }
        nbytes -= 16 * 2;
    }
}
static void adpcm_neon(const uint8_t* in, int16_t* out, int32_t nbytes, int flags, int16_t tables[8][2][8]) {
    HhAdpcmCols cols;
    cols.key = NULL;
    while (nbytes > 0) {
        int32_t shift = *in >> 4;
        const int16_t (*tbl)[8] = (const int16_t (*)[8])tables[(*in++ & 0xF) & 7];
        if (flags & 4) { if (shift > 14) shift = 14; } else if (shift > 12) shift = 12;
        if (cols.key != &tbl[0][0]) hh_adpcm_cols(&cols, tbl);
        for (int half = 0; half < 2; half++) {
            hh_adpcm_half(out, &cols, hh_adpcm_ins(in, shift, (flags & 4) != 0));
            in += (flags & 4) ? 2 : 4;
            out += 8;
        }
        nbytes -= 16 * 2;
    }
}

#define N 256
int main(void) {
    int16_t in[N], d[4][N], c[4][N], o1[N], o2[N];
    long bad = 0, runs = 0;
    for (int it = 0; it < 200000; it++) {
        const int32_t samples = 16 * (1 + (int)(rnd() % (N / 16)));
        HhEnvMix e;
        e.volLeft0 = ru16(); e.volRight0 = ru16(); e.reverb0 = ru16();
        e.volLeft1 = ru16(); e.volRight1 = ru16(); e.reverb1 = ru16();
        e.rampLeft = rs16(); e.rampRight = rs16(); e.rampReverb = rs16();
        const int m = rnd() % 4 == 0;
        e.dryLeftMask = m && (rnd() & 1) ? -1 : 0; e.dryRightMask = m && (rnd() & 1) ? -1 : 0;
        e.wetLeftMask = m && (rnd() & 1) ? -4 : 0; e.wetRightMask = m && (rnd() & 1) ? -2 : 0;
        e.swapLR = rnd() & 1;
        for (int i = 0; i < N; i++) {
            in[i] = rs16();
            for (int k = 0; k < 4; k++) d[k][i] = c[k][i] = rs16();
        }
        hh_env_mix_c(in, c[0], c[1], c[2], c[3], samples, e);
        hh_env_mix_neon(in, d[0], d[1], d[2], d[3], samples, e);
        if (memcmp(c, d, sizeof(c)) != 0) { if (bad++ < 5) printf("envmix mismatch it %d\n", it); }
        const int16_t gain = rs16();
        for (int i = 0; i < N; i++) { o1[i] = o2[i] = rs16(); }
        hh_mix_c(in, o1, samples, gain);
        hh_mix_neon(in, o2, samples, gain);
        if (memcmp(o1, o2, sizeof(o1)) != 0) { if (bad++ < 5) printf("mix mismatch it %d gain %d\n", it, gain); }
        {   /* filter: random taps (full range) and history */
            int16_t taps[8], h1[8], h2[8];
            const int32_t blocks = samples / 8;
            for (int i = 0; i < 8; i++) { taps[i] = rs16(); h1[i] = h2[i] = rs16(); }
            for (int i = 0; i < N; i++) { o1[i] = o2[i] = 0; }
            filter_ref(in, o1, blocks, taps, h1);
            hh_filter_neon(in, o2, blocks, taps, h2);
            if (memcmp(o1, o2, sizeof(o1)) != 0 || memcmp(h1, h2, sizeof(h1)) != 0) {
                if (bad++ < 5) printf("filter mismatch it %d\n", it);
            }
        }
        {   /* resample: random table, pitch (up to ~2x), accumulator; input long enough for the stride */
            static int16_t src[4 * N + 16];
            int16_t r1[N], r2[N];
            for (int i = 0; i < 64; i++) for (int j = 0; j < 4; j++) rs_table[i][j] = rs16();
            for (int i = 0; i < 4 * N + 16; i++) src[i] = rs16();
            const uint16_t pitch = (uint16_t)(rnd() % 0x10000);
            uint32_t a1 = rnd() & 0xFFFF, a2 = a1;
            const int32_t ns = samples & ~1;
            const int16_t* e1 = resample_ref(src, r1, ns, &a1, pitch);
            const int16_t* e2 = hh_resample_neon(src, r2, ns, (const int16_t (*)[4])rs_table, &a2, (uint32_t)pitch << 1);
            if (memcmp(r1, r2, (size_t)ns * 2) != 0 || e1 != e2 || a1 != a2) {
                if (bad++ < 5) printf("resample mismatch it %d pitch %u\n", it, pitch);
            }
        }
        {   /* adpcm: random tables (full range), bytes, 4-bit and 2-bit frames */
            static int16_t tables[8][2][8];
            static uint8_t data[9 * 64];
            int16_t a1[16 + 16 * 32], a2[16 + 16 * 32];
            const int frames = 1 + (int)(rnd() % 32), flags = (rnd() & 1) ? 4 : 0;
            for (int i = 0; i < 8; i++) for (int j = 0; j < 2; j++) for (int m = 0; m < 8; m++) tables[i][j][m] = rs16();
            for (int i = 0; i < 9 * 64; i++) data[i] = (uint8_t)rnd();
            for (int i = 0; i < 16; i++) a1[i] = a2[i] = rs16();
            adpcm_ref(data, a1 + 16, frames * 32, flags, tables);
            adpcm_neon(data, a2 + 16, frames * 32, flags, tables);
            if (memcmp(a1, a2, (size_t)(16 + frames * 16) * 2) != 0) {
                if (bad++ < 5) printf("adpcm mismatch it %d flags %d\n", it, flags);
            }
        }
        runs++;
    }
    printf("%ld random runs (envmix + mix + filter + resample + adpcm), %ld mismatches\n", runs, bad);
    return bad != 0;
}
