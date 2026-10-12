/* From BattleShip port/audio/mixer.c (MIT, see port/NOTICE.md), converted to C.
 * BattleShip's mixer is itself derived from the Starship PC port (below).
 *
 * SPDX-License-Identifier: MIT
 *
 * Portions of this file are derived from the Starship (Star Fox 64) PC port
 *   Copyright (c) The Harbour Masters
 *   https://github.com/HarbourMasters/Starship
 * Licensed under the MIT License; see port/NOTICE.md.
 */

/**
 * mixer.c — CPU-side Acmd audio interpreter for SSB64 PC port
 *
 * Replaces N64 RSP audio microcode with scalar CPU implementations.
 * Standard N64 ABI: 16 opcodes (A_SPNOOP through A_SETLOOP).
 *
 * Simulates the RSP's 4KB DMEM as a flat byte array. Each opcode
 * implementation reads/writes this buffer identically to the RSP
 * microcode, producing bit-exact audio output.
 *
 * Ported from Starship (Star Fox 64) PC port mixer.c, adapted for
 * SSB64's standard N64 audio ABI (stereo, no 5.1 surround).
 *
 * Key differences from Starship:
 *   - A_ENVMIXER (3): Standard N64 envelope mixer (stereo dry/wet),
 *     not Starship's extended 5.1 surround version (opcode 19)
 *   - A_SETVOL (9): Volume/target/rate setup (Starship uses EnvSetup1/2)
 *   - A_POLEF (14): IIR pole filter (Starship uses FIR A_FILTER/7)
 *   - A_MIXER uses rspa.nbytes, not an explicit count parameter
 *   - A_INTERLEAVE is stereo-only (2 channel)
 *   - A_LOADBUFF/A_SAVEBUFF use rspa.out/in from prior A_SETBUFF
 */

#include <stdint.h>
#include <string.h>
#include "audio/mixer.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* The decomp's include/string.h (first on the include path) lacks memmove. */
extern void *memmove(void *, const void *, size_t);

/* ------------------------------------------------------------------ */
/*  Rounding helpers                                                  */
/* ------------------------------------------------------------------ */

#define ROUND_UP_16(v)   (((v) + 15) & ~15)
#define ROUND_UP_32(v)   (((v) + 31) & ~31)
#define ROUND_UP_8(v)    (((v) + 7) & ~7)

/* ------------------------------------------------------------------ */
/*  Simulated DMEM — 4KB, matching N64 RSP data memory                */
/* ------------------------------------------------------------------ */

#define DMEM_SIZE 0x1000  /* 4096 bytes */

/* DMEM buffer starts at address 0 (standard N64 audio ucode).
 * No offset subtraction needed, unlike Starship's custom ucode. */
#define BUF_U8(a)  (rspa.buf + (a))
#define BUF_S16(a) ((int16_t*)(rspa.buf + (a)))

/* ------------------------------------------------------------------ */
/*  Mixer state — persists across Acmd calls within a frame           */
/* ------------------------------------------------------------------ */

static struct {
	/* Buffer parameters (set by A_SETBUFF) */
	uint16_t in;
	uint16_t out;
	uint16_t nbytes;

	/* Volume state (set by A_SETVOL, consumed by A_ENVMIXER) */
	int16_t  vol_left;
	int16_t  vol_right;
	int16_t  tgt_left;
	int16_t  tgt_right;
	int16_t  rate_left_m;   /* ramp rate mantissa */
	uint16_t rate_left_l;   /* ramp rate low bits */
	int16_t  rate_right_m;
	uint16_t rate_right_l;
	int16_t  dry_amt;
	int16_t  wet_amt;

	/* ADPCM state */
	int16_t *adpcm_loop_state;
	int16_t  adpcm_table[8][2][8];

	/* Pole filter coefficients (loaded via A_LOADADPCM for POLEF) */
	int16_t  polef_coefs[8];

	/* Simulated DMEM */
	uint8_t  buf[DMEM_SIZE];
} rspa;

/* ------------------------------------------------------------------ */
/*  Resample table — 64-entry 4-tap Lagrange interpolation            */
/* ------------------------------------------------------------------ */

