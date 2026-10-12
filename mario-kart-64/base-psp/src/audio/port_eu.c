#include <ultra64.h>
#include <macros.h>
#include <PR/ucode.h>

#include "audio/synthesis.h"
#include "audio/seqplayer.h"
#include "audio/port_eu.h"
#include "audio/load.h"
#include "audio/heap.h"
#include "audio/data.h"

OSMesgQueue D_801937C0;
OSMesgQueue D_801937D8;
OSMesgQueue D_801937F0;
OSMesgQueue D_80193808;

struct EuAudioCmd sAudioCmd[0x100];

// Seems oversized by 1
OSMesg D_80194020[2];
OSMesg D_80194028[32]; /* was 4: overflowed (batches silently dropped) during a paced reset */
OSMesg D_80194038[1];
OSMesg D_8019403C[1];

u8 D_800EA3A0[] = { 0, 0, 0, 0 };

u8 D_800EA3A4[] = { 0, 0, 0, 0 };

OSMesgQueue* D_800EA3A8 = &D_801937C0;
OSMesgQueue* D_800EA3AC = &D_801937D8;
OSMesgQueue* D_800EA3B0 = &D_801937F0;
OSMesgQueue* D_800EA3B4 = &D_80193808;

char port_eu_unused_string0[] = "DAC:Lost 1 Frame.\n";
char port_eu_unused_string1[] = "DMA: Request queue over.( %d )\n";
char port_eu_unused_string2[] = "DMA [ %d lines] TIMEOUT\n";
char port_eu_unused_string3[] = "Warning: WaveDmaQ contains %d msgs.\n";
char port_eu_unused_string4[] = "Audio:now-max tasklen is %d / %d\n";
char port_eu_unused_string5[] = "Audio:Warning:ABI Tasklist length over (%d)\n";

s32 D_800EA484 = 128;

char port_eu_unused_string6[] = "AudioSend: %d -> %d (%d)\n";

s32 D_800EA4A4 = 0;

char port_eu_unused_string7[] = "Undefined Port Command %d\n";

#ifndef TARGET_N64
#include <stdlib.h>
/* The reset hold (the user's device report after v38: the menu OK chime hard-cut going into a race).
 * Leaving the menus the game fades the music out (func_800CA330: 200 updates, ~1.1 s) and plays the
 * OK chime, then loads the course and only then sends the audio reset (setup_race -> func_800CA008),
 * which disables every player and cuts what is left within ~5 buffers. On the N64 the course load
 * takes about as long as that fade, so by the reset the music is gone and the chime has rung out
 * (mupen: the level falls ~26 dB over ~0.6 s before the reset). The port loads in a fraction of the
 * time, so the reset landed a third of the way into the fade, on the chime. So a reset request that
 * arrives while a sequence player is still fading out (state 2) waits, rendering normal frames,
 * until that fade ends (the player disables itself) -- at most RESET_HOLD_MAX buffers. The shim's
 * osRecvMesg never blocks, so unlike the N64 the game thread doesn't wait in audio_reset_session_eu:
 * it carries on into the race and queues the race's audio behind the reset, and the drain below
 * stays shut through the hold as through the reset -- drained during the hold, those commands ran
 * and the reset then wiped them (the race began 6.7 s silent under qemu). MK64_RESETHOLD=0: as before. */
#define RESET_HOLD_MAX 90 /* buffers (~16 ms each): 1.5 s */
static s32 sResetHold;
static s32 sResetHoldBuffers;
static s32 reset_hold_fading(void) {
    static s32 sOn = -1;
    s32 i;
    if (sOn < 0) { const char* e = getenv("MK64_RESETHOLD"); sOn = !(e && e[0] == '0'); }
    if (!sOn) return 0;
    for (i = 0; i < SEQUENCE_PLAYERS; i++) {
        if (gSequencePlayers[i].enabled && gSequencePlayers[i].state == 2 && gSequencePlayers[i].fadeRemainingFrames != 0) return 1;
    }
    return 0;
}
#if defined(TARGET_HANDHELD)
#include <unistd.h>
/* The game thread, right after it sends a reset (audio_reset_session_eu). The N64 blocks there
 * until the audio thread has finished the reset; the shim never blocks, so the port normally runs
 * on. A HELD reset is where that shows: the race would appear while the menu music was still
 * fading, and start ~1 s before its own audio. So when this reset will be held, wait like the
 * console -- the loading screen stays up through the fade and the reset (~1 s at most on the
 * device, whose course loads are fast), the audio frames coming from the backstop mixer. Other
 * resets run on as before. */
