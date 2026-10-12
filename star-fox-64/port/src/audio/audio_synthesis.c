#include "sys.h"
#include "sf64audio.h"

#define DMEM_WET_SCRATCH 0x470
#define DMEM_COMPRESSED_ADPCM_DATA 0x990
#define DMEM_LEFT_CH 0x990
#define DMEM_RIGHT_CH 0xB10
#define DMEM_HAAS_TEMP 0x650
#define DMEM_TEMP 0x450
#define DMEM_UNCOMPRESSED_NOTE 0x5F0
#define DMEM_WET_LEFT_CH 0xC90
#define DMEM_WET_RIGHT_CH 0xE10 // = DMEM_WET_LEFT_CH + DMEM_1CH_SIZE
#define SAMPLE_SIZE sizeof(s16)

typedef enum {
    /* 0 */ HAAS_EFFECT_DELAY_NONE,
    /* 1 */ HAAS_EFFECT_DELAY_LEFT, // Delay left channel so that right channel is heard first
    /* 2 */ HAAS_EFFECT_DELAY_RIGHT // Delay right channel so that left channel is heard first
} HaasEffectDelaySide;

s32 D_80145D40; // unused

// all of these are part of the DFT-related function
f32 gIDCT_Buffer[256];
f32 gIDCT_Work[256];
f32 gIDCT_CoefTable[515]; // precomputed rotation coefficient table for a fast IDCT

f32 D_80146D54;
f32 D_80146D58;
f32 D_80146D5C;
f32 D_80146D60;
f32 D_80146D64;
f32 D_80146D68;
f32 D_80146D6C;
f32 D_80146D70;

static const char devstr0[] = "Table Remake\n";
static const char devstr1[] = "-------------------Undefined Ctype \n";
static const char devstr2[] = "Error? Limit OVER %d in %d\n";
static const char devstr3[] = "";
static const char devstr4[] = "Get %x  %x\n";
static const char devstr5[] = "Break %x %d %d \n";
static const char devstr6[] = "Terminate-Canceled Channel %d,Phase %d\n";
static const char devstr7[] = "S->W\n";
static const char devstr8[] = "W->S\n";
static const char devstr9[] = "S-Resample Pitch %x (old %d -> delay %d)\n";

// Original name: Nas_CpuFX
void AudioSynth_InitNextReverbRingBuf(s32 sampleCount, s32 itemIndex, s32 reverbIndex) {
    ReverbRingBufferItem* ringItem;
    SynthesisReverb* reverb = &gSynthReverbs[reverbIndex];
    s32 numSamples;
    s32 extraSamples;
    s32 i;
    s32 j;
    s32 pad1;
    s32 pad2;
    s32 pad3;

    if ((reverb->downsampleRate != 1) && (reverb->framesToIgnore == 0)) {
        ringItem = &reverb->items[reverb->curFrame][itemIndex];
        osInvalDCache(ringItem->toDownsampleLeft, 0x300);
        j = 0;
        for (i = 0; i < ringItem->lengthA / 2; i++, j += reverb->downsampleRate) {
            reverb->leftRingBuf[ringItem->startPos + i] = ringItem->toDownsampleLeft[j];
            reverb->rightRingBuf[ringItem->startPos + i] = ringItem->toDownsampleRight[j];
        }
        for (i = 0; i < ringItem->lengthB / 2; i++, j += reverb->downsampleRate) {
            reverb->leftRingBuf[i] = ringItem->toDownsampleLeft[j];
            reverb->rightRingBuf[i] = ringItem->toDownsampleRight[j];
        }
    }

    ringItem = &reverb->items[reverb->curFrame][itemIndex];
    numSamples = sampleCount / reverb->downsampleRate;
    extraSamples = reverb->nextRingBufPos + numSamples - reverb->bufSizePerChan;

    if (extraSamples < 0) {
        ringItem->lengthA = numSamples * SAMPLE_SIZE;
        ringItem->lengthB = 0;
        ringItem->startPos = reverb->nextRingBufPos;
        reverb->nextRingBufPos += numSamples;
    } else {
        ringItem->lengthA = (numSamples - extraSamples) * SAMPLE_SIZE;
        ringItem->lengthB = extraSamples * SAMPLE_SIZE;
        ringItem->startPos = reverb->nextRingBufPos;
        reverb->nextRingBufPos = extraSamples;
    }
    ringItem->numSamplesAfterDownsampling = numSamples;
    ringItem->chunkLen = sampleCount;
}

void AudioSynth_InverseDiscreteCosineTransform(f32* buffer0, f32* buffer1, s32 length, f32* buffer2) {
    f32 temp_ft0;
    f32 var_fs0;
    f32* buff0fromStart;
    f32* buf2half2;
    f32* buf2half3;
    f32* buff1half1;
    f32* buff0FromEnd;
    f32* buff1half2;
    s32 half;
    s32 i;
    s32 size;

    size = 1 << length;
    half = size >> 1;

    // Initialize buffer 2 if it is the wrong size for this calculation
    if (size != (s32) buffer2[0]) {
        buf2half2 = &buffer2[half];
        buf2half3 = &buf2half2[half];
        var_fs0 = 0.0f;
        temp_ft0 = D_PI / (f32) (2 * size);
        for (i = 0; i < half; i++) {
            *buf2half2++ = (__cosf(var_fs0) - __sinf(var_fs0)) * 0.707107f;
            *buf2half3++ = (__cosf(var_fs0) + __sinf(var_fs0)) * 0.707107f;
            var_fs0 += temp_ft0;
        }
    }

    // reset the buffer pointers
    buf2half2 = &buffer2[half];
    buf2half3 = &buf2half2[half];
    buff1half1 = buffer1;
    buff0fromStart = buffer0;

    // handle i = 0 case
    buffer1[0] = buffer0[0];
    buffer1[half] = buffer0[half];

    // advance buffer pointers
    buf2half2++;
    buf2half3++;
    buff0fromStart++;
    buff0FromEnd = &buffer0[size - 1];
    buff1half1++;
    buff1half2 = &buffer1[size - 1];

    // convert to real amplitudes
    for (i = 1; i < half; i++) {
        *buff1half1++ = (*buf2half2 * *buff0fromStart) + (*buf2half3 * *buff0FromEnd);
        *buff1half2-- = (*buf2half3 * *buff0fromStart) - (*buf2half2 * *buff0FromEnd);
        buff0fromStart++;
        buf2half3++;
        buf2half2++;
        buff0FromEnd--;
    }

    // FFT buffer 1 using buffer 2
    AudioSynth_HartleyTransform(buffer1, length, buffer2);

    buff0fromStart = buffer0;
    buff0FromEnd = &buffer0[size - 1];
    buff1half1 = buffer1;
    buff1half2 = &buffer1[half];

    // Copy even entries of buffer 0 into the first half of buffer 1. Copy odd entries into the second half in reverse
    // order
    for (i = 0; i < half; i++) {
        *buff0fromStart = *buff1half1++;
        *buff0FromEnd = *buff1half2++;
        buff0fromStart += 2;
        buff0FromEnd -= 2;
    }
}

