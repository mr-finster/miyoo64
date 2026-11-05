/* n_aspMain, the n_audio microcode, in C: runs the command lists Harvest Moon 64's n_audio library builds (the nusys
 * audio manager, decomp.patch: nuaustlmgr.c) with the kernels in hh_mixer.c. The command set is plain n_audio's
 * (mupen64plus rsp-hle names it alist_process_naudio): the DMEM offsets are the library's, and the state addresses
 * a command packs into 24 bits are in the N64's RAM (the audio heap), which the port maps at 0x80000000. Host side. */
#include <stdint.h>
#include "hh_mixer.h"
#include "port.h"

static inline void *n64ptr(uint32_t a) {
    if (a < 0x20000000u) a |= 0x80000000u; /* K0_TO_PHYS'd or packed into 24 bits: the audio heap, N64 RAM */
    return (void *) (uintptr_t) a;
}

void hh_aspmain_run(const uint32_t *cmd, int count) {
    for (int k = 0; k < count; k++, cmd += 2) {
        const uint32_t w0 = cmd[0], w1 = cmd[1];
        switch (w0 >> 24) {
        case 1: /* ADPCM */
            aADPCMdecImpl((uint8_t) (w1 >> 28), n64ptr(w0 & 0xFFFFFF), (int) ((w1 >> 16) & 0xFFF), (uint16_t) ((w1 >> 12) & 0xF),
                          (uint16_t) (w1 & 0xFFF));
            break;
        case 2: aClearBufferImpl((uint16_t) (w0 & 0xFFFFFF), (int) (w1 & 0xFFF)); break;
        case 3: aEnvMixerImpl((uint8_t) (w0 >> 16), n64ptr(w1), (int16_t) w0); break;
        case 4: aLoadBufferImpl(n64ptr(w1), (uint16_t) (w0 & 0xFFF), (uint16_t) ((w0 >> 12) & 0xFFF)); break;
        case 5: /* RESAMPLE */
            aResampleImpl((uint8_t) (w1 >> 30), (uint16_t) (w1 >> 14), n64ptr(w0 & 0xFFFFFF), (uint16_t) ((w1 >> 2) & 0xFFF),
                          (uint8_t) (w1 & 3));
            break;
        case 6: aSaveBufferImpl((uint16_t) (w0 & 0xFFF), n64ptr(w1), (uint16_t) ((w0 >> 12) & 0xFFF)); break;
        case 9: aSetVolumeImpl((uint8_t) (w0 >> 16), (int16_t) w0, (int16_t) (w1 >> 16), (int16_t) w1); break;
        case 10: aDMEMMoveImpl((uint16_t) (w0 & 0xFFFFFF), (uint16_t) (w1 >> 16), (int) (w1 & 0xFFFF)); break;
        case 11: aLoadADPCMImpl((int) (w0 & 0xFFFFFF), n64ptr(w1)); break;
        case 12: aMixImpl((uint8_t) (w0 >> 16), (int16_t) w0, (uint16_t) (w1 >> 16), (uint16_t) w1); break;
        case 13: aInterleaveImpl(); break;
        case 14: aRate1LowImpl((uint16_t) w1); break;
        case 15: aSetLoopImpl(n64ptr(w1)); break;
        default: break; /* SPNOOP, SEGMENT, SETBUFF: nothing in n_audio */
        }
    }
}