void port_audio_reset_wait(void) {
    extern void port_audio_mix_lock(void), port_audio_mix_unlock(void);
    OSMesg m;
    s32 held, r, ms;
    port_audio_mix_lock();
    held = sResetHold || (gAudioResetStatus == 0 && reset_hold_fading());
    port_audio_mix_unlock();
    if (!held) return;
    { extern int port_audio_backstop_running(void); if (!port_audio_backstop_running()) return; } /* nothing would mix */
    for (ms = 0; ms < 3000; ms += 2) { /* the reset's done message; 3 s at most */
        port_audio_mix_lock();
        r = osRecvMesg(D_800EA3B4, &m, 0);
        port_audio_mix_unlock();
        if (r == 0) break;
        usleep(2000);
    }
}
#endif
#endif

struct SPTask* create_next_audio_frame_task(void) {
    u32 samplesRemainingInAI;
    s32 writtenCmds;
    s32 index;
    OSTask_t* task;
    s32 var_s0;
    s16* currAiBuffer;
    s32 oldDmaCount;
    OSMesg sp58;
    OSMesg sp54;
    s32 writtenCmdsCopy;

    gAudioFrameCount++;
    if ((gAudioFrameCount % gAudioBufferParameters.presetUnk4) != 0) {
        return NULL;
    }
    osSendMesg(D_800EA3A8, (OSMesg) gAudioFrameCount, OS_MESG_NOBLOCK);

    gAudioTaskIndex ^= 1;
    gCurrAiBufferIndex++;
    gCurrAiBufferIndex %= NUMAIBUFFERS;
    index = (gCurrAiBufferIndex + 1) % NUMAIBUFFERS;
    samplesRemainingInAI = osAiGetLength() / 4;

    if (gAiBufferLengths[index] != 0) {
        osAiSetNextBuffer(gAiBuffers[index], gAiBufferLengths[index] * 4);
    }
    oldDmaCount = gCurrAudioFrameDmaCount;
    for (var_s0 = 0; var_s0 < gCurrAudioFrameDmaCount; var_s0++) {
        if (osRecvMesg(&gCurrAudioFrameDmaQueue, NULL, 0) == 0) {
            oldDmaCount -= 1;
        }
    }
    if (oldDmaCount != 0) {
        for (var_s0 = 0; var_s0 < oldDmaCount; var_s0++) {
            osRecvMesg(&gCurrAudioFrameDmaQueue, NULL, 1);
        }
    }
    oldDmaCount = gCurrAudioFrameDmaQueue.validCount;
    if (oldDmaCount != 0) {
        for (var_s0 = 0; var_s0 < oldDmaCount; var_s0++) {
            osRecvMesg(&gCurrAudioFrameDmaQueue, NULL, 0);
        }
    }
    gCurrAudioFrameDmaCount = 0;
    decrease_sample_dma_ttls();
#ifndef TARGET_N64
    if (sResetHold) {
        /* holding a reset for the music's fade-out (see reset_hold_fading) */
        if (!reset_hold_fading() || ++sResetHoldBuffers >= RESET_HOLD_MAX) {
            sResetHold = 0;
            gAudioResetStatus = 5;
        }
    } else
#endif
    if (osRecvMesg(D_800EA3B0, &sp58, 0) != -1) {
        gAudioResetPresetIdToLoad = (u8) (u32) sp58;
#ifndef TARGET_N64
        if (gAudioResetStatus == 0 && reset_hold_fading()) {
            sResetHold = 1;
            sResetHoldBuffers = 0;
        } else
#endif
        gAudioResetStatus = 5;
    }
    if (gAudioResetStatus != 0) {
        /* Same as the N64: keep synthesizing while the players are disabled and
         * the notes release (status >= 3, step() == 1) so the fade is actually
         * rendered, and produce nothing only once the heap is being rebuilt
         * (status < 3).  Device round 13: the port used to return NULL for
         * EVERY reset state, which (a) hard-cut the fade the N64 makes audible
         * and (b) left the AI push above (gAiBufferLengths is never 0) re-queuing
         * the LAST REAL FRAME ~10 times until status 3 zeroed the buffers -- the
         * "menu chime machine-guns behind the GP load" bug.  What the port must
         * still not do during the reset is drain the game's command queue; that
         * gate is at the drain below. */
        if (audio_shut_down_and_reset_step() == 0) {
            if (gAudioResetStatus == 0) {
                osSendMesg(D_800EA3B4, (OSMesg) (u32) gAudioResetPresetIdToLoad, OS_MESG_NOBLOCK);
            }
            return NULL;
        }
    }

