/* From BattleShip port/bridge/audio_bridge.cpp (MIT, see port/NOTICE.md), converted to C. */
/**
 * audio_bridge.c -- port-side audio asset loader.
 *
 * Replaces syAudioLoadAssets() (called from the PORT path of
 * syAudioThreadMain in src/sys/audio.c). The audio banks are read straight
 * from the player's ROM slice (hh/romdata.h) instead of an O2R archive.
 *
 * N64 audio banks (ALBankFile) are nested structures whose pointer fields
 * are 4-byte big-endian offsets into the CTL file. Even though this target
 * is ILP32 (same struct layout as the N64) the data is big-endian and the
 * host is little-endian, so each struct is parsed field by field into native
 * structs allocated from the game's own audio heap (sSYAudioHeap, backed by
 * the static gSYAudioHeapBuffer), exactly as BattleShip does.
 *
 * TBL (sample table) data is raw ADPCM bytes, used in place: wavetable base
 * pointers point directly into the read-only ROM slice (too big for the
 * audio heap). Sequence data inside the SBK is copied into the heap.
 *
 * BattleShip blob name -> ROM region (Torch yaml yamls/us/audio.yml):
 *   audio/S1_music_sbk    ROMDATA_S1_MUSIC_SBK   .. ROMDATA_B1_SOUNDS1_CTL
 *   audio/B1_sounds1_ctl  ROMDATA_B1_SOUNDS1_CTL .. ROMDATA_B1_SOUNDS1_TBL
 *   audio/B1_sounds1_tbl  ROMDATA_B1_SOUNDS1_TBL .. ROMDATA_B1_SOUNDS2_CTL
 *   audio/B1_sounds2_ctl  ROMDATA_B1_SOUNDS2_CTL .. ROMDATA_B1_SOUNDS2_TBL
 *   audio/B1_sounds2_tbl  ROMDATA_B1_SOUNDS2_TBL .. ROMDATA_FGM_UNK
 *   audio/fgm_unk         ROMDATA_FGM_UNK        .. ROMDATA_FGM_TBL
 *   audio/fgm_tbl         ROMDATA_FGM_TBL        .. ROMDATA_FGM_UCD
 *   audio/fgm_ucd         ROMDATA_FGM_UCD        .. ROMDATA_SLICE_END
 */

#include <ssb_types.h>
#include <PR/libaudio.h>
#include <sys/audio.h>
#include <sys/scheduler.h>

#include <string.h>

#include "bridge/audio_bridge.h"
#include "romdata.h"
#include "port_log.h"

/* Decomp globals (src/sys/audio.c) not all exported by sys/audio.h. */
extern SYAudioSettings dSYAudioPublicSettings;
extern SYAudioSettings dSYAudioPublicSettings2;
extern SYAudioSettings dSYAudioPublicSettings3;
extern SYAudioSettings sSYAudioCurrentSettings;
extern ALBank *sSYAudioSequenceBank1;
extern ALBank *sSYAudioSequenceBank2;
extern ALSeqFile *sSYAudioSeqFile;
extern u8 *sSYAudioBGMSequenceDatas[];
extern Acmd *sSYAudioAcmdListBuffers[];
extern SYTaskAudio *sSYAudioSchedulerTasks[];
extern s16 *sSYAudioDataBuffers[];
extern ALHeap sSYAudioHeap;

/* ========================================================================= */
/*  Big-endian readers                                                       */
/* ========================================================================= */

static inline u16 readBE16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline s16 readBE16s(const u8 *p) { return (s16)readBE16(p); }
static inline u32 readBE32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline s32 readBE32s(const u8 *p) { return (s32)readBE32(p); }

/* ========================================================================= */
/*  ROM regions                                                              */
/* ========================================================================= */

typedef struct AudioBlob {
    const u8 *data;
    u32 size;
} AudioBlob;

/* Only valid during/after portAudioLoadAssets; point into the read-only
 * ROM slice mapping, never into the heap. */
static AudioBlob sounds1_ctl, sounds1_tbl, sounds2_ctl, sounds2_tbl;
static AudioBlob music_sbk;
static AudioBlob fgm_unk_blob, fgm_tbl_blob, fgm_ucd_blob;

