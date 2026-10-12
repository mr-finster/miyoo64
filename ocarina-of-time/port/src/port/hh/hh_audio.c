/* Audio backend: the N64 model on a CPU.
 *
 * The N64 ran OoT's audio thread once per retrace: AudioThread_Update() builds an
 * aspMain command list (the unmodified decomp synthesis path -- we do NOT define
 * TARGET_PSP) that the RSP executed into an AI buffer, and the next update handed
 * the previous buffer to the DAC with osAiSetNextBuffer(). Here a pthread does
 * the same at 60 Hz: the command list runs on oot-PSP's C mixer
 * (OotPspMixer_ExecuteCommandList), osAiSetNextBuffer() (src/audio/internal/os.c)
 * lands in OotPspAudioBackend_Queue(), which appends to a ring the SDL callback
 * drains, and osAiGetLength() reports the ring fill so the game sizes the next
 * buffer as it did against the AI.
 *
 * Game-thread audio calls reach the audio thread through the port's
 * osSendMesg/osRecvMesg (one global lock in hh_libultra.c), as on the N64.
 *
 * Save states (handheld-kit): each update runs under the mix lock, which the kit holds while
 * it captures or restores game memory, so the audio thread never touches the audio heap
 * mid-snapshot; the handheld menu mutes the output, which also pauses the updates (the music
 * picks up where it was when the menu closes).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "ultra64.h"
#include "audio.h"
#include "oot_psp_mixer.h"
#include "hh_game.h"
#include "hh_declick.h"

s32 DmaMgr_AudioDmaHandler(OSPiHandle* pihandle, OSIoMesg* mb, s32 direction);
void AudioThread_InitExternalPool(void* ramAddr, u32 size);
void Audio_Init(void);
void Audio_InitSound(void);
AudioTask* AudioThread_Update(void);

#define HH_AUDIO_RATE 32000
#define HH_RING_FRAMES 16384 /* stereo frames (~0.5 s) */

static s16 sRing[HH_RING_FRAMES * 2];
static volatile u32 sRingRead; /* frames, monotonically increasing */
static volatile u32 sRingWrite;
static pthread_mutex_t sRingLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sMixLock = PTHREAD_MUTEX_INITIALIZER; /* held for a whole update */
static volatile int sMuted; /* the handheld menu is up: silence, and no updates */
static volatile int sExiting; /* HhAudio_BeginExit: silence, but the updates run */
static pthread_t sThread;
static volatile int sRunning;
static int sSync; /* OOT_AUDIO_SYNC: updates driven per game frame */
static u32 sSyncFrac; /* OOT_AUDIO_SYNC's virtual DAC: the part-sample left over */
static int sInitialised;
static u32 sUnderruns;
static FILE* sDump;
static int sOpened; /* the SDL output is open (its callback runs) */

/* statefix B (handheld-kit k1.8, hh_declick.h): no click after the menu or a load. After any gap (the first
 * sound, a dry ring, a load's flush) the card waits until the ring holds one callback's worth again (the
 * cushion), then fades in over 10 ms; muting (the menu), a load's hold and a ring that runs dry fade out over
 * 3 ms. Once faded out for the menu or a load the callback takes nothing, so what the ring holds still plays
 * when the menu closes (as before). A load used to play on the old timeline's queued sound and then the
 * restored game's, and the menu cut the card from one sample to the next. The cushion is the SDL buffer (512
 * frames on the device), below what the game keeps queued: each update queues one VI of samples, 528-560
 * frames whatever the fill (thread.c clamps an AI buffer to the target +/- 16), so the ring is back above
 * the cushion one update (~17 ms) after a gap. At full gain the samples pass untouched. OOT_DECLICK=0: as
 * before. */
static HhDeclick sDk;
static int sDeclick = 1;
/* The AUDIO line's "card" part: the fewest frames a playing callback found in the ring over the 10 s, how many
 * found exactly one buffer (hh_declick.h fades that out as a ring about to run dry), and the gaps the sound came
 * back from (hh_declick.h's restarts: the menu, a load, a dry ring). */
static volatile u32 sCardMin = 0xFFFFFFFFu, sCardExact;
static u32 sLogGapsSeen;

/* 64 KB, the external pool oot-PSP gives the audio thread: the audio engine allocates from
 * it, so it is game state (hh_game.h). */
static u8 sAudioExternalPool[0x10000] __attribute__((aligned(16))) HH_GS_BSS;

