/**
 * The player's ROM, DMA from it, and MIO0.
 *
 * The ROM file is mapped read-only (page cache, not process RAM) and checked
 * against the US rev0 SHA-1 once. Every PI DMA copies straight out of it, as on
 * the N64; any asset words the copy touches are then put in native byte order
 * with the generated maps (port_swapmap.h). Segments stored MIO0-compressed are
 * fixed after decoding instead: mio0Decode (C, replacing the decomp's hand asm)
 * finds which ROM block its input was DMA'd from.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "libultra/ultra64.h"
#include "savestate.h"
#include "port_os.h"
#include "port_swapmap.h"

#define ROM_SIZE 0x1000000

static const u8* sRom;
static u32 sRomSize;

/* ------------------------------------------------------------------ SHA-1 --- */
typedef struct {
    u32 h[5];
    u64 len;
    u8 buf[64];
    u32 n;
} Sha1;

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(Sha1* s, const u8* p) {
    u32 w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((u32) p[i * 4] << 24) | ((u32) p[i * 4 + 1] << 16) | ((u32) p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

/* Set by sdl/rom_find.c around the first-boot check: called every 256 KB, 0-99. */
void (*port_rom_hash_progress)(int pct);

static void sha1(const u8* data, u32 len, u8 out[20]) {
    Sha1 s = { { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 };
    u32 i;
    u8 tail[128];
    u32 rem = len % 64, full = len - rem, tl;
    for (i = 0; i < full; i += 64) {
        if (port_rom_hash_progress != NULL && (i & 0x3FFFF) == 0) {
            port_rom_hash_progress((int) ((u64) i * 100 / len));
        }
        sha1_block(&s, data + i);
    }
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + full, rem);
    tail[rem] = 0x80;
    tl = (rem < 56) ? 64 : 128;
    {
        u64 bits = (u64) len * 8;
        for (i = 0; i < 8; i++) {
            tail[tl - 1 - i] = (u8) (bits >> (8 * i));
        }
    }
    for (i = 0; i < tl; i += 64) {
        sha1_block(&s, tail + i);
    }
    for (i = 0; i < 5; i++) {
        out[i * 4] = s.h[i] >> 24; out[i * 4 + 1] = s.h[i] >> 16; out[i * 4 + 2] = s.h[i] >> 8; out[i * 4 + 3] = s.h[i];
    }
}

static const u8 kUsRev0Sha1[20] = { 0x5f, 0x65, 0x8e, 0x88, 0xff, 0xa9, 0xde, 0x23, 0xcb, 0xa6,
                                    0x98, 0x6a, 0x8f, 0xd3, 0xd3, 0xa9, 0x0d, 0x7b, 0x43, 0x40 };

/* ------------------------------------------------------------------- open --- */
int port_rom_open_checked(const char* path, int skipHash, char* err, int errlen);

int port_rom_open(const char* path, char* err, int errlen) {
    return port_rom_open_checked(path, 0, err, errlen);
}

/* skipHash: the SHA-1 already passed for this exact file (sdl/rom_find.c). */
int port_rom_open_checked(const char* path, int skipHash, char* err, int errlen) {
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errlen, "cannot open %s", path);
        return 0;
    }
    if (fstat(fd, &st) != 0 || st.st_size != ROM_SIZE) {
        snprintf(err, errlen, "%s is not a 16 MB F-Zero X ROM", path);
        close(fd);
        return 0;
    }
    void* map = mmap(NULL, ROM_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        snprintf(err, errlen, "cannot map %s", path);
        return 0;
    }
    const u8* rom = map;
    if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
        /* .v64 / .n64 dumps: normalise once into anonymous memory */
        u8* copy = malloc(ROM_SIZE);
        if (copy == NULL) {
            snprintf(err, errlen, "out of memory");
            munmap(map, ROM_SIZE);
            return 0;
        }
        if (rom[0] == 0x37 && rom[1] == 0x80) {
            for (u32 i = 0; i < ROM_SIZE; i += 2) { copy[i] = rom[i + 1]; copy[i + 1] = rom[i]; }
        } else if (rom[0] == 0x40 && rom[1] == 0x12) {
            for (u32 i = 0; i < ROM_SIZE; i += 4) {
                copy[i] = rom[i + 3]; copy[i + 1] = rom[i + 2]; copy[i + 2] = rom[i + 1]; copy[i + 3] = rom[i];
            }
        } else {
            snprintf(err, errlen, "%s is not an N64 ROM", path);
            free(copy);
            munmap(map, ROM_SIZE);
            return 0;
        }
        munmap(map, ROM_SIZE);
        rom = copy;
    }
    if (!skipHash) {
        u8 digest[20];
        sha1(rom, ROM_SIZE, digest);
        if (memcmp(digest, kUsRev0Sha1, 20) != 0) {
            snprintf(err, errlen, "%s is not F-Zero X (USA) rev0 - SHA-1 mismatch", path);
            /* A refused ROM gives its 16 MB back: rom_find.c goes on to the card after the game folder's
             * copy is refused, and a .v64/.n64 copy is anonymous memory (a quarter of the handheld's). */
            if (rom == map) {
                munmap(map, ROM_SIZE);
            } else {
                free((void*) rom);
            }
            return 0;
        }
    }
    sRom = rom;
    sRomSize = ROM_SIZE;
    if (!skipHash) {
        port_log("rom: %s verified (F-Zero X US rev0)\n", path);
    }
    return 1;
}

