/*
 * Port note: RyoSaeba89/DKR-GC platform/gc/audio/audio_mixer.c (CC0), itself a
 * transcription of sm64-port's mixer.c, used here as the RSP audio task for the
 * Handheld/desktop port. Changes: dram() is the identity (the port's RSP
 * addresses are host pointers below 0x20000000, which K0_TO_PHYS leaves
 * alone), and the entry point is port_audio_run_cmds(). Sample data stays
 * big-endian bytes (ADPCM is byte-coded; RAW16 loads get a swapped copy,
 * port_raw16_host); ADPCM books and loop states are byte-swapped when the
 * bank is parsed (libultra bnkf.c, #ifdef PORT).
 */
/*
 * The audio task: executing an N64 audio command list on the CPU.
 *
 * The game's sound engine is unchanged by the port. libultra's synthesiser
 * still runs -- alAudioFrame still walks the sequence and sound players once
 * per retrace and still emits a list of Acmd words. What is gone is the RSP
 * that used to execute that list, so this file executes it instead.
 *
 * The command set is the audio microcode ABI in include/PR/abi.h: a small
 * stack machine over a scratch area of 16-bit samples, with commands to load
 * and store buffers, decode ADPCM, resample, apply an envelope, mix with a
 * gain, and finally interleave a stereo pair out to DRAM. Fifteen opcodes
 * carry all the work, and all fifteen are implemented here -- that is not an
 * estimate, it is a census of the aXxx macros the synthesiser under
 * libultra/src/audio actually expands:
 *
 *     aSetBuffer 33  aMix 12  aLoadBuffer 10  aClearBuffer 10  aSaveBuffer 7
 *     aSetVolume 5   aDMEMMove 5   aResample 3  aLoadADPCM 3   aSegment 2
 *     aPoleFilter 2  aEnvMixer 2   aSetLoop 1   aInterleave 1  aADPCMdec 1
 *
 * Two properties make this tractable. The buffers are plain arrays rather than
 * anything hardware-shaped, so "DMEM" is just a static array here. And both
 * machines are big-endian, so sample data lifted straight out of the asset
 * image needs no byte swapping -- which is the single largest reason a port
 * like this is easier from N64 to GameCube than to a little-endian target.
 *
 * The arithmetic below is sm64-port's src/pc/mixer.c, scalar path, transcribed
 * opcode by opcode: the microcode is shared between the two games, so the
 * semantics carry across exactly. The one exception is A_POLEF, which
 * sm64-port does not implement and which is reconstructed here from the
 * coefficients its caller builds; its comment carries the derivation. What is
 * *not* shared, and is the port's own
 * work, is everything around it -- the command walker (sm64-port has none: it
 * redefines the aXxx macros to call the implementations directly and never
 * builds a list at all), the address normalisation below, and the instrument.
 *
 * DKR cannot take sm64-port's shortcut. audiomgr.c genuinely builds a list
 * into a double-buffered ACMDList, reads back the length alAudioFrame returns,
 * and hands the buffer to the scheduler as an OS_TASK; bypassing that would
 * mean rewriting the audio manager. Decoding w0/w1 keeps the game's
 * architecture exactly as the ROM has it.
 */

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <ultra64.h>


#include <string.h>
#include <stdlib.h>

#include "port.h"
#include "acmd_neon.h"

#if ACMD_NEON
/* DKR_AUDIONEON=0: the C ADPCM and resample loops, for A/B (the NEON kernels
 * are proved equal by tools/mixer_test.c and the DKR_AUDIO_DUMP md5). */
static int sAcmdNeon = -1;
static int acmd_neon_on(void) {
    if (sAcmdNeon < 0) { const char *e = getenv("DKR_AUDIONEON"); sAcmdNeon = !(e && e[0] == '0'); }
    return sAcmdNeon;
}
#endif
/* The benchmark's NEON-off run (main.c), with gfx_soft_set_neon. */
void acmd_set_neon(int on) {
#if ACMD_NEON
    sAcmdNeon = on ? 1 : 0;
#else
    (void) on;
#endif
}

/* ---- DMEM ---------------------------------------------------------------- */

/*
 * The microcode's scratch memory. The command list's addresses are offsets
 * into it, and libultra fixes the layout in synthInternals.h from
 * AL_MAX_RSP_SAMPLES = 160: the highest buffer is AL_AUX_R_OUT at 2048, one
 * more 320-byte buffer long, so 2368 bytes are ever addressed. 0x1000 leaves
 * the rounding-up every opcode does (to 8, 16 or 32 bytes) room to spill into
 * without a bounds test in the inner loops.
 */
#define AUDIO_DMEM_SIZE 0x1000

/*
 * A guard on either side, and the reason for it.
 *
 * The resampler writes up to eight samples *below* its input pointer to
 * reinstate the tail of the previous call, and reads four below that. With the
 * resampler's input at AL_DECODER_OUT (320) that never reaches address zero,
 * so the guard is not load-bearing today -- it is there so a future buffer at
 * offset 0 cannot corrupt whatever the linker happened to place in front of
 * this array. sm64-port relies on the equivalent writes landing in the struct
 * fields declared before its buffer, which is not a property worth inheriting.
 */
#define AUDIO_DMEM_GUARD 64

static u8 sDmem[AUDIO_DMEM_GUARD + AUDIO_DMEM_SIZE + AUDIO_DMEM_GUARD]
    __attribute__((aligned(32)));

#define DMEM_U8(a)  (sDmem + AUDIO_DMEM_GUARD + (a))
#define DMEM_S16(a) ((s16 *) (sDmem + AUDIO_DMEM_GUARD + ((a) & ~1u)))

#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v)  (((v) + 7) & ~7)

/* ---- DRAM addresses ------------------------------------------------------ */