void port_log_always(const char* fmt, ...);
static u32 sDiagUpdates, sDiagTasks, sDiagCmds, sDiagQueued, sDiagPeak;
/* Every 10 s into console.log (the device has no terminal): proof the audio
 * thread runs and the mixer gets work. The v15 device profile showed almost no
 * mixer samples where v9 had ~24%, and an SDL open failure only went to
 * stdout, which a launcher start drops. */
static u32 sLogTasks, sLogCmds, sLogUnderrunsSeen;

static u32 HhRing_Fill(void) {
    return sRingWrite - sRingRead;
}

#if defined(MIYOO)
static FILE* sLenLog; /* OOT_AUDIO_LENLOG=<file> (tests): each update's buffer length in frames, one a line */
#endif
s32 OotPspAudioBackend_Queue(const void* buf, u32 size) {
    const s16* in = buf;
    u32 frames = size / 4;
    u32 i;
#if defined(MIYOO)
    if (sLenLog != NULL) {
        fprintf(sLenLog, "%u\n", (unsigned)frames);
    }
#endif

    if (sDump != NULL) {
        fwrite(buf, 1, size, sDump);
    }
    sDiagQueued += size;
    for (i = 0; i < size / 2; i += 7) {
        u32 v = (u32)(in[i] < 0 ? -in[i] : in[i]);
        if (v > sDiagPeak) {
            sDiagPeak = v;
        }
    }
    pthread_mutex_lock(&sRingLock);
    if (HhRing_Fill() + frames > HH_RING_FRAMES) {
        frames = HH_RING_FRAMES - HhRing_Fill();
    }
    for (i = 0; i < frames; i++) {
        u32 w = (sRingWrite + i) % HH_RING_FRAMES;

        sRing[w * 2] = in[i * 2];
        sRing[w * 2 + 1] = in[i * 2 + 1];
    }
    sRingWrite += frames;
    pthread_mutex_unlock(&sRingLock);
    return 0;
}

s32 OotPspAudioBackend_SetFrequency(u32 frequency) {
    (void)frequency;
    return HH_AUDIO_RATE;
}

#if defined(MIYOO)
/* OOT_AUDIOFILL (debug.cfg audiofill=, default on; the Miyoo Mini, 2026-10-09). OoT sizes each update's buffer so
 * that what the AI still holds plus the new buffer is one frame plus EXTRA_BUFFERED_AI_SAMPLES_TARGET (0x80). Told
 * the whole ring, it leaves the Mini's SDL, which drains a whole callback at a time (512 frames every 16 ms), a
 * margin of 48 frames (1.5 ms): the lowest fill a callback finds is 560 (OOT_AUDIO_SIMCB, below). An update that
 * runs a few ms late leaves a callback short -- an underrun, a gap of silence (the device's +23..+32 per 10 s).
 * Here the game is told the fill less one callback (the size SDL granted), so it keeps that much more in the ring
 * (+16 ms latency): 0 underruns with updates up to 14 ms late in the simulation. The synthesis is the game's own;
 * only the per-update lengths it picks (clamped to 528..560 either way) follow the reported fill, as they always
 * did. audiofill=0: the whole fill, as before. */
static u32 sCbFrames;      /* frames a callback takes (SDL's granted samples); 0 without audio */
static int sAudioFill = 1; /* OOT_AUDIOFILL */
#endif
/* Bytes still to play, like the AI's remaining DMA length. */
u32 OotPspAudioBackend_GetLength(void) {
#if defined(MIYOO)
    if (sAudioFill) {
        const u32 fill = HhRing_Fill();
        return (fill > sCbFrames ? fill - sCbFrames : 0) * 4;
    }
#endif
    return HhRing_Fill() * 4;
}

s32 OotPspAudioBackend_NeedsRefillUrgently(void) {
    return HhRing_Fill() < 1024;
}

s32 OotPspAudioBackend_NeedsRefillDuringIo(void) {
    return HhRing_Fill() < 2048;
}

