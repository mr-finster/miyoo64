/**
 * SDL audio out: replaces the N64 AI (the MK64 kit's backend).
 *
 * The game's audio engine (AudioThread_CreateTask, sf64/port_audio.c) hands the
 * AI a variable-length buffer of stereo s16 samples at the audio-session rate
 * (32000 Hz in every Star Fox spec) via osAiSetNextBuffer(), and sizes the next
 * mix from osAiGetLength().  Samples are resampled (linear, stateful) to a
 * fixed 32 kHz if the session rate differs, and pushed into a single-producer/
 * single-consumer FIFO that the SDL audio callback drains.  The legacy
 * SDL_OpenAudio() API is used so the same file builds against SDL 2 (desktop)
 * and SDL 1.2 (handheld).
 *
 * SF64_VCLOCK=1 (tests) opens no device: a virtual AI plays one VI's worth of
 * samples per audio tick (port_audio_out_virtual_vi), so osAiGetLength -- and
 * with it every task's length and the game's audio-driven logic -- is the same
 * on every run.
 */
#include <PR/ultratypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(HANDHELD)
#include <SDL/SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#include "../port.h"
#include "savestate.h"
#include "hh_declick.h"

#if defined(MIYOO)
/* The Miyoo's SDL builds differ in what they grant: the obtained rate wins (port_audio_out_init; DKR's). */
static u32 sOutFreq SAVESTATE_EXCLUDE = 32000;
#define OUT_FREQ sOutFreq
#define OUT_FREQ_INIT 32000
#else
#define OUT_FREQ 32000
#define OUT_FREQ_INIT OUT_FREQ
#endif
/* Resampler state (MK64's 26.8 kHz mix ran 19% fast played straight); Star
 * Fox's 32 kHz passes through. */
static u32 sInRate = OUT_FREQ_INIT;
static s16 sPrevL, sPrevR;   // last input sample of the previous push
static u32 sPhase = 0x10000; // 16.16 position of the next output sample, measured from sPrev
static u32 sStep = 0x10000;  // input samples per output sample, 16.16

void port_audio_out_set_rate(u32 freq) {
    if (freq < 8000 || freq > 48000) freq = OUT_FREQ;
    /* The game re-asserts the session rate at every mode/track change (race
     * start included) with the SAME value. Resetting the resampler phase and
     * previous-sample history mid-stream on an unchanged rate injects a
     * discontinuity -- the audible "skip" heard just before a race. Only
     * reset when the rate actually changes. */
    if (freq == sInRate) return;
    port_log("audio rate: %u Hz session -> %d Hz out\n", freq, OUT_FREQ);
    sInRate = freq;
    sStep = (u32) (((u64) freq << 16) / OUT_FREQ);
    sPhase = 0x10000;
    sPrevL = sPrevR = 0;
}

#define OUT_BLOCK 512                 // SDL callback granularity (samples)
#define FIFO_SAMPLES (OUT_BLOCK * 16) // ring capacity in stereo samples

/* The ring is shared with the SDL callback thread -- exclude it from
 * savestates so a load never rewrites indices out from under the live
 * device. The game's audio engine (in the saved region) re-drives it. */
static s16 sFifo[FIFO_SAMPLES * 2] SAVESTATE_EXCLUDE;
static volatile u32 sFifoRead SAVESTATE_EXCLUDE;  // total samples read (callback thread only)
static volatile u32 sFifoWrite SAVESTATE_EXCLUDE; // total samples written (game thread only)
static int sMuted SAVESTATE_EXCLUDE;            /* menu settings, not game state */
static int sOpened SAVESTATE_EXCLUDE;
static u32 sObtainedSamples SAVESTATE_EXCLUDE = OUT_BLOCK;
static volatile u32 sUnderruns SAVESTATE_EXCLUDE;
static int sVolume SAVESTATE_EXCLUDE = 255; // 0..255, driven by the OS volume menu

static u32 fifo_count(void) {
    return sFifoWrite - sFifoRead;
}

/* statefix B (handheld-kit k1.8 586b125, hh_declick.h): no click after the menu or a load. After any gap
 * (the first sound, a dry ring, a load's flush) the card waits until the ring holds DECLICK_CUSHION
 * frames again (or the ring has stopped growing for 100 ms), then fades in over 10 ms; muting (the
 * menu), a load's hold and a ring that runs dry fade out over 3 ms (a dry ring: its last frames; a ring
 * a buffer empties exactly counts as dry). At full gain the samples pass untouched: steady play is bit
 * for bit as before. The cushion: port_audio_frame tops the queue up to CUSHION_OUT (3200 frames,
 * 100 ms; the ring ~2950 after the device's buffer) at every frame and the game sizes its tasks to keep
 * it there, so the ring at a callback in steady play stays well above 1600 (the PERF line's "ring": the
 * least it held at a callback that played), and the game's first frame after a gap mixes up to six
 * ticks (about 3300 frames) while below it. 1600 frames (50 ms) is half of that.
 * SF64_DECLICK=0: as before (a hard mute, no fades, no flush at a load). */
