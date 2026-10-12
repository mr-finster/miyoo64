/* NEON mixer bit-exactness test (round 8): envmix_neon vs envmix_c and the
 * NEON aMixImpl vs the scalar formula, 200k randomized cases with forced edge
 * values. Build and run under qemu-arm:
 *   $SDK/bin/arm-linux-gnueabihf-gcc -O2 -fwrapv -DPORT=1 -DN_MICRO=1 \
 *     -march=armv7ve+neon-vfpv4 -mfpu=neon-vfpv4 -Iport/hh/bs -Iport/hh -static \
 *     -o /tmp/mixtest tools/mixtest.c && qemu-arm /tmp/mixtest
 * Expect every mismatch count 0; with -DMUTATE the ADPCM and resample
 * counts must be non-zero (the test catches a one-step change). */
#include "../port/hh/bs/mixer.c"
#include <stdio.h>
#include <stdlib.h>

/* Verbatim copies of the pre-NEON scalar loops (round 12 baseline). -DMUTATE
 * plants a one-step change in each, which the test must catch. */
static void ref_adpcm(uint8_t flags, int16_t *state) {
	uint8_t *in = BUF_U8(rspa.in);
	int16_t *out = BUF_S16(rspa.out);
	int nbytes = ROUND_UP_32(rspa.nbytes);
	int i, j, k;

	if (flags & 0x01) { /* A_INIT */
		memset(out, 0, 16 * sizeof(int16_t));
	} else if (flags & 0x02) { /* A_LOOP */
		memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
	} else {
		memcpy(out, state, 16 * sizeof(int16_t));
	}
	out += 16;

	while (nbytes > 0) {
		int shift = *in >> 4;
		int table_index = *in++ & 0xf;
		int16_t (*tbl)[8] = rspa.adpcm_table[table_index];

		for (i = 0; i < 2; i++) {
			int16_t ins[8];
			int16_t prev1 = out[-1];
			int16_t prev2 = out[-2];

			if (flags & 4) {
				/* 2-bit ADPCM */
				unsigned rshift = (shift < 14) ? (14 - shift) : 0;
				for (j = 0; j < 2; j++) {
					uint8_t byte = *in++;
					ins[j * 4]     = adpcm_predict_sample(byte, 0xc0, 8, rshift);
					ins[j * 4 + 1] = adpcm_predict_sample(byte, 0x30, 10, rshift);
					ins[j * 4 + 2] = adpcm_predict_sample(byte, 0x0c, 12, rshift);
					ins[j * 4 + 3] = adpcm_predict_sample(byte, 0x03, 14, rshift);
				}
			} else {
				/* 4-bit ADPCM */
				unsigned rshift = (shift < 12) ? (12 - shift) : 0;
				for (j = 0; j < 4; j++) {
					uint8_t byte = *in++;
					ins[j * 2]     = adpcm_predict_sample(byte, 0xf0, 8, rshift);
					ins[j * 2 + 1] = adpcm_predict_sample(byte, 0x0f, 12, rshift);
				}
			}

			for (j = 0; j < 8; j++) {
				int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
				for (k = 0; k < j; k++) {
					acc += tbl[1][((j - k) - 1)] * ins[k];
				}
				acc >>= 11;
#ifdef MUTATE
				if (j == 5) acc += 1;
#endif
				*out++ = clamp16(acc);
			}
		}
		nbytes -= 16 * sizeof(int16_t);
	}
	memcpy(state, out - 16, 16 * sizeof(int16_t));
}

