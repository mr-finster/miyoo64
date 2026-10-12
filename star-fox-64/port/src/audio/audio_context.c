#include "sys.h"
#include "sf64audio.h"

u64 gAudioContextStart[2];
SynthesisReverb gSynthReverbs[4];
u8 sAudioContextPad10[0x10]; // 0x10
u16 D_8014C1B0;              // set to spec->unk_14, never used
s8 gUseReverb;
s8 gNumSynthReverbs;
s16 gDCTF_PlaybackRate; // IDCT related
NoteSampleState* gSampleStateList;
// 0x4
AudioAllocPool gSessionPool;
AudioAllocPool gInitPool;
AudioAllocPool gMiscPool;
u8 gAudioContextPad20[0x20]; // 0x20
AudioAllocPool gCachePool;
AudioAllocPool gPersistentCommonPool;
AudioAllocPool gTemporaryCommonPool;
AudioCache gSeqCache;
AudioCache gFontCache;
AudioCache gSampleBankCache;
PermanentCache gPermanentPool;
AudioSampleCache gPersistentSampleCache;
// 0x4
AudioSampleCache gTemporarySampleCache;
// 0x4
AudioSessionPoolSplit gSessionPoolSplit;
AudioCachePoolSplit gCachePoolSplit;
AudioCommonPoolSplit gPersistentCommonPoolSplit;
// 0x4
AudioCommonPoolSplit gTemporaryCommonPoolSplit;
// 0x4
u8 gSampleFontLoadStatus[64];
u8 gFontLoadStatus[64];
u8 gSeqLoadStatus[256];
volatile u8 gAudioResetStep;
u8 gAudioSpecId;
s32 gResetFadeoutFramesLeft;
u8 sAudioContextPad1000[0x1000]; // 0x1000 gap
Note* gNotes;
// 0x4
SequencePlayer gSeqPlayers[SEQ_PLAYER_MAX];
SequenceChannel gSeqChannels[48];
SequenceLayer gSeqLayers[64];
SequenceChannel gSeqChannelNone;
AudioListItem gLayerFreeList;
NotePool gNoteFreeLists;
Sample* gUsedSamples[128];
AudioPreloadReq gPreloadSampleStack[128];
s32 gNumUsedSamples;
s32 gPreloadSampleStackTop;
AudioAsyncLoad gAsyncLoads[16];
OSMesgQueue gExternalLoadQueue;
OSMesg gExternalLoadMsg[16];
OSMesgQueue gPreloadSampleQueue;
OSMesg gPreloadSampleMsg[16];
OSMesgQueue gCurAudioFrameDmaQueue;
OSMesg gCurAudioFrameDmaMsg[64];
OSIoMesg gCurAudioFrameDmaIoMsgBuf[64];
OSMesgQueue gSyncDmaQueue;
OSMesg gSyncDmaMsg[1];
// 0x4
OSIoMesg gSyncDmaIoMsg;
SampleDma gSampleDmas[0x100];
u32 gSampleDmaCount;
u32 gSampleDmaListSize1;
s32 D_80155A50; // Set to 0, never used
// 0x4
u8 gSampleDmaReuseQueue1[0x100];
u8 gSampleDmaReuseQueue2[0x100];
u8 gSampleDmaReuseQueue1RdPos;
u8 gSampleDmaReuseQueue2RdPos;
u8 gSampleDmaReuseQueue1WrPos;
u8 gSampleDmaReuseQueue2WrPos;
AudioTable* gSequenceTable;
AudioTable* gSoundFontTable;
AudioTable* gSampleBankTable;
u8* gSeqFontTable;
s16 gNumSequences;
SoundFont* gSoundFontList;
// 0x4
AudioBufferParameters gAudioBufferParams;
s32 gSampleDmaBuffSize;
s32 gMaxAudioCmds;
s32 gNumNotes;
s16 gMaxTempo;
s8 gAudioSoundMode;
volatile s32 gAudioTaskCountQ;
s32 gCurAudioFrameDmaCount;
s32 gAudioTaskIndexQ;
s32 gCurAiBuffIndex;
Acmd* gAbiCmdBuffs[2];
Acmd* gCurAbiCmdBuffer;
SPTask* gAudioCurTask;
SPTask gAudioRspTasks[2];
f32 gMaxTempoTvTypeFactors;
s32 gRefreshRate;
s16* gAiBuffers[3];
s16 gAiBuffLengths[3];
u32 gAudioRandom;
u32 gAudioErrorFlags;
volatile u32 gAudioResetTimer;