static int loadBlob(const char *name, u32 rom_start, u32 rom_end, AudioBlob *out)
{
    u32 size = rom_end - rom_start;
    const u8 *p = romdata_ptr(rom_start, size);

    if (p == NULL) {
        port_log("audio_bridge: ROM region %s (0x%08X, 0x%X bytes) unavailable\n",
                 name, (unsigned)rom_start, (unsigned)size);
        out->data = NULL;
        out->size = 0;
        return 0;
    }
    out->data = p;
    out->size = size;
    return 1;
}

/* ========================================================================= */
/*  Heap allocation                                                          */
/* ========================================================================= */

static int sHeapExhausted;

static void *heapAlloc(s32 size)
{
    void *p = alHeapAlloc(&sSYAudioHeap, 1, size);

    if (p == NULL && !sHeapExhausted) {
        sHeapExhausted = 1;
        port_log("audio_bridge: audio heap exhausted (request %d bytes)\n", (int)size);
    }
    return p;
}

/* ========================================================================= */
/*  ALBankFile parser -- big-endian N64 binary -> native structs             */
/* ========================================================================= */

/* N64 ROM binary field sizes (all big-endian):
 * ALBankFile:    2(rev) + 2(cnt) + 4*cnt(offsets)
 * ALBank:        2(instCnt) + 1(flags) + 1(pad) + 4(rate) + 4(perc) + 4*instCnt
 * ALInstrument:  12 x u8 + 2(bend) + 2(cnt) + 4*soundCount = 16+4*N
 * ALSound:       4(env) + 4(key) + 4(wav) + 1(pan) + 1(vol) + 1(flags) + 1(pad)
 * ALEnvelope:    4+4+4+1+1
 * ALKeyMap:      6 x u8
 * ALWaveTable:   4(base) + 4(len) + 1(type) + 1(flags) + 2(pad) + 4(loop) + 4(book)
 * ALADPCMBook:   4(order) + 4(npred) + 2*order*npred*8(book)
 * ALADPCMloop:   4+4+4+32(state)
 * ALRawLoop:     4+4+4 */

/* Offset -> parsed pointer cache (replaces BattleShip's std::unordered_map).
 * Only used while a bank is being parsed; nothing persistent. */
#define BANK_CACHE_SLOTS 16384 /* power of two; CTLs are < 64 KB */

typedef struct BankCacheEntry {
    u32 key; /* CTL offset + 1, 0 = empty */
    void *ptr;
} BankCacheEntry;

typedef struct BankParser {
    const u8 *ctl;
    u32 ctlSize;
    u8 *tbl;
    int bad;
} BankParser;

static BankCacheEntry sBankCache[BANK_CACHE_SLOTS];

static void cacheReset(void)
{
    memset(sBankCache, 0, sizeof(sBankCache));
}

static u32 cacheSlot(u32 off)
{
    return (off * 2654435761u) >> (32 - 14); /* 14 = log2(BANK_CACHE_SLOTS) */
}

static void *cacheLookup(u32 off)
{
    u32 i = cacheSlot(off);

    while (sBankCache[i].key != 0) {
        if (sBankCache[i].key == off + 1)
            return sBankCache[i].ptr;
        i = (i + 1) & (BANK_CACHE_SLOTS - 1);
    }
    return NULL;
}

static void *cacheStore(u32 off, void *ptr)
{
    u32 i = cacheSlot(off);
    u32 n;

    if (ptr == NULL)
        return NULL;
    for (n = 0; n < BANK_CACHE_SLOTS; n++) {
        if (sBankCache[i].key == 0 || sBankCache[i].key == off + 1) {
            sBankCache[i].key = off + 1;
            sBankCache[i].ptr = ptr;
            return ptr;
        }
        i = (i + 1) & (BANK_CACHE_SLOTS - 1);
    }
    return ptr; /* table full: just stop caching (never happens for SSB CTLs) */
}

/* Returns a pointer to `need` bytes at `off` in the CTL, or NULL. */
static const u8 *ctlAt(BankParser *bp, u32 off, u32 need)
{
    if (off > bp->ctlSize || need > bp->ctlSize - off) {
        if (!bp->bad)
            port_log("audio_bridge: CTL offset 0x%X (+%u) out of range (size 0x%X)\n",
                     (unsigned)off, (unsigned)need, (unsigned)bp->ctlSize);
        bp->bad = 1;
        return NULL;
    }
    return bp->ctl + off;
}