static void HhAudio_Callback(void* user, Uint8* stream, int len) {
    s16* out = (s16*)stream;
    u32 want = (u32)len / 4;
    u32 have;
    u32 i;

    (void)user;
    if ((sMuted && !sDeclick) || sExiting || sSync) { /* the menu is up (what the ring holds plays when it
                                        * closes), the game is quitting, or OOT_AUDIO_SYNC's virtual DAC
                                        * drains the ring instead */
        memset(stream, 0, (size_t)len);
        return;
    }
    pthread_mutex_lock(&sRingLock);
    have = HhRing_Fill();
    if (sDeclick && (!hhd_ready(&sDk, have, want) || ((sMuted || sDk.hold) && sDk.gain == 0))) {
        /* after a gap, waiting for the cushion; or faded out for the menu or a load: silence, the ring kept */
        pthread_mutex_unlock(&sRingLock);
        memset(stream, 0, (size_t)len);
        return;
    }
    if (have < want) {
        sUnderruns++;
    }
    if (have < sCardMin) {
        sCardMin = have;
    }
    sCardExact += have == want;
    for (i = 0; i < want; i++) {
        if (i < have) {
            u32 r = (sRingRead + i) % HH_RING_FRAMES;

            out[i * 2] = sRing[r * 2];
            out[i * 2 + 1] = sRing[r * 2 + 1];
        } else {
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
        }
    }
    sRingRead += (have < want) ? have : want;
    if (sDeclick) { /* the fades (muted or held: down to silence; a dry ring: its last frames) */
        hhd_apply(&sDk, out, have < want ? have : want, want, sMuted || sDk.hold);
    }
    pthread_mutex_unlock(&sRingLock);
}

/* The kit's savestate.c around a load (k1.8): 1 before it, the card fades out and holds; 0 after it, once a
 * whole callback has run under the hold (a load from the page cache takes under a millisecond: the fade out
 * must reach the card before the queue under it goes; at most 50 ms), the old timeline's queue is dropped
 * (the callback locked out; it never takes the mix lock) and the restored game's sound fades in once the
 * cushion is back. OOT_AUDIO_SYNC's card is silent (its virtual DAC empties the ring at the mix unlock). */
static void HhAudio_SleepMs(uint32_t ms) {
    SDL_Delay(ms);
}
void port_audio_out_gap(int begin) {
    if (!sDeclick || !sOpened || sSync) {
        return;
    }
    if (begin) {
        hhd_hold(&sDk);
        return;
    }
    hhd_settle(&sDk, HhAudio_SleepMs, 50);
    SDL_LockAudio();
    pthread_mutex_lock(&sRingLock);
    sRingRead = sRingWrite;
    pthread_mutex_unlock(&sRingLock);
    hhd_flush(&sDk);
    sDk.hold = 0;
    SDL_UnlockAudio();
}


static uint64_t sMixCpuNs; /* the mixer's CPU time since the last AUDIO line */
static uint64_t sUpdCpuNs; /* AudioThread_Update's (the game's sequence and synthesis code), likewise */

/* handheld-kit's hooks: save states hold the mix lock around a snapshot or a restore. There
 * is no backstop mixer here (the audio thread is the only one), so parking it is a no-op. */
void port_audio_mix_lock(void) {
    pthread_mutex_lock(&sMixLock);
}
void port_audio_mix_unlock(void) {
    if (sSync) {
        /* OOT_AUDIO_SYNC (tests): after a save or a load the ring starts empty, so the process
         * that saved and the one that loaded see the same fill from here on and make the same
         * audio (qstate.sh compares the dumps from this mark). */
        pthread_mutex_lock(&sRingLock);
        sRingRead = sRingWrite;
        pthread_mutex_unlock(&sRingLock);
        sSyncFrac = 0;
        if (sDump != NULL) {
            fflush(sDump);
            port_log_always("AUDIO savestate mark %ld\n", ftell(sDump));
        }
    }
    pthread_mutex_unlock(&sMixLock);
}
void port_audio_backstop_enable(int on) {
    (void)on;
}
void port_audio_out_set_muted(int muted) {
    sMuted = muted;
}

static void HhAudio_RunOneUpdateLocked(void);
static void HhAudio_RunOneUpdate(void) {
    pthread_mutex_lock(&sMixLock);
    HhAudio_RunOneUpdateLocked();
    pthread_mutex_unlock(&sMixLock);
}