u64 gAudioContextEnd[2];

ReverbSettings sCoTrReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x40, 0x4000, 0xD000, 0x3000 } };
ReverbSettings sMeReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x38, 0x6400, 0x1000, 0x1000 } };
ReverbSettings sTiReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x40, 0x4000, 0, 0 } };
ReverbSettings sSxReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x5000, 0, 0 } };
ReverbSettings sZoReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x40, 0x6000, 0, 0 } };
ReverbSettings sA6ReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sVeReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x5800, 0, 0 } };
ReverbSettings sSyReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x5000, 0, 0 } };
ReverbSettings sFoReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sSoReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x40, 0x6000, 0, 0 } };
ReverbSettings sBoReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sKaReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x40, 0x4000, 0, 0 } };
ReverbSettings sAqReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 2, 0x28, 0x5800, 0, 0 } };
ReverbSettings sSzReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sMaReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x38, 0x5000, 0, 0 } };
ReverbSettings sAndReverbSettings[3] = {
    { 1, 0x30, 0x3000, 0, 0 },
    { 1, 0x38, 0x3000, 0, 0 },
    { 2, 0x48, 0x6000, 0xC000, 0x4000 },
};
ReverbSettings sVsReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sVsLastReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sSpec18ReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sSpec19ReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sSpec20ReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sSpec21ReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sOpeningReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x6800, 0, 0 } };
ReverbSettings sTitleReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x18, 0x3000, 0, 0 } };
ReverbSettings sMapReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 2, 0x18, 0x3000, 0, 0 } };
ReverbSettings sGameOverReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x18, 0x3000, 0, 0 } };
ReverbSettings sSpec26ReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings sEndingReverbSettings[2] = { { 1, 0x30, 0x3000, 0, 0 }, { 1, 0x30, 0x3000, 0, 0 } };
ReverbSettings D_800C76A8[] = {
    // unused?
    { 1, 0x40, 0x4FFF, 0, 0 },
    { 1, 0x30, 0x4FFF, 0, 0 },
    { 1, 0x30, 0x4FFF, 0, 0 },
    { 1, 0x30, 0x4FFF, 0, 0 },
};