static ALEnvelope *parseEnvelope(BankParser *bp, u32 off)
{
    ALEnvelope *e = cacheLookup(off);
    const u8 *r;

    if (e != NULL)
        return e;
    if ((r = ctlAt(bp, off, 14)) == NULL || (e = heapAlloc(sizeof(ALEnvelope))) == NULL)
        return NULL;
    e->attackTime = readBE32s(r + 0);
    e->decayTime = readBE32s(r + 4);
    e->releaseTime = readBE32s(r + 8);
    e->attackVolume = r[12];
    e->decayVolume = r[13];
    return cacheStore(off, e);
}

static ALKeyMap *parseKeyMap(BankParser *bp, u32 off)
{
    ALKeyMap *k = cacheLookup(off);
    const u8 *r;

    if (k != NULL)
        return k;
    if ((r = ctlAt(bp, off, 6)) == NULL || (k = heapAlloc(sizeof(ALKeyMap))) == NULL)
        return NULL;
    k->velocityMin = r[0];
    k->velocityMax = r[1];
    k->keyMin = r[2];
    k->keyMax = r[3];
    k->keyBase = r[4];
    k->detune = (s8)r[5];
    return cacheStore(off, k);
}

static ALADPCMBook *parseBook(BankParser *bp, u32 off)
{
    ALADPCMBook *b = cacheLookup(off);
    const u8 *r;
    s32 order, npred, n, i;

    if (b != NULL)
        return b;
    if ((r = ctlAt(bp, off, 8)) == NULL)
        return NULL;
    order = readBE32s(r + 0);
    npred = readBE32s(r + 4);
    /* Legal N64 ADPCM books are order 2, npredictors <= 8. Reject corrupt
     * fields before the multiply can overflow and under-allocate. */
    if (order <= 0 || order > 8 || npred <= 0 || npred > 8) {
        port_log("audio_bridge: book at 0x%X has bad order=%d npred=%d\n",
                 (unsigned)off, (int)order, (int)npred);
        return NULL;
    }
    n = order * npred * 8; /* s16 entries */
    if ((r = ctlAt(bp, off, 8 + n * 2)) == NULL)
        return NULL;
    /* BattleShip allocates order*npred*16 s16s (twice what is filled). */
    if ((b = heapAlloc(8 + order * npred * 16 * (s32)sizeof(s16))) == NULL)
        return NULL;
    b->order = order;
    b->npredictors = npred;
    for (i = 0; i < n; i++)
        b->book[i] = readBE16s(r + 8 + i * 2);
    return cacheStore(off, b);
}

static ALADPCMloop *parseADPCMLoop(BankParser *bp, u32 off)
{
    ALADPCMloop *l = cacheLookup(off);
    const u8 *r;
    int i;

    if (l != NULL)
        return l;
    if ((r = ctlAt(bp, off, 44)) == NULL || (l = heapAlloc(sizeof(ALADPCMloop))) == NULL)
        return NULL;
    l->start = readBE32(r + 0);
    l->end = readBE32(r + 4);
    l->count = readBE32(r + 8);
    for (i = 0; i < 16; i++) /* ADPCM_STATE: 16 s16 at offset 12 */
        l->state[i] = readBE16s(r + 12 + i * 2);
    return cacheStore(off, l);
}

static ALRawLoop *parseRawLoop(BankParser *bp, u32 off)
{
    ALRawLoop *l = cacheLookup(off);
    const u8 *r;

    if (l != NULL)
        return l;
    if ((r = ctlAt(bp, off, 12)) == NULL || (l = heapAlloc(sizeof(ALRawLoop))) == NULL)
        return NULL;
    l->start = readBE32(r + 0);
    l->end = readBE32(r + 4);
    l->count = readBE32(r + 8);
    return cacheStore(off, l);
}