/*
 * Normalising an address the command list carries.
 *
 * The list holds two different flavours of DRAM pointer, and they do not
 * arrive the same way:
 *
 *   - most sites pass osVirtualToPhysical(p), which this port defines as the
 *     identity precisely so the mixer receives something it can dereference
 *     (see the note in ultra/os_system.c);
 *   - load.c passes K0_TO_PHYS(p) for the ADPCM decoder state and loop state,
 *     and that macro is a literal `& 0x1FFFFFFF`. It is not a call the port
 *     can intercept, and on the GameCube it strips the 0x80000000 that names
 *     MEM1.
 *
 * So the walker puts the bit back on anything that has lost it, exactly as
 * segmented_to_virtual does for display-list pointers in gfx/gfx_gx.c. All of
 * MEM1 lives at 0x80000000 and nothing the audio path touches lives below it,
 * so the test is unambiguous.
 */
static void *dram(u32 addr) {
    return (void *) (uintptr_t) addr; /* port: DRAM addresses are host pointers */
}

/* RAW16 sample data reaches DRAM as the ROM has it, big-endian s16, and this
 * task reads DMEM as host s16 (ADPCM is byte-coded, RAW16 is not). DKR's banks
 * hold 26 RAW16 waves, mostly looped single-cycle instrument tones, and they
 * played byte-swapped (the GoldenEye port 84b67c6, where it was the music's
 * bass). alRaw16Pull (libultra load.c, #ifdef PORT) hands each load a swapped
 * copy from this ring; a frame's list runs before the next is built, so the
 * ring only has to hold one frame's RAW16 loads. The DMA buffers keep the
 * ROM's bytes. */
s32 port_raw16_host(s32 addr, s32 n) {
    static u8 sRing[0x10000] __attribute__((aligned(16)));
    static s32 sCursor;
    const u8 *src = (const u8 *) dram((u32) addr);
    u8 *dst;
    s32 i;
    if (n <= 0 || n > (s32) sizeof(sRing)) return addr;
    if (sCursor + n > (s32) sizeof(sRing)) sCursor = 0;
    dst = sRing + sCursor;
    sCursor = (sCursor + n + 15) & ~15;
    for (i = 0; i + 1 < n; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
    }
    return (s32) (uintptr_t) dst;
}

/* ---- microcode state ----------------------------------------------------- */

static struct {
    u16 in;
    u16 out;
    u16 nbytes;

    s16 vol[2];

    u16 dry_right;
    u16 wet_left;
    u16 wet_right;

    s16 target[2];
    s32 rate[2];

    s16 vol_dry;
    s16 vol_wet;

    s16 *adpcm_loop_state;

    s16 adpcm_table[8][2][8];
} sRspa;

static const s16 sResampleTable[64][4] = {
    { 0x0c39,  0x66ad,  0x0d46,    -33 }, { 0x0b39,  0x6696,  0x0e5f,    -40 },
    { 0x0a44,  0x6669,  0x0f83,    -48 }, { 0x095a,  0x6626,  0x10b4,    -56 },
    { 0x087d,  0x65cd,  0x11f0,    -65 }, { 0x07ab,  0x655e,  0x1338,    -74 },
    { 0x06e4,  0x64d9,  0x148c,    -84 }, { 0x0628,  0x643f,  0x15eb,    -95 },
    { 0x0577,  0x638f,  0x1756,   -106 }, { 0x04d1,  0x62cb,  0x18cb,   -118 },
    { 0x0435,  0x61f3,  0x1a4c,   -130 }, { 0x03a4,  0x6106,  0x1bd7,   -143 },
    { 0x031c,  0x6007,  0x1d6c,   -156 }, { 0x029f,  0x5ef5,  0x1f0b,   -170 },
    { 0x022a,  0x5dd0,  0x20b3,   -184 }, { 0x01be,  0x5c9a,  0x2264,   -198 },
    { 0x015b,  0x5b53,  0x241e,   -212 }, { 0x0101,  0x59fc,  0x25e0,   -226 },
    { 0x00ae,  0x5896,  0x27a9,   -240 }, { 0x0063,  0x5720,  0x297a,   -254 },
    { 0x001f,  0x559d,  0x2b50,   -268 }, {    -30,  0x540d,  0x2d2c,   -280 },
    {    -84,  0x5270,  0x2f0d,   -293 }, {   -132,  0x50c7,  0x30f3,   -304 },
    {   -173,  0x4f14,  0x32dc,   -314 }, {   -210,  0x4d57,  0x34c8,   -323 },
    {   -241,  0x4b91,  0x36b6,   -330 }, {   -267,  0x49c2,  0x38a5,   -336 },
    {   -289,  0x47ed,  0x3a95,   -340 }, {   -306,  0x4611,  0x3c85,   -341 },
    {   -320,  0x4430,  0x3e74,   -340 }, {   -330,  0x424a,  0x4060,   -337 },
    {   -337,  0x4060,  0x424a,   -330 }, {   -340,  0x3e74,  0x4430,   -320 },
    {   -341,  0x3c85,  0x4611,   -306 }, {   -340,  0x3a95,  0x47ed,   -289 },
    {   -336,  0x38a5,  0x49c2,   -267 }, {   -330,  0x36b6,  0x4b91,   -241 },
    {   -323,  0x34c8,  0x4d57,   -210 }, {   -314,  0x32dc,  0x4f14,   -173 },
    {   -304,  0x30f3,  0x50c7,   -132 }, {   -293,  0x2f0d,  0x5270,    -84 },
    {   -280,  0x2d2c,  0x540d,    -30 }, {   -268,  0x2b50,  0x559d, 0x001f },
    {   -254,  0x297a,  0x5720, 0x0063 }, {   -240,  0x27a9,  0x5896, 0x00ae },
    {   -226,  0x25e0,  0x59fc, 0x0101 }, {   -212,  0x241e,  0x5b53, 0x015b },
    {   -198,  0x2264,  0x5c9a, 0x01be }, {   -184,  0x20b3,  0x5dd0, 0x022a },
    {   -170,  0x1f0b,  0x5ef5, 0x029f }, {   -156,  0x1d6c,  0x6007, 0x031c },
    {   -143,  0x1bd7,  0x6106, 0x03a4 }, {   -130,  0x1a4c,  0x61f3, 0x0435 },
    {   -118,  0x18cb,  0x62cb, 0x04d1 }, {   -106,  0x1756,  0x638f, 0x0577 },
    {    -95,  0x15eb,  0x643f, 0x0628 }, {    -84,  0x148c,  0x64d9, 0x06e4 },
    {    -74,  0x1338,  0x655e, 0x07ab }, {    -65,  0x11f0,  0x65cd, 0x087d },
    {    -56,  0x10b4,  0x6626, 0x095a }, {    -48,  0x0f83,  0x6669, 0x0a44 },
    {    -40,  0x0e5f,  0x6696, 0x0b39 }, {    -33,  0x0d46,  0x66ad, 0x0c39 }
};