const u8* port_rom_bytes(void) {
    return sRom;
}

/* -------------------------------------------------------------- byte order --- */
/* Per-segment index: run position every 1024 words, so a DMA of a small piece of
 * a big segment does not walk the run list from the start. */
#define IDX_STRIDE 1024
typedef struct {
    u32 run;
    u32 word; /* first word of that run */
} RunIndex;
static RunIndex* sIndex[64];

static void build_index(int s) {
    const SwapSegment* seg = &gSwapSegments[s];
    u32 words = seg->size / 4, n = words / IDX_STRIDE + 1, r = 0, w = 0;
    RunIndex* idx = calloc(n, sizeof(RunIndex));
    for (u32 i = 0; i < n; i++) {
        u32 target = i * IDX_STRIDE;
        while (r < seg->nruns && w + seg->runs[r].words <= target) {
            w += seg->runs[r].words;
            r++;
        }
        idx[i].run = r;
        idx[i].word = w;
    }
    sIndex[s] = idx;
}

/* Put image bytes [lo, hi) (offsets into segment s's image) of `dst` - which
 * holds those same bytes, big-endian, at dst[0] - into native order. `src` is
 * the whole big-endian image (for words the range only partly covers). */
static void swap_range(int s, const u8* be_image, u8* dst, u32 lo, u32 hi) {
    const SwapSegment* seg = &gSwapSegments[s];
    u32 wlo = lo / 4, whi = (hi + 3) / 4;
    if (sIndex[s] == NULL) {
        build_index(s);
    }
    const RunIndex* ix = &sIndex[s][wlo / IDX_STRIDE];
    u32 r = ix->run, rw = ix->word;
    while (r < seg->nruns && rw + seg->runs[r].words <= wlo) {
        rw += seg->runs[r].words;
        r++;
    }
    for (u32 w = wlo; w < whi && r < seg->nruns; w++) {
        while (w >= rw + seg->runs[r].words) {
            rw += seg->runs[r].words;
            r++;
            if (r >= seg->nruns) {
                return;
            }
        }
        const u8* p = gSwapPerms[seg->runs[r].perm];
        if (seg->runs[r].perm == 0) {
            continue; /* identity */
        }
        const u8* be = be_image + w * 4;
        for (u32 k = 0; k < 4; k++) {
            u32 off = w * 4 + k;
            if (off >= lo && off < hi) {
                dst[off - lo] = be[p[k]];
            }
        }
    }
}

void port_rom_dma(u32 romOffset, void* dst, u32 size) {
    romOffset &= 0x0FFFFFFF;
    if (sRom == NULL || romOffset >= sRomSize) {
        memset(dst, 0, size);
        return;
    }
    if (romOffset + size > sRomSize) {
        size = sRomSize - romOffset;
    }
    memcpy(dst, sRom + romOffset, size);
    for (int s = 0; s < gSwapSegmentCount; s++) {
        const SwapSegment* seg = &gSwapSegments[s];
        if (seg->compressed) {
            continue;
        }
        u32 a = romOffset > seg->rom ? romOffset : seg->rom;
        u32 b = (romOffset + size) < (seg->rom + seg->size) ? (romOffset + size) : (seg->rom + seg->size);
        if (a < b) {
            swap_range(s, sRom + seg->rom, (u8*) dst + (a - romOffset), a - seg->rom, b - seg->rom);
        }
    }
    /* remember where this buffer came from, for mio0Decode */
    extern void port_mio0_note_dma(const void* dst, u32 rom, u32 size);
    port_mio0_note_dma(dst, romOffset, size);
}

