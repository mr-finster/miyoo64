/* mixer_test.c: the NEON ADPCM and resample kernels (port/audio/acmd_neon.h)
 * against the C loops on random buffers. Build and run with tools/mixer_test.sh
 * (ARM cross compiler + qemu-arm). -DACMD_MUTATE plants two bugs (2048 -> 2047,
 * vrshr -> vshr) and the test must then report mismatches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../port/audio/acmd_neon.h"

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int16_t rnd16(void) {
    switch (rnd() & 3) { /* bias toward the extremes that make clamps fire */
        case 0: return (rnd() & 1) ? 32767 : -32768;
        case 1: return (int16_t) ((int32_t) (rnd() & 0x3ff) - 512);
        default: return (int16_t) rnd();
    }
}

int main(int argc, char **argv) {
    const int runs = argc > 1 ? atoi(argv[1]) : 200000;
    static int16_t tables[16][2][8], rtab[64][4]; /* 8-15: what an out-of-range index reads */
    static uint8_t in8[9 * 20];
    static int16_t oc[16 + 320], on[16 + 320], rin[1024], rc[320], rn[320];
    long bad_adpcm = 0, bad_res = 0;
    int r, i;
#if !ACMD_NEON
    puts("not an ARM NEON build"); return 2;
#else
    for (r = 0; r < runs; r++) {
        /* ADPCM: 1..20 frames, random book, random history */
        const int frames = 1 + rnd() % 20;
        const uint8_t *pc, *pn;
        AcmdAdpcmCols cols; cols.key = NULL;
        for (i = 0; i < 16 * 2 * 8; i++) (&tables[0][0][0])[i] = rnd16();
        for (i = 0; i < frames * 9; i++) in8[i] = (uint8_t) rnd();
        for (i = 0; i < 16; i++) oc[i] = on[i] = rnd16();
        pc = pn = in8;
        for (i = 0; i < frames; i++) {
            pc = acmd_adpcm_frame_c(pc, oc + 16 + i * 16, (const int16_t (*)[2][8]) tables);
            pn = acmd_adpcm_frame_neon(pn, on + 16 + i * 16, (const int16_t (*)[2][8]) tables, &cols);
        }
        if (pc != pn || memcmp(oc, on, (16 + frames * 16) * sizeof(int16_t))) bad_adpcm++;

        /* Resample: n in 8..160 (even), any pitch, random table and input */
        {
            const int n = 8 * (1 + rnd() % 20);
            const uint32_t step = (uint32_t) (rnd() & 0xFFFF) << 1;
            uint32_t ac = rnd() & 0xFFFF, an = ac;
            const int16_t *ec, *en;
            for (i = 0; i < 64 * 4; i++) (&rtab[0][0])[i] = rnd16();
            for (i = 0; i < 1024; i++) rin[i] = rnd16();
            ec = acmd_resample_c(rin, rc, n, (const int16_t (*)[4]) rtab, &ac, step);
            en = acmd_resample_neon(rin, rn, n, (const int16_t (*)[4]) rtab, &an, step);
            if (ec != en || ac != an || memcmp(rc, rn, n * sizeof(int16_t))) bad_res++;
        }
    }
    printf("adpcm: %d runs, %ld mismatches\nresample: %d runs, %ld mismatches\n", runs, bad_adpcm, runs, bad_res);
    return (bad_adpcm || bad_res) ? 1 : 0;
#endif
}
