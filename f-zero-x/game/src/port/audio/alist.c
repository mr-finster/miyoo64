/**
 * F-Zero X's audio microcode (aspMain, a NEAD variant) run on the CPU: the
 * game's own command list (synthesis.c, encoded by include/PR/abi.h) decoded
 * word by word and executed on the C mixer. The command set and field layout
 * follow mupen64plus rsp-hle's alist_process_nead_fz table (reference only).
 * F-Zero X's synthesis emits CLEARBUFF, LOADBUFF, SAVEBUFF, MIXER, INTERLEAVE,
 * LOADADPCM, SETLOOP, SETBUFF, ADPCM, DMEMMOVE, RESAMPLE and ENVSETUP1/2 +
 * ENVMIXER; the rest of the table is decoded for completeness.
 */
#include <stdio.h>
#include <stdlib.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#include "mixer.h"

#define PTR(w) ((void*) (uintptr_t) (w))

void port_audio_run_alist(const u64* list, u32 bytes) {
    const u32* w = (const u32*) list;
    const u32* end = w + (bytes / 8) * 2;
    static u32 sUnknownLogged;
    static int sTrace = -1;
    static u32 sTasks;
    if (sTrace < 0) {
        const char* e = getenv("FZX_AUDIO_TRACE");
        sTrace = e != NULL && e[0] == '1';
    }
    if (sTrace && (sTasks++ % 60) == 0) {
        /* FZX_AUDIO_TRACE=1: once a second, the command mix of one task. */
        u32 counts[32] = { 0 }, i;
        const u32* p;
        char line[256];
        int len = 0;
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
            case A_INTERLEAVE:
                aInterleaveImpl(w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF, (w0 >> 12) & 0xFF0);
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
                aEnvMixerNeadImpl((w0 >> 12) & 0xFF0, (w0 >> 8) & 0xFF, (w0 >> 4) & 1, xors,
                                  (w1 >> 20) & 0xFF0, (w1 >> 12) & 0xFF0, (w1 >> 4) & 0xFF0, (w1 << 4) & 0xFF0);
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
            case A_S8DEC: /* not in F-Zero X's microcode table; the game has no S8 samples */
                aS8DecImpl((w0 >> 16) & 0xFF, (int16_t*) PTR(w1));
                break;
            case A_DUPLICATE: /* count copies of 128 bytes (the kit's helper makes count + 1) */
                if (((w0 >> 16) & 0xFF) != 0) {
                    aDuplicateImpl(w0 & 0xFFFF, w1 >> 16, ((w0 >> 16) & 0xFF) - 1);
                }
                break;
            case A_SPNOOP:
            case 3:
            case 6:
            case 7:
            case 9:
            case 14:
            case 24:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31:
                break;
            default:
                if (sUnknownLogged++ < 8) {
                    port_log("audio: unknown aspMain command %08X %08X\n", w0, w1);
                }
                break;
        }
    }
}
