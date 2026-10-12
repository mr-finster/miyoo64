/**
 * Star Fox 64 (from f-zero-x 47fc96b: F-Zero X's fonts share the layout).
 * Sound fonts arrive from the ROM in the N64's byte order, and the decomp reads
 * them as native structs (include/sf64audio.h). A font opens with u32 offsets -- [0] the
 * drum list, [1..n] the instruments -- to Drum / Instrument / Sample /
 * AdpcmLoop / AdpcmBook records. Their fields are bytes, 32-bit words and s16
 * arrays, so on a 32-bit little-endian build the layout already matches: one
 * in-place pass per fresh load (AudioLoad_RelocateFont, before it relocates)
 * fixes the byte order.
 *
 * - The Sample's first word is a bitfield (codec:4 medium:2 unk:1 reloc:1
 *   size:24, first field most significant on the N64); it is re-packed in the
 *   compiler's own bitfield order through an identical declaration.
 * - Envelopes stay big-endian: sequences embed their own, so the ADSR reads
 *   every envelope that way (audio_effects.c ENV_DELAY).
 * - Samples, loops and books can be shared, so each is converted once
 *   (a bitmap over the font's words).
 */
#include <stdint.h>
#include <string.h>
#include <ultra64.h>
#include "port.h"
#include "savestate.h"

typedef struct {
    uint32_t codec : 4;
    uint32_t medium : 2;
    uint32_t unk_bit26 : 1;
    uint32_t isRelocated : 1;
    uint32_t size : 24;
} PortSampleBits; /* sf64audio.h's Sample word 0, same declaration */

#define FONT_MAX_BYTES 0x40000
static uint8_t sSeen[FONT_MAX_BYTES / 4 / 8] SAVESTATE_EXCLUDE; /* scratch for one conversion */

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static inline void swap32_at(uint8_t* p) {
    uint32_t v = be32(p);
    memcpy(p, &v, 4);
}

static inline void swap16_at(uint8_t* p) {
    uint8_t t = p[0];
    p[0] = p[1];
    p[1] = t;
}

static inline uint32_t native32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* True the first time a record at `off` is visited (off is 4-byte aligned). */
static int first_visit(uint32_t off) {
    uint32_t w = off >> 2;
    if (sSeen[w >> 3] & (1u << (w & 7))) {
        return 0;
    }
    sSeen[w >> 3] |= (uint8_t) (1u << (w & 7));
    return 1;
}

static int in_font(uint32_t off, uint32_t len, uint32_t size) {
    return off != 0 && (off & 3) == 0 && off <= size && len <= size - off;
}

static void conv_loop(uint8_t* f, uint32_t size, uint32_t off) {
    uint32_t i;
    if (!in_font(off, 0x10, size) || !first_visit(off)) {
        return;
    }
    swap32_at(f + off + 0x0); /* start */
    swap32_at(f + off + 0x4); /* end */
    swap32_at(f + off + 0x8); /* count */
    if (native32(f + off + 0x8) != 0 && in_font(off, 0x30, size)) {
        for (i = 0; i < 16; i++) {
            swap16_at(f + off + 0x10 + i * 2); /* predictorState */
        }
    }
}

static void conv_book(uint8_t* f, uint32_t size, uint32_t off) {
    uint32_t i, n;
    int32_t order, npred;
    if (!in_font(off, 8, size) || !first_visit(off)) {
        return;
    }
    swap32_at(f + off + 0x0);
    swap32_at(f + off + 0x4);
    order = (int32_t) native32(f + off + 0x0);
    npred = (int32_t) native32(f + off + 0x4);
    if (order <= 0 || npred <= 0 || order > 16 || npred > 16) {
        port_log("audio: font book at %X has order %d x %d predictors\n", off, order, npred);
        return;
    }
    n = 8u * (uint32_t) order * (uint32_t) npred;
    if (!in_font(off, 8 + n * 2, size)) {
        return;
    }
    for (i = 0; i < n; i++) {
        swap16_at(f + off + 8 + i * 2);
    }
}

static void conv_sample(uint8_t* f, uint32_t size, uint32_t off) {
    PortSampleBits bits;
    uint32_t v;
    if (!in_font(off, 0x10, size) || !first_visit(off)) {
        return;
    }
    v = be32(f + off);
    memset(&bits, 0, sizeof(bits));
    bits.codec = v >> 28;
    bits.medium = (v >> 26) & 3;
    bits.unk_bit26 = (v >> 25) & 1;
    bits.isRelocated = (v >> 24) & 1;
    bits.size = v & 0xFFFFFF;
    memcpy(f + off, &bits, 4);
    swap32_at(f + off + 0x4); /* sampleAddr (offset into the sample bank) */
    swap32_at(f + off + 0x8); /* loop */
    swap32_at(f + off + 0xC); /* book */
    conv_loop(f, size, native32(f + off + 0x8));
    conv_book(f, size, native32(f + off + 0xC));
}

static void conv_instrument(uint8_t* f, uint32_t size, uint32_t off) {
    uint32_t k;
    if (!in_font(off, 0x20, size) || !first_visit(off)) {
        return;
    }
    for (k = 0x04; k < 0x20; k += 4) {
        swap32_at(f + off + k); /* envelope, then (sample, tuning) x 3 */
    }
    /* The same samples AudioLoad_RelocateFont relocates. */
    if (f[off + 1] != 0) {
        conv_sample(f, size, native32(f + off + 0x08));
    }
    conv_sample(f, size, native32(f + off + 0x10));
    if (f[off + 2] != 0x7F) {
        conv_sample(f, size, native32(f + off + 0x18));
    }
}

static void conv_drum(uint8_t* f, uint32_t size, uint32_t off) {
    if (!in_font(off, 0x10, size) || !first_visit(off)) {
        return;
    }
    swap32_at(f + off + 0x4); /* tunedSample.sample */
    swap32_at(f + off + 0x8); /* tunedSample.tuning */
    swap32_at(f + off + 0xC); /* envelope */
    conv_sample(f, size, native32(f + off + 0x4));
}

void port_audio_font_to_native(void* font, uint32_t size, int32_t numInstruments, int32_t numDrums) {
    uint8_t* f = (uint8_t*) font;
    uint32_t drums, i;
    int32_t k;

    if (size < 4 || size > FONT_MAX_BYTES || numInstruments < 0 || (uint32_t) (numInstruments + 1) * 4 > size) {
        port_log("audio: font of %u bytes (%d instruments) not converted\n", size, numInstruments);
        return;
    }
    memset(sSeen, 0, (size / 4 + 7) / 8);
    for (k = 0; k <= numInstruments; k++) {
        swap32_at(f + k * 4);
    }
    drums = native32(f);
    if (drums != 0 && numDrums > 0 && in_font(drums, (uint32_t) numDrums * 4, size)) {
        for (i = 0; i < (uint32_t) numDrums; i++) {
            swap32_at(f + drums + i * 4);
            conv_drum(f, size, native32(f + drums + i * 4));
        }
    }
    for (k = 1; k <= numInstruments; k++) {
        conv_instrument(f, size, native32(f + k * 4));
    }
}