void AudioSynth_HartleyTransform(f32* x, s32 m, f32* tab) {
    s32 n;
    s32 im, i, j;
    f32 b;
    s32 pont;
    s32 n2, n4, n8, n_tab;
    s32 is, id;
    f32 *px, *py, *pw, *pz;
    f32 *px1, *py1, *pw1, *pz1;
    f32 *pcc1, *pcc3, *pss1, *pss3;
    f32 a, a3;
    f32 temp;

    n = 1 << m;
    n2 = n << 1;
    n_tab = n / 8 - 1;

    switch (n) {
        case 1:
            break;
        case 2:
            temp = (x[0] + x[1]) * 0.707107f;
            x[1] = (x[0] - x[1]) * 0.707107f;
            x[0] = temp;
            break;

        case 4:
            temp = (x[0] + x[2]) / 2.0f;
            x[2] = (x[0] - x[2]) / 2.0f;
            x[0] = temp;

            temp = (x[1] + x[3]) / 2.0f;
            x[3] = (x[1] - x[3]) / 2.0f;
            x[1] = temp;

            temp = x[0] - temp;
            x[0] = x[0] + x[1];
            x[1] = x[2] + x[3];
            x[3] = x[2] - x[3];
            x[2] = temp;
            break;

        default:
            if (n != (s32) *tab) {
                *tab = n;
                pcc1 = tab + 1;
                pss1 = pcc1 + n_tab;
                pcc3 = pss1 + n_tab;
                pss3 = pcc3 + n_tab;
                a = 6.283186f / n;

                for (j = 0; j < n_tab; j++) {
                    *pcc1++ = __cosf(a);
                    *pss1++ = __sinf(a);
                    *pcc3++ = __cosf(3.0f * a);
                    *pss3++ = __sinf(3.0f * a);
                    a += 6.283186f / n;
                }
            }

            pont = 1;
            for (im = 0; im < m - 1; im++) {
                is = 1;
                id = n2;
                n2 >>= 1;
                n4 = n2 >> 2;
                n8 = n2 >> 3;
                do {
                    for (i = is - 1; i < n; i = i + id) {
                        px = x + i;
                        py = px + n4;
                        pw = py + n4;
                        pz = pw + n4;

                        D_80146D54 = *px;
                        *px = *pw + D_80146D54;
                        D_80146D58 = *py;
                        *py = *py + *pz;
                        D_80146D5C = *pw;

                        *pw = ((D_80146D54 - D_80146D5C) + D_80146D58) - *pz;
                        *pz = ((D_80146D54 - D_80146D5C) - D_80146D58) + *pz;

                        if (n4 > 1) {
                            px = px + n8;
                            py = px + n4;
                            pw = py + n4;
                            pz = pw + n4;

                            D_80146D54 = *px;
                            *px = *pw + D_80146D54;

                            D_80146D58 = *py;
                            *py = *pz + D_80146D58;

                            *pw = (D_80146D54 - *pw) * 1.414214f;
                            *pz = (D_80146D58 - *pz) * 1.414214f;

                            pcc1 = tab + pont;
                            pss1 = pcc1 + n_tab;
                            pcc3 = pss1 + n_tab;
                            pss3 = pcc3 + n_tab;

                            for (j = 1; j < n8; j++) {
                                px = x + i + j;
                                py = px + n4;
                                pw = py + n4;
                                pz = pw + n4;

                                D_80146D54 = *px;
                                D_80146D58 = *py;
                                D_80146D5C = *pw;
                                D_80146D60 = *pz;

                                px1 = x + i + n4 - j;
                                py1 = px1 + n4;
                                pw1 = py1 + n4;
                                pz1 = pw1 + n4;

                                D_80146D64 = *px1;
                                D_80146D68 = *py1;
                                D_80146D6C = *pw1;
                                D_80146D70 = *pz1;

                                *px = D_80146D54 + D_80146D5C;
                                *py = D_80146D58 + D_80146D60;

                                *pw = ((D_80146D54 - D_80146D5C + D_80146D64 - D_80146D6C) * *pcc1) +
                                      ((D_80146D60 - D_80146D58 + D_80146D68 - D_80146D70) * *pss1);

                                *pz = ((D_80146D54 - D_80146D5C - D_80146D64 + D_80146D6C) * *pcc3) -
                                      ((D_80146D60 - D_80146D58 - D_80146D68 + D_80146D70) * *pss3);

                                *px1 = D_80146D64 + D_80146D6C;
                                *py1 = D_80146D68 + D_80146D70;

                                *pw1 = ((D_80146D54 - D_80146D5C + D_80146D64 - D_80146D6C) * *pss1) -
                                       ((D_80146D60 - D_80146D58 + D_80146D68 - D_80146D70) * *pcc1);

                                *pz1 = ((D_80146D54 - D_80146D5C - D_80146D64 + D_80146D6C) * *pss3) +
                                       ((D_80146D60 - D_80146D58 - D_80146D68 + D_80146D70) * *pcc3);

                                pcc1 += pont;
                                pcc3 += pont;
                                pss1 += pont;
                                pss3 += pont;
                            }
                        }
                    }
                    is = 2 * id - n2 + 1;
                    id = 4 * id;
                } while (is < n);

                pont = 2 * pont;

                px = x;
                for (i = 0; i < n; i++) {
                    *px /= 1.414214f;
                    px++;
                }
            }

            is = 1;
            id = 4;
            do {
                for (i = is - 1; i < n; i += id) {
                    px = x + i;
                    D_80146D54 = *px;
                    *px = D_80146D54 + *(px + 1);
                    *(px + 1) = D_80146D54 - *(px + 1);
                }
                is = 2 * id - 1;
                id = 4 * id;
            } while (is < n);

            px = x;
            for (i = 0; i < n; i++) {
                *px /= 1.414214f;
                px++;
            }

            j = 1;
            pw = x;
            for (i = 1; i < n; i++) {
                pont = n >> 1;
                if (i < j) {
                    py = x + j - 1;
                    D_80146D54 = *py;
                    *py = *pw;
                    *pw = D_80146D54;
                }
                pw++;
                while (pont < j) {
                    j -= pont;
                    pont >>= 1;
                }
                j += pont;
            }
            break;
    }
}

#if defined(TARGET_HANDHELD)
/* DCTF sample data stays in the sample bank's big-endian s16 words. */
static inline s16 DCTF_READ(const s16* p) {
    return (s16) AUDIO_READ_BE16(p);
}
#else
#define DCTF_READ(p) (*(p))
#endif