static const int16_t resample_table[64][4] = {
	{ 0x0c39, 0x66ad, 0x0d46, 0xffdf }, { 0x0b39, 0x6696, 0x0e5f, 0xffd8 },
	{ 0x0a44, 0x6669, 0x0f83, 0xffd0 }, { 0x095a, 0x6626, 0x10b4, 0xffc8 },
	{ 0x087d, 0x65cd, 0x11f0, 0xffbf }, { 0x07ab, 0x655e, 0x1338, 0xffb6 },
	{ 0x06e4, 0x64d9, 0x148c, 0xffac }, { 0x0628, 0x643f, 0x15eb, 0xffa1 },
	{ 0x0577, 0x638f, 0x1756, 0xff96 }, { 0x04d1, 0x62cb, 0x18cb, 0xff8a },
	{ 0x0435, 0x61f3, 0x1a4c, 0xff7e }, { 0x03a4, 0x6106, 0x1bd7, 0xff71 },
	{ 0x031c, 0x6007, 0x1d6c, 0xff64 }, { 0x029f, 0x5ef5, 0x1f0b, 0xff56 },
	{ 0x022a, 0x5dd0, 0x20b3, 0xff48 }, { 0x01be, 0x5c9a, 0x2264, 0xff3a },
	{ 0x015b, 0x5b53, 0x241e, 0xff2c }, { 0x0101, 0x59fc, 0x25e0, 0xff1e },
	{ 0x00ae, 0x5896, 0x27a9, 0xff10 }, { 0x0063, 0x5720, 0x297a, 0xff02 },
	{ 0x001f, 0x559d, 0x2b50, 0xfef4 }, { 0xffe2, 0x540d, 0x2d2c, 0xfee8 },
	{ 0xffac, 0x5270, 0x2f0d, 0xfedb }, { 0xff7c, 0x50c7, 0x30f3, 0xfed0 },
	{ 0xff53, 0x4f14, 0x32dc, 0xfec6 }, { 0xff2e, 0x4d57, 0x34c8, 0xfebd },
	{ 0xff0f, 0x4b91, 0x36b6, 0xfeb6 }, { 0xfef5, 0x49c2, 0x38a5, 0xfeb0 },
	{ 0xfedf, 0x47ed, 0x3a95, 0xfeac }, { 0xfece, 0x4611, 0x3c85, 0xfeab },
	{ 0xfec0, 0x4430, 0x3e74, 0xfeac }, { 0xfeb6, 0x424a, 0x4060, 0xfeaf },
	{ 0xfeaf, 0x4060, 0x424a, 0xfeb6 }, { 0xfeac, 0x3e74, 0x4430, 0xfec0 },
	{ 0xfeab, 0x3c85, 0x4611, 0xfece }, { 0xfeac, 0x3a95, 0x47ed, 0xfedf },
	{ 0xfeb0, 0x38a5, 0x49c2, 0xfef5 }, { 0xfeb6, 0x36b6, 0x4b91, 0xff0f },
	{ 0xfebd, 0x34c8, 0x4d57, 0xff2e }, { 0xfec6, 0x32dc, 0x4f14, 0xff53 },
	{ 0xfed0, 0x30f3, 0x50c7, 0xff7c }, { 0xfedb, 0x2f0d, 0x5270, 0xffac },
	{ 0xfee8, 0x2d2c, 0x540d, 0xffe2 }, { 0xfef4, 0x2b50, 0x559d, 0x001f },
	{ 0xff02, 0x297a, 0x5720, 0x0063 }, { 0xff10, 0x27a9, 0x5896, 0x00ae },
	{ 0xff1e, 0x25e0, 0x59fc, 0x0101 }, { 0xff2c, 0x241e, 0x5b53, 0x015b },
	{ 0xff3a, 0x2264, 0x5c9a, 0x01be }, { 0xff48, 0x20b3, 0x5dd0, 0x022a },
	{ 0xff56, 0x1f0b, 0x5ef5, 0x029f }, { 0xff64, 0x1d6c, 0x6007, 0x031c },
	{ 0xff71, 0x1bd7, 0x6106, 0x03a4 }, { 0xff7e, 0x1a4c, 0x61f3, 0x0435 },
	{ 0xff8a, 0x18cb, 0x62cb, 0x04d1 }, { 0xff96, 0x1756, 0x638f, 0x0577 },
	{ 0xffa1, 0x15eb, 0x643f, 0x0628 }, { 0xffac, 0x148c, 0x64d9, 0x06e4 },
	{ 0xffb6, 0x1338, 0x655e, 0x07ab }, { 0xffbf, 0x11f0, 0x65cd, 0x087d },
	{ 0xffc8, 0x10b4, 0x6626, 0x095a }, { 0xffd0, 0x0f83, 0x6669, 0x0a44 },
	{ 0xffd8, 0x0e5f, 0x6696, 0x0b39 }, { 0xffdf, 0x0d46, 0x66ad, 0x0c39 },
};

/* ------------------------------------------------------------------ */
/*  Clamping helpers                                                  */
/* ------------------------------------------------------------------ */

static inline int16_t clamp16(int32_t v) {
	if (v < -0x8000) return -0x8000;
	if (v > 0x7fff) return 0x7fff;
	return (int16_t)v;
}

static inline int16_t adpcm_predict_sample(uint8_t byte, uint8_t mask,
                                           unsigned lshift, unsigned rshift) {
	int16_t sample = (uint16_t)(byte & mask) << lshift;
	return sample >> rshift;
}

/* ================================================================== */
/*  A_CLEARBUFF (opcode 2) — Zero a region of DMEM                    */
/* ================================================================== */