static ALWaveTable *parseWaveTable(BankParser *bp, u32 off)
{
    ALWaveTable *w = cacheLookup(off);
    const u8 *r;
    u32 loopOff;

    if (w != NULL)
        return w;
    if ((r = ctlAt(bp, off, 20)) == NULL || (w = heapAlloc(sizeof(ALWaveTable))) == NULL)
        return NULL;
    w->base = bp->tbl + readBE32(r + 0); /* points directly into TBL data */
    w->len = readBE32s(r + 4);
    w->type = r[8];
    w->flags = 1; /* mark as "already patched" */
    /* Union starts at offset 12 in ROM (after 2 pad bytes at 10-11). */
    loopOff = readBE32(r + 12);
    if (w->type == AL_ADPCM_WAVE) {
        u32 bookOff = readBE32(r + 16);
        w->waveInfo.adpcmWave.loop = loopOff ? parseADPCMLoop(bp, loopOff) : NULL;
        w->waveInfo.adpcmWave.book = bookOff ? parseBook(bp, bookOff) : NULL;
    } else {
        w->waveInfo.rawWave.loop = loopOff ? parseRawLoop(bp, loopOff) : NULL;
    }
    return cacheStore(off, w);
}

static ALSound *parseSound(BankParser *bp, u32 off)
{
    ALSound *s = cacheLookup(off);
    const u8 *r;
    u32 envOff, keyOff, wavOff;

    if (s != NULL)
        return s;
    if ((r = ctlAt(bp, off, 16)) == NULL || (s = heapAlloc(sizeof(ALSound))) == NULL)
        return NULL;
    envOff = readBE32(r + 0);
    keyOff = readBE32(r + 4);
    wavOff = readBE32(r + 8);
    s->envelope = envOff ? parseEnvelope(bp, envOff) : NULL;
    s->keyMap = keyOff ? parseKeyMap(bp, keyOff) : NULL;
    s->wavetable = wavOff ? parseWaveTable(bp, wavOff) : NULL;
    s->samplePan = r[12];
    s->sampleVolume = r[13];
    s->flags = 1; /* already patched */
    return cacheStore(off, s);
}

static ALInstrument *parseInstrument(BankParser *bp, u32 off)
{
    ALInstrument *inst = cacheLookup(off);
    const u8 *r;
    s16 soundCount, i;
    s32 allocSize;

    if (inst != NULL)
        return inst;
    if ((r = ctlAt(bp, off, 16)) == NULL)
        return NULL;
    soundCount = readBE16s(r + 14);
    if (soundCount < 0 || (r = ctlAt(bp, off, 16 + soundCount * 4)) == NULL)
        return NULL;
    /* Variable length: base struct + soundCount pointer slots. */
    allocSize = sizeof(ALInstrument) + (soundCount > 1 ? (soundCount - 1) * sizeof(ALSound *) : 0);
    if ((inst = heapAlloc(allocSize)) == NULL)
        return NULL;
    inst->volume = r[0];
    inst->pan = r[1];
    inst->priority = r[2];
    inst->flags = 1; /* already patched */
    inst->tremType = r[4];
    inst->tremRate = r[5];
    inst->tremDepth = r[6];
    inst->tremDelay = r[7];
    inst->vibType = r[8];
    inst->vibRate = r[9];
    inst->vibDepth = r[10];
    inst->vibDelay = r[11];
    inst->bendRange = readBE16s(r + 12);
    inst->soundCount = soundCount;
    for (i = 0; i < soundCount; i++) {
        u32 sndOff = readBE32(r + 16 + i * 4);
        inst->soundArray[i] = sndOff ? parseSound(bp, sndOff) : NULL;
    }
    return cacheStore(off, inst);
}

static ALBank *parseBank(BankParser *bp, u32 off)
{
    ALBank *bank = cacheLookup(off);
    const u8 *r;
    s16 instCount, i;
    s32 allocSize;
    u32 percOff;

    if (bank != NULL)
        return bank;
    if ((r = ctlAt(bp, off, 12)) == NULL)
        return NULL;
    instCount = readBE16s(r + 0);
    if (instCount < 0 || (r = ctlAt(bp, off, 12 + instCount * 4)) == NULL)
        return NULL;
    allocSize = sizeof(ALBank) + (instCount > 1 ? (instCount - 1) * sizeof(ALInstrument *) : 0);
    if ((bank = heapAlloc(allocSize)) == NULL)
        return NULL;
    bank->instCount = instCount;
    bank->flags = 1; /* already patched */
    bank->pad = r[3];
    bank->sampleRate = readBE32s(r + 4);
    percOff = readBE32(r + 8);
    bank->percussion = percOff ? parseInstrument(bp, percOff) : NULL;
    for (i = 0; i < instCount; i++) {
        u32 instOff = readBE32(r + 12 + i * 4);
        bank->instArray[i] = instOff ? parseInstrument(bp, instOff) : NULL;
    }
    return cacheStore(off, bank);
}