#define DECLICK_CUSHION 1600u
static HhDeclick sDk SAVESTATE_EXCLUDE;
static int sDeclick SAVESTATE_EXCLUDE = 1;
static volatile u32 sRingMin SAVESTATE_EXCLUDE = 0xFFFFFFFFu; /* least frames queued at a callback that played */

/* Consumer: SDL's audio thread. On underrun, silence keeps the stream timed. */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    s16 *out = (s16 *)stream;
    u32 want = (u32)len / 4; // stereo frames
    u32 have = fifo_count();
    if (have == 0) sUnderruns++; /* as before: a callback that found the ring empty */
    if (sDeclick && !hhd_ready(&sDk, have, want)) { /* after a gap: silence until the cushion is back */
        memset(out, 0, (size_t)want * 4);
        return;
    }
    if (have < sRingMin) sRingMin = have;
    u32 n = have < want ? have : want;
    u32 base = sFifoRead % FIFO_SAMPLES;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (base + i) % FIFO_SAMPLES;
        if (sMuted && !sDeclick) {
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
        } else {
            out[i * 2]     = (s16)((sFifo[idx * 2]     * sVolume) >> 8);
            out[i * 2 + 1] = (s16)((sFifo[idx * 2 + 1] * sVolume) >> 8);
        }
    }
    sFifoRead += n;
    if (n < want) {
        memset(&out[n * 2], 0, (want - n) * 4);
    }
    if (sDeclick) hhd_apply(&sDk, out, n, want, sMuted || sDk.hold); /* the fades (muted: down to silence) */
}

u32 port_audio_out_underruns(void) {
    return sUnderruns;
}

/* The PERF line: the least the ring held at a callback that played since the last call (0: none
 * played), then reset; and the times the sound came back after a gap (hh_declick.h). */
u32 port_audio_out_take_ring_min(void) {
    const u32 m = sRingMin;
    sRingMin = 0xFFFFFFFFu;
    return m == 0xFFFFFFFFu ? 0 : m;
}
u32 port_audio_out_gaps(void) {
    return sDk.gaps;
}

/* The kit's savestate.c around a load (k1.8): 1 before it, the card fades out and holds; 0 after it
 * (the mix lock already released: the callback never takes it), once a whole callback has run under
 * the hold (hhd_settle, at most 50 ms: a load from the page cache takes under a millisecond, and
 * dropping the queue under a buffer still at full gain was a step), the old timeline's queue is
 * dropped with the callback locked out, and the restored game's sound fades in once the cushion is
 * back. The backstop may already have mixed for the restored game; dropping that is a few ms of a
 * sound that starts from silence anyway. */
static void declick_sleep(uint32_t ms) { SDL_Delay(ms); }
void port_audio_out_gap(int begin) {
    if (!sDeclick || !sOpened) return;
    if (begin) {
        hhd_hold(&sDk);
        return;
    }
    hhd_settle(&sDk, declick_sleep, 50);
    SDL_LockAudio();
    sFifoRead = sFifoWrite;
    hhd_flush(&sDk);
    sDk.hold = 0;
    SDL_UnlockAudio();
}

/* ---- Virtual AI (SF64_VCLOCK=1) -------------------------------------------
 * No device: the queue is a count the game clock drains, one VI's worth
 * (session rate / 60, fractions carried) per audio tick. */
static int sVirtual SAVESTATE_EXCLUDE = -1;
static u32 sVirtQueued; /* session-rate samples (saved: game-clock state, no other thread) */
static u32 sVirtFrac;   /* sixtieths of a sample */

int port_audio_out_virtual(void) {
    if (sVirtual < 0) {
        const char *e = getenv("SF64_VCLOCK");
        sVirtual = e != NULL && e[0] == '1';
    }
    return sVirtual;
}

void port_audio_out_virtual_vi(void) {
    u32 n;
    sVirtFrac += sInRate;
    n = sVirtFrac / 60;
    sVirtFrac %= 60;
    sVirtQueued = sVirtQueued > n ? sVirtQueued - n : 0;
}

/* ---- Backstop mixer thread ----------------------------------------------
 * The N64 mixed audio on its own thread, so a slow frame never starved it.
 * The game thread mixes here (port_audio_frame), and the long stalls are
 * frames that take long to render (device round 2: 80-170 ms in r3d under
 * full-screen translucent effects). While the game thread renders or waits
 * for its deadline (port_audio_render(1)..(0)) it touches no sound state, so
 * this thread may run the engine itself if the ring falls to LOW_WATER. The
 * mix lock keeps the two apart: port_audio_render(0) waits for a pass in
 * progress. Game code is never interrupted by the sound engine this way,
 * unlike a free-running thread (MK64's), which the N64 never did either. */