void aClearBufferImpl(uint16_t addr, int nbytes) {
	nbytes = ROUND_UP_16(nbytes);
	memset(BUF_U8(addr), 0, nbytes);
}

/* ================================================================== */
/*  A_SETBUFF (opcode 8) — Configure in/out/count for next command    */
/* ================================================================== */

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
	rspa.in = in;
	rspa.out = out;
	rspa.nbytes = nbytes;
}

/* ================================================================== */
/*  A_LOADBUFF (opcode 4) — Load from DRAM into DMEM                  */
/*  Uses rspa.out as dest DMEM addr, rspa.nbytes as count.            */
/* ================================================================== */

void aLoadBufferImpl(uintptr_t source_addr) {
	if (source_addr == 0) return;
	memcpy(BUF_U8(rspa.out), (const void*)source_addr, ROUND_UP_8(rspa.nbytes));
}

/* ================================================================== */
/*  A_SAVEBUFF (opcode 6) — Save from DMEM to DRAM                    */
/*  Uses rspa.in as source DMEM addr, rspa.nbytes as count.           */
/* ================================================================== */

void aSaveBufferImpl(uintptr_t dest_addr) {
	if (dest_addr == 0) return;
	memcpy((void*)dest_addr, BUF_U8(rspa.in), ROUND_UP_8(rspa.nbytes));
}

/* ================================================================== */
/*  A_DMEMMOVE (opcode 10) — Move data within DMEM                    */
/* ================================================================== */

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
	nbytes = ROUND_UP_16(nbytes);
	memmove(BUF_U8(out_addr), BUF_U8(in_addr), nbytes);
}

/* ================================================================== */
/*  A_LOADADPCM (opcode 11) — Load ADPCM codebook from DRAM           */
/* ================================================================== */

void aLoadADPCMImpl(int count, uintptr_t book_addr) {
	if (book_addr == 0) return;

	/* PORT: zero the table before copying so unused predictor slots
	 * default to zero coefficients instead of retaining a previous
	 * voice's book.  SSB64's books are uniformly order=2,
	 * npredictors=4 → count=128 bytes, which fills slots [0..3] of
	 * the 256-byte rspa.adpcm_table.  Without the memset, slots
	 * [4..7] keep whatever the previous voice's aLoadADPCM left
	 * there.  If a wave's ADPCM data ever encodes a frame whose
	 * table_index ∈ {4..7}, the decoder reads predictor coefficients
	 * from another sample → wildly wrong sample at the start of that
	 * frame.  Caused the Sector Z Arwing-whoosh + strong-hit SFX
	 * corruption (90% of jumps at ADPCM frame boundary, idx%16==15).
	 * See docs/bugs/audio_adpcm_codebook_stale_slots_2026-05-02.md.
	 *
	 * On N64 this was fine because the equivalent unused tail was
	 * deterministic-per-wave (the bytes immediately following the
	 * book in ROM, never read), not "previous voice's book." */
	if (count < 0) count = 0;
	if ((size_t)count > sizeof(rspa.adpcm_table)) {
		count = (int)sizeof(rspa.adpcm_table);
	}
	memset(rspa.adpcm_table, 0, sizeof(rspa.adpcm_table));
	memcpy(rspa.adpcm_table, (const void*)book_addr, count);
}

/* ================================================================== */
/*  A_SETLOOP (opcode 15) — Set ADPCM loop state pointer              */
/* ================================================================== */

void aSetLoopImpl(uintptr_t adpcm_loop_state) {
	rspa.adpcm_loop_state = (int16_t*)adpcm_loop_state;
}

/* ================================================================== */
/*  A_ADPCM (opcode 1) — ADPCM decompression                         */
/*  Scalar implementation from Starship.                              */
/* ================================================================== */