static void HhAudio_RunOneUpdateLocked(void) {
    AudioTask* task;

    if (sSync) {
        /* OOT_AUDIO_SYNC: a virtual DAC. One VI's worth of samples (32000 / 60) leaves the
         * ring per update, so the fill the game sizes its next buffer from (osAiGetLength) is
         * the same in every run, not whatever the host's audio thread drained. */
        u32 n = (HH_AUDIO_RATE + sSyncFrac) / 60;

        sSyncFrac = (HH_AUDIO_RATE + sSyncFrac) % 60;
        pthread_mutex_lock(&sRingLock);
        if (n > HhRing_Fill()) {
            n = HhRing_Fill();
        }
        sRingRead += n;
        pthread_mutex_unlock(&sRingLock);
    }
    {   /* the game's own audio update (sequences, notes, the command list): its CPU time for the AUDIO line */
        struct timespec u0, u1;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &u0);
        task = AudioThread_Update();
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &u1);
        sUpdCpuNs += (uint64_t)(u1.tv_sec - u0.tv_sec) * 1000000000u + (uint64_t)u1.tv_nsec - (uint64_t)u0.tv_nsec;
    }

    sDiagUpdates++;
    if (task != NULL && task->task.t.type == M_AUDTASK && task->task.t.data_ptr != NULL) {
        sDiagTasks++;
        sDiagCmds += task->task.t.data_size / sizeof(Acmd);
        sLogTasks++;
        sLogCmds += task->task.t.data_size / sizeof(Acmd);
        struct timespec a, b;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
        OotPspMixer_ExecuteCommandList((const Acmd*)task->task.t.data_ptr,
                                       (s32)(task->task.t.data_size / sizeof(Acmd)));
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
        sMixCpuNs += (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000u + (uint64_t)b.tv_nsec - (uint64_t)a.tv_nsec;
    }
    if (sDiagUpdates % 600 == 0) {
        /* mix: the mixer's own CPU time (this thread only), ms per second over the 10 s since the
         * last line; mixneon=alt tags which kernels ran (hh_mixer.c) */
        extern int HhMixer_NeonAltStep(void);
        const int neon = HhMixer_NeonAltStep();
        port_log_always("AUDIO upd %u tasks %u cmds %u underruns +%u fill %u seq %d%d%d%d | mix %.2f ms/s%s | update %.2f ms/s"
                        " | card min %d exact %u gaps %u\n", sDiagUpdates,
                        sLogTasks, sLogCmds, sUnderruns - sLogUnderrunsSeen, HhRing_Fill(), gAudioCtx.seqPlayers[0].enabled,
                        gAudioCtx.seqPlayers[1].enabled, gAudioCtx.seqPlayers[2].enabled, gAudioCtx.seqPlayers[3].enabled,
                        sMixCpuNs / 1e7, neon < 0 ? "" : neon ? " neon 1" : " neon 0", sUpdCpuNs / 1e7,
                        sCardMin == 0xFFFFFFFFu ? -1 : (int)sCardMin, sCardExact, sDk.gaps - sLogGapsSeen);
        sCardMin = 0xFFFFFFFFu;
        sCardExact = 0;
        sLogGapsSeen = sDk.gaps;
        sMixCpuNs = 0;
        sUpdCpuNs = 0;
        sLogTasks = sLogCmds = 0;
        sLogUnderrunsSeen = sUnderruns;
    }
    if (getenv("OOT_AUDIO_DIAG") != NULL && sDiagUpdates % 60 == 0) {
        port_log_always("AUDIO upd %u tasks %u cmds/task %u queued %u peak %u reset %d spec %d players %d%d%d%d "
                        "wpos %u rpos %u q %d/%d fin %d\n",
                        sDiagUpdates, sDiagTasks, sDiagTasks ? sDiagCmds / sDiagTasks : 0, sDiagQueued, sDiagPeak,
                        gAudioCtx.resetStatus, gAudioCtx.specId, gAudioCtx.seqPlayers[0].enabled,
                        gAudioCtx.seqPlayers[1].enabled, gAudioCtx.seqPlayers[2].enabled,
                        gAudioCtx.seqPlayers[3].enabled, gAudioCtx.threadCmdWritePos, gAudioCtx.threadCmdReadPos,
                        gAudioCtx.threadCmdProcQueueP ? (int)gAudioCtx.threadCmdProcQueueP->validCount : -1,
                        gAudioCtx.threadCmdProcQueueP ? (int)gAudioCtx.threadCmdProcQueueP->msgCount : -1,
                        gAudioCtx.threadCmdQueueFinished);
        sDiagTasks = sDiagCmds = sDiagQueued = sDiagPeak = 0;
    }
}