    gAudioTask = &gAudioTasks[gAudioTaskIndex];
    gAudioCmd = gAudioCmdBuffers[gAudioTaskIndex];
    index = gCurrAiBufferIndex;
    currAiBuffer = gAiBuffers[index];
    gAiBufferLengths[index] =
        ((gAudioBufferParameters.samplesPerFrameTarget - samplesRemainingInAI + EXTRA_BUFFERED_AI_SAMPLES_TARGET) &
         ~0xF) +
        SAMPLES_TO_OVERPRODUCE;
    if (gAiBufferLengths[index] < gAudioBufferParameters.minAiBufferLength) {
        gAiBufferLengths[index] = gAudioBufferParameters.minAiBufferLength;
    }
    if (gAiBufferLengths[index] > gAudioBufferParameters.maxAiBufferLength) {
        gAiBufferLengths[index] = gAudioBufferParameters.maxAiBufferLength;
    }
#ifndef TARGET_N64
    /* Ports: the game issues the spec change (audio reset) and the sequence
     * start together.  The sequence-start command must stay queued in
     * D_800EA3AC until the reset's reload (which reallocates the note pools and
     * clears bank/seq load status) is done, or the just-started sequence gets
     * wiped and the music dies after ~0.25 s.  So: no draining mid-reset.  The
     * queue is deep enough (below) to hold every batch the game pushes across
     * the ~29-call walk. */
    if (gAudioResetStatus == 0 && !sResetHold) /* a held reset counts: the game didn't wait for it */
#endif
    while (osRecvMesg(D_800EA3AC, &sp54, 0) != -1) {
        func_800CBCB0((u32) sp54);
    }
    gAudioCmd = synthesis_execute((Acmd*) gAudioCmd, &writtenCmds, currAiBuffer, gAiBufferLengths[index]);
    gAudioRandom = osGetCount() * (gAudioRandom + gAudioFrameCount);
    gAudioRandom = gAudioRandom + gAiBuffers[index][gAudioFrameCount & 0xFF];

    index = gAudioTaskIndex;
    gAudioTask->msgqueue = NULL;
    // wtf?
    writtenCmdsCopy += 0;
    gAudioTask->msg = NULL;

    task = &gAudioTask->task.t;
    task->type = M_AUDTASK;
    task->flags = 0;
    task->ucode_boot = rspF3DBootStart;
    task->ucode_boot_size = (u8*) rspF3DBootEnd - (u8*) rspF3DBootStart;
    task->ucode = rspAspMainStart;
    task->ucode_data = rspAspMainDataStart;
    task->ucode_size = 0x1000; // (This size is ignored (according to sm64))
    task->ucode_data_size = (rspAspMainDataEnd - rspAspMainDataStart) * sizeof(u64);
    task->dram_stack = NULL;
    task->dram_stack_size = 0;
    task->output_buff = NULL;
    task->output_buff_size = NULL;
    task->data_ptr = (u64*) gAudioCmdBuffers[index];
    task->data_size = writtenCmds * sizeof(u64);
    task->yield_data_ptr = NULL;
    task->yield_data_size = 0;
    writtenCmdsCopy = writtenCmds;
    if (D_800EA484 < writtenCmds) {
        D_800EA484 = writtenCmdsCopy;
    }
    return gAudioTask;
}