void aADPCMdecImpl(uint8_t flags, int16_t *state) {
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

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	/* NEON (ocarina-of-time 2b629ab, adapted): a half-frame of 8 outputs is an
	 * 8x10 matrix product -- output k = t0[k]*prev2 + t1[k]*prev1 + (ins[k] << 11)
	 * + sum_{j<k} t1[k-1-j]*ins[j] -- so the column of ins[j] is P[8-j .. 15-j]
	 * of P = {0 x8, 2048, t1[0..6]}. The s32 sums wrap like the C (-fwrapv) in
	 * any order; clamp16(acc >> 11) is vqshrn #11. The decoded input is the
	 * signed nibble (pair) << shift, with the shift capped at 12 (14) exactly
	 * as adpcm_predict_sample's rshift = 0 case does. */
	{
		const int two_bit = (flags & 4) != 0;
		const int16_t *key = NULL;
		int16x8_t c[10];
		while (nbytes > 0) {
			int shift = *in >> 4;
			const int16_t (*tbl)[8] = (const int16_t (*)[8])rspa.adpcm_table[*in++ & 0xf];
			if (two_bit ? shift > 14 : shift > 12) shift = two_bit ? 14 : 12;
			if (&tbl[0][0] != key) {
				int16_t P[16];
				memset(P, 0, 8 * sizeof(int16_t));
				P[8] = 2048;
				memcpy(&P[9], tbl[1], 7 * sizeof(int16_t));
				c[0] = vld1q_s16(tbl[0]);
				c[1] = vld1q_s16(tbl[1]);
				for (j = 0; j < 8; j++) c[2 + j] = vld1q_s16(&P[8 - j]);
				key = &tbl[0][0];
			}
			for (i = 0; i < 2; i++) {
				int8x8_t v;
				if (!two_bit) {
					uint32_t w;
					memcpy(&w, in, 4);
					in += 4;
					const uint8x8_t b = vreinterpret_u8_u32(vdup_n_u32(w));
					const uint8x8x2_t z = vzip_u8(vshr_n_u8(b, 4), vand_u8(b, vdup_n_u8(0xF))); /* hi0 lo0 hi1 lo1 .. */
					v = vshr_n_s8(vreinterpret_s8_u8(vshl_n_u8(z.val[0], 4)), 4);          /* sign-extend 4 bits */
				} else {
					const uint8x8_t b = vcreate_u8(((uint64_t)in[0] * 0x01010101ull) | ((uint64_t)in[1] * 0x0101010100000000ull));
					in += 2;
					v = vshr_n_s8(vreinterpret_s8_u8(vshl_u8(b, vcreate_s8(0x0604020006040200ull))), 6); /* top pair first */
				}
				const int16x8_t ins = vshlq_s16(vmovl_s8(v), vdupq_n_s16((int16_t)shift));
				const int16x4_t il = vget_low_s16(ins), ih = vget_high_s16(ins);
				const int16_t p2 = out[-2], p1 = out[-1];
				int32x4_t lo = vmull_n_s16(vget_low_s16(c[0]), p2), hi = vmull_n_s16(vget_high_s16(c[0]), p2);
				lo = vmlal_n_s16(lo, vget_low_s16(c[1]), p1);
				hi = vmlal_n_s16(hi, vget_high_s16(c[1]), p1);
#define HH_AD(J, V, L) lo = vmlal_lane_s16(lo, vget_low_s16(c[2 + J]), V, L); hi = vmlal_lane_s16(hi, vget_high_s16(c[2 + J]), V, L);
				HH_AD(0, il, 0) HH_AD(1, il, 1) HH_AD(2, il, 2) HH_AD(3, il, 3)
#undef HH_AD
#define HH_AD(J, V, L) hi = vmlal_lane_s16(hi, vget_high_s16(c[2 + J]), V, L); /* columns 4-7 are 0 for outputs 0-3 */
				HH_AD(4, ih, 0) HH_AD(5, ih, 1) HH_AD(6, ih, 2) HH_AD(7, ih, 3)
#undef HH_AD
				vst1q_s16(out, vcombine_s16(vqshrn_n_s32(lo, 11), vqshrn_n_s32(hi, 11)));
				out += 8;
			}
			nbytes -= 16 * sizeof(int16_t);
		}
		(void)k;
	}
#else
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
				*out++ = clamp16(acc);
			}
		}
		nbytes -= 16 * sizeof(int16_t);
	}
#endif
	memcpy(state, out - 16, 16 * sizeof(int16_t));
}

/* ================================================================== */
/*  A_RESAMPLE (opcode 5) — 4-tap interpolated resampling             */
/*  Scalar implementation from Starship.                              */
/* ================================================================== */