/* Parses bank 0 of a CTL file (the only one SSB64 uses). */
static ALBank *parseFirstBank(const AudioBlob *ctl, const AudioBlob *tbl)
{
    BankParser bp;
    const u8 *r;
    ALBankFile *bf;
    s16 bankCount, i;
    s32 allocSize;

    bp.ctl = ctl->data;
    bp.ctlSize = ctl->size;
    bp.tbl = (u8 *)tbl->data; /* read-only in practice: the mixer only loads from it */
    bp.bad = 0;
    cacheReset();

    if ((r = ctlAt(&bp, 0, 4)) == NULL)
        return NULL;
    bankCount = readBE16s(r + 2);
    if (bankCount <= 0 || ctlAt(&bp, 0, 4 + bankCount * 4) == NULL) {
        port_log("audio_bridge: CTL has bad bank count %d\n", (int)bankCount);
        return NULL;
    }
    allocSize = sizeof(ALBankFile) + (bankCount > 1 ? (bankCount - 1) * sizeof(ALBank *) : 0);
    if ((bf = heapAlloc(allocSize)) == NULL)
        return NULL;
    bf->revision = readBE16s(r + 0);
    bf->bankCount = bankCount;
    for (i = 0; i < bankCount; i++)
        bf->bankArray[i] = parseBank(&bp, readBE32(r + 4 + i * 4));
    return bf->bankArray[0];
}

/* ========================================================================= */
/*  ALSeqFile parser                                                         */
/* ========================================================================= */

/* N64 binary: 2(rev) + 2(cnt) + cnt * (4(offset) + 4(len)); offsets are
 * relative to the start of the SBK. */
static ALSeqFile *parseSeqFile(const u8 *sbk, u32 sbkSize)
{
    ALSeqFile *sf;
    u8 *sbkCopy;
    s16 seqCount, i;
    s32 allocSize;

    if (sbkSize < 4)
        return NULL;
    seqCount = readBE16s(sbk + 2);
    if (seqCount <= 0 || 4 + (u32)seqCount * 8 > sbkSize) {
        port_log("audio_bridge: SBK has bad sequence count %d\n", (int)seqCount);
        return NULL;
    }
    allocSize = sizeof(ALSeqFile) + (seqCount > 1 ? (seqCount - 1) * sizeof(ALSeqData) : 0);
    if ((sf = heapAlloc(allocSize)) == NULL)
        return NULL;
    sf->revision = readBE16s(sbk + 0);
    sf->seqCount = seqCount;

    /* Copy the raw SBK into the heap so sequence pointers stay valid. */
    if ((sbkCopy = heapAlloc((s32)sbkSize)) == NULL)
        return NULL;
    memcpy(sbkCopy, sbk, sbkSize);

    for (i = 0; i < seqCount; i++) {
        u32 off = readBE32(sbk + 4 + i * 8);
        s32 len = readBE32s(sbk + 4 + i * 8 + 4);

        if (off > sbkSize || len < 0 || (u32)len > sbkSize - off) {
            port_log("audio_bridge: sequence %d out of range (0x%X+0x%X)\n",
                     (int)i, (unsigned)off, (unsigned)len);
            off = 0;
            len = 0;
        }
        sf->seqArray[i].offset = sbkCopy + off;
        sf->seqArray[i].len = len;
    }
    return sf;
}

/* ========================================================================= */
/*  SYAudioPackage parser                                                    */
/* ========================================================================= */

/* N64 binary: 4(count) + count * 4(offset); offsets are relative to the
 * start of the package. The entries' payloads stay big-endian (the n_env
 * FGM code reads them byte-wise). */