static inline s16 clamp16(s32 v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (s16) v;
}

static inline s32 clamp32(s64 v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (s32) v;
}

/* ---- the opcodes --------------------------------------------------------- */

static void a_clear_buffer(u16 addr, int nbytes) {
    memset(DMEM_U8(addr), 0, (size_t) ROUND_UP_16(nbytes));
}

static void a_load_buffer(const void *src) {
    memcpy(DMEM_U8(sRspa.in), src, (size_t) ROUND_UP_8(sRspa.nbytes));
}

static void a_save_buffer(s16 *dst) {
    memcpy(dst, DMEM_S16(sRspa.out), (size_t) ROUND_UP_8(sRspa.nbytes));
}

static void a_load_adpcm(int num_entries_times_16, const void *book) {
    memcpy(sRspa.adpcm_table, book, (size_t) num_entries_times_16);
}

static void a_set_buffer(u8 flags, u16 in, u16 out, u16 nbytes) {
    if (flags & A_AUX) {
        sRspa.dry_right = in;
        sRspa.wet_left = out;
        sRspa.wet_right = nbytes;
        return;
    }
    sRspa.in = in;
    sRspa.out = out;
    sRspa.nbytes = nbytes;
}

static void a_set_volume(u8 flags, s16 v, s16 t, s16 r) {
    if (flags & A_AUX) {
        sRspa.vol_dry = v;
        sRspa.vol_wet = r;
    } else if (flags & A_VOL) {
        sRspa.vol[(flags & A_LEFT) ? 0 : 1] = v;
    } else {
        int c = (flags & A_LEFT) ? 0 : 1;

        sRspa.target[c] = v;
        sRspa.rate[c] = (s32) ((u32) (u16) t << 16 | (u32) (u16) r);
    }
}

/*
 * Interleave two mono buffers into one stereo buffer.
 *
 * `nbytes` is the size of ONE channel, so the command produces `nbytes/2`
 * stereo pairs -- twice `nbytes` bytes of output. That factor is the whole
 * point of this comment, because getting it wrong is not silent:
 *
 * This port originally wrote `ROUND_UP_16(nbytes) / sizeof(s16) / 2` pairs,
 * i.e. half of them, and left the upper half of the output buffer untouched.
 * save.c then saves `outCount << 2` bytes from DMEM 0 regardless, so every
 * audio frame carried 160 real stereo samples followed by 160 samples of
 * whatever was last at AL_TEMP_0 -- which is AL_RESAMPLER_OUT, the raw
 * un-enveloped voice output, at full amplitude. That is what pinned the
 * `peak` counter to 32767 on nearly every beat, and it is what the user heard
 * as "inaudible, ca craque dans tous les sens": half of every frame was noise.
 *
 * ref-sm64gc's aInterleaveImpl does `ROUND_UP_16(nbytes) / sizeof(s16) / 8`
 * iterations of eight pairs, which is the same count written differently:
 * nbytes/2 pairs. Unrolling by eight buys nothing here, so it is one pair per
 * iteration with the count spelt out.
 */
static void a_interleave(u16 left, u16 right) {
    int count = ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16);
    s16 *l = DMEM_S16(left);
    s16 *r = DMEM_S16(right);
    s16 *d = DMEM_S16(sRspa.out);

    while (count > 0) {
        *d++ = *l++;
        *d++ = *r++;
        --count;
    }
}

static void a_dmem_move(u16 in_addr, u16 out_addr, int nbytes) {
    memmove(DMEM_U8(out_addr), DMEM_U8(in_addr), (size_t) ROUND_UP_16(nbytes));
}

static void a_set_loop(s16 *loop_state) {
    sRspa.adpcm_loop_state = loop_state;
}