static void* HhAudio_Thread(void* arg) {
    struct timespec next;
    const long periodNs = 16666667L;

    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (sRunning) {
        struct timespec now;

        if (!sMuted || sExiting) {
            HhAudio_RunOneUpdate();
        }
        next.tv_nsec += periodNs;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        /* More than 3 updates behind (the menu, a state being read from the card): start
         * the timeline again from now rather than running the missed updates in a burst,
         * which would skip the music ahead and overflow the ring. */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec) > 3 * periodNs) {
            next = now;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

#if defined(MIYOO)
/* OOT_AUDIO_SIMCB=1 (tests; qemu's dummy SDL driver drains at its own pace and qemu's threads keep no steady
 * period): the device's timing on a virtual clock, deterministic. Each game frame advances it 50 ms (a 20 fps
 * gameplay frame), in which the 60 Hz updates and the callbacks of SDL's granted size (512 frames: every 16 ms,
 * 62.5 Hz) run in time order from the game loop -- no audio thread, no SDL callback. The AUDIO lines count the
 * underruns as on the device. OOT_AUDIO_SIMOUT=<file>: what the simulated DAC played (s16 stereo), gaps and all.
 * OOT_AUDIO_SIMJIT=<ms>: each update runs late by 0..ms (a fixed pseudo-random sequence), as the audio thread wakes
 * late on a busy device. Every 625 callbacks (10 s) an "AUDIO sim" line: the lowest fill a callback found. */
static int sSimCb;
static uint64_t sSimNow, sSimUpd, sSimCbT; /* virtual ns: now, the next update (on time), the next callback */
static uint64_t sSimJitNs, sSimLate;       /* OOT_AUDIO_SIMJIT; this update's lateness */
static uint32_t sSimRand = 12345u, sSimCalls, sSimMin = 0xFFFFFFFFu;
static FILE* sSimOut;
static void HhAudio_RunOneUpdate(void);
static void HhAudio_SimFrame(void) {
    static s16 buf[4096 * 2];
    const u32 frames = sCbFrames > 0 && sCbFrames <= 4096 ? sCbFrames : 512;
    const uint64_t end = sSimNow + 50000000ull;

    for (;;) {
        const uint64_t tu = sSimUpd + sSimLate;
        const uint64_t t = tu <= sSimCbT ? tu : sSimCbT;
        if (t >= end) {
            break;
        }
        if (tu <= sSimCbT) {
            HhAudio_RunOneUpdate();
            sSimUpd += 1000000000ull / 60;
            sSimRand = sSimRand * 1103515245u + 12345u;
            sSimLate = sSimJitNs ? (uint64_t)((sSimRand >> 8) % 65536u) * sSimJitNs / 65536u : 0;
        } else {
            const u32 fill = HhRing_Fill();
            if (fill < sSimMin) {
                sSimMin = fill;
            }
            if (++sSimCalls % 625 == 0) {
                port_log_always("AUDIO sim: %u callbacks, the lowest fill a callback found %u frames (one takes %u)\n",
                                sSimCalls, sSimMin, frames);
                sSimMin = 0xFFFFFFFFu;
            }
            HhAudio_Callback(NULL, (Uint8*)buf, (int)(frames * 4));
            if (sSimOut != NULL) {
                fwrite(buf, 4, frames, sSimOut);
            }
            sSimCbT += (uint64_t)frames * 1000000000ull / HH_AUDIO_RATE;
        }
    }
    sSimNow = end;
}
#endif

void OotPspAudio_Init(void) {
    SDL_AudioSpec want, have;
    const char* dump;

    if (sInitialised) {
        return;
    }
    sInitialised = 1;

    if ((dump = getenv("OOT_AUDIO_DUMP")) != NULL) {
        sDump = fopen(dump, "wb"); /* raw s16 stereo @ 32 kHz, exactly what the game produced */
    }
#if defined(MIYOO)
    if ((dump = getenv("OOT_AUDIO_LENLOG")) != NULL) {
        sLenLog = fopen(dump, "w");
    }
#endif

    AudioLoad_SetDmaHandler(DmaMgr_AudioDmaHandler);
    AudioThread_InitExternalPool(sAudioExternalPool, sizeof(sAudioExternalPool));
    Audio_Init();
    Audio_InitSound();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        memset(&want, 0, sizeof(want));
        want.freq = HH_AUDIO_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = HhAudio_Callback;
        if (SDL_OpenAudio(&want, &have) == 0) {
            const char* e = getenv("OOT_DECLICK");

            sDeclick = !(e != NULL && e[0] == '0');
            hhd_init(&sDk, HH_AUDIO_RATE, (uint32_t)have.samples); /* the cushion: one callback's worth */
            port_log_always("AUDIO open %d Hz fmt %04X ch %d samples %d%s\n", have.freq, (unsigned)have.format,
                            (int)have.channels, (int)have.samples, sDeclick ? "" : " (declick off)");
            sOpened = 1;
#if defined(MIYOO)
            {
                const char* e = getenv("OOT_AUDIOFILL");
                sCbFrames = have.samples;
                sAudioFill = !(e != NULL && e[0] == '0');
                port_log_always("AUDIO fill: %s (audiofill=%d: the game is told the ring's fill less %u frames)\n",
                                sAudioFill ? "one callback in reserve" : "the whole ring, as before", sAudioFill,
                                sAudioFill ? (unsigned)sCbFrames : 0u);
            }
            if (getenv("OOT_AUDIO_SIMCB") != NULL) {
                const char* o = getenv("OOT_AUDIO_SIMOUT");
                const char* j = getenv("OOT_AUDIO_SIMJIT");
                sSimCb = 1;
                sSimOut = o != NULL ? fopen(o, "wb") : NULL;
                sSimJitNs = j != NULL ? (uint64_t)(atof(j) * 1000000.0) : 0;
                port_log_always("AUDIO OOT_AUDIO_SIMCB: a simulated %u-frame callback every %.2f ms against 60 Hz updates, "
                                "on a virtual clock\n", (unsigned)sCbFrames, sCbFrames * 1000.0 / HH_AUDIO_RATE);
            } else
#endif
            SDL_PauseAudio(0);
        } else {
            port_log_always("AUDIO SDL_OpenAudio failed: %s\n", SDL_GetError());
        }
    } else {
        port_log_always("AUDIO SDL audio init failed: %s\n", SDL_GetError());
    }

    sRunning = 1;
    if (getenv("OOT_AUDIO_SYNC") != NULL) {
        /* profiling / A-B runs: no wall-clock audio thread; HhAudio_SyncFrame
         * runs this frame's updates from the main loop, so every build does
         * the same audio work per game frame (under qemu the thread's share
         * swung the instruction counts by 10%+). */
        sSync = 1;
        return;
    }
#if defined(MIYOO)
    if (sSimCb) {
        return; /* OOT_AUDIO_SIMCB: the game loop runs the updates and the callbacks (HhAudio_SimFrame) */
    }
#endif
    pthread_create(&sThread, NULL, HhAudio_Thread, NULL);
}