void AudioSynth_DCTF_ProcessBlockFreqDomain(s16** sampleBlockAddr) {
    s16* bufPtr;
    s32 shiftFactor;
    s32 mode;
    u8 blockHeader;
    u8 temp_u1;
    s32 headerPart;
    s32 block;
    s32 decodedBufIndex;
    u16 bufValue;
    u32 header;
    s32 i;
    s32 j;

    /**
     * Each compressed block begins with a 32-bit header. The header contains four 8-bit descriptors,
     * one for each 64-coefficient partition of the 256-point transform.
     * 256 coefficients -> 4 partitions -> 64 coefficients each ->one header byte controls each partition
     */
    bufPtr = *sampleBlockAddr;

    // necessary because many modes don't write every coefficient.
    for (decodedBufIndex = 255; decodedBufIndex >= 0; decodedBufIndex--) {
        gIDCT_Buffer[decodedBufIndex] = 0.0f;
    }

    headerPart = DCTF_READ(bufPtr++);
    header = headerPart << 0x10;
    headerPart = DCTF_READ(bufPtr++);
    header |= headerPart;

    for (block = 0; block < 4; block++) {
        decodedBufIndex = block * 0x40;
        blockHeader = (header >> 0x18) & 0xFF;
        header <<= 8;

        mode = ((blockHeader >> 4) & 0xF);
        if (mode == 0) {
            continue;
        }
        shiftFactor = blockHeader & 0xF;

        switch (mode) {
            case 1: // Variable-length 4-bit signed coefficients
                while (true) {
                    bufValue = DCTF_READ(bufPtr++);
                    for (i = 0; i < 4; i++) {
                        temp_u1 = (bufValue >> 0xC) & 0xF;
                        bufValue <<= 4;
                        gIDCT_Buffer[(u32) decodedBufIndex++] = ((temp_u1 & 7) - 4) << shiftFactor;
                        if (temp_u1 >= 8) {
                            goto case_1_break;
                        }
                    }
                }
            case_1_break:
                continue;

            case 2: // Fixed-length packed 4-bit coefficients
                for (i = 0; i < 16; i++) {
                    bufValue = DCTF_READ(bufPtr++);
                    for (j = 0; j < 4; j++) {
                        temp_u1 = (bufValue >> 0xC) & 0xF;
                        bufValue <<= 4;
                        gIDCT_Buffer[decodedBufIndex++] = (temp_u1 - 8) << shiftFactor;
                    }
                }
                break;

            case 6: // Run-length encoded sparse coefficients
                while (true) {
                    bufValue = DCTF_READ(bufPtr++);
                    temp_u1 = (bufValue >> 8) & 0xFF;
                    gIDCT_Buffer[decodedBufIndex] = ((temp_u1 & 0x3F) - 0x20) << shiftFactor;
                    if (temp_u1 >> 6 == 0) {
                        break;
                    }
                    decodedBufIndex += temp_u1 >> 6;
                    temp_u1 = bufValue & 0xFF;
                    gIDCT_Buffer[decodedBufIndex] = ((temp_u1 & 0x3F) - 0x20) << shiftFactor;
                    if (temp_u1 >> 6 == 0) {
                        break;
                    }
                    decodedBufIndex += temp_u1 >> 6;
                }
                break;

            case 3: // Variable-length 7-bit coefficients
                while (true) {
                    bufValue = DCTF_READ(bufPtr++);
                    temp_u1 = (bufValue >> 8) & 0xFF;

                    gIDCT_Buffer[decodedBufIndex++] = ((temp_u1 & 0x7F) - 0x40) << shiftFactor;

                    if (temp_u1 >= 0x80) {
                        break;
                    }
                    temp_u1 = bufValue & 0xFF;
                    gIDCT_Buffer[decodedBufIndex++] = ((temp_u1 & 0x7F) - 0x40) << shiftFactor;
                    if (temp_u1 >= 0x80) {
                        break;
                    }
                }
                continue;

            case 4: // Variable-length 12-bit coefficients
                while (true) {
                    bufValue = DCTF_READ(bufPtr++);
                    gIDCT_Buffer[decodedBufIndex] = ((bufValue & 0xFFF) - 0x800) << shiftFactor;
                    if (bufValue >> 0xC == 0) {
                        break;
                    }
                    decodedBufIndex += bufValue >> 0xC;
                }
                break;

            case 5: // Variable-length 15-bit coefficients
                while (true) {
                    bufValue = DCTF_READ(bufPtr++);
                    gIDCT_Buffer[decodedBufIndex] = ((bufValue & 0x7FFF) - 0x4000) << shiftFactor;
                    if (bufValue >> 0xF == 1) {
                        break;
                    }
                    decodedBufIndex++;
                }
                break;
        }
        if (decodedBufIndex) {}
    }

    *sampleBlockAddr = bufPtr;
}

void AudioSynth_DCTF_ProcessBlock(s16* ramAddr, DCTF_Sample* DCTFSample) {
    s32 i;

    if (DCTFSample->sampleAddr != NULL) {
        DCTFSample->sampleBlockAddr = DCTFSample->sampleAddr;
        DCTFSample->sampleAddr = NULL;
    }

    DCTFSample->freqBlockAcc += gDCTF_PlaybackRate;
    while (DCTFSample->freqBlockAcc > 0x1000) {
        AudioSynth_DCTF_ProcessBlockFreqDomain(&DCTFSample->sampleBlockAddr);
        DCTFSample->freqBlockAcc -= 0x1000;
    }

    AudioSynth_InverseDiscreteCosineTransform(gIDCT_Buffer, gIDCT_Work, 8, gIDCT_CoefTable);

    for (i = 0; i < ARRAY_COUNT(gIDCT_Buffer); i++) {
        if (gIDCT_Buffer[i] > 32767.0f) {
            gIDCT_Buffer[i] = 32767.0f;
        }
        if (gIDCT_Buffer[i] < -32767.0f) {
            gIDCT_Buffer[i] = -32767.0f;
        }
    }

    for (i = 0; i < ARRAY_COUNT(gIDCT_Buffer); i++, ramAddr++) {
        *ramAddr = (s16) gIDCT_Buffer[i];
    }
}

s32 AudioSynth_DCTF_ProcessSample(s32 length, s16* ramAddr, DCTF_Sample* DCTFSample) {
    s32 pad;
    s32 maxSampleBlocks;
    s32 i;
    s32 blockIdx;
    s16* sampleDmaInQueueAddr = (s16*) DCTFSample->dmaInQueue->ramAddr;

    for (i = 0; i < DCTFSample->numSampleBlocks; i++) {
        ramAddr[i] = sampleDmaInQueueAddr[i];
    }

    blockIdx = DCTFSample->numSampleBlocks;
    maxSampleBlocks = (length - DCTFSample->numSampleBlocks + 255) / 256;
    DCTFSample->numSampleBlocks = (maxSampleBlocks * 256) + DCTFSample->numSampleBlocks - length;

    for (i = 0; i < maxSampleBlocks; i++) {
        AudioSynth_DCTF_ProcessBlock(&ramAddr[blockIdx], DCTFSample);
        blockIdx += 0x100;
    }

    for (i = 0; i < DCTFSample->numSampleBlocks; i++) {
        sampleDmaInQueueAddr[i] = ramAddr[length + i];
    }

    return maxSampleBlocks;
}

// Sets up DMA and Decoding parameters for a CODEC_DCTF type sample
u8* AudioSynth_DCTF_Decode(Sample* bookSample, s32 length, u32 flags, DCTF_Sample* DCTFSample) {
    s32 pad;
    SampleDma* sampleDmaInQueue;
    SampleDma* destSample; // Destination Sample to be decoded as 16-bit PCM

    if (flags == A_INIT) {
        DCTFSample->sampleAddr = (s16*) bookSample->sampleAddr;
        DCTFSample->numSampleBlocks = 0;
        DCTFSample->maxSampleBlocks = 0;
        DCTFSample->freqBlockAcc = 0;

        if (gSampleDmaReuseQueue1RdPos != gSampleDmaReuseQueue1WrPos) {
            DCTFSample->dmaInQueue = &gSampleDmas[gSampleDmaReuseQueue1[gSampleDmaReuseQueue1RdPos++]];
            DCTFSample->dmaInQueue->devAddr = -1;
            DCTFSample->dmaInQueue->sizeUnused = 0;
        }
    }

    if (gSampleDmaReuseQueue1RdPos != gSampleDmaReuseQueue1WrPos) {
        destSample = &gSampleDmas[gSampleDmaReuseQueue1[gSampleDmaReuseQueue1RdPos++]];
    }

    if (1) {} //! FAKE

    destSample->ttl = 2;
    destSample->devAddr = bookSample->sampleAddr;
    destSample->sizeUnused = length * 2;

    sampleDmaInQueue = DCTFSample->dmaInQueue;
    sampleDmaInQueue->ttl = 2;

    DCTFSample->maxSampleBlocks += AudioSynth_DCTF_ProcessSample(length, (s16*) destSample->ramAddr, DCTFSample);

    return destSample->ramAddr;
}

Acmd* AudioSynth_LoadReverbRingBufferPart(Acmd* aList, u16 dmem, u16 startPos, s32 size, s32 reverbIndex) {
    aLoadBuffer(aList++, OS_K0_TO_PHYSICAL(&gSynthReverbs[reverbIndex].leftRingBuf[startPos]), dmem, size);
    aLoadBuffer(aList++, OS_K0_TO_PHYSICAL(&gSynthReverbs[reverbIndex].rightRingBuf[startPos]), dmem + DMEM_1CH_SIZE,
                size);
    return aList;
}

Acmd* AudioSynth_SaveReverbRingBufferPart(Acmd* aList, u16 dmem, u16 startPos, s32 size, s32 reverbIndex) {
    aSaveBuffer(aList++, dmem, OS_K0_TO_PHYSICAL(&gSynthReverbs[reverbIndex].leftRingBuf[startPos]), size);
    aSaveBuffer(aList++, dmem + DMEM_1CH_SIZE, OS_K0_TO_PHYSICAL(&gSynthReverbs[reverbIndex].rightRingBuf[startPos]),
                size);
    return aList;
}