static SYAudioPackage *parsePackage(const u8 *data, u32 size)
{
    SYAudioPackage *pkg;
    u8 *pkgData;
    s32 count, i;
    s32 allocSize;

    if (size < 4)
        return NULL;
    count = readBE32s(data);
    if (count <= 0 || 4 + (u32)count * 4 > size) {
        port_log("audio_bridge: package has bad count %d\n", (int)count);
        return NULL;
    }
    allocSize = sizeof(SYAudioPackage) + (count > 1 ? (count - 1) * sizeof(uintptr_t) : 0);
    if ((pkg = heapAlloc(allocSize)) == NULL)
        return NULL;
    pkg->count = count;

    /* Copy the raw package into the heap so offset pointers stay valid. */
    if ((pkgData = heapAlloc((s32)size)) == NULL)
        return NULL;
    memcpy(pkgData, data, size);

    for (i = 0; i < count; i++)
        pkg->data[i] = (uintptr_t)(pkgData + readBE32(data + 4 + i * 4));
    return pkg;
}

/* ========================================================================= */
/*  Main loader                                                              */
/* ========================================================================= */

void portAudioLoadAssets(void)
{
    ALBank *bank;
    s32 maxSeqLen, i;

    sHeapExhausted = 0;

    /* Initialize heap. */
    memset(sSYAudioCurrentSettings.heap_base, 0, sSYAudioCurrentSettings.heap_size);
    alHeapInit(&sSYAudioHeap, sSYAudioCurrentSettings.heap_base,
               (s32)sSYAudioCurrentSettings.heap_size);

    if (!loadBlob("B1_sounds1_ctl", ROMDATA_B1_SOUNDS1_CTL, ROMDATA_B1_SOUNDS1_TBL, &sounds1_ctl) ||
        !loadBlob("B1_sounds1_tbl", ROMDATA_B1_SOUNDS1_TBL, ROMDATA_B1_SOUNDS2_CTL, &sounds1_tbl) ||
        !loadBlob("B1_sounds2_ctl", ROMDATA_B1_SOUNDS2_CTL, ROMDATA_B1_SOUNDS2_TBL, &sounds2_ctl) ||
        !loadBlob("B1_sounds2_tbl", ROMDATA_B1_SOUNDS2_TBL, ROMDATA_FGM_UNK, &sounds2_tbl) ||
        !loadBlob("S1_music_sbk", ROMDATA_S1_MUSIC_SBK, ROMDATA_B1_SOUNDS1_CTL, &music_sbk)) {
        port_log("audio_bridge: core audio data missing, audio disabled\n");
        return;
    }

    /* Bank -> slot mapping must match the original SYAudioSettings:
     *   bank1_start = B1_sounds2_ctl -> sSYAudioSequenceBank1 (SFX direct-index)
     *   bank2_start = B1_sounds1_ctl -> sSYAudioSequenceBank2 (music CSP players)
     * sys/audio.c does n_alCSPSetBank(player, sSYAudioSequenceBank2) and reads
     * SFX sounds from sSYAudioSequenceBank1->instArray[0]->soundArray. */
    bank = parseFirstBank(&sounds1_ctl, &sounds1_tbl);
    if (bank == NULL) {
        port_log("audio_bridge: failed to parse B1_sounds1_ctl\n");
        return;
    }
    sSYAudioSequenceBank2 = bank;

    bank = parseFirstBank(&sounds2_ctl, &sounds2_tbl);
    if (bank == NULL) {
        port_log("audio_bridge: failed to parse B1_sounds2_ctl\n");
        return;
    }
    sSYAudioSequenceBank1 = bank;

    /* Sequence file. */
    sSYAudioSeqFile = parseSeqFile(music_sbk.data, music_sbk.size);
    if (sSYAudioSeqFile == NULL) {
        port_log("audio_bridge: failed to parse S1_music_sbk\n");
        return;
    }

    /* Find max sequence length and allocate sequence data buffers. */
    maxSeqLen = 0;
    for (i = 0; i < sSYAudioSeqFile->seqCount; i++) {
        sSYAudioSeqFile->seqArray[i].len += (sSYAudioSeqFile->seqArray[i].len & 1);
        if (maxSeqLen < sSYAudioSeqFile->seqArray[i].len)
            maxSeqLen = sSYAudioSeqFile->seqArray[i].len;
    }
    for (i = 0; i < SYAUDIO_BGMPLAYERS_NUM; i++)
        sSYAudioBGMSequenceDatas[i] = heapAlloc(maxSeqLen);

    /* Acmd list double buffers. */
    sSYAudioAcmdListBuffers[0] = heapAlloc(0x8000);
    sSYAudioAcmdListBuffers[1] = heapAlloc(0x8000);

    /* Scheduler task buffers. */
    sSYAudioSchedulerTasks[0] = heapAlloc(sizeof(SYTaskAudio));
    sSYAudioSchedulerTasks[1] = heapAlloc(sizeof(SYTaskAudio));

    /* Audio output data buffers (triple buffered, 0xE60 bytes = 920 stereo frames). */
    sSYAudioDataBuffers[0] = heapAlloc(0xE60);
    sSYAudioDataBuffers[1] = heapAlloc(0xE60);
    sSYAudioDataBuffers[2] = heapAlloc(0xE60);

    /* FGM packages (optional -- the game still runs without SFX). */
    if (loadBlob("fgm_unk", ROMDATA_FGM_UNK, ROMDATA_FGM_TBL, &fgm_unk_blob)) {
        /* fgm_unk is [BE u32 count][16-byte struct[count]]: u8 unk0..unk3 then
         * f32 unk4/unk8/unkC (ALWhatever8009EE0C_3). The consumer indexes it
         * with a 16-byte stride, so unk44 points at the first struct. The f32
         * words are converted from BE to host order; the u8s are copied. */
        const u8 *raw = fgm_unk_blob.data;
        s32 count = readBE32s(raw);
        s32 totalStructBytes = count * 16;

        if (count <= 0 || 4 + (u32)totalStructBytes > fgm_unk_blob.size) {
            port_log("audio_bridge: fgm_unk rejected (count=%d size=%u)\n",
                     (int)count, (unsigned)fgm_unk_blob.size);
        } else {
            u8 *flat = heapAlloc(totalStructBytes);

            if (flat != NULL) {
                for (i = 0; i < count; i++) {
                    const u8 *src = raw + 4 + i * 16;
                    u8 *dst = flat + i * 16;
                    s32 w;

                    memcpy(dst, src, 4);
                    for (w = 1; w < 4; w++) {
                        u32 v = readBE32(src + w * 4);
                        memcpy(dst + w * 4, &v, 4);
                    }
                }
                dSYAudioPublicSettings.unk4C = sSYAudioCurrentSettings.unk4C = (u16)count;
                dSYAudioPublicSettings.unk44 = sSYAudioCurrentSettings.unk44 = (uintptr_t *)flat;
            }
        }
    }

    if (loadBlob("fgm_tbl", ROMDATA_FGM_TBL, ROMDATA_FGM_UCD, &fgm_tbl_blob)) {
        SYAudioPackage *pkg = parsePackage(fgm_tbl_blob.data, fgm_tbl_blob.size);

        if (pkg != NULL) {
            dSYAudioPublicSettings2.fgm_table_count = sSYAudioCurrentSettings.fgm_table_count = (u16)pkg->count;
            dSYAudioPublicSettings.fgm_table_data = sSYAudioCurrentSettings.fgm_table_data = pkg->data;
        }
    }

    if (loadBlob("fgm_ucd", ROMDATA_FGM_UCD, ROMDATA_SLICE_END, &fgm_ucd_blob)) {
        SYAudioPackage *pkg = parsePackage(fgm_ucd_blob.data, fgm_ucd_blob.size);

        if (pkg != NULL) {
            dSYAudioPublicSettings3.fgm_ucode_count = sSYAudioCurrentSettings.fgm_ucode_count = (u16)pkg->count;
            dSYAudioPublicSettings3.fgm_ucode_data = sSYAudioCurrentSettings.fgm_ucode_data = pkg->data;
        }
    }
}

/* Nothing to release: all data lives in the ROM mapping or the audio heap. */
void portAudioShutdownAssets(void)
{
}
