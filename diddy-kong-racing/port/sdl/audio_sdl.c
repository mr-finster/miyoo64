/**
 * SDL audio out: replaces the N64 AI (port of src/port/psp/audio_out.c).
 *
 * The game's audio engine (create_next_audio_frame_task) hands the AI a
 * variable-length buffer of stereo s16 samples at the audio-session rate
 * (26800 Hz on every MK64 preset) via osAiSetNextBuffer(), and sizes the next
 * mix from osAiGetLength().  Samples are resampled (linear, stateful) to a
 * fixed 32 kHz and pushed into a single-producer/single-consumer FIFO that
 * the SDL audio callback drains.  The legacy SDL_OpenAudio() API is used so
 * the same file builds against SDL 2 (desktop) and SDL 1.2 (handheld).
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
static int sVolume SAVESTATE_EXCLUDE = 255; // 0..255, driven by the handheld OS volume menu

static u32 fifo_count(void) {
    return sFifoWrite - sFifoRead;
}

/* statefix B (the port kit k1.8, hh_declick.h): no click after the menu or a load. After any gap (the
 * first sound, a dry ring, a load's flush) the card waits until the ring holds DECLICK_CUSHION again,
 * then fades in over 10 ms; muting (the menu), a load's hold and a ring that runs dry fade out over
 * 3 ms. A load used to play on the old timeline's queued sound and then the restored game's, and the
 * slow frames after it (textures coming back) ran the ring dry again and again: each a hard stop and a
 * hard restart mid-waveform, the short crackle. The cushion is below what the game keeps queued: it
 * steers the queue to osAiGetLength's 1470-sample aim (ultra_port.c), ~970-2040 output frames between
 * two audio frames in steady play, and the first audio frame after a gap queues ~1230 by itself. At
 * full gain the samples pass untouched (the volume scaling as before). DKR_DECLICK=0: as before. */
#define DECLICK_CUSHION 1024u
static HhDeclick sDk SAVESTATE_EXCLUDE;
static int sDeclick SAVESTATE_EXCLUDE = 1;

/* Consumer: SDL's audio thread. On underrun, silence keeps the stream timed. */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    s16 *out = (s16 *)stream;
    u32 want = (u32)len / 4; // stereo frames
    u32 have = fifo_count();
    if (have == 0) sUnderruns++;
    if (sDeclick && !hhd_ready(&sDk, have, want)) { /* after a gap: wait for the cushion */
        memset(out, 0, (size_t) len);
        return;
    }
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
    if (n < want) memset(&out[n * 2], 0, (want - n) * 4);
    if (sDeclick) hhd_apply(&sDk, out, n, want, sMuted || sDk.hold); /* the fades (muted or held: down to silence) */
}

/* savestate.c around a load (kit k1.8): 1 before it, the card fades out and holds; 0 after it, once a
 * whole callback has run under the hold (a load from the page cache takes under a millisecond: the fade
 * out must reach the card before the queue under it goes; at most 50 ms), the old timeline's queue is
 * dropped (the callback locked out; it never takes the mix lock) and the restored game's sound fades in
 * once the cushion is back. */
static void dk_sleep_ms(uint32_t ms) { SDL_Delay(ms); }
void port_audio_out_gap(int begin) {
    if (!sDeclick || !sOpened) return;
    if (begin) {
        hhd_hold(&sDk);
        return;
    }
    hhd_settle(&sDk, dk_sleep_ms, 50);
    SDL_LockAudio();
    sFifoRead = sFifoWrite;
    hhd_flush(&sDk);
    sDk.hold = 0;
    SDL_UnlockAudio();
}

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
extern void port_audiomgr_frame(void);
#define create_next_audio_frame_task port_audiomgr_frame
static SDL_mutex *sMixLock SAVESTATE_EXCLUDE;
static volatile int sLoading SAVESTATE_EXCLUDE;
static SDL_Thread *sLoadMixer SAVESTATE_EXCLUDE;
#define CUSHION_OUT ((3600u * OUT_FREQ) / 26800u) /* the 3600-sample game cushion, in output samples */

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

/* B16 (2026-10-03, the final exact speed check): the thread only mixes while sBackstop is set,
 * and nothing in DKR sets it (savestate.c's swap(0) ... enable(was) only puts back the 0 it
 * read), so it never mixed: it woke every 5 ms all session for nothing (~200 wakeups a second,
 * each a switch away from the game thread on the single core). It is now started by the first
 * backstop enable instead of at init, i.e. never in DKR today; the mixer calls, and so every
 * sample, are the same. DKR_LAZYMIX=0: started at init, as before (the A/B; the benchmark's
 * lazymix=both). Everything here runs on the game thread (init, the savestate swaps, the
 * benchmark's switch), so sLoadMixer needs no lock. */