// Original name: __Nas_WaveTerminateProcess
void AudioSynth_DisableSampleStates(s32 updateIndex, s32 noteIndex) {
    NoteSampleState* sampleState;
    s32 i;

    for (i = updateIndex + 1; i < gAudioBufferParams.ticksPerUpdate; i++) {
        if (!gSampleStateList[(gNumNotes * i) + noteIndex].bitField0.needsInit) {
            gSampleStateList[(gNumNotes * i) + noteIndex].bitField0.enabled = false;
        } else {
            break;
        }
    }
}

/**
 * Original name: __Nas_PushDrvReg
 *
 * Sync the sample states between the notes and the list
 */
void AudioSynth_SyncSampleStates(s32 updateIndex) {
    NoteSampleState* noteSampleState;
    NoteSampleState* sampleState;
    s32 i;

    for (i = 0; i < gNumNotes; i++) {
        noteSampleState = &gNotes[i].sampleState;
        sampleState = &gSampleStateList[gNumNotes * updateIndex + i];
        if (noteSampleState->bitField0.enabled) {
            *sampleState = *noteSampleState;
            noteSampleState->bitField0.needsInit = false;
        } else {
            sampleState->bitField0.enabled = false;
        }
    }
}

// Original name: Nas_smzAudioFrame
Acmd* AudioSynth_Update(Acmd* aList, s32* cmdCount, s16* aiBufStart, s32 aiBufLen) {
    Acmd* aCmdPtr;
    s32* aiBufPtr;
    s32 chunkLen;
    s32 i;
    s32 j;

    aCmdPtr = aList;
    for (i = gAudioBufferParams.ticksPerUpdate; i > 0; i--) {
        AudioSeq_ProcessSequences(i - 1);
        AudioSynth_SyncSampleStates(gAudioBufferParams.ticksPerUpdate - i);
    }

    aiBufPtr = (s32*) aiBufStart;
    for (i = gAudioBufferParams.ticksPerUpdate; i > 0; i--) {
        if (i == 1) {
            chunkLen = aiBufLen;
        } else if ((aiBufLen / i) >= gAudioBufferParams.samplesPerTickMax) {
            chunkLen = gAudioBufferParams.samplesPerTickMax;
        } else if (gAudioBufferParams.samplesPerTickMin >= (aiBufLen / i)) {
            chunkLen = gAudioBufferParams.samplesPerTickMin;
        } else {
            chunkLen = gAudioBufferParams.samplesPerTick;
        }

        for (j = 0; j < gNumSynthReverbs; j++) {
            if (gSynthReverbs[j].useReverb) {
                AudioSynth_InitNextReverbRingBuf(chunkLen, gAudioBufferParams.ticksPerUpdate - i, j);
            }
        }

        aCmdPtr = AudioSynth_ProcessSamples((s16*) aiBufPtr, chunkLen, aCmdPtr, gAudioBufferParams.ticksPerUpdate - i);
        aiBufLen -= chunkLen;
        aiBufPtr += chunkLen;
    }

    for (j = 0; j < gNumSynthReverbs; j++) {
        if (gSynthReverbs[j].framesToIgnore != 0) {
            gSynthReverbs[j].framesToIgnore--;
        }
        gSynthReverbs[j].curFrame ^= 1;
    }

    *cmdCount = aCmdPtr - aList;

    return aCmdPtr;
}

// Original name: Nas_LoadAuxBuffer
Acmd* AudioSynth_LoadReverbSamples(Acmd* aList, s32 aiBufLen, s16 reverbIndex, s16 updateIndex) {
    ReverbRingBufferItem* sp64 = &gSynthReverbs[reverbIndex].items[gSynthReverbs[reverbIndex].curFrame][updateIndex];
    s16 sp62;
    s16 sp60;

    aClearBuffer(aList++, DMEM_WET_LEFT_CH, DMEM_2CH_SIZE);

    if (gSynthReverbs[reverbIndex].downsampleRate == 1) {
        aList =
            AudioSynth_LoadReverbRingBufferPart(aList, DMEM_WET_LEFT_CH, sp64->startPos, sp64->lengthA, reverbIndex);
        if (sp64->lengthB != 0) {
            aList = AudioSynth_LoadReverbRingBufferPart(aList, sp64->lengthA + DMEM_WET_LEFT_CH, 0, sp64->lengthB,
                                                        reverbIndex);
        }
        aAddMixer(aList++, 0x300, DMEM_WET_LEFT_CH, DMEM_LEFT_CH, 0x7FFF);
        aMix(aList++, 0x30, gSynthReverbs[reverbIndex].decayRatio + 0x8000, DMEM_WET_LEFT_CH, DMEM_WET_LEFT_CH);
    } else {
        sp62 = (sp64->startPos & 7) * 2;
        sp60 = ALIGN16(sp62 + sp64->lengthA);
        aList = AudioSynth_LoadReverbRingBufferPart(aList, DMEM_WET_SCRATCH, sp64->startPos - (sp62 / 2), DMEM_1CH_SIZE,
                                                    reverbIndex);
        if (sp64->lengthB != 0) {
            aList = AudioSynth_LoadReverbRingBufferPart(aList, sp60 + DMEM_WET_SCRATCH, 0, DMEM_1CH_SIZE - sp60,
                                                        reverbIndex);
        }
        aSetBuffer(aList++, 0, sp62 + DMEM_WET_SCRATCH, DMEM_WET_LEFT_CH, aiBufLen * 2);
        aResample(aList++, gSynthReverbs[reverbIndex].resampleFlags, gSynthReverbs[reverbIndex].unk_0A,
                  OS_K0_TO_PHYSICAL(gSynthReverbs[reverbIndex].unk_30));
        aSetBuffer(aList++, 0, sp62 + DMEM_UNCOMPRESSED_NOTE, DMEM_WET_RIGHT_CH, aiBufLen * 2);
        aResample(aList++, gSynthReverbs[reverbIndex].resampleFlags, gSynthReverbs[reverbIndex].unk_0A,
                  OS_K0_TO_PHYSICAL(gSynthReverbs[reverbIndex].unk_34));
        aAddMixer(aList++, 0x300, DMEM_WET_LEFT_CH, DMEM_LEFT_CH, 0x7FFF);
        aMix(aList++, 0x30, gSynthReverbs[reverbIndex].decayRatio + 0x8000, DMEM_WET_LEFT_CH, DMEM_WET_LEFT_CH);
    }

    if ((gSynthReverbs[reverbIndex].leakRtL != 0) || (gSynthReverbs[reverbIndex].leakLtR != 0)) {
        aDMEMMove(aList++, DMEM_WET_LEFT_CH, DMEM_WET_SCRATCH, DMEM_1CH_SIZE);
        aMix(aList++, DMEM_1CH_SIZE >> 4, gSynthReverbs[reverbIndex].leakRtL, DMEM_WET_RIGHT_CH, DMEM_WET_LEFT_CH);
        aMix(aList++, DMEM_1CH_SIZE >> 4, gSynthReverbs[reverbIndex].leakLtR, DMEM_WET_SCRATCH, DMEM_WET_RIGHT_CH);
    }
    return aList;
}

// Original name: Nas_SaveAuxBuffer
Acmd* AudioSynth_SaveReverbSamples(Acmd* aList, s16 reverbIndex, s16 updateIndex) {
    ReverbRingBufferItem* sp24;

    sp24 = &gSynthReverbs[reverbIndex].items[gSynthReverbs[reverbIndex].curFrame][updateIndex];
    switch (gSynthReverbs[reverbIndex].downsampleRate) {
        case 1:
            aList = AudioSynth_SaveReverbRingBufferPart(aList, DMEM_WET_LEFT_CH, sp24->startPos, sp24->lengthA,
                                                        reverbIndex);
            if (sp24->lengthB != 0) {
                aList = AudioSynth_SaveReverbRingBufferPart(aList, sp24->lengthA + DMEM_WET_LEFT_CH, 0, sp24->lengthB,
                                                            reverbIndex);
            }
            break;

        default:
            aSaveBuffer(aList++, DMEM_WET_LEFT_CH,
                        OS_K0_TO_PHYSICAL(gSynthReverbs[reverbIndex]
                                              .items[gSynthReverbs[reverbIndex].curFrame][updateIndex]
                                              .toDownsampleLeft),
                        0x300);
            gSynthReverbs[reverbIndex].resampleFlags = 0;
            break;
    }

    return aList;
}

