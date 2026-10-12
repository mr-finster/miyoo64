/* The native port's fixups for the sound files the game reads from the ROM (M2, game/include/port_fix.h). Game side.
 * The audio engine's files (the decomp's src/audio.h): the SBN file table, INIT (bank, song and ambience lists), PER
 * and PRG (drum and instrument presets), BGM songs, MSEQ ambience, SEF sound effects and BK instrument banks. Their
 * headers and tables are swapped to the host's byte order once, right after the read; the command streams (songs,
 * effects, envelopes) are bytes and stay as they are. What the RSP reads in the N64 (ADPCM samples, codebooks and
 * loop states) stays big-endian too: the port's mixer reads it as the RSP does. */
#include "common.h"
#include "audio.h"
#include "port_fix.h"
#include "port.h"

static void swap_halves(void *data, u32 bytes) {
    u16 *h = data;
    for (u32 i = 0; i < bytes / 2; i++) h[i] = __builtin_bswap16(h[i]);
}

/* parts shared inside one file are swapped once: a bit per halfword offset */
#define AU_MAX (1u << 17)
static u32 sAuSeen[AU_MAX / 2 / 32];
static u32 sAuSize;
static void au_seen_reset(u32 size) {
    sAuSize = size < AU_MAX ? size : AU_MAX;
    __builtin_memset(sAuSeen, 0, (sAuSize / 2 + 31) / 32 * 4);
}
static int au_claim(u32 off, u32 len) {
    if ((off & 1) || off + len > sAuSize || off + len < off) return 0;
    const u32 bit = off / 2;
    if (sAuSeen[bit >> 5] & (1u << (bit & 31))) return 0;
    sAuSeen[bit >> 5] |= 1u << (bit & 31);
    return 1;
}

void port_fix_audio_words(void *data, u32 bytes) { port_fix_words(data, bytes); }
void port_fix_audio_halves(void *data, u32 bytes) { swap_halves(data, bytes); }

/* INIT's header: the metadata words, then six u16 offsets and sizes */
void port_fix_init_header(void *data) {
    INITHeader *h = data;
    PORT_SWAP32(h->mdata.signature);
    PORT_SWAP32(h->mdata.size);
    swap_halves(&h->bankListOffset, 6 * sizeof(u16));
}

/* INIT's bank list: {u16 fileIndex, u8 bankIndex, u8 bankSet} */
void port_fix_bank_list(void *data, u32 bytes) {
    InitBankEntry *e = data;
    for (u32 i = 0; i < bytes / sizeof(InitBankEntry); i++) PORT_SWAP16(e[i].fileIndex);
}

/* PER: drum presets (BGMDrumInfo: u16 bankPatch, u16 keyBase, bytes); PRG: instrument presets (u16 bankPatch, bytes) */
void port_fix_drums(void *data, u32 bytes) {
    BGMDrumInfo *d = data;
    for (u32 i = 0; i < bytes / sizeof(BGMDrumInfo); i++) {
        PORT_SWAP16(d[i].bankPatch);
        PORT_SWAP16(d[i].keyBase);
    }
}
void port_fix_instruments(void *data, u32 bytes) {
    BGMInstrumentInfo *p = data;
    for (u32 i = 0; i < bytes / sizeof(BGMInstrumentInfo); i++) PORT_SWAP16(p[i].bankPatch);
}

/* BGM: header; up to four compositions (u32 commands to 0, PLAY_PHRASE 0x1000xxxx points at 16 u32 track words);
 * the drum and instrument tables. The tracks themselves are byte streams. */
void port_fix_bgm(void *data, u32 size) {
    BGMHeader *h = data;
    u8 *file = data;
    PORT_SWAP32(h->signature);
    PORT_SWAP32(h->size);
    PORT_SWAP32(h->name);
    swap_halves(h->info.compositions, 8 * sizeof(u16)); /* compositions[4], drums, drumCount, instruments, count */
    au_seen_reset(size);
    for (int v = 0; v < 4; v++) {
        const u32 comp = (u32) h->info.compositions[v] << 2;
        if (comp == 0 || !au_claim(comp, 4)) continue;
        for (u32 pos = comp; pos + 4 <= sAuSize; pos += 4) {
            u32 *w = (u32 *) (file + pos);
            if (pos != comp && !au_claim(pos, 4)) break; /* the rest was done from another composition */
            *w = __builtin_bswap32(*w);
            if (*w == 0) break;
            if ((*w >> 28) == 1) { /* play phrase: 16 track words at the composition's start + offset */
                const u32 phrase = comp + ((*w & 0xFFFF) << 2);
                if (phrase + 64 <= sAuSize && au_claim(phrase, 64)) port_fix_words(file + phrase, 64);
            }
        }
    }
    if (h->info.drums != 0) port_fix_drums(file + ((u32) h->info.drums << 2), h->info.drumCount * sizeof(BGMDrumInfo));
    if (h->info.instruments != 0)
        port_fix_instruments(file + ((u32) h->info.instruments << 2), h->info.instrumentCount * sizeof(BGMInstrumentInfo));
}