static volatile int sLazyMix SAVESTATE_EXCLUDE = 1;
static volatile int sMixQuit SAVESTATE_EXCLUDE; /* port_audio_set_lazymix(1): an idle thread ends */
static void load_mixer_start(void);
void port_audio_backstop_enable(int on) { sBackstop = on; if (on) load_mixer_start(); }
int port_audio_backstop_swap(int on) { const int was = sBackstop; sBackstop = on; if (on) load_mixer_start(); return was; }

static int load_mixer_thread(void *arg) {
    (void)arg;
    for (;;) {
        const u32 cnt = fifo_count();
        if (sOpened && sBackstop && (sLoading ? cnt < CUSHION_OUT : cnt < LOW_WATER_OUT)) {
            port_audio_mix_lock();
            create_next_audio_frame_task();
            create_next_audio_frame_task();
            port_audio_mix_unlock();
            sThreadMixes++;
        } else {
            if (sMixQuit) return 0; /* the benchmark switched to the lazy start: joined there */
            SDL_Delay(sLoading ? 3 : 5);
        }
    }
    return 0;
}

static void load_mixer_start(void) {
    if (sLoadMixer || !sOpened) return;
#if SDL_MAJOR_VERSION >= 2
    sLoadMixer = SDL_CreateThread(load_mixer_thread, "dkr_loadmix", NULL);
#else
    sLoadMixer = SDL_CreateThread(load_mixer_thread, NULL);
#endif
    if (!sLoadMixer) port_log("audio: load-mixer thread failed: %s\n", SDL_GetError());
}

/* The benchmark's lazymix=both (main.c bench_switch): 1 the lazy start (an idle thread ends and is
 * joined, within its 5 ms sleep), 0 the thread running, as DKR_LAZYMIX=0. */
void port_audio_set_lazymix(int on) {
    sLazyMix = on ? 1 : 0;
    if (!on) { load_mixer_start(); return; }
    if (sLoadMixer && !sBackstop) { sMixQuit = 1; SDL_WaitThread(sLoadMixer, NULL); sLoadMixer = NULL; sMixQuit = 0; }
}

void port_audio_loading(int on) {
    sLoading = on;
}

void port_audio_out_init(void) {
    SDL_AudioSpec want, got;
#if defined(MIYOO)
    /* The Mini's SDL brings up its MI system (MI_SYS_Init) in the video init, and its dsp audio driver
     * crashes in SDL_AudioInit without it (signal 11 at iteration 0, the user's Mini Plus 2026-10-09).
     * The first start opened the screen for the data build first; every later start, with the data
     * cached, came here first. So the video subsystem always comes up before audio. */
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
     * granted on desktop, and the device's driver takes it too. If the rate were
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
        port_log("audio: resampling to the obtained %d Hz\n", got.freq);
    }
#endif
    sObtainedSamples = got.samples ? got.samples : OUT_BLOCK;
    sFifoRead = sFifoWrite = 0;
    {
        const char *e = getenv("DKR_DECLICK");
        sDeclick = !(e && e[0] == '0');
        hhd_init(&sDk, OUT_FREQ, DECLICK_CUSHION);
    }
    sOpened = 1;
    SDL_PauseAudio(0);
    port_log("audio: SDL out %d Hz, %u-sample buffer\n", got.freq, sObtainedSamples);
    sMixLock = SDL_CreateMutex();
    { const char *e = getenv("DKR_LAZYMIX"); sLazyMix = !(e && e[0] == '0'); }
    if (!sLazyMix || sBackstop) load_mixer_start(); /* B16: else on the first backstop enable */
}

#if defined(MIYOO)
/* the watchdog's audio line (miyoo_watchdog.c, plain reads from its thread) */
int miyoo_wd_detail_audio(char *buf, int n) {
    unsigned sEvtqDrops, sEvtqLost;
    { extern void port_audio_evtq_counts(unsigned *drops, unsigned *lost); port_audio_evtq_counts(&sEvtqDrops, &sEvtqLost); }
    return snprintf(buf, n, "opened %d, %u Hz out, ring %u samples (read %u, written %u), underruns %u, muted %d, backstop %d, load mixer %s, "
                    "event queue drops %u, lost API events %u",
                    sOpened, (unsigned) OUT_FREQ, (unsigned) fifo_count(), (unsigned) sFifoRead, (unsigned) sFifoWrite, (unsigned) sUnderruns,
                    sMuted, sBackstop, sLoadMixer ? "started" : "not started", sEvtqDrops, sEvtqLost);
}
#endif
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
    { /* DKR_AUDIO_DUMP=<path>: raw s16le stereo at the game's session rate
       * (see the "audio rate" log line), pre-resample/pre-drop, so it muxes
       * 1:1 with the frame dumps regardless of wall-clock speed. */
        static FILE *sDump SAVESTATE_EXCLUDE; static int sDumpInit SAVESTATE_EXCLUDE;
        if (!sDumpInit) {
            sDumpInit = 1;
            const char *p = getenv("DKR_AUDIO_DUMP");
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

/* port_audio_frame() is provided by the game (src/main.c). */
void init_audiomanager(void) { }