/**
 * Original name: Nas_DriveRsp
 *
 * Process all samples embedded in a note. Every sample has numSamplesPerUpdate processed,
 * and each of those are mixed together into both DMEM_LEFT_CH and DMEM_RIGHT_CH
 */
Acmd* AudioSynth_ProcessSamples(s16* aiBuf, s32 aiBufLen, Acmd* aList, s32 updateIndex) {
    u8 noteIndices[60];
    s16 reverbIndex = 0;
    s16 noteCount = 0;
    s32 j;

    if (gNumSynthReverbs == 0) {
        if (gSynthReverbs[reverbIndex].useReverb) {} // fake?

        for (j = 0; j < gNumNotes; j++) {
            if (gSampleStateList[gNumNotes * updateIndex + j].bitField0.enabled) {
                noteIndices[noteCount++] = j;
            }
        }
    } else {
        NoteSampleState* sampleState;

        for (reverbIndex = 0; reverbIndex < gNumSynthReverbs; reverbIndex++) {
            for (j = 0; j < gNumNotes; j++) {
                sampleState = &gSampleStateList[gNumNotes * updateIndex + j];
                if (sampleState->bitField0.enabled && (sampleState->bitField1.reverbIndex == reverbIndex)) {
                    noteIndices[noteCount++] = j;
                }
            }
        }
        for (j = 0; j < gNumNotes; j++) {
            sampleState = &gSampleStateList[gNumNotes * updateIndex + j];
            if (sampleState->bitField0.enabled && (sampleState->bitField1.reverbIndex >= gNumSynthReverbs)) {
                noteIndices[noteCount++] = j;
            }
        }
    }

    aClearBuffer(aList++, DMEM_LEFT_CH, DMEM_2CH_SIZE);

    for (reverbIndex = 0, j = 0; reverbIndex < gNumSynthReverbs; reverbIndex++) {
        gUseReverb = gSynthReverbs[reverbIndex].useReverb;
        if (gUseReverb) {
            // Loads reverb samples from DRAM (ringBuffer) into DMEM (DMEM_WET_LEFT_CH)
            aList = AudioSynth_LoadReverbSamples(aList, aiBufLen, reverbIndex, updateIndex);
        }

        while (j < noteCount) {
            if (reverbIndex != gSampleStateList[updateIndex * gNumNotes + noteIndices[j]].bitField1.reverbIndex) {
                break;
            }
            aList =
                AudioSynth_ProcessSample(noteIndices[j], &gSampleStateList[updateIndex * gNumNotes + noteIndices[j]],
                                         &gNotes[noteIndices[j]].synthesisState, aiBuf, aiBufLen, aList, updateIndex);
            j++;
        }

        if (gSynthReverbs[reverbIndex].useReverb) {
            aList = AudioSynth_SaveReverbSamples(aList, reverbIndex, updateIndex);
        }
    }

    while (j < noteCount) {
        aList = AudioSynth_ProcessSample(noteIndices[j], &gSampleStateList[updateIndex * gNumNotes + noteIndices[j]],
                                         &gNotes[noteIndices[j]].synthesisState, aiBuf, aiBufLen, aList, updateIndex);
        j++;
    }

    j = aiBufLen * 2;
    aSetBuffer(aList++, 0, 0, DMEM_TEMP, j);
    aInterleave(aList++, DMEM_LEFT_CH, DMEM_RIGHT_CH);
    aSaveBuffer(aList++, DMEM_TEMP, OS_K0_TO_PHYSICAL(aiBuf), j * 2);

    return aList;
}

