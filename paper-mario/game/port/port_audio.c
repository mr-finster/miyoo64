/* The game's audio manager for the native port (M2). Game side. nusys's audio thread (nuAuMgr, src/audio/core/
 * system.c) woke on every retrace, ran the last frame's command list on the RSP and built the next with
 * alAudioFrame, AlFrameSize or AlMinFrameSize samples as the AI's buffer ran low or full. Here each retrace builds a
 * frame and runs it at once in port_mixer.c, as long as the host's output wants more (port_audio_out.c): a slow game
 * frame is made up with more audio frames, so the music keeps its tempo. */
#include "common.h"
#include "audio.h"
#include "nu/nusys.h"
#include "port.h"

extern Acmd* AlCmdListBuffers[3];
extern u8* D_800A3628[3];
extern s32 AlFrameSize, AlMinFrameSize;
extern u8 AuHeapBase[AUDIO_HEAP_SIZE];
extern u32 nuAuFrameCounter;
Acmd* alAudioFrame(Acmd* cmdList, s32* cmdLen, s16* outBuf, s32 outLen);
void nuAuCleanDMABuffers(void);

static s32 sReady;

void port_audio_boot(void) {
    port_mixer_set_heap(AuHeapBase, AUDIO_HEAP_SIZE);
    port_audio_out_init(HARDWARE_OUTPUT_RATE);
    sReady = AlCmdListBuffers[0] != nullptr && D_800A3628[0] != nullptr && AlFrameSize > 0;
    port_log("audio: %s, frames of %d or %d samples at %d Hz\n", sReady ? "on" : "OFF (no engine)", AlFrameSize,
             AlMinFrameSize, HARDWARE_OUTPUT_RATE);
}

void port_audio_retrace(void) {
    s32 guard;
    u32 perfT0;
    if (!sReady) {
        return;
    }
    perfT0 = port_time_us();
    port_audio_out_mark();
    for (guard = 0; guard < 8; guard++) {
        s32 len;
        Acmd* end;
        const s32 samples = port_audio_out_want(AlFrameSize, AlMinFrameSize);
        if (samples <= 0) {
            break;
        }
        end = alAudioFrame(AlCmdListBuffers[0], &len, (s16*) D_800A3628[0], samples);
        port_mixer_run((const u32*) AlCmdListBuffers[0], end - AlCmdListBuffers[0]);
        nuAuFrameCounter++;
        nuAuCleanDMABuffers();
        port_audio_out_push((const s16*) D_800A3628[0], samples);
    }
    port_perf_audio_us += port_time_us() - perfT0;
}