extern void port_audio_tick_pair(void);
static SDL_mutex *sMixLock SAVESTATE_EXCLUDE;
static volatile int sRendering SAVESTATE_EXCLUDE;
static SDL_Thread *sBackstopThread SAVESTATE_EXCLUDE;

void port_audio_mix_lock(void)   { if (sMixLock) SDL_mutexP(sMixLock); }
void port_audio_mix_unlock(void) { if (sMixLock) SDL_mutexV(sMixLock); }

/* ~47 ms: the game keeps the ring near its ~100 ms cushion, so it only falls
 * this low when a frame has stalled well past its deadline. */
#define LOW_WATER_OUT 1500u
static volatile u32 sThreadMixes SAVESTATE_EXCLUDE; /* PERF: how often the backstop had to mix */
u32 port_audio_thread_mixes(void) { return sThreadMixes; }

static volatile int sBackstop SAVESTATE_EXCLUDE; /* the game's audio engine is initialised */
void port_audio_backstop_enable(int on) { sBackstop = on; }

void port_audio_render(int on) {
    if (on) {
        sRendering = 1;
    } else {
        port_audio_mix_lock(); /* a backstop pass in progress finishes first */
        sRendering = 0;
        port_audio_mix_unlock();
    }
}

static int backstop_thread(void *arg) {
    (void)arg;
    for (;;) {
        /* Muted = the handheld menu is up (it runs inside the frame's present):
         * the game is paused, so its music must not run on in the background. */
        if (sOpened && sBackstop && sRendering && !sMuted && fifo_count() < LOW_WATER_OUT) {
            port_audio_mix_lock();
            if (sRendering) { /* re-checked: the game thread may have left meanwhile */
                port_audio_tick_pair();
                sThreadMixes++;
            }
            port_audio_mix_unlock();
        } else {
            SDL_Delay(4);
        }
    }
    return 0;
}

void port_audio_out_init(void) {
    SDL_AudioSpec want, got;
    if (port_audio_out_virtual()) {
        port_log("audio: virtual AI (SF64_VCLOCK), no device\n");
        return;
    }
#if defined(MIYOO)
    /* Video before audio (main.c opens the panel first): the Mini's dsp driver crashes in SDL_AudioInit without the
     * MI system init that SDL's video init does. A guard, in case a path ever comes here first. */
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        port_log_always("audio: SDL video init failed: %s\n", SDL_GetError());
#endif
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        port_log("audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return;
    }
    memset(&want, 0, sizeof(want));
    want.freq = OUT_FREQ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = OUT_BLOCK;
    want.callback = audio_callback;
    if (SDL_OpenAudio(&want, &got) != 0) {
        port_log("audio: SDL_OpenAudio failed: %s\n", SDL_GetError());
        return;
    }
    /* The obtained spec is authoritative; 32 kHz s16 stereo is universally
     * granted on desktop, and handheld's driver takes it too. If the rate were
     * renegotiated the resampler target would need updating — flag it. */
    if (got.freq != OUT_FREQ || got.channels != 2 || got.format != AUDIO_S16SYS) {
        port_log("audio: obtained %d Hz ch %d fmt %04X (wanted %d/2/%04X)\n",
                 got.freq, got.channels, got.format, OUT_FREQ, AUDIO_S16SYS);
    }
#if defined(MIYOO)
    if (got.freq != (int) OUT_FREQ && got.freq >= 8000 && got.freq <= 96000) {
        sOutFreq = (u32) got.freq;
        sStep = (u32) (((u64) sInRate << 16) / OUT_FREQ);
        sPhase = 0x10000;
        port_log_always("audio: resampling to the obtained %d Hz\n", got.freq);
    }
#endif
    sObtainedSamples = got.samples ? got.samples : OUT_BLOCK;
    sFifoRead = sFifoWrite = 0;
    {
        const char *e = getenv("SF64_DECLICK");
        sDeclick = !(e && e[0] == '0');
        hhd_init(&sDk, OUT_FREQ, DECLICK_CUSHION);
    }
    sOpened = 1;
    SDL_PauseAudio(0);
    port_log("audio: SDL out %d Hz, %u-sample buffer\n", got.freq, sObtainedSamples);
    sMixLock = SDL_CreateMutex();
#if SDL_MAJOR_VERSION >= 2
    sBackstopThread = SDL_CreateThread(backstop_thread, "sf64_backstop", NULL);
#else
    sBackstopThread = SDL_CreateThread(backstop_thread, NULL);
#endif
    if (!sBackstopThread) port_log("audio: backstop thread failed: %s\n", SDL_GetError());
}