static void ref_resample(uint8_t flags, uint16_t pitch, int16_t *state) {
	int16_t tmp[16];
	int16_t *in_initial = BUF_S16(rspa.in);
	int16_t *in = in_initial;
	int16_t *out = BUF_S16(rspa.out);
	int nbytes = ROUND_UP_16(rspa.nbytes);
	uint32_t pitch_accumulator;
	int i;
	const int16_t *tbl;
	int32_t sample;

	if (flags & 0x01) { /* A_INIT */
		memset(tmp, 0, 5 * sizeof(int16_t));
	} else {
		memcpy(tmp, state, 16 * sizeof(int16_t));
	}
	if (flags & 0x02) { /* A_LOOP */
		memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
		in -= tmp[5] / (int)sizeof(int16_t);
	}
	in -= 4;
	pitch_accumulator = (uint16_t)tmp[4];
	memcpy(in, tmp, 4 * sizeof(int16_t));

	do {
		for (i = 0; i < 8; i++) {
			tbl = resample_table[pitch_accumulator * 64 >> 16];
			sample = ((int32_t)in[0] * tbl[0] +
			          (int32_t)in[1] * tbl[1] +
			          (int32_t)in[2] * tbl[2] +
			          (int32_t)in[3] * tbl[3]
#ifdef MUTATE
			          + 0x4000
#endif
			          ) >> 15;
			*out++ = clamp16(sample);

			pitch_accumulator += (pitch << 1);
			in += pitch_accumulator >> 16;
			pitch_accumulator %= 0x10000;
		}
		nbytes -= 8 * sizeof(int16_t);
	} while (nbytes > 0);

	state[4] = (int16_t)pitch_accumulator;
	memcpy(state, in, 4 * sizeof(int16_t));
	i = (int)(in - in_initial + 4) & 7;
	in -= i;
	if (i != 0) {
		i = -8 - i;
	}
	state[5] = (int16_t)i;
	memcpy(state + 8, in, 8 * sizeof(int16_t));
}

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int16_t r16(void) {
    switch (rnd() % 8) { case 0: return -32768; case 1: return 32767; case 2: return 0; case 3: return -1; default: return (int16_t)rnd(); }
}
static int32_t r32(void) {
    switch (rnd() % 10) { case 0: return INT32_MAX; case 1: return INT32_MIN; case 2: return 0; default: return (int32_t)rnd(); }
}
static uint8_t snap[DMEM_SIZE];
int main(void) {
    long bad = 0, tests = 0, steady = 0;
    for (int it = 0; it < 200000; it++) {
        for (int i = 0; i < DMEM_SIZE; i += 2) *(int16_t *)(rspa.buf + i) = r16();
        const int count = 8 * (1 + rnd() % 23);
        const int16_t *in = BUF_S16(0x5C0 + 16 * (rnd() % 4)); /* the voice's input buffer area */
        AudioRamp a[2], b[2];
        for (int c = 0; c < 2; c++) {
            a[c].target = (int32_t)r16() << 16;
            int mode = rnd() % 4;
            a[c].value = mode == 0 ? a[c].target : (mode == 1 ? (int32_t)r16() << 16 : r32());
            a[c].step = mode == 0 ? 0 : (rnd() % 3 == 0 ? 0 : r32() / (int32_t)(1 + rnd() % 4096));
        }
        if (a[0].step == 0 && a[0].value == a[0].target && a[1].step == 0 && a[1].value == a[1].target) steady++;
        int16_t dry = r16(), wet = r16();
        memcpy(b, a, sizeof a);
        memcpy(snap, rspa.buf, DMEM_SIZE);
        envmix_c(in, count, a, dry, wet);
        uint8_t outc[DMEM_SIZE]; memcpy(outc, rspa.buf, DMEM_SIZE);
        memcpy(rspa.buf, snap, DMEM_SIZE);
        envmix_neon(in, count, b, dry, wet);
        tests++;
        if (memcmp(outc, rspa.buf, DMEM_SIZE) || memcmp(a, b, sizeof a)) { if (bad++ < 5) printf("envmix mismatch it=%d\n", it); }
        /* aMix */
        memcpy(rspa.buf, snap, DMEM_SIZE);
        int16_t g = r16(); if (g == -32768) g = -32767;
        rspa.nbytes = (uint16_t)(count * 2);
        uint16_t ia = 0x100, oa = 0x400;
        { int16_t *ip = BUF_S16(ia), *op = BUF_S16(oa); for (int i = 0; i < count; i++) { int32_t smp = op[i] + (((int32_t)ip[i] * g) >> 15); op[i] = clamp16(smp); } }
        memcpy(outc, rspa.buf, DMEM_SIZE);
        memcpy(rspa.buf, snap, DMEM_SIZE);
        aMixImpl(0, g, ia, oa);
        if (memcmp(outc, rspa.buf, DMEM_SIZE)) { if (bad++ < 5) printf("amix mismatch it=%d\n", it); }
    }
    printf("tests %ld (steady %ld) mismatches %ld\n", tests, steady, bad);

    /* ADPCM and resample: whole commands on random DMEM, state and flags. */
    long abad = 0, rbad = 0;
    for (int it = 0; it < 200000; it++) {
        for (int i = 0; i < DMEM_SIZE; i += 2) *(int16_t *)(rspa.buf + i) = r16();
        for (int t = 0; t < 8; t++) for (int a2 = 0; a2 < 2; a2++) for (int k2 = 0; k2 < 8; k2++) rspa.adpcm_table[t][a2][k2] = r16();
        int16_t st1[16], st2[16], loop[16];
        for (int i = 0; i < 16; i++) { st1[i] = r16(); loop[i] = r16(); }
        memcpy(st2, st1, sizeof st1);
        rspa.adpcm_loop_state = loop;
        uint8_t fl = (uint8_t)(rnd() & 7);
        if ((fl & 3) == 3) fl &= ~2;
        rspa.in = (uint16_t)(0x100 + 2 * (rnd() % 16));
        rspa.out = 0x600;
        rspa.nbytes = (uint16_t)(32 * (1 + rnd() % 12));
        memcpy(snap, rspa.buf, DMEM_SIZE);
        ref_adpcm(fl, st1);
        uint8_t o1[DMEM_SIZE]; memcpy(o1, rspa.buf, DMEM_SIZE);
        memcpy(rspa.buf, snap, DMEM_SIZE);
        aADPCMdecImpl(fl, st2);
        if (memcmp(o1, rspa.buf, DMEM_SIZE) || memcmp(st1, st2, sizeof st1)) { if (abad++ < 5) printf("adpcm mismatch it=%d flags=%d\n", it, fl); }

        for (int i = 0; i < DMEM_SIZE; i += 2) *(int16_t *)(rspa.buf + i) = r16();
        for (int i = 0; i < 16; i++) st1[i] = r16();
        st1[5] = (int16_t)(-2 * (int)(rnd() % 8)); /* A_LOOP back-step, even bytes */
        memcpy(st2, st1, sizeof st1);
        fl = (uint8_t)(rnd() & 3);
        if (fl == 3) fl = 1; /* INIT|LOOP reads uninitialised tmp[] in the C: never issued */
        uint16_t pitch = (uint16_t)(rnd() % 3 == 0 ? rnd() : rnd() % 0x8000);
        rspa.in = (uint16_t)(0x200 + 16 * (rnd() % 8));
        rspa.out = 0xA00;
        rspa.nbytes = (uint16_t)(16 * (1 + rnd() % 11));
        memcpy(snap, rspa.buf, DMEM_SIZE);
        ref_resample(fl, pitch, st1);
        memcpy(o1, rspa.buf, DMEM_SIZE);
        memcpy(rspa.buf, snap, DMEM_SIZE);
        aResampleImpl(fl, pitch, st2);
        if (memcmp(o1, rspa.buf, DMEM_SIZE) || memcmp(st1, st2, sizeof st1)) { if (rbad++ < 5) printf("resample mismatch it=%d\n", it); }
    }
    printf("adpcm mismatches %ld, resample mismatches %ld\n", abad, rbad);
    bad += abad + rbad;

    return bad != 0;
}
