/*
 * Bit-exact test of the mixer's NEON paths against its C loops (after
 * ocarina-of-time's mixer_test.c). mixer_test.sh builds src/port/audio/mixer.c twice
 * for the handheld -- as shipped (NEON) and with PORT_MIXER_SCALAR (the C loops,
 * public functions renamed ref_*) -- links both into this program and runs it
 * under qemu-arm. Every trial loads the same random DMEM into both mixers, runs
 * one command with the same random arguments, and compares all of DMEM and the
 * state the command writes back. Arguments stay inside what Star Fox's
 * synthesis sends (ranges below); byte contents are fully random.
 *
 *   mixer_test [trials per kernel] [seed]    exit 0 = no mismatch
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef short ADPCM_STATE[16];
typedef short RESAMPLE_STATE[16];

#define DMEM 0x1100

#define DECL(p)                                                                          \
    void p##aLoadBufferImpl(const void* source_addr, uint16_t dest_addr, uint16_t nbytes); \
    void p##aSaveBufferImpl(uint16_t source_addr, int16_t* dest_addr, uint16_t nbytes);    \
    void p##aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes);      \
    void p##aLoadADPCMImpl(int num_entries_times_16, const int16_t* book_source_addr);      \
    void p##aSetLoopImpl(ADPCM_STATE* adpcm_loop_state);                                   \
    void p##aADPCMdecImpl(uint8_t flags, ADPCM_STATE state);                               \
    void p##aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state);            \
    void p##aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr, uint16_t count);
DECL()
DECL(ref_)

static uint32_t sRng;
static uint32_t rnd(void) { /* xorshift32 */
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}
static uint32_t below(uint32_t n) { return rnd() % n; }

static uint8_t sDmem[DMEM];
static int16_t sOutA[DMEM / 2], sOutB[DMEM / 2];

static void fill_random(void) {
    uint32_t i;
    for (i = 0; i < DMEM; i++) {
        sDmem[i] = (uint8_t) rnd();
    }
}

static void load_both(void) {
    aLoadBufferImpl(sDmem, 0, DMEM);
    ref_aLoadBufferImpl(sDmem, 0, DMEM);
}

static int dmem_same(const char* what, uint32_t trial) {
    aSaveBufferImpl(0, sOutA, DMEM);
    ref_aSaveBufferImpl(0, sOutB, DMEM);
    if (memcmp(sOutA, sOutB, DMEM) != 0) {
        uint32_t i;
        for (i = 0; i < DMEM / 2 && sOutA[i] == sOutB[i]; i++) {
        }
        printf("MISMATCH %s trial %u: DMEM %03X neon %d ref %d\n", what, trial, i * 2, sOutA[i], sOutB[i]);
        return 0;
    }
    return 1;
}

static int16_t rnd_s16(void) {
    /* mostly full-range, sometimes the extremes the clamps are about */
    switch (below(8)) {
        case 0: return -0x8000;
        case 1: return 0x7FFF;
        default: return (int16_t) rnd();
    }
}

static uint32_t test_adpcm(uint32_t trials) {
    uint32_t t, bad = 0;
    for (t = 0; t < trials; t++) {
        int16_t book[8 * 2 * 8], loopA[16], loopB[16];
        ADPCM_STATE stA, stB;
        const int order = 2, npred = below(2) ? 2 : 4; /* Star Fox's books: 2x2 or 2x4 */
        const uint32_t frames = 1 + below(16), nbytes = frames * 32;
        const uint16_t out = (uint16_t) (2 * below(0x200));      /* up to 0x400 + 32 + 512 */
        const uint16_t in = (uint16_t) (0x700 + below(0x1100 - 0x700 - frames * 9 - 16));
        const uint8_t flags = (uint8_t) below(3); /* A_CONTINUE, A_INIT, A_LOOP */
        int i;
        for (i = 0; i < 8 * order * npred; i++) {
            book[i] = rnd_s16();
        }
        for (i = 0; i < 16; i++) {
            stA[i] = stB[i] = rnd_s16();
            loopA[i] = loopB[i] = rnd_s16();
        }
        fill_random();
        /* Frame headers as an encoder writes them: a predictor the book has (an
         * index past the table reads the mixer's own state and DMEM -- no data
         * does that), any scale 0..15 (Nintendo's encoder stays within 0..12). */
        for (i = 0; i < (int) frames; i++) {
            sDmem[in + i * 9] = (uint8_t) ((below(16) << 4) | below(npred));
        }
        load_both();
        aLoadADPCMImpl(16 * order * npred, book);
        ref_aLoadADPCMImpl(16 * order * npred, book);
        aSetLoopImpl(&loopA);
        ref_aSetLoopImpl(&loopB);
        aSetBufferImpl(0, in, out, (uint16_t) nbytes);
        ref_aSetBufferImpl(0, in, out, (uint16_t) nbytes);
        aADPCMdecImpl(flags, stA);
        ref_aADPCMdecImpl(flags, stB);
        if (!dmem_same("adpcm", t) || memcmp(stA, stB, sizeof(stA)) != 0) {
            if (bad++ < 3) {
                printf("  adpcm trial %u: in %03X out %03X nbytes %u flags %u npred %d\n", t, in, out, nbytes, flags,
                       npred);
            }
        }
    }
    return bad;
}