/* ~100 ms of output (3200 samples) kept queued beyond what the game sees. */
#define CUSHION_OUT 3200u

/* Bytes of audio still queued (FIFO + device buffer est.), game sample units. */
u32 port_audio_out_queued_bytes(void) {
    u32 queued;
    if (port_audio_out_virtual()) {
        return sVirtQueued * 4;
    }
    queued = fifo_count();
    if (sOpened) queued += sObtainedSamples / 2; // avg samples in flight at the device
    if (sInRate != OUT_FREQ) {
        queued = (u32) (((u64) queued * sInRate) / OUT_FREQ); // in the game's sample units
    }
    return queued * 4;
}

/* osAiGetLength(): what is "left in the AI" -- the queue minus the cushion
 * (f-zero-x 47fc96b). The game sizes each task from it (1056..1104
 * samples, aiming at 0x80 left over), so its own feedback keeps the queue near
 * the cushion and the music on the VI clock, as on the N64; reporting the
 * whole queue pinned every task to the minimum and the refill made up the
 * difference with extra sequencer ticks. */
u32 port_audio_out_ai_length(void) {
    u32 queued;
    if (port_audio_out_virtual()) {
        return sVirtQueued * 4;
    }
    queued = port_audio_out_queued_bytes() / 4;
    return queued > CUSHION_OUT ? (queued - CUSHION_OUT) * 4 : 0;
}

/* port_audio_frame's refill test: the queue has fallen below the cushion. */
int port_audio_out_below_cushion(void) {
    return !port_audio_out_virtual() && port_audio_out_queued_bytes() < CUSHION_OUT * 4;
}

static void fifo_push(const s16 *samples, u32 n) {
    u32 i;
    u32 space = FIFO_SAMPLES - fifo_count();
    if (n > space) {
        n = space; // drop what does not fit
    }
    for (i = 0; i < n; i++) {
        u32 idx = (sFifoWrite + i) % FIFO_SAMPLES;
        sFifo[idx * 2] = samples[i * 2];
        sFifo[idx * 2 + 1] = samples[i * 2 + 1];
    }
    sFifoWrite += n;
}

/* osAiSetNextBuffer(): queue `bytes` of stereo s16 samples. */
void port_audio_out_push(const s16 *samples, u32 bytes) {
    u32 n = bytes / 4;
    if (n == 0) return;
    { /* SF64_AUDIO_DUMP=<path>: raw s16le stereo at the game's session rate
       * (see the "audio rate" log line), pre-resample/pre-drop, so it muxes
       * 1:1 with the frame dumps regardless of wall-clock speed. */
        static FILE *sDump SAVESTATE_EXCLUDE; static int sDumpInit SAVESTATE_EXCLUDE;
        if (!sDumpInit) {
            sDumpInit = 1;
            const char *p = getenv("SF64_AUDIO_DUMP");
            if (p) sDump = fopen(p, "wb");
        }
        if (sDump) fwrite(samples, 1, bytes, sDump);
    }
    if (port_audio_out_virtual()) {
        sVirtQueued += n;
        return;
    }
    if (sInRate == OUT_FREQ) {
        fifo_push(samples, n);
        return;
    }
    {
        // Linear interpolation between x[i-1] and x[i], x[-1] = previous push's last sample.
        s16 tmp[256 * 2];
        u32 k = 0;
        for (;;) {
            u32 idx = sPhase >> 16;
            s32 frac, a, b;
            if (idx >= n) {
                break; // needs input that has not arrived yet
            }
            frac = (s32) (sPhase & 0xFFFF);
            a = idx == 0 ? sPrevL : samples[(idx - 1) * 2];
            b = samples[idx * 2];
            tmp[k * 2] = (s16) (a + (((b - a) * frac) >> 16));
            a = idx == 0 ? sPrevR : samples[(idx - 1) * 2 + 1];
            b = samples[idx * 2 + 1];
            tmp[k * 2 + 1] = (s16) (a + (((b - a) * frac) >> 16));
            sPhase += sStep;
            if (++k == 256) {
                fifo_push(tmp, k);
                k = 0;
            }
        }
        if (k) {
            fifo_push(tmp, k);
        }
        sPhase -= n << 16;
        sPrevL = samples[(n - 1) * 2];
        sPrevR = samples[(n - 1) * 2 + 1];
    }
}

void port_audio_out_set_muted(int muted) {
    sMuted = muted;
}

void port_audio_out_set_volume(int vol) { // 0..255, the OS menu hook
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    sVolume = vol;
}