void eu_process_audio_cmd(struct EuAudioCmd* cmd) {
    s32 i;
    switch (cmd->u.s.op) {
        case 0x81:
            preload_sequence(cmd->u.s.arg2, 3);
            break;

        case 0x82:
        case 0x88:
            load_sequence(cmd->u.s.bankId, cmd->u.s.arg2, cmd->u.s.arg3);
            func_800CBA64(cmd->u.s.bankId, cmd->u2.as_s32);
            break;

        case 0x83:
            if (gSequencePlayers[cmd->u.s.bankId].enabled != false) {
                if (cmd->u2.as_s32 == 0) {
                    sequence_player_disable(&gSequencePlayers[cmd->u.s.bankId]);
                } else {
                    seq_player_fade_to_zero_volume(cmd->u.s.bankId, cmd->u2.as_s32);
                }
            }
            break;

        case 0xf0:
            gAudioLibSoundMode = cmd->u2.as_s32;
            break;

        case 0xf1:
            for (i = 0; i < 4; i++) {
                gSequencePlayers[i].muted = true;
                gSequencePlayers[i].recalculateVolume = true;
            }
            break;

        case 0xf2:
            for (i = 0; i < 4; i++) {
                gSequencePlayers[i].muted = false;
                gSequencePlayers[i].recalculateVolume = true;
            }
            break;
        case 0xF3:
            func_800BB388(cmd->u.s.bankId, cmd->u.s.arg2, cmd->u.s.arg3);
            break;
    }
}

void seq_player_fade_to_zero_volume(s32 arg0, s32 fadeOutTime) {
    struct SequencePlayer* player;

    if (fadeOutTime == 0) {
        fadeOutTime = 1;
    }
    player = &gSequencePlayers[arg0];
    player->state = 2;
    player->fadeRemainingFrames = fadeOutTime;
    player->fadeVelocity = -(player->fadeVolume / fadeOutTime);
}

void func_800CBA64(s32 playerIndex, s32 fadeInTime) {
    struct SequencePlayer* player;

    if (fadeInTime != 0) {
        player = &gSequencePlayers[playerIndex];
        player->state = 1;
        player->fadeTimerUnkEu = fadeInTime;
        player->fadeRemainingFrames = fadeInTime;
        player->fadeVolume = 0.0f;
        player->fadeVelocity = 0.0f;
    }
}

void port_eu_init_queues(void) {
    D_800EA3A0[0] = 0;
    D_800EA3A4[0] = 0;
    osCreateMesgQueue(D_800EA3A8, D_80194020, 1);
    osCreateMesgQueue(D_800EA3AC, D_80194028, 32);
    osCreateMesgQueue(D_800EA3B0, D_80194038, 1);
    osCreateMesgQueue(D_800EA3B4, D_8019403C, 1);
}

void func_800CBB48(s32 arg0, s32* arg1) {
    struct EuAudioCmd* cmd = &sAudioCmd[D_800EA3A0[0] & 0xff];
    cmd->u.first = arg0;
    cmd->u2.as_u32 = *arg1;
    D_800EA3A0[0]++;
}

void func_800CBB88(u32 arg0, f32 arg1) {
    func_800CBB48(arg0, (s32*) &arg1);
}

void func_800CBBB8(u32 arg0, u32 arg1) {
    func_800CBB48(arg0, (s32*) &arg1);
}

void func_800CBBE8(u32 arg0, s8 arg1) {
    s32 sp34 = arg1 << 24;
    func_800CBB48(arg0, &sp34);
}