// Original name: Nas_SynthMain
Acmd* AudioSynth_ProcessSample(s32 noteIndex, NoteSampleState* sampleState, NoteSynthesisState* synthState, s16* aiBuf,
                               s32 aiBufLen, Acmd* aList, s32 updateIndex) {
    s32 pad11C[3];
    Sample* bookSample;
    AdpcmLoop* loopInfo;
    void* currentBook;
    s32 pad104[2];
    bool sampleFinished;
    bool loopToPoint;
    s32 flags;
    u16 resampleRateFixedPoint;
    s32 numSamplesToLoad;
    s32 padE8[3];
    s32 skipBytes;
    s32 padD8[3];
    u32 sampleAddr;
    s32 padC8;
    s32 numSamplesToLoadAdj;
    s32 numSamplesProcessed;
    u32 endPos;
    s32 nSamplesToProcess;
    s32 padB4[5];
    s32 numTrailingSamplesToIgnore;
    s32 pad9C[3];
    s32 frameSize;
    s32 pad8C;
    s32 skipInitialSamples;
    s32 sampleDmaStart;
    s32 pad80;
    s32 numParts;
    s32 curPart;
    s32 numSamplesInThisIteration;
    s32 sampleDataChunkAlignPad;
    s32 resampledTempLen;
    u16 noteSamplesDmemAddrBeforeResampling;
    s32 pasdasd;
    s32 frameIndex;
    s32 sampleDataOffset;
    Note* note;
    u16 sp56;
    s32 numSamplesInFirstFrame;
    s32 delaySide;
    s32 padasdsa;
    s32 nFramesToDecode;
    s32 nFirstFrameSamplesToIgnore;
    s32 dmemUncompressedAddrOffset1;
    u32 sampleslenFixedPoint;
    u8* samplesToLoadAddr;
    s32 DCTFSampleAddr;
    s32 gain;
    u32 nEntries;
    s32 aligned;
    s16 addr;
    s32 samplesRemaining;
    s32 numSamplesToDecode;

    currentBook = NULL;
    note = &gNotes[noteIndex];
    flags = A_CONTINUE;

    // Initialize the synthesis state
    if (sampleState->bitField0.needsInit == true) {
        flags = A_INIT;
        synthState->restart = 0;
        synthState->samplePosInt = 0;
        synthState->samplePosFrac = 0;
        synthState->curVolLeft = 0;
        synthState->curVolRight = 0;
        synthState->numParts = synthState->prevHaasEffectRightDelaySize = synthState->prevHaasEffectLeftDelaySize = 0;
        note->sampleState.bitField0.finished = 0;
    }

    resampleRateFixedPoint = sampleState->resampleRate;

    // Process the sample in either one or two parts
    numParts = sampleState->bitField1.hasTwoParts + 1;

    // Determine number of samples to load based on numSamplesPerUpdate and relative frequency
    sampleslenFixedPoint = ((resampleRateFixedPoint * aiBufLen) * 2) + synthState->samplePosFrac;
    numSamplesToLoad = sampleslenFixedPoint >> 16;
    synthState->samplePosFrac = sampleslenFixedPoint & 0xFFFF;

    if ((synthState->numParts == 1) && (numParts == 2)) {
        numSamplesToLoad += 2;
        sp56 = 2;
    } else if ((synthState->numParts == 2) && (numParts == 1)) {
        numSamplesToLoad -= 4;
        sp56 = 4;
    } else {
        sp56 = 0;
    }

    synthState->numParts = numParts;

    if (sampleState->bitField1.isSyntheticWave) {
        aList = AudioSynth_LoadWaveSamples(aList, sampleState, synthState, numSamplesToLoad);
        noteSamplesDmemAddrBeforeResampling = DMEM_UNCOMPRESSED_NOTE + (synthState->samplePosInt * SAMPLE_SIZE);
        synthState->samplePosInt += numSamplesToLoad;
    } else {
        bookSample = *((Sample**) sampleState->waveSampleAddr);
        loopInfo = bookSample->loop;

        endPos = loopInfo->end;
        sampleAddr = bookSample->sampleAddr;
        resampledTempLen = 0;

        // If the frequency requested is more than double that of the raw sample,
        // then the sample processing is split into two parts.
        for (curPart = 0; curPart < numParts; curPart++) {
            numSamplesProcessed = 0;
            dmemUncompressedAddrOffset1 = 0;

            // Adjust the number of samples to load only if there are two parts and an odd number of samples
            if (numParts == 1) {
                numSamplesToLoadAdj = numSamplesToLoad;
            } else if (numSamplesToLoad & 1) {
                // round down for the first part
                // round up for the second part
                numSamplesToLoadAdj = (numSamplesToLoad & ~1) + (curPart * 2);
            } else {
                numSamplesToLoadAdj = numSamplesToLoad;
            }

            // Load the ADPCM codeBook
            if ((bookSample->codec == CODEC_ADPCM) && (currentBook != bookSample->book->book)) {
                switch (sampleState->bitField1.bookOffset) {
                    case 1:
                        currentBook = &gInvalidAdpcmCodeBook[1];
                        break;

                    case 2:
                        currentBook = &gInvalidAdpcmCodeBook[2];
                        break;

                    default:
                    case 3:
                        currentBook = bookSample->book->book;
                        break;
                }

                nEntries = (SAMPLES_PER_FRAME * bookSample->book->order) * bookSample->book->numPredictors;
                aLoadADPCM(aList++, nEntries, OS_K0_TO_PHYSICAL(currentBook));
            }

            // Continue processing samples until the number of samples needed to load is reached
            while (numSamplesProcessed != numSamplesToLoadAdj) {
                sampleFinished = false;
                loopToPoint = false;

                samplesRemaining = endPos - synthState->samplePosInt;

                // Calculate number of samples to process this loop
                nSamplesToProcess = numSamplesToLoadAdj - numSamplesProcessed;

                nFirstFrameSamplesToIgnore = synthState->samplePosInt & 0xF;
                if ((nFirstFrameSamplesToIgnore == 0) && (!synthState->restart)) {
                    nFirstFrameSamplesToIgnore = SAMPLES_PER_FRAME;
                }

                numSamplesInFirstFrame = SAMPLES_PER_FRAME - nFirstFrameSamplesToIgnore;

                // Determine the number of samples to decode based on whether the end will be reached or not.
                if (nSamplesToProcess < samplesRemaining) {
                    // The end will not be reached.
                    nFramesToDecode =
                        ((nSamplesToProcess - numSamplesInFirstFrame) + SAMPLES_PER_FRAME - 1) / SAMPLES_PER_FRAME;
                    numSamplesToDecode = nFramesToDecode * SAMPLES_PER_FRAME;
                    numTrailingSamplesToIgnore = (numSamplesInFirstFrame + numSamplesToDecode) - nSamplesToProcess;
                } else {
                    // The end will be reached.
                    numSamplesToDecode = samplesRemaining - numSamplesInFirstFrame;
                    numTrailingSamplesToIgnore = 0;

                    if (numSamplesToDecode <= 0) {
                        numSamplesToDecode = 0;
                        numSamplesInFirstFrame = samplesRemaining;
                    }

                    nFramesToDecode = (numSamplesToDecode + SAMPLES_PER_FRAME - 1) / SAMPLES_PER_FRAME;
                    if (loopInfo->count != 0) {
                        // Loop around and restart
                        loopToPoint = true;
                    } else {
                        sampleFinished = true;
                    }
                }

                // Set parameters based on compression type
                switch (bookSample->codec) {
                    case CODEC_ADPCM:
                        // 16 2-byte samples (32 bytes) compressed into 4-bit samples (8 bytes) + 1 header byte
                        frameSize = 9;
                        skipInitialSamples = SAMPLES_PER_FRAME;
                        sampleDmaStart = 0;
                        break;

                    case CODEC_S8:
                        // 16 2-byte samples (32 bytes) compressed into 8-bit samples (16 bytes)
                        frameSize = 16;
                        skipInitialSamples = SAMPLES_PER_FRAME;
                        sampleDmaStart = 0;
                        break;

                    case CODEC_DCTF:
                        DCTFSampleAddr = AudioSynth_DCTF_Decode(bookSample, numSamplesToLoadAdj, flags,
                                                                &synthState->synthesisBuffers->DCTFSample);
                        if (0) {}
                        aLoadBuffer(aList++, OS_K0_TO_PHYSICAL(DCTFSampleAddr), DMEM_UNCOMPRESSED_NOTE,
                                    (numSamplesToLoadAdj + SAMPLES_PER_FRAME) * 2);
                        flags = A_CONTINUE;
                        skipBytes = 0;
                        numSamplesProcessed = numSamplesToLoadAdj;
                        dmemUncompressedAddrOffset1 = numSamplesToLoadAdj;
                        goto codec_dctf_break;

                    default:
                        break;
                }

                aligned = ALIGN16((nFramesToDecode * frameSize) + SAMPLES_PER_FRAME);
                addr = DMEM_COMPRESSED_ADPCM_DATA - aligned;

                // Move the compressed raw sample data from ram into the rsp (DMEM)
                if (nFramesToDecode != 0) {
                    if (1) {}
                    // Get the offset from the start of the sample to where the sample is currently playing from
                    frameIndex = (synthState->samplePosInt + skipInitialSamples - nFirstFrameSamplesToIgnore) / 16;
                    sampleDataOffset = frameIndex * frameSize;

                    // Get the ram address of the requested sample chunk
                    if (bookSample->medium == MEDIUM_RAM) {
                        // Sample is already loaded into ram
                        samplesToLoadAddr = (u8*) (sampleDmaStart + sampleDataOffset + sampleAddr);
                    } else {
                        // This medium is not in ram, so dma the requested sample into ram
                        samplesToLoadAddr =
                            AudioLoad_DmaSampleData(sampleDmaStart + sampleDataOffset + sampleAddr, aligned, flags,
                                                    &synthState->sampleDmaIndex, bookSample->medium);
                    }

                    // Move the raw sample chunk from ram to the rsp
                    sampleDataChunkAlignPad = (u32) samplesToLoadAddr % SAMPLES_PER_FRAME;
                    aLoadBuffer(aList++, OS_K0_TO_PHYSICAL(samplesToLoadAddr - sampleDataChunkAlignPad), addr, aligned);
                } else {
                    numSamplesToDecode = 0;
                    sampleDataChunkAlignPad = 0;
                }

                if (synthState->restart) {
                    aSetLoop(aList++, OS_K0_TO_PHYSICAL(bookSample->loop->predictorState));
                    flags = A_LOOP;
                    synthState->restart = false;
                }

                numSamplesInThisIteration = (numSamplesToDecode + numSamplesInFirstFrame) - numTrailingSamplesToIgnore;

                // Decompress the raw sample chunks in the rsp
                // Goes from adpcm (compressed) sample data to pcm (uncompressed) sample data
                if (numSamplesProcessed == 0) {
                    switch (bookSample->codec) {
                        case CODEC_ADPCM:
                            aSetBuffer(aList++, 0, addr + sampleDataChunkAlignPad, DMEM_UNCOMPRESSED_NOTE,
                                       numSamplesToDecode * SAMPLE_SIZE);
                            aADPCMdec(aList++, flags, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers));
                            break;

                        case CODEC_S8:
                            aSetBuffer(aList++, 0, addr + sampleDataChunkAlignPad, DMEM_UNCOMPRESSED_NOTE,
                                       numSamplesToDecode * SAMPLE_SIZE);
                            aS8Dec(aList++, flags, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers));
                            break;
                    }

                    skipBytes = nFirstFrameSamplesToIgnore * SAMPLE_SIZE;
                } else {
                    aligned = ALIGN16(dmemUncompressedAddrOffset1 + SAMPLES_PER_FRAME);
                    switch (bookSample->codec) {
                        case CODEC_ADPCM:
                            aSetBuffer(aList++, 0, addr + sampleDataChunkAlignPad, DMEM_UNCOMPRESSED_NOTE + aligned,
                                       numSamplesToDecode * SAMPLE_SIZE);
                            aADPCMdec(aList++, flags, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers));
                            break;

                        case CODEC_S8:
                            aSetBuffer(aList++, 0, addr + sampleDataChunkAlignPad, DMEM_UNCOMPRESSED_NOTE + aligned,
                                       numSamplesToDecode * SAMPLE_SIZE);
                            aS8Dec(aList++, flags, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers));
                            break;
                    }

                    aDMEMMove(aList++, DMEM_UNCOMPRESSED_NOTE + aligned + (nFirstFrameSamplesToIgnore * SAMPLE_SIZE),
                              DMEM_UNCOMPRESSED_NOTE + dmemUncompressedAddrOffset1,
                              numSamplesInThisIteration * SAMPLE_SIZE);
                }

                numSamplesProcessed += numSamplesInThisIteration;

                switch (flags) {
                    case A_INIT:
                        skipBytes = SAMPLES_PER_FRAME * SAMPLE_SIZE;
                        dmemUncompressedAddrOffset1 = (numSamplesToDecode + SAMPLES_PER_FRAME) * SAMPLE_SIZE;
                        break;

                    case A_LOOP:
                        dmemUncompressedAddrOffset1 =
                            (numSamplesInThisIteration * SAMPLE_SIZE) + dmemUncompressedAddrOffset1;
                        break;

                    default:
                        if (dmemUncompressedAddrOffset1 != 0) {
                            dmemUncompressedAddrOffset1 =
                                (numSamplesInThisIteration * SAMPLE_SIZE) + dmemUncompressedAddrOffset1;
                        } else {
                            dmemUncompressedAddrOffset1 =
                                (nFirstFrameSamplesToIgnore + numSamplesInThisIteration) * SAMPLE_SIZE;
                        }
                        break;
                }

                flags = A_CONTINUE;

            codec_dctf_break:

                // Update what to do with the samples next
                if (sampleFinished) {
                    aClearBuffer(aList++, DMEM_UNCOMPRESSED_NOTE + dmemUncompressedAddrOffset1,
                                 (numSamplesToLoadAdj - numSamplesProcessed) * SAMPLE_SIZE);
                    sampleState->bitField0.finished = true;
                    note->sampleState.bitField0.finished = true;
                    AudioSynth_DisableSampleStates(updateIndex, noteIndex);
                    break; // break out of the for-loop
                } else if (loopToPoint) {
                    synthState->restart = true;
                    synthState->samplePosInt = loopInfo->start;
                } else {
                    synthState->samplePosInt += nSamplesToProcess;
                }
            }

            switch (numParts) {
                case 1:
                    noteSamplesDmemAddrBeforeResampling = DMEM_UNCOMPRESSED_NOTE + skipBytes;
                    break;

                case 2:
                    switch (curPart) {
                        case 0:
                            aInterl(aList++, skipBytes + DMEM_UNCOMPRESSED_NOTE,
                                    DMEM_TEMP + (SAMPLES_PER_FRAME * SAMPLE_SIZE), ALIGN8(numSamplesToLoadAdj / 2));
                            resampledTempLen = numSamplesToLoadAdj;
                            noteSamplesDmemAddrBeforeResampling = DMEM_TEMP + (SAMPLES_PER_FRAME * SAMPLE_SIZE);
                            if (sampleState->bitField0.finished) {
                                aClearBuffer(aList++, resampledTempLen + noteSamplesDmemAddrBeforeResampling,
                                             numSamplesToLoadAdj + SAMPLES_PER_FRAME);
                            }
                            break;

                        case 1:
                            aInterl(aList++, skipBytes + DMEM_UNCOMPRESSED_NOTE,
                                    resampledTempLen + DMEM_TEMP + (SAMPLES_PER_FRAME * SAMPLE_SIZE),
                                    ALIGN8(numSamplesToLoadAdj / 2));
                            break;

                        default:
                            break;
                    }

                    break;
            }

            if (sampleState->bitField0.finished) {
                break;
            }
        }
    }

    // Update the flags for the signal processing below
    flags = A_CONTINUE;
    if (sampleState->bitField0.needsInit == true) {
        flags = A_INIT;
        sampleState->bitField0.needsInit = false;
    }

    flags = sp56 | flags;

    // Resample the decompressed mono-signal to the correct pitch
    aList = AudioSynth_FinalResample(aList, synthState, aiBufLen * SAMPLE_SIZE, resampleRateFixedPoint,
                                     noteSamplesDmemAddrBeforeResampling, flags);

    if (flags & A_INIT) {
        flags = A_INIT;
    }

    if (sampleState->bitField1.bookOffset == 3) {
        aUnkCmd19(aList++, 0, aiBufLen * SAMPLE_SIZE, DMEM_TEMP, DMEM_TEMP);
    }

    // Apply the gain to the mono-signal to adjust the volume
    gain = sampleState->gain;
    if (gain != 0) {
        // A gain of 0x10 (a UQ4.4 number) is equivalent to 1.0 and represents no volume change
        if (gain < 0x10) {
            gain = 0x10;
        }
        aHiLoGain(aList++, gain, (aiBufLen + SAMPLES_PER_FRAME) * SAMPLE_SIZE, DMEM_TEMP, 0);
    }

    // Determine the behavior of the audio processing that leads to the haas effect
    if ((sampleState->leftDelaySize != 0) || (synthState->prevHaasEffectLeftDelaySize != 0)) {
        delaySide = HAAS_EFFECT_DELAY_LEFT;
    } else if ((sampleState->rightDelaySize != 0) || (synthState->prevHaasEffectRightDelaySize != 0)) {
        delaySide = HAAS_EFFECT_DELAY_RIGHT;
    } else {
        delaySide = HAAS_EFFECT_DELAY_NONE;
    }

    aList = AudioSynth_ProcessEnvelope(aList, sampleState, synthState, aiBufLen, DMEM_TEMP, delaySide, flags);
    if (sampleState->bitField0.usesHeadsetPanEffects) {
        if (!(flags & A_INIT)) {
            flags = A_CONTINUE;
        }
        aList = AudioSynth_ApplyHaasEffect(aList, sampleState, synthState, aiBufLen * 2, flags, delaySide);
    }

    return aList;
}