static void a_adpcm_dec(u8 flags, s16 *state) {
    u8 *in = DMEM_U8(sRspa.in);
    s16 *out = DMEM_S16(sRspa.out);
    int nbytes = ROUND_UP_32(sRspa.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if (flags & A_LOOP) {
        memcpy(out, sRspa.adpcm_loop_state, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
    out += 16;

#if ACMD_NEON
    if (acmd_neon_on()) { /* acmd_adpcm_frame_neon: this loop as an 8x10 matrix product per half-frame */
        AcmdAdpcmCols cols;
        cols.key = NULL;
        while (nbytes > 0) {
            in = (u8 *) acmd_adpcm_frame_neon(in, out, (const int16_t (*)[2][8]) sRspa.adpcm_table, &cols);
            out += 16;
            nbytes -= 16 * (int) sizeof(s16);
        }
        memcpy(state, out - 16, 16 * sizeof(s16));
        return;
    }
#endif
    while (nbytes > 0) {
        int shift = *in >> 4;          /* 0..12 */
        int table_index = *in++ & 0xf; /* 0..7  */
        s16 (*tbl)[8] = sRspa.adpcm_table[table_index];
        int i;

        for (i = 0; i < 2; i++) {
            s16 ins[8];
            s16 prev1 = out[-1];
            s16 prev2 = out[-2];
            int j, k;

            for (j = 0; j < 4; j++) {
                /*
                 * Sign-extend each nibble explicitly. The reference writes
                 * `((n << 28) >> 28)` on an int, which is a signed overflow
                 * for n >= 8 -- undefined in C, and a compiler is entitled to
                 * decide it cannot happen and drop the arithmetic shift.
                 * sm64-port lives with it on the compilers it is built with;
                 * this port is built with GCC 16, and a decoder that turns
                 * every negative nibble positive is exactly a decoder whose
                 * output is full of steps.
                 */
                s32 hi = *in >> 4;
                s32 lo = *in++ & 0xF;

                if (hi & 8) {
                    hi -= 16;
                }
                if (lo & 8) {
                    lo -= 16;
                }
                ins[j * 2] = (s16) (hi << shift);
                ins[j * 2 + 1] = (s16) (lo << shift);
            }
            for (j = 0; j < 8; j++) {
                s32 acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);

                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
        nbytes -= 16 * (int) sizeof(s16);
    }
    memcpy(state, out - 16, 16 * sizeof(s16));
}

static void a_resample(u8 flags, u16 pitch, s16 *state) {
    s16 tmp[16];
    s16 *in_initial = DMEM_S16(sRspa.in);
    s16 *in = in_initial;
    s16 *out = DMEM_S16(sRspa.out);
    int nbytes = ROUND_UP_16(sRspa.nbytes);
    u32 pitch_accumulator;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(s16));
    } else {
        memcpy(tmp, state, 16 * sizeof(s16));
    }
    /* Bit 1 of the resampler's flags is not one of the named A_* constants --
     * abi.h gives that bit three different names for three different opcodes.
     * It is the "continue from the saved tail" bit, kept numeric here rather
     * than given a name this port has not verified. */
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / (int) sizeof(s16);
    }
    in -= 4;
    pitch_accumulator = (u16) tmp[4];
    memcpy(in, tmp, 4 * sizeof(s16));

#if ACMD_NEON
    if (acmd_neon_on()) { /* acmd_resample_neon; the do-while below makes 8 outputs even for nbytes <= 0 */
        uint32_t pa = pitch_accumulator; /* u32 is unsigned long here */
        in = (s16 *) acmd_resample_neon(in, out, nbytes > 0 ? nbytes / (int) sizeof(s16) : 8, sResampleTable,
                                        &pa, (u32) pitch << 1);
        pitch_accumulator = pa;
    } else
#endif
    do {
        for (i = 0; i < 8; i++) {
            const s16 *tbl = sResampleTable[pitch_accumulator * 64 >> 16];
            s32 sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                         ((in[1] * tbl[1] + 0x4000) >> 15) +
                         ((in[2] * tbl[2] + 0x4000) >> 15) +
                         ((in[3] * tbl[3] + 0x4000) >> 15);

            *out++ = clamp16(sample);

            pitch_accumulator += (u32) pitch << 1;
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * (int) sizeof(s16);
    } while (nbytes > 0);

    state[4] = (s16) pitch_accumulator;
    memcpy(state, in, 4 * sizeof(s16));
    i = (int) (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = (s16) i;
    memcpy(state + 8, in, 8 * sizeof(s16));
}

static void a_env_mixer(u8 flags, s16 *state) {
    s16 *in = DMEM_S16(sRspa.in);
    s16 *dry[2] = { DMEM_S16(sRspa.out), DMEM_S16(sRspa.dry_right) };
    s16 *wet[2] = { DMEM_S16(sRspa.wet_left), DMEM_S16(sRspa.wet_right) };
    int nbytes = ROUND_UP_16(sRspa.nbytes);
    s16 target[2];
    s32 rate[2];
    s16 vol_dry, vol_wet;
    s32 vols[2][8];
    int c, i;

    if (flags & A_INIT) {
        target[0] = sRspa.target[0];
        target[1] = sRspa.target[1];
        rate[0] = sRspa.rate[0];
        rate[1] = sRspa.rate[1];
        vol_dry = sRspa.vol_dry;
        vol_wet = sRspa.vol_wet;

        /*
         * The eight lanes are the ramp inside one vector: lane i carries the
         * volume the (i+1)th sample of each group of eight is multiplied by,
         * and every group advances the whole ramp by one `rate`. See the note
         * on the loop below for why that is an addition and not a scaling.
         */
        for (i = 0; i < 8; i++) {
            vols[0][i] = clamp32((s64) ((s32) sRspa.vol[0] << 16) +
                                 (s64) rate[0] * (i + 1) / 8);
            vols[1][i] = clamp32((s64) ((s32) sRspa.vol[1] << 16) +
                                 (s64) rate[1] * (i + 1) / 8);
        }
    } else {
        memcpy(vols[0], state, 32);
        memcpy(vols[1], state + 16, 32);
        target[0] = state[32];
        target[1] = state[35];
        rate[0] = ((s32) state[33] << 16) | (u16) state[34];
        rate[1] = ((s32) state[36] << 16) | (u16) state[37];
        vol_dry = state[38];
        vol_wet = state[39];
    }

    /*
     * `rate` is an INCREMENT, not a multiplier, and getting that wrong was the
     * crackle.
     *
     * This function was transcribed from sm64-port's scalar `aEnvMixerImpl`,
     * where the envelope is exponential: the lanes are seeded with
     * `vol * (1 + (rate - 1) * (i + 1) / 8)` and each group of eight scales
     * them by `rate`. That is right for SM64's audio ABI and wrong for this
     * one, and the proof is in this repository rather than in any reasoning
     * about microcode. `libultra/src/audio/env.c` computes the value it hands
     * over as
     *
     *     a = (tgt - vol) / count; a *= 8;          -- _getRate
     *
     * a volume *difference* per eight samples, and then keeps its own copy of
     * the envelope in step with the RSP as
     *
     *     r = ((ratem << 16) + ratel) / 65536.0;
     *     ivol += (r * samples) / 8.0;             -- _getVol
     *
     * which is an addition. So the game's model of what the microcode does is
     * additive, and it is the authority here.
     *
     * Read multiplicatively, a perfectly ordinary DKR rate -- a segment of one
     * second gives `a` around 12, i.e. rate ~= 0xBE666 -- scaled every lane by
     * twelve within a single group of eight and then let the target clamp pin
     * it, so the gain sawtoothed once per eight samples and reset. That is
     * exactly the defect the 2026-09-06 capture measured: in the left channel,
     * every sample at a multiple of eight wrong, the error uncorrelated with
     * the signal and larger than it. `vol[0] * (rate - 0x10000)` also
     * overflowed a signed 32-bit multiply at those rates, which flipped the
     * ramp's sign on top of it.
     *
     * Also note the test below: with an increment, "rising" is `rate > 0` as a
     * signed 16.16 value. The old `(rate >> 16) > 0` called every rate smaller
     * than 1.0 -- every gentle fade IN -- a fade out, and clamped it to the
     * target from the wrong side.
     */
#if defined(__ARM_NEON)
    /* NEON (device round 2: the scalar mixer was ~3 ms per audio frame on the
     * Cortex-A7). Bit-exact with the C below: the eight lanes are independent,
     * vqaddq_s32 is clamp32 of the 64-bit sum, vrshrq/vqrshrn by 15 are the
     * "+ 0x4000 >> 15" roundings, and vqrshrn saturates like clamp16. */
    if (nbytes > 0) {
        int32x4_t v[2][2];
        for (c = 0; c < 2; c++) { v[c][0] = vld1q_s32((const int32_t *) &vols[c][0]); v[c][1] = vld1q_s32((const int32_t *) &vols[c][4]); }
        do {
            const int16x8_t x = vld1q_s16(in);
            const int32x4_t xin[2] = { vmovl_s16(vget_low_s16(x)), vmovl_s16(vget_high_s16(x)) };
            for (c = 0; c < 2; c++) {
                const int32x4_t tgt = vdupq_n_s32((s32) target[c] << 16), tgt16 = vdupq_n_s32(target[c]);
                const int32x4_t r = vdupq_n_s32(rate[c]);
                int16x4_t dh[2], wh[2];
                int h;
                for (h = 0; h < 2; h++) {
                    int32x4_t vv = v[c][h];
                    const int32x4_t hi = vshrq_n_s32(vv, 16);
                    const uint32x4_t over = rate[c] > 0 ? vcgtq_s32(hi, tgt16) : vcltq_s32(hi, tgt16);
                    vv = vbslq_s32(over, tgt, vv);
                    const int32x4_t vh = vshrq_n_s32(vv, 16);
                    {
                        const int32x4_t g = vrshrq_n_s32(vmulq_n_s32(vh, vol_dry), 15);
                        const int32x4_t d = vmovl_s16(vld1_s16(dry[c] + h * 4));
                        dh[h] = vqrshrn_n_s32(vaddq_s32(vmulq_n_s32(d, 0x7fff), vmulq_s32(xin[h], g)), 15);
                    }
                    if (flags & A_AUX) {
                        const int32x4_t g = vrshrq_n_s32(vmulq_n_s32(vh, vol_wet), 15);
                        const int32x4_t w = vmovl_s16(vld1_s16(wet[c] + h * 4));
                        wh[h] = vqrshrn_n_s32(vaddq_s32(vmulq_n_s32(w, 0x7fff), vmulq_s32(xin[h], g)), 15);
                    }
                    v[c][h] = vqaddq_s32(vv, r);
                }
                vst1_s16(dry[c], dh[0]); vst1_s16(dry[c] + 4, dh[1]);
                dry[c] += 8;
                if (flags & A_AUX) {
                    vst1_s16(wet[c], wh[0]); vst1_s16(wet[c] + 4, wh[1]);
                    wet[c] += 8;
                }
            }
            nbytes -= 16;
            in += 8;
        } while (nbytes > 0);
        for (c = 0; c < 2; c++) { vst1q_s32((int32_t *) &vols[c][0], v[c][0]); vst1q_s32((int32_t *) &vols[c][4], v[c][1]); }
    }
#else
    do {
        for (c = 0; c < 2; c++) {
            for (i = 0; i < 8; i++) {
                if (rate[c] > 0) {
                    /* Rising towards the target: never overshoot it. */
                    if ((vols[c][i] >> 16) > target[c]) {
                        vols[c][i] = (s32) target[c] << 16;
                    }
                } else {
                    /* Falling towards it: same, from the other side. */
                    if ((vols[c][i] >> 16) < target[c]) {
                        vols[c][i] = (s32) target[c] << 16;
                    }
                }
                dry[c][i] = clamp16((dry[c][i] * 0x7fff +
                                     in[i] * (((vols[c][i] >> 16) * vol_dry + 0x4000) >> 15) +
                                     0x4000) >> 15);
                if (flags & A_AUX) {
                    wet[c][i] = clamp16((wet[c][i] * 0x7fff +
                                         in[i] * (((vols[c][i] >> 16) * vol_wet + 0x4000) >> 15) +
                                         0x4000) >> 15);
                }
                vols[c][i] = clamp32((s64) vols[c][i] + rate[c]);
            }
            dry[c] += 8;
            if (flags & A_AUX) {
                wet[c] += 8;
            }
        }
        nbytes -= 16;
        in += 8;
    } while (nbytes > 0);
#endif

    memcpy(state, vols[0], 32);
    memcpy(state + 16, vols[1], 32);
    state[32] = target[0];
    state[35] = target[1];
    state[33] = (s16) (rate[0] >> 16);
    state[34] = (s16) rate[0];
    state[36] = (s16) (rate[1] >> 16);
    state[37] = (s16) rate[1];
    state[38] = vol_dry;
    state[39] = vol_wet;
}

/*
 * A_POLEF: the reverb's damping filter.
 *
 * This is the one opcode ref-sm64gc does not implement, so unlike everything
 * above it is reconstructed rather than transcribed. The reconstruction is not
 * guesswork -- the caller pins the arithmetic down completely.
 *
 * `_filterBuffer` (libultra/src/audio/mips1/reverb.c:429) emits exactly three
 * commands:
 *
 *     aSetBuffer (0, buff, buff, count << 1);          // in place
 *     aLoadADPCM (32, lp->fcvec.fccoef);               // 16 s16 coefficients
 *     aPoleFilter(lp->first, lp->fgain, lp->fstate);
 *
 * and `_init_lpfilter` (drvrnew.c) fills those sixteen coefficients:
 *
 *     fc            = (lp->fc * 16384) >> 15
 *     lp->fgain     = 16384 - fc
 *     fccoef[0..7]  = 0
 *     fccoef[8+k]   = 16384 * (fc/16384)^(k+1),  k = 0..7
 *
 * Three things follow, and together they leave no freedom.
 *
 * 1. Loading the coefficients through aLoadADPCM puts them in the ADPCM book,
 *    which the decoder reads as book[0][0..7] (the two-samples-back predictor)
 *    and book[1][0..7] (the one-sample-back predictor). Here the first half is
 *    all zeros: the second pole is switched off, and what the reverb actually
 *    asks for is a one-pole filter. The eight powers in the second half are the
 *    RSP computing eight outputs per vector step; a scalar CPU needs only the
 *    first, because y[n-1] is available at every sample.
 *
 * 2. The fixed-point base is 16384, not the decoder's 2048 -- SCALE in
 *    drvrnew.c, and the reason `fgain = SCALE - fc` is written that way.
 *
 * 3. fgain + fc == 16384 exactly, so the filter has unity gain at DC. That is
 *    the property that makes it safe inside the delay loop, and the check that
 *    the shift is right: any other shift would make the reverb either die or
 *    run away.
 *
 * So per sample:  y = (fcoef[0] * y[-2] + fcoef[8] * y[-1] + gain * x) >> 14.
 *
 * The two-samples-back term is kept even though this game always loads zero
 * there, because it costs one multiply and it is what makes this a *pole*
 * filter rather than a special case that happens to work.
 *
 * State is POLEF_STATE, `short[4]` (include/PR/abi.h:250). Two slots hold the
 * output history; A_INIT means start from silence, which is what `lp->first`
 * requests on the first frame after the reverb is built.
 *
 * Why this matters beyond the tail's colour: `_saveBuffer(out_ptr, buff2)`
 * writes the *filtered* buffer back into the delay line, so the filter sits
 * inside the feedback path. Skipping it leaves an undamped accumulator, which
 * is why GC_AUDIO_FX had to force the reverb off while this was missing.
 */
static void a_pole_filter(u8 flags, s16 gain, s16 *state) {
    const s16 *coef2 = sRspa.adpcm_table[0][0]; /* fccoef[0..7]: y[n-2] */
    const s16 *coef1 = sRspa.adpcm_table[0][1]; /* fccoef[8..15]: y[n-1] */
    s16 *in = DMEM_S16(sRspa.in);
    s16 *out = DMEM_S16(sRspa.out);
    int count = ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16);
    s32 prev1;
    s32 prev2;
    int i;

    if (flags & A_INIT) {
        prev2 = 0;
        prev1 = 0;
    } else {
        prev2 = state[0];
        prev1 = state[1];
    }

    for (i = 0; i < count; i++) {
        s32 acc = coef2[0] * prev2 + coef1[0] * prev1 + gain * (s32) in[i];
        s16 y = clamp16(acc >> 14);

        out[i] = y;
        prev2 = prev1;
        prev1 = y;
    }

    state[0] = (s16) prev2;
    state[1] = (s16) prev1;
}

static void a_mix(s16 gain, u16 in_addr, u16 out_addr) {
    int nbytes = ROUND_UP_32(sRspa.nbytes);
    s16 *in = DMEM_S16(in_addr);
    s16 *out = DMEM_S16(out_addr);
    int i;

    if (gain == -0x8000) {
        /* -1.0 is the one gain the fixed-point multiply below cannot express,
         * so the microcode special-cases it to a plain subtraction. */
        while (nbytes > 0) {
            for (i = 0; i < 16; i++) {
                *out = clamp16(*out - *in++);
                out++;
            }
            nbytes -= 16 * (int) sizeof(s16);
        }
        return;
    }

#if defined(__ARM_NEON)
    /* NEON: out*0x7fff + in*gain fits in s32 (gain != -0x8000 here), and
     * vqrshrn by 15 is exactly clamp16((x + 0x4000) >> 15). */
    {
        const int16x4_t k = vdup_n_s16(0x7fff), g = vdup_n_s16(gain);
        for (i = 0; i < nbytes / (int) sizeof(s16); i += 8) {
            const int16x8_t o = vld1q_s16(out + i), x = vld1q_s16(in + i);
            int32x4_t lo = vmlal_s16(vmull_s16(vget_low_s16(o), k), vget_low_s16(x), g);
            int32x4_t hi = vmlal_s16(vmull_s16(vget_high_s16(o), k), vget_high_s16(x), g);
            vst1q_s16(out + i, vcombine_s16(vqrshrn_n_s32(lo, 15), vqrshrn_n_s32(hi, 15)));
        }
    }
#else
    while (nbytes > 0) {
        for (i = 0; i < 16; i++) {
            s32 sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;

            *out++ = clamp16(sample);
        }
        nbytes -= 16 * (int) sizeof(s16);
    }
#endif
}

/* ---- instrumentation ----------------------------------------------------- */

/*
 * The same shape as the renderer's `ignored:` line, for the same reason.
 *
 * Verification here is not visual: "the mixer is silent" and "the mixer is
 * running" differ by one integer, so the integer gets printed rather than
 * argued about. Three numbers do it -- what the walker executed, what it
 * dropped, and the peak absolute sample that actually left DMEM for DRAM.
 *
 * The peak is sampled at aSaveBuffer because that is the only opcode that
 * writes back out of DMEM, so it measures what the audio manager will hand to
 * osAiSetNextBuffer rather than anything internal to the mix.
 */
#ifdef GC_DEBUG
u32 gGcAudioOpcodes[16];
u32 gGcAudioIgnored[256];
u32 gGcAudioCmds;
u32 gGcAudioSaves;
u32 gGcAudioPeak;
u32 gGcAudioSamples;
u32 gGcAudioClipped;

/*
 * Where the clicks are born.
 *
 * The DAC side counts a thousand discontinuities a second in a race and none
 * in the menus (`ai steps`), with zero underruns -- so the steps are in the
 * mixed signal, not in its delivery. Every voice passes through three stages
 * that carry state from one 160-sample chunk to the next: the ADPCM decoder,
 * the resampler and the envelope mixer. A stage whose output does not join up
 * with its own previous chunk is a stage whose state handling is wrong, and
 * which one it is decides what to read. So each stage remembers the last
 * sample it produced (or, for the envelope mixer, received) per state pointer
 * -- the state pointer is the voice -- and counts the joins that jump by more
 * than a third of full scale. Steps *inside* a chunk are counted too, apart.
 */
#define AUD_STEP_THRESHOLD 11000
#define AUD_VOICE_SLOTS 64

u32 gGcAudStepAdpcmJoin, gGcAudStepAdpcmIn;
u32 gGcAudStepResampJoin, gGcAudStepResampIn;
u32 gGcAudStepEnvJoin, gGcAudStepEnvIn;

typedef struct {
    const void *key;
    s16 last;
} AudVoiceLast;

static AudVoiceLast sVoiceLast[3][AUD_VOICE_SLOTS];

/* The last sample this stage produced for this voice; -1 slot state means
 * "never seen", in which case nothing is counted for the join. */
static s16 *voice_last(int stage, const void *key) {
    AudVoiceLast *tbl = sVoiceLast[stage];
    int i;

    for (i = 0; i < AUD_VOICE_SLOTS; i++) {
        if (tbl[i].key == key) {
            return &tbl[i].last;
        }
    }
    for (i = 0; i < AUD_VOICE_SLOTS; i++) {
        if (tbl[i].key == NULL) {
            tbl[i].key = key;
            tbl[i].last = 0;
            return NULL;
        }
    }
    return NULL;
}

static void note_steps(int stage, const void *key, const s16 *samples, int n, BOOL init,
                       u32 *join, u32 *inside) {
    s16 *last = voice_last(stage, key);
    int i;

    if (n <= 0) {
        return;
    }
    if (last != NULL && !init) {
        s32 d = (s32) samples[0] - (s32) *last;

        if (d > AUD_STEP_THRESHOLD || d < -AUD_STEP_THRESHOLD) {
            (*join)++;
        }
    }
    for (i = 1; i < n; i++) {
        s32 d = (s32) samples[i] - (s32) samples[i - 1];

        if (d > AUD_STEP_THRESHOLD || d < -AUD_STEP_THRESHOLD) {
            (*inside)++;
        }
    }
    if (last != NULL) {
        *last = samples[n - 1];
    } else {
        s16 *slot = voice_last(stage, key);

        if (slot != NULL) {
            *slot = samples[n - 1];
        }
    }
}

u32 gGcAudLane0[7], gGcAudLaneN[7], gGcAudLaneCount[7];

/*
 * Curvature of a mono buffer at lane 0 of each eight-sample vector, against
 * lane 4 of the same vector. See the note in gc_ultra.h: the captured output
 * has every eighth left sample wrong, so this asks each stage in turn whether
 * the buffer it is holding already has that shape.
 */
static void note_lane0(int probe, const s16 *b, int n) {
    int i;

    for (i = 8; i + 8 < n; i += 8) {
        s32 c0 = (s32) b[i - 1] - 2 * (s32) b[i] + (s32) b[i + 1];
        s32 c4 = (s32) b[i + 3] - 2 * (s32) b[i + 4] + (s32) b[i + 5];

        gGcAudLane0[probe] += (u32) (c0 < 0 ? -c0 : c0);
        gGcAudLaneN[probe] += (u32) (c4 < 0 ? -c4 : c4);
        gGcAudLaneCount[probe]++;
    }
}

#define GC_AUDIO_IGNORE(op) (gGcAudioIgnored[op]++)

/*
 * `peak` alone turned out to be too blunt an instrument, and it cost a day.
 *
 * It is a maximum over roughly half a million samples a second, so one clipped
 * sample pins it at 32767 and it reads exactly the same as a mix that is
 * railing continuously. Before the interleave fix it really was railing; after
 * it, peak still touched full scale in a quarter of the beats -- and that
 * number cannot distinguish "a loud drum hit clipped one sample" from "a
 * quarter of the output is square".
 *
 * So the clipped samples are counted as well. The ratio is what matters:
 * a handful in half a million is a loud game, several per cent is distortion.
 */
static void note_saved(const s16 *samples, int nbytes) {
    int n = nbytes / (int) sizeof(s16);
    int i;

    gGcAudioSaves++;
    gGcAudioSamples += (u32) n;
    for (i = 0; i < n; i++) {
        s32 v = samples[i];

        if (v < 0) {
            v = -v;
        }
        if (v >= 32767) {
            gGcAudioClipped++;
        }
        if ((u32) v > gGcAudioPeak) {
            gGcAudioPeak = (u32) v;
        }
    }
}
#else
#define GC_AUDIO_IGNORE(op)          ((void) 0)
#define note_saved(samples, nbytes)  ((void) 0)
#define note_steps(stage, key, samples, n, init, join, inside) ((void) 0)
#define note_lane0(probe, b, n)      ((void) 0)
#endif

/* ---- the walker ---------------------------------------------------------- */

/*
 * Whether this file renders the audio command list.
 *
 * It does now, and the flag stays because the output stage still has to know:
 * an unfilled buffer is not silence, it is whatever the allocator left there,
 * and played back sixty times a second that is a continuous tone. os_ai.c asks
 * this before it copies anything into the DAC ring.
 */

/*
 * Entry point from the scheduler.
 *
 * The list is executed to completion before returning, because the audio
 * manager blocks on the completion message and then immediately queues the
 * buffer it expects to have been filled.
 */
void port_audio_run_cmds(const void *cmdList, unsigned int sizeBytes) {
    const u32 *cmd = (const u32 *) cmdList;
    const u32 *end = cmd + (sizeBytes / sizeof(u32));

#ifdef GC_DEBUG
    memset(gGcAudioOpcodes, 0, sizeof(gGcAudioOpcodes));
    memset(gGcAudioIgnored, 0, sizeof(gGcAudioIgnored));
    gGcAudioCmds = 0;
    gGcAudioSaves = 0;
    gGcAudioPeak = 0;
    gGcAudioSamples = 0;
    gGcAudioClipped = 0;
#endif

    while (cmd + 1 < end) {
        u32 w0 = cmd[0];
        u32 w1 = cmd[1];
        u32 op = w0 >> 24;

        cmd += 2;

#ifdef GC_DEBUG
        gGcAudioCmds++;
        if (op < 16) {
            gGcAudioOpcodes[op]++;
        }
#endif

        switch (op) {
            case A_SPNOOP:
                break;

            case A_ADPCM:
                a_adpcm_dec((u8) (w0 >> 16), (s16 *) dram(w1));
#ifdef GC_DEBUG
                note_steps(0, dram(w1), DMEM_S16(sRspa.out) + 16,
                           ROUND_UP_32(sRspa.nbytes) / (int) sizeof(s16),
                           ((w0 >> 16) & (A_INIT | A_LOOP)) != 0,
                           &gGcAudStepAdpcmJoin, &gGcAudStepAdpcmIn);
                note_lane0(6, DMEM_S16(sRspa.out) + 16,
                           ROUND_UP_32(sRspa.nbytes) / (int) sizeof(s16));
#endif
                break;

            case A_CLEARBUFF:
                a_clear_buffer((u16) w0, (int) w1);
                break;

            case A_ENVMIXER:
#ifdef GC_DEBUG
                /* The input, before mixing: this is the resampled, decoded
                 * voice as the envelope sees it, per voice. */
                note_steps(2, dram(w1), DMEM_S16(sRspa.in),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16),
                           ((w0 >> 16) & A_INIT) != 0, &gGcAudStepEnvJoin, &gGcAudStepEnvIn);
#endif
#ifdef GC_DEBUG
                /* One voice's worth, either side of the envelope mixer: what
                 * it is handed, and the two buffers it accumulates into. */
                note_lane0(2, DMEM_S16(sRspa.in),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16));
#endif
                a_env_mixer((u8) (w0 >> 16), (s16 *) dram(w1));
#ifdef GC_DEBUG
                note_lane0(3, DMEM_S16(sRspa.out),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16));
                note_lane0(4, DMEM_S16(sRspa.dry_right),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16));
#endif
                break;

            case A_LOADBUFF:
                a_load_buffer(dram(w1));
                break;

            case A_RESAMPLE:
                a_resample((u8) (w0 >> 16), (u16) w0, (s16 *) dram(w1));
#ifdef GC_DEBUG
                note_steps(1, dram(w1), DMEM_S16(sRspa.out),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16),
                           ((w0 >> 16) & A_INIT) != 0, &gGcAudStepResampJoin,
                           &gGcAudStepResampIn);                note_lane0(5, DMEM_S16(sRspa.out),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16));