static uint32_t test_resample(uint32_t trials) {
    uint32_t t, bad = 0;
    for (t = 0; t < trials; t++) {
        RESAMPLE_STATE stA, stB;
        const uint32_t nbytes = 16 * (1 + below(24)); /* up to 192 samples out */
        const uint16_t in = (uint16_t) (0x40 + 2 * below(0x40));
        const uint16_t out = (uint16_t) (0x500 + 2 * below(0x80));
        const uint16_t pitch = (uint16_t) (below(4) == 0 ? rnd() : below(0x8000) + 0x100);
        const uint8_t flags = (uint8_t) (below(3) == 0 ? 1 : below(4) == 0 ? 2 : 0);
        int i;
        for (i = 0; i < 16; i++) {
            stA[i] = stB[i] = rnd_s16();
        }
        /* state[5] is what the previous call left: 0 or -8 - (1..7) */
        stA[5] = stB[5] = (int16_t) (below(2) ? 0 : -8 - (int) (1 + below(7)));
        fill_random();
        load_both();
        aSetBufferImpl(0, in, out, (uint16_t) nbytes);
        ref_aSetBufferImpl(0, in, out, (uint16_t) nbytes);
        aResampleImpl(flags, pitch, stA);
        ref_aResampleImpl(flags, pitch, stB);
        if (!dmem_same("resample", t) || memcmp(stA, stB, sizeof(stA)) != 0) {
            if (bad++ < 3) {
                printf("  resample trial %u: in %03X out %03X nbytes %u pitch %04X flags %u\n", t, in, out, nbytes,
                       pitch, flags);
            }
        }
    }
    return bad;
}

static uint32_t test_mix(uint32_t trials) {
    uint32_t t, bad = 0;
    for (t = 0; t < trials; t++) {
        const uint16_t count = (uint16_t) (16 * (1 + below(0x60))); /* up to 0x600 bytes */
        const int16_t gain = rnd_s16();
        const uint16_t out = (uint16_t) (2 * below(0x200));
        /* the synthesis mixes in place (reverb decay) or between disjoint buffers */
        const uint16_t in = below(4) == 0 ? out : (uint16_t) (0xA80 + 2 * below(0x20));
        fill_random();
        load_both();
        aMixImpl(gain, in, out, count);
        ref_aMixImpl(gain, in, out, count);
        if (!dmem_same("mix", t)) {
            if (bad++ < 3) {
                printf("  mix trial %u: in %03X out %03X count %u gain %d\n", t, in, out, count, gain);
            }
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    const uint32_t trials = argc > 1 ? (uint32_t) strtoul(argv[1], NULL, 0) : 200000;
    uint32_t a, r, m;
    sRng = argc > 2 ? (uint32_t) strtoul(argv[2], NULL, 0) : 0x5F64A0D1u;
    if (sRng == 0) {
        sRng = 1;
    }
    a = test_adpcm(trials);
    r = test_resample(trials);
    m = test_mix(trials);
    printf("mixer_test: %u trials each: adpcm %u, resample %u, mix %u mismatches\n", trials, a, r, m);
    return (a | r | m) ? 1 : 0;
}
