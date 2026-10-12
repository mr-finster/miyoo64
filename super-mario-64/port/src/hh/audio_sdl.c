/**
 * SDL audio out: replaces the N64 AI (the MK64 port's kit, from beckerd's
 * PSP audio_out.c).
 *
 * SM64 US mixes stereo s16 at 32 kHz: port_sm64_audio_frame() (hh_main.c) runs
 * the game's create_next_audio_buffer() for one frame and pushes the result here.
 * Pushed samples go through a stateful linear resampler (a pass-through at 32 kHz;
 * MK64 needed it for 26.8 kHz) into a single-producer/single-consumer FIFO that
 * the SDL audio callback drains. The legacy SDL_OpenAudio() API is used so the
 * same file builds against SDL 2 (desktop) and SDL 1.2 (handheld).
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

#if defined(MIYOO)
/* The Miyoo's SDL builds differ in what they grant: the obtained rate wins (port_audio_out_init). */
static u32 sOutFreq SAVESTATE_EXCLUDE = 32000;
#define OUT_FREQ sOutFreq
#define OUT_FREQ_INIT 32000
#else
#define OUT_FREQ 32000
#define OUT_FREQ_INIT OUT_FREQ
#endif
/* Resampler state: playing the 26.8 kHz mix straight ran everything 19% fast
 * on PSP; same maths here. */
static u32 sInRate = OUT_FREQ_INIT;
static s16 sPrevL, sPrevR;   // last input sample of the previous push
static u32 sPhase = 0x10000; // 16.16 position of the next output sample, measured from sPrev
static u32 sStep = 0x10000;  // input samples per output sample, 16.16

void port_audio_out_set_rate(u32 freq) {
    if (freq < 8000 || freq > 48000) freq = OUT_FREQ_INIT;
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
#define FIFO_SAMPLES (OUT_BLOCK * 32) // ring capacity in stereo samples (32 blocks: room for the cushion, an audio
                                      // reset's frames at once (hh_main.c SM64_RESETNOWAIT) and a game frame; was 16)

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
static int sVolume SAVESTATE_EXCLUDE = 255; // 0..255, driven by the the handheld OS volume menu

static u32 fifo_count(void) {
    return sFifoWrite - sFifoRead;
}

/* SM64_AUDIO_OUTDUMP=<path> (test hook, 2026-10-06: the Snowman's Land yell recordings): raw s16le stereo at 32 kHz,
 * the samples the SDL callback hands the device (after the resampler, the ring and the volume), underrun silence left
 * out. While it is on, a full ring makes the game thread wait for room instead of dropping (a desktop run outpaces the
 * dummy device), so every mixed sample reaches the dump. */
static FILE *sOutDump SAVESTATE_EXCLUDE;

/* Consumer: SDL's audio thread. On underrun, silence keeps the stream timed. */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    s16 *out = (s16 *)stream;
    u32 want = (u32)len / 4; // stereo frames
    u32 have = fifo_count();
    u32 n = have < want ? have : want;
    u32 base = sFifoRead % FIFO_SAMPLES;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (base + i) % FIFO_SAMPLES;
        if (sMuted) {
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
        } else {
            out[i * 2]     = (s16)((sFifo[idx * 2]     * sVolume) >> 8);
            out[i * 2 + 1] = (s16)((sFifo[idx * 2 + 1] * sVolume) >> 8);
        }
    }
    if (sOutDump && n) fwrite(out, 4, n, sOutDump); /* SM64_AUDIO_OUTDUMP (below): what went to the device */
    sFifoRead += n;
    if (n < want) {
        memset(&out[n * 2], 0, (want - n) * 4);
        if (n == 0) sUnderruns++;
    }
}

u32 port_audio_out_pushed(void) { return sFifoWrite; } /* SM64_AVLOG (controller_sdl.c) */

u32 port_audio_out_underruns(void) {
    return sUnderruns;
}

/* ---- Load-time mixer thread -------------------------------------------
 * The N64 mixed audio on its own thread, so a course load never starved it.
 * Our game thread mixes synchronously, and a load is a multi-second stall
 * with no fixed hot spot (round-4 pumps between stages and inside MIO0
 * still left gaps). While the game thread is inside a load (port_audio_loading
 * (1)..(0)), this thread keeps the FIFO topped up itself. A mutex serialises
 * the mixer so the game thread's own port_audio_frame never overlaps it. */
extern void port_sm64_audio_frame(void); /* one game frame of audio (hh_main.c) */
static SDL_mutex *sMixLock SAVESTATE_EXCLUDE;
static volatile int sLoading SAVESTATE_EXCLUDE;
static SDL_Thread *sLoadMixer SAVESTATE_EXCLUDE;
#define CUSHION_OUT 4300u /* ~134 ms, MK64's game cushion (3600 samples at 26.8 kHz), in output samples */

void port_audio_mix_lock(void)   { if (sMixLock) SDL_mutexP(sMixLock); }
void port_audio_mix_unlock(void) { if (sMixLock) SDL_mutexV(sMixLock); }

/* Outside loads the thread is a low-water backstop (device round 5: the
 * menu->first-race stall was not inside setup_race). The game thread refills
 * to the cushion every frame and one frame drains ~1070 samples, so the ring
 * never falls to LOW_WATER unless the game thread has stalled for ~90 ms --
 * a load, a texture-import burst, a transition -- and then this thread keeps
 * the callback fed until it returns. */