#endif
                break;

            case A_SAVEBUFF: {
                s16 *dst = (s16 *) dram(w1);

                a_save_buffer(dst);
                note_saved(dst, ROUND_UP_8(sRspa.nbytes));
                break;
            }

            /*
             * Segment addressing. The synthesiser emits exactly one of these
             * per frame -- aSegment(cmdPtr++, 0, 0) in synthesizer.c: segment
             * zero, base zero, which is the identity and nothing to do. A
             * non-zero base would mean audio pointers need resolving through a
             * table, so it is counted rather than assumed away.
             */
            case A_SEGMENT:
                if (w1 != 0) {
                    GC_AUDIO_IGNORE(op);
                }
                break;

            case A_SETBUFF:
                a_set_buffer((u8) (w0 >> 16), (u16) w0, (u16) (w1 >> 16), (u16) w1);
                break;

            case A_SETVOL:
                a_set_volume((u8) (w0 >> 16), (s16) w0, (s16) (w1 >> 16), (s16) w1);
                break;

            case A_DMEMMOVE:
                a_dmem_move((u16) w0, (u16) (w1 >> 16), (int) (u16) w1);
                break;

            case A_LOADADPCM:
                a_load_adpcm((int) (w0 & 0xFFFFFF), dram(w1));
                break;

            case A_MIXER:
                a_mix((s16) w0, (u16) (w1 >> 16), (u16) w1);
                break;

            case A_INTERLEAVE:
            #ifdef GC_DEBUG
                /* The finished mix, one channel each, as the interleave sees
                 * it. If mainL is dirty here and mixL below is not, the
                 * interleave itself is at fault. */
                note_lane0(0, DMEM_S16((u16) (w1 >> 16)),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16));
                note_lane0(1, DMEM_S16((u16) w1),
                           ROUND_UP_16(sRspa.nbytes) / (int) sizeof(s16));
#endif
                a_interleave((u16) (w1 >> 16), (u16) w1);
                break;

            /*
             * A_POLEF is only ever emitted by the reverb, so the first one is
             * the moment the effect starts running -- a path that was refused
             * outright until 2026-09-04 and has never executed on hardware.
             * Marked once, so a crash near here has something to point at.
             */
            case A_POLEF: {
                a_pole_filter((u8) (w0 >> 16), (s16) w0, (s16 *) dram(w1));
                break;
            }

            case A_SETLOOP:
                a_set_loop((s16 *) dram(w1));
                break;

            default:
                GC_AUDIO_IGNORE(op);
                break;
        }
    }
}