// Original name: NA_SPEC_CONFIG
AudioSpec gAudioSpecs[] = {
    /*  0 AUDIOSPEC_CO */
    { 32000, 2, 22, ARRAY_COUNT(sCoTrReverbSettings), sCoTrReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  1 AUDIOSPEC_ME */
    { 32000, 2, 22, ARRAY_COUNT(sMeReverbSettings), sMeReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  2 AUDIOSPEC_TI */
    { 32000, 2, 22, ARRAY_COUNT(sTiReverbSettings), sTiReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  3 AUDIOSPEC_SX */
    { 32000, 2, 22, ARRAY_COUNT(sSxReverbSettings), sSxReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  4 AUDIOSPEC_ZO */
    { 32000, 2, 22, ARRAY_COUNT(sZoReverbSettings), sZoReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  5 AUDIOSPEC_A6 */
    { 32000, 2, 22, ARRAY_COUNT(sA6ReverbSettings), sA6ReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  6 AUDIOSPEC_VE */
    { 32000, 2, 22, ARRAY_COUNT(sVeReverbSettings), sVeReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  7 AUDIOSPEC_SY */
    { 32000, 2, 22, ARRAY_COUNT(sSyReverbSettings), sSyReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  8 AUDIOSPEC_FO */
    { 32000, 2, 22, ARRAY_COUNT(sFoReverbSettings), sFoReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /*  9 AUDIOSPEC_SO */
    { 32000, 2, 22, ARRAY_COUNT(sSoReverbSettings), sSoReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /* 10 AUDIOSPEC_BO */
    { 32000, 2, 22, ARRAY_COUNT(sBoReverbSettings), sBoReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /* 11 AUDIOSPEC_KA */
    { 32000, 2, 22, ARRAY_COUNT(sKaReverbSettings), sKaReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /* 12 AUDIOSPEC_AQ */
    { 32000, 1, 22, ARRAY_COUNT(sAqReverbSettings), sAqReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x2B000, 0x35000 },
    /* 13 AUDIOSPEC_SZ */
    { 32000, 2, 22, ARRAY_COUNT(sSzReverbSettings), sSzReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /* 14 AUDIOSPEC_MA */
    { 32000, 2, 22, ARRAY_COUNT(sMaReverbSettings), sMaReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /* 15 AUDIOSPEC_AND */
    { 32000, 2, 22, ARRAY_COUNT(sAndReverbSettings), sAndReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x34000 },
    /* 16 AUDIOSPEC_VS */
    { 32000, 2, 22, ARRAY_COUNT(sVsReverbSettings), sVsReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
    /* 17 AUDIOSPEC_VS_LAST */
    { 32000, 2, 22, ARRAY_COUNT(sVsLastReverbSettings), sVsLastReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x35000 },
    /* 18 AUDIOSPEC_18 */
    { 32000, 2, 22, ARRAY_COUNT(sSpec18ReverbSettings), sSpec18ReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x35000 },
    /* 19 AUDIOSPEC_19 */
    { 32000, 2, 22, ARRAY_COUNT(sSpec19ReverbSettings), sSpec19ReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x35000 },
    /* 20 AUDIOSPEC_20 */
    { 32000, 2, 22, ARRAY_COUNT(sSpec20ReverbSettings), sSpec20ReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x35000 },
    /* 21 AUDIOSPEC_21 */
    { 32000, 2, 32, ARRAY_COUNT(sSpec21ReverbSettings), sSpec21ReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x00000 },
    /* 22 AUDIOSPEC_OPENING */
    { 32000, 1, 32, ARRAY_COUNT(sOpeningReverbSettings), sOpeningReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x00000 },
    /* 23 AUDIOSPEC_TITLE */
    { 32000, 1, 32, ARRAY_COUNT(sTitleReverbSettings), sTitleReverbSettings, 0x7FFF, 0x1200, 0xA000, 0, 0x5B00, 0x1D00,
      0, 0x00000, 0x00000 },
    /* 24 AUDIOSPEC_MAP */
    { 32000, 1, 22, ARRAY_COUNT(sMapReverbSettings), sMapReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x00000, 0x60000 },
    /* 25 AUDIOSPEC_GAME_OVER */
    { 32000, 2, 22, ARRAY_COUNT(sGameOverReverbSettings), sGameOverReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x00000 },
    /* 26 AUDIOSPEC_26 */
    { 32000, 2, 22, ARRAY_COUNT(sSpec26ReverbSettings), sSpec26ReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x00000 },
    /* 27 AUDIOSPEC_ENDING */
    { 32000, 2, 32, ARRAY_COUNT(sEndingReverbSettings), sEndingReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000,
      0x2400, 0, 0x1B000, 0x00000 },
    /* 28 AUDIOSPEC_TR */
    { 32000, 2, 22, ARRAY_COUNT(sCoTrReverbSettings), sCoTrReverbSettings, 0x7FFF, 0x1200, 0x1100, 0, 0x5000, 0x2400, 0,
      0x1B000, 0x35000 },
};
s32 D_800C7C28 = 0x20000000; // unused?
s16 gSeqTicksPerBeat = 0x30;
s32 gAudioHeapSize = 0xAFE00;
s32 gInitPoolSize = 0x26000;
u32 gPermanentPoolSize = 0x21000;
u16 gSequenceMedium = MEDIUM_RAM;
u16 gSoundFontMedium = MEDIUM_RAM;
u16 gSampleBankMedium = MEDIUM_RAM;
