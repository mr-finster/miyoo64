/**
 * Star Fox 64's audio microcode (aspMain, the NEAD variant rsp-hle calls
 * "nead_sf") run on the CPU: the game's own command list (audio_synthesis.c,
 * encoded by include/PR/abi.h) decoded word by word and executed on the kit's
 * C mixer. Command table and field layout follow mupen64plus rsp-hle's
 * alist_process_nead_sf (reference only); the decoder is f-zero-x's
 * (47fc96b, alist_process_nead_fz), with Star Fox's extra commands:
 * RESAMPLE_ZOH (6), INTERLEAVE with the SETBUFF count (13), HILOGAIN (24),
 * DUPLICATE (26).
 *
 * Star Fox's synthesis emits CLEARBUFF, ADDMIXER, RESAMPLE, RESAMPLE_ZOH,
 * SETBUFF, DMEMMOVE, LOADADPCM, MIXER, INTERLEAVE, SETLOOP, INTERL, ENVSETUP1/2,
 * ENVMIXER, LOADBUFF, SAVEBUFF, ADPCM, S8DEC, HILOGAIN, UNK19 and DUPLICATE.
 * UNK19 (25) is not in the microcode's table (rsp-hle: unknown; OoT's aspMain
 * lacks it too) and only follows notes in sample-book mode 3: a no-op, logged
 * once. S8DEC (23) is decoded although rsp-hle lists 23 as a no-op: the game
 * emits it for CODEC_S8 samples, so decoding them is the only way they sound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ultra64.h>
#include "port.h"
#include "mixer.h"
#include "savestate.h"

#define PTR(w) ((void*) (uintptr_t) (w))

/* SF64_AUDIO_TRACE=1: once a second, the command mix of one task. */
static void trace_task(const u32* w, const u32* end, u32 bytes) {
    static u32 sTasks SAVESTATE_EXCLUDE;
    u32 counts[32] = { 0 }, i;
    const u32* p;
    char line[256];
    int len = 0;
    if ((sTasks++ % 60) != 0) {
        return;
    }
    for (p = w; p < end; p += 2) {
        counts[p[0] >> 24 & 31]++;
    }
    for (i = 0; i < 32; i++) {
        if (counts[i] != 0) {
            len += snprintf(line + len, sizeof(line) - len, " %u:%u", i, counts[i]);
        }
    }
    port_log("audio task %u (%u cmds):%s\n", sTasks - 1, bytes / 8, line);
}

static void log_once(u32* seen, const char* what, u32 w0, u32 w1) {
    if (*seen < 4) {
        port_log("audio: %s %08X %08X\n", what, w0, w1);
    }
    (*seen)++;
}