/* MSEQ: header (u16 track settings offset and data start) and its track settings; the data is a byte stream */
void port_fix_mseq(void *data) {
    MSEQHeader *h = data;
    PORT_SWAP32(h->signature);
    PORT_SWAP32(h->size);
    PORT_SWAP32(h->name);
    PORT_SWAP16(h->trackSettingsOffset);
    PORT_SWAP16(h->dataStart);
    if (h->trackSettingsCount != 0 && h->trackSettingsOffset != 0) {
        MSEQTrackData *t = (MSEQTrackData *) ((u8 *) data + h->trackSettingsOffset);
        for (u32 i = 0; i < h->trackSettingsCount; i++) {
            PORT_SWAP16(t[i].time);
            PORT_SWAP16(t[i].delta);
            PORT_SWAP16(t[i].goal);
        }
    }
}

/* SEF: header; sections 0-3 hold {u16 offset, u16 info} per sound (a polyphonic one points at 2/4/8 more such pairs);
 * sections 4-7 and the extra section hold their sounds' bytes inline */
void port_fix_sef(void *data, u32 size) {
    SEFHeader *h = data;
    u8 *file = data;
    PORT_SWAP32(h->mdata.signature);
    PORT_SWAP32(h->mdata.size);
    PORT_SWAP32(h->name);
    swap_halves(h->sections, 9 * sizeof(u16)); /* sections[8], section2000 */
    au_seen_reset(size);
    for (int s = 0; s < 4; s++) {
        const u32 start = h->sections[s];
        if (start == 0) continue;
        u32 end = size; /* up to the next section, at most 0xC0 sounds */
        for (int t = 0; t < 9; t++) {
            const u32 o = t < 8 ? h->sections[t] : h->section2000;
            if (o > start && o < end) end = o;
        }
        if (end > start + 0xC0 * 4) end = start + 0xC0 * 4;
        for (u32 pos = start; pos + 4 <= end; pos += 4) {
            if (!au_claim(pos, 4)) continue;
            u16 *e = (u16 *) (file + pos);
            e[0] = __builtin_bswap16(e[0]);
            e[1] = __builtin_bswap16(e[1]);
            const u32 mode = (e[1] & 0x60) >> 5;
            if (e[0] != 0 && mode != 0) {
                const u32 n = 2u << (mode - 1), list = e[0];
                if (list + n * 4 <= size && au_claim(list, n * 4)) swap_halves(file + list, n * 4);
            }
        }
    }
}

/* BK: the header (read on its own first, then again at the file's start); the instruments (Instrument: offsets and
 * sizes as words, codebookSize and keyBase as u16, the sample rate as a word, envelopes as a word) and their envelope
 * presets ({u8 count, pad[3]} then {u16 press, u16 release} per entry). The loop states and codebooks (s16 arrays the
 * RSP reads) go to the host's order too: the port's mixer keeps all of its samples in host order, as the game's own
 * compiled-in ones (DummyInstrumentCodebook) are. */
void port_fix_bk_header(void *data) {
    BKHeader *h = data;
    PORT_SWAP16(h->signature);
    PORT_SWAP32(h->size);
    PORT_SWAP32(h->name);
    PORT_SWAP16(h->format);
    swap_halves(h->instruments, (16 + 7) * sizeof(u16)); /* instruments[16], then the seven lengths and starts */
}

void port_fix_bk(void *data, u32 size) {
    BKHeader *h = data;
    u8 *file = data;
    port_fix_bk_header(h);
    au_seen_reset(size);
    if (h->loopStatesStart != 0 && h->loopStatesStart + (u32) h->loopStatesLength <= size)
        swap_halves(file + h->loopStatesStart, h->loopStatesLength);
    if (h->predictorsStart != 0 && h->predictorsStart + (u32) h->predictorsLength <= size)
        swap_halves(file + h->predictorsStart, h->predictorsLength);
    for (int i = 0; i < 16; i++) {
        const u32 off = h->instruments[i];
        if (off == 0 || off + sizeof(Instrument) > size || !au_claim(off, sizeof(Instrument))) continue;
        Instrument *in = (Instrument *) (file + off);
        PORT_SWAP32(in->wavData);
        PORT_SWAP32(in->wavDataLength);
        PORT_SWAP32(in->loopState);
        PORT_SWAP32(in->loopStart);
        PORT_SWAP32(in->loopEnd);
        PORT_SWAP32(in->loopCount);
        PORT_SWAP32(in->predictor);
        PORT_SWAP16(in->codebookSize);
        PORT_SWAP16(in->keyBase);
        PORT_SWAP32(in->sampleRate);
        PORT_SWAP32(in->envelopes);
        const u32 env = (u32) in->envelopes;
        if (env != 0 && env + 4 <= size && au_claim(env, 4)) {
            EnvelopePreset *p = (EnvelopePreset *) (file + env);
            if (env + 4 + p->count * 4u <= size) swap_halves(p->offsets, p->count * 4u);
        }
    }
}