void aResampleImpl(uint8_t flags, uint16_t pitch, int16_t *state) {
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

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	/* NEON: per output the 4-tap dot product in s32 (vmull, then two pairwise
	 * adds: 4 * 32768 * |taps| < 2^31, no overflow), then >> 15 and clamp16 in
	 * one vqshrn #15. Unlike OoT's aResample, this mixer does NOT round each
	 * product: the sum is shifted once, exactly as the scalar loop. Two
	 * outputs a pass (nbytes is a multiple of 16: 8 outputs a block). */
	(void)tbl; (void)sample;
	{
		const uint32_t step = (uint32_t)pitch << 1;
		do {
			for (i = 0; i < 8; i += 2) {
				const int32x4_t pa = vmull_s16(vld1_s16(in), vld1_s16(resample_table[pitch_accumulator * 64 >> 16]));
				pitch_accumulator += step;
				in += pitch_accumulator >> 16;
				pitch_accumulator %= 0x10000;
				const int32x4_t pb = vmull_s16(vld1_s16(in), vld1_s16(resample_table[pitch_accumulator * 64 >> 16]));
				pitch_accumulator += step;
				in += pitch_accumulator >> 16;
				pitch_accumulator %= 0x10000;
				const int32x2_t a2 = vpadd_s32(vget_low_s32(pa), vget_high_s32(pa));
				const int32x2_t b2 = vpadd_s32(vget_low_s32(pb), vget_high_s32(pb));
				const int16x4_t r = vqshrn_n_s32(vcombine_s32(vpadd_s32(a2, b2), vdup_n_s32(0)), 15);
				vst1_lane_s32((int32_t *)(void *)out, vreinterpret_s32_s16(r), 0);
				out += 2;
			}
			nbytes -= 8 * sizeof(int16_t);
		} while (nbytes > 0);
	}
#else
	do {
		for (i = 0; i < 8; i++) {
			tbl = resample_table[pitch_accumulator * 64 >> 16];
			sample = ((int32_t)in[0] * tbl[0] +
			          (int32_t)in[1] * tbl[1] +
			          (int32_t)in[2] * tbl[2] +
			          (int32_t)in[3] * tbl[3]) >> 15;
			*out++ = clamp16(sample);

			pitch_accumulator += (pitch << 1);
			in += pitch_accumulator >> 16;
			pitch_accumulator %= 0x10000;
		}
		nbytes -= 8 * sizeof(int16_t);
	} while (nbytes > 0);
#endif

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

/* ================================================================== */
/*  A_INTERLEAVE (opcode 13) — Stereo channel interleave              */
/*  Reads separate L/R buffers, writes interleaved L-R-L-R to out.    */
/* ================================================================== */

void aInterleaveImpl(uint16_t left, uint16_t right) {
	int count;
	int16_t *l, *r, *d;
	int i;

	if (rspa.nbytes == 0) return;

	count = rspa.nbytes / (2 * sizeof(int16_t));
	l = BUF_S16(left);
	r = BUF_S16(right);
	d = BUF_S16(rspa.out);

	for (i = 0; i < count; i++) {
		*d++ = *l++;
		*d++ = *r++;
	}
}

/* ================================================================== */
/*  A_SETVOL (opcode 9) — Set volume parameters for A_ENVMIXER        */
/*                                                                    */
/*  Flag combos from pull chain (n_env.c):                            */
/*    A_LEFT  | A_VOL  (0x06): set left current volume                */
/*    A_RIGHT | A_VOL  (0x04): set right current volume               */
/*    A_LEFT  | A_RATE (0x02): set left target + ramp rate            */
/*    A_RIGHT | A_RATE (0x00): set right target + ramp rate           */
/*    A_AUX          (0x08): set dry/wet amounts                      */
/* ================================================================== */

void aSetVolumeImpl(uint16_t flags, uint16_t vol, uint16_t voltgt, uint16_t volrate) {
	if (flags & 0x08) { /* A_AUX */
		rspa.dry_amt = (int16_t)vol;
		rspa.wet_amt = (int16_t)volrate;
	} else if (flags & 0x04) { /* A_VOL */
		if (flags & 0x02) { /* A_LEFT */
			rspa.vol_left = (int16_t)vol;
		} else { /* A_RIGHT */
			rspa.vol_right = (int16_t)vol;
		}
	} else { /* A_RATE */
		if (flags & 0x02) { /* A_LEFT */
			rspa.tgt_left = (int16_t)vol;
			rspa.rate_left_m = (int16_t)voltgt;
			rspa.rate_left_l = volrate;
		} else { /* A_RIGHT */
			rspa.tgt_right = (int16_t)vol;
			rspa.rate_right_m = (int16_t)voltgt;
			rspa.rate_right_l = volrate;
		}
	}
}

/* ================================================================== */
/*  A_ENVMIXER (opcode 3) — N_AUDIO envelope mixer                    */
/*                                                                    */
/*  SSB64 builds with N_MICRO, whose compact command fields are       */
/*  relative offsets into the N_AUDIO ucode workspace.  The envmixer  */
/*  consumes the fixed main input buffer and accumulates into fixed   */
/*  dry/wet output buses.  Its persisted state is an 80-byte block:   */
/*  wet/dry amounts, left/right targets, left/right ramp steps, and   */
/*  left/right ramp values.                                           */
/* ================================================================== */
#define MIX_MAIN_L PORT_NAUDIO_DRY_LEFT
#define MIX_MAIN_R PORT_NAUDIO_DRY_RIGHT
#define MIX_AUX_L  PORT_NAUDIO_WET_LEFT
#define MIX_AUX_R  PORT_NAUDIO_WET_RIGHT

typedef struct {
	int32_t value;
	int32_t target;
	int32_t step;
} AudioRamp;

static inline int16_t ramp_step(AudioRamp *ramp) {
	int reached;

	/* PORT: do the accumulation in int64 so an overshoot is detected
	 * by the existing reach check instead of wrapping int32 silently.
	 *
	 * When n_env.c's `_getRate` is called with count==0 (em_segEnd
	 * zero) and tgt>=vol — typical for a voice that just needs to
	 * settle at its target with no attack ramp — it returns the
	 * sentinel `ratem=0x7FFF, ratel=0xFFFF` meaning "saturate to
	 * target immediately."  The N64 RSP ucode applies that as a
	 * one-sample jump.  In the port, plain `value += rate/8` with
	 * value already near positive max (e.g. em_volume * n_eqpower at
	 * extreme pan ≈ 29542 << 16 = 1.94e9, target equal) overflows
	 * int32, wraps to a large negative, and the reach check
	 * (value >= target) becomes false because value is now negative.
	 * The ramp continues wrapping every ~16 samples, producing
	 * sawtooth-shaped sign flips audible as harsh 2 kHz noise on
	 * whichever channel is being driven near the rail (= the
	 * dominant channel at extreme pan).  See:
	 *   docs/bugs/audio_envmix_ramp_overflow_*.
	 *
	 * Issue #108: hit SFX corrupted at extreme stage |X|. */
	int64_t next = (int64_t)ramp->value + (int64_t)ramp->step;
	if (next > INT32_MAX) next = INT32_MAX;
	if (next < INT32_MIN) next = INT32_MIN;
	ramp->value = (int32_t)next;
	reached = (ramp->step <= 0) ? (ramp->value <= ramp->target)
	                            : (ramp->value >= ramp->target);
	if (reached) {
		ramp->value = ramp->target;
		ramp->step = 0;
	}
	return (int16_t)(ramp->value >> 16);
}

static inline int32_t load_s32_from_s16(const int16_t *p) {
	int32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void store_s32_to_s16(int16_t *p, int32_t v) {
	memcpy(p, &v, sizeof(v));
}

/* The per-sample reference (and the non-NEON path). */
static void envmix_c(const int16_t *in, int count, AudioRamp *ramps, int16_t dry_amt, int16_t wet_amt) {
	int i;
	for (i = 0; i < count; i++) {
		int16_t s = in[i];
		int16_t l_vol = ramp_step(&ramps[0]);
		int16_t r_vol = ramp_step(&ramps[1]);
		int16_t l_dry = clamp16(((int32_t)l_vol * dry_amt + 0x4000) >> 15);
		int16_t r_dry = clamp16(((int32_t)r_vol * dry_amt + 0x4000) >> 15);
		int16_t l_wet = clamp16(((int32_t)l_vol * wet_amt + 0x4000) >> 15);
		int16_t r_wet = clamp16(((int32_t)r_vol * wet_amt + 0x4000) >> 15);
		int16_t *dryL = BUF_S16(MIX_MAIN_L);
		int16_t *dryR = BUF_S16(MIX_MAIN_R);
		int16_t *wetL = BUF_S16(MIX_AUX_L);
		int16_t *wetR = BUF_S16(MIX_AUX_R);

		dryL[i] = clamp16(dryL[i] + (((int32_t)s * l_dry) >> 15));
		dryR[i] = clamp16(dryR[i] + (((int32_t)s * r_dry) >> 15));
		wetL[i] = clamp16(wetL[i] + (((int32_t)s * l_wet) >> 15));
		wetR[i] = clamp16(wetR[i] + (((int32_t)s * r_wet) >> 15));
	}

}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* NEON, bit-exact with envmix_c (diddy-kong-racing 0db1de8 did the same for its
 * libultra mixer; this is the n_audio one). The ramps stay scalar -- they are
 * serial with an int64 clamp and a reach check -- and are skipped outright once
 * both have settled (step 0 at the target: every later volume is the target,
 * the common case). vqrdmulh is clamp16((a*b + 0x4000) >> 15) exactly
 * (-32768*-32768 saturates to 32767 either way); the accumulation widens to
 * 32 bits, adds (s*g) >> 15 and saturates back like clamp16. */
static inline int16x8_t envmix_acc(int16x8_t acc, int16x8_t s, int16x8_t g) {
	int32x4_t lo = vaddw_s16(vshrq_n_s32(vmull_s16(vget_low_s16(s), vget_low_s16(g)), 15), vget_low_s16(acc));
	int32x4_t hi = vaddw_s16(vshrq_n_s32(vmull_s16(vget_high_s16(s), vget_high_s16(g)), 15), vget_high_s16(acc));
	return vcombine_s16(vqmovn_s32(lo), vqmovn_s32(hi));
}
static void envmix_neon(const int16_t *in, int count, AudioRamp *ramps, int16_t dry_amt, int16_t wet_amt) {
	int16_t *dryL = BUF_S16(MIX_MAIN_L), *dryR = BUF_S16(MIX_MAIN_R);
	int16_t *wetL = BUF_S16(MIX_AUX_L), *wetR = BUF_S16(MIX_AUX_R);
	const int16x8_t dry = vdupq_n_s16(dry_amt), wet = vdupq_n_s16(wet_amt);
	int16_t lv[8], rv[8];
	int i, k;
	for (i = 0; i + 8 <= count; i += 8) {
		int16x8_t l, r;
		if (ramps[0].step == 0 && ramps[0].value == ramps[0].target &&
		    ramps[1].step == 0 && ramps[1].value == ramps[1].target) {
			l = vdupq_n_s16((int16_t)(ramps[0].value >> 16));
			r = vdupq_n_s16((int16_t)(ramps[1].value >> 16));
		} else {
			for (k = 0; k < 8; k++) { lv[k] = ramp_step(&ramps[0]); rv[k] = ramp_step(&ramps[1]); }
			l = vld1q_s16(lv); r = vld1q_s16(rv);
		}
		const int16x8_t s = vld1q_s16(in + i);
		const int16x8_t l_dry = vqrdmulhq_s16(l, dry), r_dry = vqrdmulhq_s16(r, dry);
		const int16x8_t l_wet = vqrdmulhq_s16(l, wet), r_wet = vqrdmulhq_s16(r, wet);
		vst1q_s16(dryL + i, envmix_acc(vld1q_s16(dryL + i), s, l_dry));
		vst1q_s16(dryR + i, envmix_acc(vld1q_s16(dryR + i), s, r_dry));
		vst1q_s16(wetL + i, envmix_acc(vld1q_s16(wetL + i), s, l_wet));
		vst1q_s16(wetR + i, envmix_acc(vld1q_s16(wetR + i), s, r_wet));
	}
}
#endif

void aEnvMixerImpl(uint8_t flags, int16_t *state) {
	int16_t *in = BUF_S16(rspa.in);
	int nbytes = ROUND_UP_16(rspa.nbytes);
	int count = nbytes / (int)sizeof(int16_t);
	int16_t dry_amt, wet_amt;
	AudioRamp ramps[2];
	int16_t save[40];

	if (flags & 0x01) { /* A_INIT — load from SETVOL params */
		int32_t rate_left = ((int32_t)rspa.rate_left_m << 16) | rspa.rate_left_l;
		int32_t rate_right = ((int32_t)rspa.rate_right_m << 16) | rspa.rate_right_l;

		dry_amt = rspa.dry_amt;
		wet_amt = rspa.wet_amt;
		ramps[0].value = (int32_t)rspa.vol_left << 16;
		ramps[0].target = (int32_t)rspa.tgt_left << 16;
		ramps[0].step = rate_left / 8;
		ramps[1].value = (int32_t)rspa.vol_right << 16;
		ramps[1].target = (int32_t)rspa.tgt_right << 16;
		ramps[1].step = rate_right / 8;
	} else { /* A_CONTINUE — restore from ENVMIX_STATE */
		memcpy(save, state, sizeof(save));
		wet_amt = save[0];
		dry_amt = save[2];
		ramps[0].target = (int32_t)save[4] << 16;
		ramps[1].target = (int32_t)save[6] << 16;
		ramps[0].step = load_s32_from_s16(save + 8);
		ramps[1].step = load_s32_from_s16(save + 10);
		ramps[0].value = load_s32_from_s16(save + 16);
		ramps[1].value = load_s32_from_s16(save + 18);
	}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	envmix_neon(in, count, ramps, dry_amt, wet_amt);
#else
	envmix_c(in, count, ramps, dry_amt, wet_amt);
#endif

	memset(save, 0, sizeof(save));
	save[0] = wet_amt;
	save[2] = dry_amt;
	save[4] = (int16_t)(ramps[0].target >> 16);
	save[6] = (int16_t)(ramps[1].target >> 16);
	store_s32_to_s16(save + 8, ramps[0].step);
	store_s32_to_s16(save + 10, ramps[1].step);
	store_s32_to_s16(save + 16, ramps[0].value);
	store_s32_to_s16(save + 18, ramps[1].value);
	memcpy(state, save, sizeof(save));
}

/* ================================================================== */
/*  A_MIXER (opcode 12) — Gain-weighted mix                           */
/*  out = clamp(out + ((in * gain) >> 15))                            */
/*  Count from rspa.nbytes (set by prior A_SETBUFF).                  */
/* ================================================================== */

void aMixImpl(uint8_t flags, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
	int nbytes = ROUND_UP_16(rspa.nbytes);
	int16_t *in = BUF_S16(in_addr);
	int16_t *out = BUF_S16(out_addr);
	int32_t sample;

	(void)flags;

	if (gain == -0x8000) {
		/* Special case: subtract only */
		while (nbytes > 0) {
			int i;
			for (i = 0; i < 16 && nbytes > 0; i++) {
				sample = *out - *in++;
				*out++ = clamp16(sample);
				nbytes -= sizeof(int16_t);
			}
		}
		return;
	}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	/* NEON, bit-exact: (in*gain) >> 15 widened, added to out, saturated like
	 * clamp16 (nbytes is a multiple of 16: whole 8-sample vectors). */
	{
		const int16x4_t g = vdup_n_s16(gain);
		int i;
		(void)sample;
		for (i = 0; i < nbytes / (int)sizeof(int16_t); i += 8) {
			const int16x8_t o = vld1q_s16(out + i), x = vld1q_s16(in + i);
			const int32x4_t lo = vaddw_s16(vshrq_n_s32(vmull_s16(vget_low_s16(x), g), 15), vget_low_s16(o));
			const int32x4_t hi = vaddw_s16(vshrq_n_s32(vmull_s16(vget_high_s16(x), g), 15), vget_high_s16(o));
			vst1q_s16(out + i, vcombine_s16(vqmovn_s32(lo), vqmovn_s32(hi)));
		}
	}
#else
	while (nbytes > 0) {
		int i;
		for (i = 0; i < 16 && nbytes > 0; i++) {
			sample = *out + (((int32_t)*in++ * gain) >> 15);
			*out++ = clamp16(sample);
			nbytes -= sizeof(int16_t);
		}
	}
#endif
}

/* ================================================================== */
/*  A_POLEF (opcode 14) — N_AUDIO pole / IIR filter                   */
/* ================================================================== */

static inline int32_t vmulf(int16_t x, int16_t y) {
	return (((int32_t)x * (int32_t)y) + 0x4000) >> 15;
}

static int32_t reverse_dot(int n, const int16_t *x, const int16_t *y) {
	int32_t acc = 0;
	y += n;
	while (n != 0) {
		acc += *x++ * *--y;
		n--;
	}
	return acc;
}

static void aPoleFilterPoleImpl(uint8_t flags, uint16_t gain, int16_t *state) {
	int16_t *src = BUF_S16(rspa.in);
	int16_t *dst = BUF_S16(rspa.out);
	int nbytes = ROUND_UP_16(rspa.nbytes);
	int16_t *table = &rspa.adpcm_table[0][0][0];
	int16_t *h1 = table;
	int16_t *h2 = table + 8;
	int16_t h2_before[8];
	int16_t l1 = 0;
	int16_t l2 = 0;
	int i;

	if (!(flags & 0x01)) {
		l1 = state[2];
		l2 = state[3];
	}

	for (i = 0; i < 8; i++) {
		h2_before[i] = h2[i];
		h2[i] = (int16_t)(((int32_t)h2[i] * gain) >> 14);
	}

	while (nbytes > 0) {
		int16_t frame[8];
		for (i = 0; i < 8; i++) {
			frame[i] = src[i];
		}
		for (i = 0; i < 8; i++) {
			int32_t acc = (int32_t)frame[i] * gain;
			acc += (int32_t)h1[i] * l1 + (int32_t)h2_before[i] * l2;
			acc += reverse_dot(i, h2, frame);
			dst[i] = clamp16(acc >> 14);
		}
		l1 = dst[6];
		l2 = dst[7];
		src += 8;
		dst += 8;
		nbytes -= 16;
	}

	memcpy(state, dst - 4, 4 * sizeof(int16_t));
}

static void aPoleFilterIirImpl(uint8_t flags, int16_t *state) {
	int16_t *src = BUF_S16(rspa.in);
	int16_t *dst = BUF_S16(rspa.out);
	int nbytes = ROUND_UP_16(rspa.nbytes);
	int16_t *table = &rspa.adpcm_table[0][0][0];
	int16_t frame[8] = {0};
	int16_t ibuf[4] = {0};
	uint16_t index = 7;
	int32_t prev;
	int i;

	if (!(flags & 0x01)) {
		frame[6] = state[2];
		frame[7] = state[3];
		ibuf[1] = state[4];
		ibuf[2] = state[5];
	}

	prev = vmulf(table[9], frame[6]) * 2;
	while (nbytes > 0) {
		for (i = 0; i < 8; i++) {
			int32_t acc;
			ibuf[index & 3] = *src++;
			acc = prev +
			      vmulf(table[0], ibuf[index & 3]) +
			      vmulf(table[1], ibuf[(index - 1) & 3]) +
			      vmulf(table[0], ibuf[(index - 2) & 3]);
			acc += vmulf(table[8], frame[index]) * 2;
			prev = vmulf(table[9], frame[index]) * 2;
			dst[i] = frame[i] = (int16_t)acc;
			index = (index + 1) & 7;
		}
		dst += 8;
		nbytes -= 16;
	}

	state[2] = frame[6];
	state[3] = frame[7];
	state[4] = ibuf[(index - 2) & 3];
	state[5] = ibuf[(index - 1) & 3];
}

void aPoleFilterImpl(uint8_t flags, uint16_t gain, int16_t *state) {
	int16_t *table = &rspa.adpcm_table[0][0][0];
	if (table[0] == 0 && table[1] == 0) {
		aPoleFilterPoleImpl(flags, gain, state);
	} else {
		aPoleFilterIirImpl(flags, state);
	}
}