void port_audio_run_alist(const void* list, u32 bytes) {
    static int sTrace SAVESTATE_EXCLUDE = -1;
    static u32 sUnknown SAVESTATE_EXCLUDE, sUnk19 SAVESTATE_EXCLUDE, sS8 SAVESTATE_EXCLUDE;
    const u32* w = (const u32*) list;
    const u32* end = w + (bytes / 8) * 2;

    if (sTrace < 0) {
        const char* e = getenv("SF64_AUDIO_TRACE");
        sTrace = e != NULL && e[0] == '1';
    }
    if (sTrace) {
        trace_task(w, end, bytes);
    }

    for (; w < end; w += 2) {
        const u32 w0 = w[0], w1 = w[1];
        switch (w0 >> 24) {
            case A_ADPCM:
                aADPCMdecImpl((w0 >> 16) & 0xFF, (int16_t*) PTR(w1));
                break;
            case A_CLEARBUFF:
                if ((w1 & 0xFFF) != 0) {
                    aClearBufferImpl(w0 & 0xFFFF, w1 & 0xFFF);
                }
                break;
            case A_ADDMIXER:
                aAddMixerImpl(w1 >> 16, w1 & 0xFFFF, (w0 >> 12) & 0xFF0);
                break;
            case A_RESAMPLE:
                aResampleImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, (int16_t*) PTR(w1));
                break;
            case A_RESAMPLE_ZOH:
                aResampleZohImpl(w0 & 0xFFFF, w1 & 0xFFFF);
                break;
            case A_SETBUFF:
                aSetBufferImpl((w0 >> 16) & 0xFF, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF);
                break;
            case A_DMEMMOVE:
                if ((w1 & 0xFFFF) != 0) {
                    aDMEMMoveImpl(w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF);
                }
                break;
            case A_LOADADPCM:
                aLoadADPCMImpl(w0 & 0xFFFF, (const int16_t*) PTR(w1));
                break;
            case A_MIXER:
                aMixImpl((int16_t) (w0 & 0xFFFF), w1 >> 16, w1 & 0xFFFF, (w0 >> 12) & 0xFF0);
                break;
            case A_INTERLEAVE: /* rsp-hle INTERLEAVE_MK: output and count from SETBUFF */
                aInterleaveOldImpl(w1 >> 16, w1 & 0xFFFF);
                break;
            case A_SETLOOP:
                aSetLoopImpl((ADPCM_STATE*) PTR(w1));
                break;
            case 16: /* NEAD_16: block copy */
                aCopyBlocksImpl(w1 >> 16, w0 & 0xFFFF, w1 & 0xFFFF, (w0 >> 16) & 0xFF);
                break;
            case A_INTERL:
                aInterlImpl(w1 & 0xFFFF, w1 >> 16, w0 & 0xFFFF);
                break;
            case A_ENVSETUP1:
                aEnvSetup1Impl((w0 >> 16) & 0xFF, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF);
                break;
            case A_ENVMIXER: {
                /* Sign flags as rsp-hle applies them: bit 3 wet L, 2 wet R, 1 dry L, 0 dry R. */
                int16_t xors[4];
                xors[2] = (int16_t) (0 - (int16_t) ((w0 & 0x8) >> 1));
                xors[3] = (int16_t) (0 - (int16_t) ((w0 & 0x4) >> 1));
                xors[0] = (int16_t) (0 - (int16_t) ((w0 & 0x2) >> 1));
                xors[1] = (int16_t) (0 - (int16_t) (w0 & 0x1));
                aEnvMixerNeadImpl((w0 >> 12) & 0xFF0, (w0 >> 8) & 0xFF, (w0 >> 4) & 1, xors, (w1 >> 20) & 0xFF0,
                                  (w1 >> 12) & 0xFF0, (w1 >> 4) & 0xFF0, (w1 << 4) & 0xFF0);
                break;
            }
            case A_LOADBUFF:
                aLoadBufferImpl(PTR(w1), w0 & 0xFFF, (w0 >> 12) & 0xFFF);
                break;
            case A_SAVEBUFF:
                aSaveBufferImpl(w0 & 0xFFF, (int16_t*) PTR(w1), (w0 >> 12) & 0xFFF);
                break;
            case A_ENVSETUP2:
                aEnvSetup2Impl(w1 >> 16, w1 & 0xFFFF);
                break;
            case A_S8DEC:
                if (sS8 == 0) {
                    log_once(&sS8, "S8 sample decoded (command 23)", w0, w1);
                }
                aS8DecImpl((w0 >> 16) & 0xFF, (int16_t*) PTR(w1));
                break;
            case A_HILOGAIN:
                aHiLoGainImpl((w0 >> 16) & 0xFF, w0 & 0xFFF, w1 >> 16);
                break;
            case A_UNK19:
                log_once(&sUnk19, "UNK19 (not in the microcode) skipped", w0, w1);
                break;
            case A_DUPLICATE: /* count copies of 128 bytes (the kit's helper makes count + 1) */
                if (((w0 >> 16) & 0xFF) != 0) {
                    aDuplicateImpl(w0 & 0xFFFF, w1 >> 16, ((w0 >> 16) & 0xFF) - 1);
                }
                break;
            case A_SPNOOP:
            case 3:
            case 7:
            case 9:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31:
                break;
            default: /* 14 POLEF: not emitted by Star Fox's synthesis */
                log_once(&sUnknown, "unexpected aspMain command", w0, w1);
                break;
        }
    }
}