Acmd* AudioSynth_LoadWaveSamples(Acmd* aList, NoteSampleState* sampleState, NoteSynthesisState* synthState,
                                 s32 numSamplesToLoad) {
    s32 numSamplesAvail;
    s32 numDuplicates;

    aLoadBuffer(aList++, OS_K0_TO_PHYSICAL(sampleState->waveSampleAddr), DMEM_UNCOMPRESSED_NOTE,
                WAVE_SAMPLE_COUNT * SAMPLE_SIZE);

    // Offset in the WAVE_SAMPLE_COUNT samples of gWaveSamples to start processing the wave for continuity
    synthState->samplePosInt = (u32) synthState->samplePosInt % WAVE_SAMPLE_COUNT;
    // Number of samples in the initial WAVE_SAMPLE_COUNT samples available to be used to process
    numSamplesAvail = WAVE_SAMPLE_COUNT - synthState->samplePosInt;

    if (numSamplesToLoad > numSamplesAvail) {
        // Duplicate (copy) the WAVE_SAMPLE_COUNT samples as many times as needed to reach numSamplesToLoad.
        // (numSamplesToLoad - numSamplesAvail) is the number of samples missing.
        // Divide by WAVE_SAMPLE_COUNT, rounding up, to get the amount of duplicates
        numDuplicates = ((numSamplesToLoad - numSamplesAvail + WAVE_SAMPLE_COUNT - 1) / WAVE_SAMPLE_COUNT);
        if (numDuplicates != 0) {
            aDuplicate(aList++, numDuplicates, DMEM_UNCOMPRESSED_NOTE,
                       DMEM_UNCOMPRESSED_NOTE + (WAVE_SAMPLE_COUNT * SAMPLE_SIZE));
        }
    }
    return aList;
}