//! @todo clenanup, something's weird with the variables. D_800EA4A4 is probably EuAudioCmd bc of the + 0x100
void func_800CBC24(void) {
    s32 temp_t6;
    s32 test;
    OSMesg thing;
    temp_t6 = D_800EA3A0[0] - D_800EA3A4[0];
    test = (u8) temp_t6;
    test = (test + 0x100) & 0xFF;
    do {
    } while (0);
    if (D_800EA4A4 < test) {
        D_800EA4A4 = test;
    }
    thing = (OSMesg) ((D_800EA3A0[0] & 0xFF) | ((D_800EA3A4[0] & 0xFF) << 8));
#ifdef TARGET_HANDHELD
    /* The backstop mixer thread drains D_800EA3AC under the mix lock, and the
     * OSMesgQueue shim is a plain read-modify-write, so this game-thread send
     * must take the same lock or a preemption mid-send loses/duplicates a batch
     * (a lost sequence_player_disable is an SFX that never stops). */
    { extern void port_audio_mix_lock(void), port_audio_mix_unlock(void);
      port_audio_mix_lock(); osSendMesg(D_800EA3AC, thing, 0); port_audio_mix_unlock(); }
#else
    osSendMesg(D_800EA3AC, thing, 0);
#endif
    D_800EA3A4[0] = D_800EA3A0[0];
}

void func_800CBCB0(u32 arg0) {
    struct EuAudioCmd* cmd;
    struct SequencePlayer* seqPlayer;
    struct SequenceChannel* chan;
    u8 end = arg0 & 0xff;
    u8 i = (arg0 >> 8) & 0xff;

    for (;;) {
        if (i == end) {
            break;
        }
        cmd = &sAudioCmd[i++ & 0xff];
        if ((cmd->u.s.op & 0xf0) == 0xf0) {
            eu_process_audio_cmd(cmd);
            goto why;
        }

        if (cmd->u.s.bankId < SEQUENCE_PLAYERS) {
            seqPlayer = &gSequencePlayers[cmd->u.s.bankId];
            if ((cmd->u.s.op & 0x80) != 0) {
                eu_process_audio_cmd(cmd);
            } else if ((cmd->u.s.op & 0x40) != 0) {
                switch (cmd->u.s.op) {
                    case 0x41:
                        seqPlayer->fadeVolumeScale = cmd->u2.as_f32;
                        seqPlayer->recalculateVolume = true;
                        break;

                    case 0x47:
                        seqPlayer->tempo = cmd->u2.as_s32 * TATUMS_PER_BEAT;
                        break;

                    case 0x48:
                        seqPlayer->transposition = cmd->u2.as_s8;
                        break;

                    case 0x46:
                        seqPlayer->seqVariationEu[cmd->u.s.arg3] = cmd->u2.as_s8;
                        break;
                }
            } else if (seqPlayer->enabled != false && cmd->u.s.arg2 < 0x10) {
                chan = seqPlayer->channels[cmd->u.s.arg2];
                if (IS_SEQUENCE_CHANNEL_VALID(chan)) {
                    switch (cmd->u.s.op) {
                        case 1:
                            chan->volumeScale = cmd->u2.as_f32;
                            chan->changes.as_bitfields.volume = true;
                            break;
                        case 2:
                            chan->volume = cmd->u2.as_f32;
                            chan->changes.as_bitfields.volume = true;
                            break;
                        case 3:
                            chan->newPan = cmd->u2.as_s8;
                            chan->changes.as_bitfields.pan = true;
                            break;
                        case 4:
                            chan->freqScale = cmd->u2.as_f32;
                            chan->changes.as_bitfields.freqScale = true;
                            break;
                        case 5:
                            chan->reverbVol = cmd->u2.as_s8;
                            break;
                        case 6:
                            if (cmd->u.s.arg3 < 8) {
                                chan->soundScriptIO[cmd->u.s.arg3] = cmd->u2.as_s8;
                            }
                            break;
                        case 8:
                            chan->stopSomething2 = cmd->u2.as_s8;
                    }
                }
            }
        }

    why:
        cmd->u.s.op = 0;
    }
}

void port_eu_init() {
    port_eu_init_queues();
}