/* ------------------------------------------------------------------- MIO0 --- */
typedef struct {
    uintptr_t dst;
    u32 rom;
    u32 size;
} DmaNote;
#define NOTES 64
/* In save states: a state can fall between a DMA and the decode that looks it up. */
static DmaNote sNotes[NOTES] SAVESTATE_INCLUDE;
static u32 sNoteNext SAVESTATE_INCLUDE;

void port_mio0_note_dma(const void* dst, u32 rom, u32 size) {
    /* Dma_LoadAssets moves big blocks 1 KB at a time: extend the previous note
     * when this chunk continues it, so the block's header stays findable. */
    if (sNoteNext > 0) {
        DmaNote* last = &sNotes[(sNoteNext - 1) % NOTES];
        if (last->dst + last->size == (uintptr_t) dst && last->rom + last->size == rom) {
            last->size += size;
            return;
        }
    }
    DmaNote* n = &sNotes[sNoteNext++ % NOTES];
    n->dst = (uintptr_t) dst;
    n->rom = rom;
    n->size = size;
}

static s32 rom_of(const void* p) {
    uintptr_t a = (uintptr_t) p;
    for (u32 i = 0; i < NOTES; i++) {
        const DmaNote* n = &sNotes[(sNoteNext - 1 - i) % NOTES];
        if (n->size != 0 && a >= n->dst && a < n->dst + n->size) {
            return (s32) (n->rom + (a - n->dst));
        }
    }
    return -1;
}

static u32 be32(const u8* p) {
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* func_800AA6BC in the decomp's mio0Decode.s: the decoded size from a header. */
u32 func_800AA6BC(const u8* header) {
    return be32(header + 4);
}

void mio0Decode(u8* src, u8* dst) {
    u32 decSize = be32(src + 4), compOff = be32(src + 8), rawOff = be32(src + 12);
    const u8* layout = src + 16;
    const u8* comp = src + compOff;
    const u8* raw = src + rawOff;
    u32 out = 0, bits = 0, nbits = 0;
    s32 rom = rom_of(src);

    while (out < decSize) {
        if (nbits == 0) {
            bits = be32(layout);
            layout += 4;
            nbits = 32;
        }
        if (bits & 0x80000000) {
            dst[out++] = *raw++;
        } else {
            u16 v = (comp[0] << 8) | comp[1];
            comp += 2;
            u32 len = (v >> 12) + 3, dist = (v & 0xFFF) + 1;
            for (u32 i = 0; i < len && out < decSize; i++, out++) {
                dst[out] = dst[out - dist];
            }
        }
        bits <<= 1;
        nbits--;
    }

    if (rom < 0) {
        port_log("mio0: decoded %u bytes from an unknown source - byte order left as is\n", decSize);
        return;
    }
    for (int s = 0; s < gSwapSegmentCount; s++) {
        const SwapSegment* seg = &gSwapSegments[s];
        if (seg->compressed && seg->rom == (u32) rom) {
            /* the map is keyed to the big-endian image, which is dst itself: fix in place */
            u8* copy = malloc(decSize);
            memcpy(copy, dst, decSize);
            swap_range(s, copy, dst, 0, decSize < seg->size ? decSize : seg->size);
            free(copy);
            return;
        }
    }
    for (int b = 0; b < gSwapBlockCount; b++) {
        if (gSwapBlocks[b].rom == (u32) rom) {
            const u8* p = gSwapPerms[gSwapBlocks[b].perm];
            if (gSwapBlocks[b].perm != 0) {
                for (u32 i = 0; i + 3 < decSize; i += 4) {
                    u8 w[4] = { dst[i], dst[i + 1], dst[i + 2], dst[i + 3] };
                    dst[i] = w[p[0]]; dst[i + 1] = w[p[1]]; dst[i + 2] = w[p[2]]; dst[i + 3] = w[p[3]];
                }
            }
            return;
        }
    }
    port_log("mio0: no byte-order map for ROM block 0x%06X (%u bytes)\n", (u32) rom, decSize);
}

/* ROM_READ (HANDHELD): the game reads ROM words directly over the PI bus. */
u32 port_rom_read32(u32 romOffset) {
    romOffset &= 0x0FFFFFFC;
    if (sRom == NULL || romOffset + 4 > sRomSize) {
        return 0;
    }
    return be32(sRom + romOffset);
}