#define LOW_WATER_OUT 1500u
static volatile u32 sThreadMixes SAVESTATE_EXCLUDE; /* PERF: how often the backstop had to mix */
u32 port_audio_thread_mixes(void) { return sThreadMixes; }

static volatile int sBackstop SAVESTATE_EXCLUDE; /* the game's audio engine is initialised */
void port_audio_backstop_enable(int on) { sBackstop = on; }

/* SM64_AUDIO_SYNC=1 (test runs): audio exactly per game frame, never by the
 * wall clock -- no backstop mixes, fixed buffer sizes, no pacing waits. SM64's
 * game logic reads audio state (cutscenes wait on sounds), so a wall-clock
 * dependent mix can change what a headless run shows; with this, a run repeats
 * frame for frame. (OoT's OOT_AUDIO_SYNC, same reason.) */
int port_audio_sync(void) {
    static int s SAVESTATE_EXCLUDE = -1;
    if (s < 0) { const char *e = getenv("SM64_AUDIO_SYNC"); s = (e && e[0] == '1'); }
    return s;
}

static int load_mixer_thread(void *arg) {
    (void)arg;
    for (;;) {
        const u32 cnt = fifo_count();
        if (sOpened && sBackstop && !port_audio_sync() && (sLoading ? cnt < CUSHION_OUT : cnt < LOW_WATER_OUT)) {
            port_audio_mix_lock();
            port_sm64_audio_frame();
            port_audio_mix_unlock();
            sThreadMixes++;
        } else {
            SDL_Delay(sLoading ? 3 : 5);
        }
    }
    return 0;
}

void port_audio_loading(int on) {
    sLoading = on;
}

void port_audio_out_init(void) {
    SDL_AudioSpec want, got;
#if defined(MIYOO)
    /* The Mini's SDL brings up its MI system (MI_SYS_Init) in the video init, and its dsp audio driver crashes in
     * SDL_AudioInit without it (the MK64 port on the user's Mini Plus, 2026-10-09: every start with the game data
     * already built, which opens no first-start screen first). So the video subsystem always comes up first. */
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        port_log("audio: SDL video init failed: %s\n", SDL_GetError());
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
     * granted on desktop, and the handheld's driver takes it too. If the rate were
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
    { const char *p = getenv("SM64_AUDIO_OUTDUMP"); if (p) sOutDump = fopen(p, "wb"); }
    sOpened = 1;
    SDL_PauseAudio(0);
    port_log("audio: SDL out %d Hz, %u-sample buffer\n", got.freq, sObtainedSamples);
    sMixLock = SDL_CreateMutex();
#if SDL_MAJOR_VERSION >= 2
    sLoadMixer = SDL_CreateThread(load_mixer_thread, "sm64_loadmix", NULL);
#else
    sLoadMixer = SDL_CreateThread(load_mixer_thread, NULL);
#endif
    if (!sLoadMixer) port_log("audio: load-mixer thread failed: %s\n", SDL_GetError());
}

/* osAiGetLength(): bytes of audio still queued (FIFO + device buffer est.). */
u32 port_audio_out_queued_bytes(void) {
    u32 queued = fifo_count();
    if (sOpened) queued += sObtainedSamples / 2; // avg samples in flight at the device
    if (sInRate != OUT_FREQ) {
        queued = (u32) (((u64) queued * sInRate) / OUT_FREQ); // in the game's sample units
    }
    return queued * 4;
}

static void fifo_push(const s16 *samples, u32 n) {
    u32 i;
    u32 space = FIFO_SAMPLES - fifo_count();
    while (sOutDump && sOpened && n > space) { /* SM64_AUDIO_OUTDUMP: wait, don't drop */
        SDL_Delay(1);
        space = FIFO_SAMPLES - fifo_count();
    }
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
    { /* SM64_AUDIO_DUMP=<path>: raw s16le stereo at the game's session rate
       * (see the "audio rate" log line), pre-resample/pre-drop, so it muxes
       * 1:1 with the frame dumps regardless of wall-clock speed. */
        static FILE *sDump SAVESTATE_EXCLUDE; static int sDumpInit SAVESTATE_EXCLUDE;
        if (!sDumpInit) {
            sDumpInit = 1;
            const char *p = getenv("SM64_AUDIO_DUMP");
            if (p) sDump = fopen(p, "wb");
        }
        if (sDump) fwrite(samples, 1, bytes, sDump);
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

void port_audio_out_set_volume(int vol) { // 0..255, the handheld OS menu hook
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    sVolume = vol;
}

/* The ring's target fill for the game thread's mix (hh_main.c): above it the next
 * frame mixes the short buffer, below it the long one, as sm64-port does. */
u32 port_audio_out_cushion_samples(void) { return CUSHION_OUT; }
/* hh_main.c SM64_RINGFILL (test hook): drop what the ring holds beyond `samples` */
void port_audio_out_test_trim(u32 samples) {
    SDL_LockAudio();
    if (fifo_count() > samples) sFifoWrite = sFifoRead + samples;
    SDL_UnlockAudio();
}
u32 port_audio_out_rate(void) { return sInRate ? sInRate : OUT_FREQ_INIT; } /* the game's samples a second (queued_bytes' unit) */