/* One update per VI of a 20 fps (3-VI) gameplay frame: same count every run. */
void HhAudio_SyncFrame(void) {
    int i;

#if defined(MIYOO)
    if (sSimCb) {
        HhAudio_SimFrame();
        return;
    }
#endif
    if (!sSync) {
        return;
    }
    for (i = 0; i < 3; i++) {
        HhAudio_RunOneUpdate();
    }
}

void OotPspAudio_Update(void) {
}

/* The way out (hh_main.c), before GameState_Destroy. EXIT GAME leaves the mixer muted, which also
 * stops the audio thread's updates, but the destroy still runs Audio_Update: a queued audio heap
 * reset (a scene or File Select load, the pause menu) then waits in AudioThread_ResetAudioHeap for
 * the audio thread to answer (osRecvMesg BLOCK), and the game froze on the last frame (the user's
 * device, 2026-10-01). From here the updates run again with the output silent. */
void HhAudio_BeginExit(void) {
    sExiting = 1;
}

/* After the game loop: the audio thread stops at its next tick (not joined: a thread caught in an
 * update must not hold the exit), then SDL_Quit (gfx_wm_shutdown) closes the device. */
void HhAudio_Shutdown(void) {
    sExiting = 1;
    sRunning = 0;
}

u32 HhAudio_Underruns(void) {
    return sUnderruns;
}

/* oot-PSP's little-endian audio-table handling (load.c, seqplayer.c, playback.c,
 * effects.c, audio.h -- enabled for TARGET_HANDHELD) notifies its Media Engine of
 * published asset ranges and state; there is no second processor here. */
void OotPspAudioSynth_WritebackMeState(void) {
}
void OotPspAudioSynth_MeInvalidateState(void) {
}
void OotPspAudioSynth_MeWritebackState(void) {
}
void OotPspAudioSynth_InvalidateMeState(void) {
}
void OotPspAudioSynth_PublishMeAssetRange(const void* address, u32 size) {
    (void)address;
    (void)size;
}