// Original name: Nas_Synth_Resample
Acmd* AudioSynth_FinalResample(Acmd* aList, NoteSynthesisState* synthState, s32 size, u16 pitch, u16 inpDmem,
                               u32 resampleFlags) {
    if (pitch == 0) {
        aClearBuffer(aList++, DMEM_TEMP, size);
    } else {
        aSetBuffer(aList++, 0, inpDmem, DMEM_TEMP, size);
        aResample(aList++, resampleFlags, pitch, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers->finalResampleState));
    }
    return aList;
}

// Original name: Nas_Synth_Envelope
Acmd* AudioSynth_ProcessEnvelope(Acmd* aList, NoteSampleState* sampleState, NoteSynthesisState* synthState,
                                 s32 aiBufLen, u16 dmemSrc, s32 delaySide, s32 flags) {
    s16 rampReverb;
    s16 rampRight;
    s16 rampLeft;
    u16 panVolLeft;
    u16 panVolRight;
    u16 curVolLeft;
    u16 curVolRight;
    s32 sourceReverbVol;
    s32 temp = 0;

    curVolLeft = synthState->curVolLeft;
    curVolRight = synthState->curVolRight;

    panVolLeft = sampleState->panVolLeft;
    panVolRight = sampleState->panVolRight;

    panVolLeft <<= 4;
    panVolRight <<= 4;

    if (panVolLeft != curVolLeft) {
        rampLeft = (panVolLeft - curVolLeft) / (aiBufLen >> 3);
    } else {
        rampLeft = 0;
    }
    if (panVolRight != curVolRight) {
        rampRight = (panVolRight - curVolRight) / (aiBufLen >> 3);
    } else {
        rampRight = 0;
    }

    sourceReverbVol = synthState->reverbVol;

    if (sampleState->reverb != sourceReverbVol) {
        temp = (((sampleState->reverb & 0x7F) - (sourceReverbVol & 0x7F)) << 8);
        rampReverb = temp / (aiBufLen >> 3);
        synthState->reverbVol = sampleState->reverb;
    } else {
        rampReverb = 0;
    }

    synthState->curVolLeft = curVolLeft + (rampLeft * (aiBufLen >> 3));
    synthState->curVolRight = curVolRight + (rampRight * (aiBufLen >> 3));

    if (sampleState->bitField0.usesHeadsetPanEffects) {
        aClearBuffer(aList++, DMEM_HAAS_TEMP, DMEM_1CH_SIZE);
        aEnvSetup1(aList++, (sourceReverbVol & 0x7F), rampReverb, rampLeft, rampRight);
        aEnvSetup2(aList++, curVolLeft, curVolRight);

        switch (delaySide) {
            case HAAS_EFFECT_DELAY_LEFT:
                aEnvMixer(aList++, dmemSrc, aiBufLen, 0, 0, ((sourceReverbVol & 0x80) >> 7),
                          sampleState->bitField0.stereoStrongRight, sampleState->bitField0.stereoStrongLeft,
                          0x65B1C9E1);
                break;

            case HAAS_EFFECT_DELAY_RIGHT:
                aEnvMixer(aList++, dmemSrc, aiBufLen, 0, 0, ((sourceReverbVol & 0x80) >> 7),
                          sampleState->bitField0.stereoStrongRight, sampleState->bitField0.stereoStrongLeft,
                          0x9965C9E1);
                break;

            default: // HAAS_EFFECT_DELAY_NONE
                aEnvMixer(aList++, dmemSrc, aiBufLen, 0, 0, ((sourceReverbVol & 0x80) >> 7),
                          sampleState->bitField0.stereoStrongRight, sampleState->bitField0.stereoStrongLeft,
                          0x99B1C9E1);
                break;
        }
    } else {
        aEnvSetup1(aList++, (sourceReverbVol & 0x7F), rampReverb, rampLeft, rampRight);
        aEnvSetup2(aList++, curVolLeft, curVolRight);
        aEnvMixer(aList++, dmemSrc, aiBufLen, 0, 0, ((sourceReverbVol & 0x80) >> 7),
                  sampleState->bitField0.stereoStrongRight, sampleState->bitField0.stereoStrongLeft, 0x99B1C9E1);
    }

    return aList;
}

/**
 * Original name: Nas_Synth_Delay
 *
 * The Haas Effect gives directionality to sound by applying a small (< 35ms) delay to either the left or right channel.
 * The delay is small enough that the sound is still perceived as one sound, but the channel that is not delayed will
 * reach our ear first and give a sense of directionality. The sound is directed towards the opposite side of the delay.
 */
Acmd* AudioSynth_ApplyHaasEffect(Acmd* aList, NoteSampleState* sampleState, NoteSynthesisState* synthState, s32 size,
                                 s32 flags, s32 delaySide) {
    u16 dmemDest;
    u8 haasEffectDelaySize;
    u8 prevHaasEffectDelaySize;
    u16 pitch;

    switch (delaySide) {
        case HAAS_EFFECT_DELAY_LEFT:
            // Delay the sample on the left channel
            // This allows the right channel to be heard first
            dmemDest = DMEM_LEFT_CH;
            haasEffectDelaySize = sampleState->leftDelaySize;
            prevHaasEffectDelaySize = synthState->prevHaasEffectLeftDelaySize;
            synthState->prevHaasEffectRightDelaySize = 0;
            synthState->prevHaasEffectLeftDelaySize = haasEffectDelaySize;
            break;

        case HAAS_EFFECT_DELAY_RIGHT:
            // Delay the sample on the right channel
            // This allows the left channel to be heard first
            dmemDest = DMEM_RIGHT_CH;
            haasEffectDelaySize = sampleState->rightDelaySize;
            prevHaasEffectDelaySize = synthState->prevHaasEffectRightDelaySize;
            synthState->prevHaasEffectRightDelaySize = haasEffectDelaySize;
            synthState->prevHaasEffectLeftDelaySize = 0;
            break;

        default: // HAAS_EFFECT_DELAY_NONE
            return aList;
    }

    if (flags != A_INIT) {
        // Slightly adjust the sample rate in order to fit a change in sample delay
        if (haasEffectDelaySize != prevHaasEffectDelaySize) {
            pitch = (((size << 0xF) / 2) - 1) / ((size + haasEffectDelaySize - prevHaasEffectDelaySize - 2) / 2);
            aSetBuffer(aList++, 0, DMEM_HAAS_TEMP, DMEM_TEMP, size + haasEffectDelaySize - prevHaasEffectDelaySize);
            aResampleZoh(aList++, pitch, 0);
        } else {
            aDMEMMove(aList++, DMEM_HAAS_TEMP, DMEM_TEMP, size);
        }
        if (prevHaasEffectDelaySize != 0) {
            aLoadBuffer(aList++, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers->panSamplesBuffer), DMEM_HAAS_TEMP,
                        ALIGN16(prevHaasEffectDelaySize));
            aDMEMMove(aList++, DMEM_TEMP, prevHaasEffectDelaySize + DMEM_HAAS_TEMP,
                      size + haasEffectDelaySize - prevHaasEffectDelaySize);
        } else {
            aDMEMMove(aList++, DMEM_TEMP, DMEM_HAAS_TEMP, size + haasEffectDelaySize);
        }
    } else {
        // Just apply a delay directly
        aDMEMMove(aList++, DMEM_HAAS_TEMP, DMEM_TEMP, size);
        aClearBuffer(aList++, DMEM_HAAS_TEMP, haasEffectDelaySize);
        aDMEMMove(aList++, DMEM_TEMP, haasEffectDelaySize + DMEM_HAAS_TEMP, size);
    }

    if (haasEffectDelaySize) { // != 0
        // Save excessive samples for next iteration
        aSaveBuffer(aList++, size + DMEM_HAAS_TEMP, OS_K0_TO_PHYSICAL(synthState->synthesisBuffers->panSamplesBuffer),
                    ALIGN16(haasEffectDelaySize));
    }

    aAddMixer(aList++, ALIGN64(size), DMEM_HAAS_TEMP, dmemDest, 0x7FFF);

    return aList;
}
