/**
 * SDL audio out: replaces the N64 AI (after the MK64 kit's audio_sdl.c).
 *
 * The game hands the AI stereo s16 buffers through osAiSetNextBuffer() and
 * sizes the next one from osAiGetLength(). Samples are resampled (linear,
 * stateful) to 32 kHz if the game's rate differs, and queued in a single-
 * producer / single-consumer FIFO that the SDL callback drains. The legacy
 * SDL_OpenAudio() API builds against SDL 2 (desktop) and SDL 1.2 (handheld).
 *
 * osAiGetLength(): the N64 reports what is left of the buffer playing now, and
 * F-Zero X (thread.c) sizes each frame to leave ~155 samples there -- far too
 * little for a callback that takes 512 at a time. So the port reports the
 * queue minus a cushion; the game then keeps the queue near the cushion.
 *
 * Headless (FZX_HEADLESS=1) has no device: port_audio_vi() drains one VI's
 * worth of samples so pacing behaves as on a device, and FZX_WAV=<path>
 * writes everything the game produced (at its own rate) as a WAV file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(HANDHELD_DEVICE)
#include <SDL/SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#include "libultra/ultra64.h"
#include "port_os.h"
#include "hh_declick.h"

extern unsigned int port_time_us(void);

#if defined(MIYOO)
/* The Miyoo's SDL builds differ in what they grant: the obtained rate wins (port_audio_init; DKR's). */
static u32 sOutFreq = 32000;
#define OUT_FREQ sOutFreq
#define OUT_FREQ_INIT 32000
#else
#define OUT_FREQ 32000
#define OUT_FREQ_INIT OUT_FREQ
#endif
#define OUT_BLOCK 512                 /* SDL callback granularity (samples) */
#define FIFO_SAMPLES (OUT_BLOCK * 16) /* ring capacity, stereo samples */
#define CUSHION (OUT_BLOCK + 256)     /* samples kept queued beyond what the game sees */

static s16 sFifo[FIFO_SAMPLES * 2];
static volatile u32 sFifoRead;  /* total samples read (callback thread only) */
static volatile u32 sFifoWrite; /* total samples written (game thread only) */
static int sOpened, sHeadless;
static u32 sObtainedSamples = OUT_BLOCK;
static volatile u32 sUnderruns;
static volatile int sMuted; /* the handheld menu is up: output silence */
/* The game has sent its first buffer. The device opens in port_platform_init, before the launch
 * prompt, the overlays, the SRAM and the game's own boot (its audio thread starts once the N64
 * logo is up, and its first buffer goes out two audio tasks later), so on every launch the
 * callback ran dry for a while with nothing to play: one "underrun" per 16 ms buffer, 11-21 in the
 * first second of every device log, all of it silence (a host run of this file under SDL's dummy
 * driver: 17 for a first buffer 250 ms after the open, 2 after it). Those aren't counted; the
 * first buffer logs how long the wait was. */
static volatile int sStarted;
static volatile u32 sDryBeforeStart;
static u32 sOpenUs;

/* statefix B (handheld-kit k1.8, hh_declick.h; folds in GoldenEye's "primed" rule, BUILDLOG 2026-10-01
 * "Found, not fixed"): no click after the menu or a load. After any gap (the first sound, a dry ring, a
 * load's flush) the card waits until the ring holds the cushion again (or the ring stops growing for
 * 100 ms), then fades in over 10 ms; muting (the menu), a load's hold and a ring that runs dry fade
 * out over 3 ms (a dry ring: its last frames; a ring a buffer empties exactly counts as dry). At full
 * gain the samples pass untouched.
 * The cushion is the port's own (cushion_samples(): 768 samples to 150 ms, after the longest recent
 * gap between the game's top-ups), read on the game thread at each top-up and handed to the callback
 * (sDkCushion). The game sizes each frame to leave ~155 samples beyond it (osAiGetLength reports the
 * ring + half a device buffer - the cushion), so in steady play the ring holds the cushion - ~100 to
 * the cushion + ~430 at a callback: a 512-sample callback never empties it exactly (at least ~250
 * spare). While the card waits it takes nothing, and the game still pushes at least
 * minAiBufferLength (528 samples) a frame, so the ring reaches any cushion in a few frames: the
 * 100 ms stall guard never decides. FZX_DECLICK=0: as before (a hard mute, the ring dropped as the
 * load ends, no fades). */
static HhDeclick sDk;
static int sDeclick = 1;
static volatile u32 sDkCushion = OUT_BLOCK + 256;  /* game thread -> callback */
static volatile u32 sRingMin = 0xFFFFFFFFu;        /* least samples queued at a callback that played (PERF) */

static u32 sInRate = OUT_FREQ_INIT;
static s16 sPrevL, sPrevR;   /* last input sample of the previous push */
static u32 sPhase = 0x10000; /* 16.16 position of the next output sample, from sPrev */
static u32 sStep = 0x10000;  /* input samples per output sample, 16.16 */

static FILE* sWav;
static u32 sWavBytes;

/* FZX_WAV runs: where a save or a load fell in the dump (the round trip compares from there). */
void port_audio_wav_mark(const char* what) {
    if (sWav != NULL) {
        port_log("audio: wav mark %u (%s)\n", (unsigned) sWavBytes, what);
    }
}

/* The cushion follows the game's top-ups (the lead's matrix X19, from paper-mario fb7ae20): the
 * longest recent gap between pushes, plus a quarter and one SDL buffer, 24 to 150 ms, the longest
 * gap decaying by 1/64 a push (~1 s). A fixed 24 ms ran the device dry on every hitch longer
 * than that (a caption, a slow picture); a deep queue all the time would put every sound late.
 * FZX_AUDIOAIM=<ms> fixes it (0: the old fixed cushion). Headless keeps the fixed one (no device;
 * the game's audio sizing, and so the sweeps, stay as they were). */
static u32 sMarkUs, sGapMaxUs, sCushionPeak;
static int sAimMs = -1;
static u32 cushion_samples(void) {
    u32 c;
    if (sAimMs < 0) {
        const char* e = getenv("FZX_AUDIOAIM");
        sAimMs = e ? atoi(e) : 1000; /* 1000: adaptive */
    }
    if (sHeadless || sAimMs == 0) {
        return CUSHION;
    }
    if (sAimMs != 1000) {
        c = (u32) sAimMs * OUT_FREQ / 1000;
    } else {
        c = (u32) (((u64) sGapMaxUs * 5 / 4) * OUT_FREQ / 1000000) + sObtainedSamples;
    }
    c = c < CUSHION ? CUSHION : c > OUT_FREQ * 150 / 1000 ? OUT_FREQ * 150 / 1000 : c;
    if (c > sCushionPeak) {
        sCushionPeak = c;
    }
    return c;
}

u32 port_audio_cushion_peak_ms(void) { /* for the exit line (platform_sdl.c) */
    return sCushionPeak * 1000 / OUT_FREQ;
}

static u32 fifo_count(void) {
    return sFifoWrite - sFifoRead;
}

static void audio_callback(void* userdata, Uint8* stream, int len) {
    s16* out = (s16*) stream;
    const u32 want = (u32) len / 4;
    const u32 have = fifo_count();
    const u32 n = have < want ? have : want;
    const u32 base = sFifoRead % FIFO_SAMPLES;
    u32 i;
    (void) userdata;
    if (n < want) {
        if (!sStarted) {
            sDryBeforeStart++; /* nothing sent yet: not an underrun */
        } else if (!sMuted) {
            sUnderruns++; /* the game is paused while the menu is up: not an underrun */
        }
    }
    if (sDeclick) {
        sDk.cushion = sDkCushion;
        if (!hhd_ready(&sDk, have, want)) { /* after a gap: silence until the cushion is back */
            memset(out, 0, (size_t) want * 4);
            return;
        }
        if (have < sRingMin) {
            sRingMin = have;
        }
    }
    for (i = 0; i < n; i++) {
        const u32 idx = (base + i) % FIFO_SAMPLES;
        out[i * 2] = (sMuted && !sDeclick) ? 0 : sFifo[idx * 2];
        out[i * 2 + 1] = (sMuted && !sDeclick) ? 0 : sFifo[idx * 2 + 1];
    }
    sFifoRead += n;
    if (n < want) {
        memset(&out[n * 2], 0, (want - n) * 4);
    }
    if (sDeclick) {
        hhd_apply(&sDk, out, n, want, sMuted || sDk.hold); /* the fades (muted or held: down to silence) */
    }
}

/* handheld_menu.c: the game is paused while the menu is up (mario-kart-64 audio_sdl.c). */
void port_audio_out_set_muted(int muted) {
    sMuted = muted; /* k1.8: with the declick the callback fades to and from silence */
}

/* savestate.c: after a load, drop what the old timeline queued. Headless runs have no
 * device draining the ring, so they keep it (their AI length stays the same).
 * Under the audio lock: the callback reads sFifoRead and stores it back advanced by up to a
 * buffer; a flush that read it in between set sFifoWrite behind the new sFifoRead, so the ring
 * looked ~4 G samples full (stale samples played, osAiGetLength garbage) until the writes caught
 * up. With the declick, a load is held (port_audio_out_gap below): the drop waits for the card's
 * fade out there; the Bench's in-memory restore drops it here and the sound fades in again. */
void port_audio_flush(void) {
    if (sOpened) {
        if (sDeclick && sDk.hold) {
            return; /* a load: port_audio_out_gap(0) drops it once the card has faded out */
        }
        SDL_LockAudio();
        sFifoWrite = sFifoRead;
        if (sDeclick) {
            hhd_flush(&sDk);
        }
        SDL_UnlockAudio();
    }
}

/* savestate.c around a load (handheld-kit k1.8): 1 before it, the card fades out and holds; 0 after it,
 * once a whole callback has run under the hold (hhd_settle, at most 50 ms: a load from the page cache
 * can take under a millisecond, and dropping the queue under a buffer still at full gain was a step),
 * the old timeline's queue is dropped with the callback locked out, and the restored game's sound
 * fades in once the cushion is back. No mix lock here: the game's audio task runs in the main loop,
 * which is where the load runs. Headless (no device) and FZX_DECLICK=0: as before. FZX_WAV runs log
 * where the load fell in the dump (the save-state round trip lines the two runs up there). */
static void gap_sleep_ms(uint32_t ms) {
    SDL_Delay(ms);
}
void port_audio_out_gap(int begin) {
    if (!begin) {
        port_audio_wav_mark("load");
    }
    if (!sDeclick || !sOpened) {
        return;
    }
    if (begin) {
        hhd_hold(&sDk);
        return;
    }
    hhd_settle(&sDk, gap_sleep_ms, 50);
    SDL_LockAudio();
    sFifoWrite = sFifoRead;
    hhd_flush(&sDk);
    sDk.hold = 0;
    SDL_UnlockAudio();
}

/* PERF line: the least the ring held at a callback that played since the last call (0: none
 * played), then reset; and the times the sound came back after a gap (hh_declick.h). */
u32 port_audio_take_ring_min(void) {
    const u32 m = sRingMin;
    sRingMin = 0xFFFFFFFFu;
    return m == 0xFFFFFFFFu ? 0 : m;
}
u32 port_audio_gaps(void) {
    return sDk.gaps;
}

u32 port_audio_underruns(void) {
    return sUnderruns;
}

static void wav_header(FILE* f, u32 dataBytes, u32 rate) {
    u8 h[44];
    u32 v;
    memcpy(h, "RIFF", 4);
    v = 36 + dataBytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    h[20] = 1; h[21] = 0; /* PCM */
    h[22] = 2; h[23] = 0; /* stereo */
    v = rate; memcpy(h + 24, &v, 4);
    v = rate * 4; memcpy(h + 28, &v, 4);
    h[32] = 4; h[33] = 0;
    h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    v = dataBytes; memcpy(h + 40, &v, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
}

void port_audio_init(int headless) {
    SDL_AudioSpec want, got;
    const char* wav = getenv("FZX_WAV");
    sHeadless = headless;
    sFifoRead = sFifoWrite = 0;
    if (wav != NULL && wav[0] != '\0') {
        sWav = fopen(wav, "wb");
        if (sWav != NULL) {
            wav_header(sWav, 0, OUT_FREQ);
        }
    }
    if (headless) {
        return;
    }
#if defined(MIYOO)
    /* The Mini's SDL brings up its MI system (MI_SYS_Init) in the video init, and its dsp audio driver
     * crashes in SDL_AudioInit without it (DKR and MK64 on the user's Mini Plus, 2026-10-09). The video
     * is up already here (port_platform_init opens it first); this keeps it that way. */
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        port_log("audio: SDL video init failed: %s\n", SDL_GetError());
    }
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
    if (got.freq != OUT_FREQ || got.channels != 2 || got.format != AUDIO_S16SYS) {
        port_log("audio: obtained %d Hz ch %d fmt %04X (wanted %d/2/%04X)\n", got.freq, got.channels, got.format,
                 OUT_FREQ, AUDIO_S16SYS);
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
    {
        const char* e = getenv("FZX_DECLICK");
        sDeclick = !(e && e[0] == '0');
        hhd_init(&sDk, OUT_FREQ, sDkCushion);
    }
    sOpened = 1;
    sOpenUs = port_time_us();
    SDL_PauseAudio(0);
    port_log("audio: SDL out %d Hz, %u-sample buffer\n", got.freq, sObtainedSamples);
}

void port_audio_quit(void) {
    if (sOpened) {
        SDL_CloseAudio();
        sOpened = 0;
    }
    if (sWav != NULL) {
        wav_header(sWav, sWavBytes, sInRate);
        fclose(sWav);
        sWav = NULL;
    }
}

void port_audio_set_frequency(u32 hz) {
    if (hz < 8000 || hz > 48000) {
        hz = OUT_FREQ;
    }
    if (hz == sInRate) {
        return; /* a re-assert of the same rate must not reset the resampler (MK64) */
    }
    port_log("audio: game rate %u Hz -> %d Hz out\n", hz, OUT_FREQ);
    sInRate = hz;
    sStep = (u32) (((u64) hz << 16) / OUT_FREQ);
    sPhase = 0x10000;
    sPrevL = sPrevR = 0;
}

/* osAiGetLength(): bytes "left in the AI", in the game's sample units. */
u32 port_audio_queued_bytes(void) {
    u32 queued = fifo_count();
    if (sOpened) {
        queued += sObtainedSamples / 2; /* average in flight at the device */
    }
    {
        const u32 cu = cushion_samples();
        queued = queued > cu ? queued - cu : 0;
        sDkCushion = cu; /* the declick's wait after a gap (read here, on the game thread) */
    }
    if (sInRate != OUT_FREQ) {
        queued = (u32) (((u64) queued * sInRate) / OUT_FREQ);
    }
    return queued * 4;
}

static void fifo_push(const s16* samples, u32 n) {
    const u32 space = FIFO_SAMPLES - fifo_count();
    u32 i;
    if (n > space) {
        n = space; /* drop what does not fit */
    }
    for (i = 0; i < n; i++) {
        const u32 idx = (sFifoWrite + i) % FIFO_SAMPLES;
        sFifo[idx * 2] = samples[i * 2];
        sFifo[idx * 2 + 1] = samples[i * 2 + 1];
    }
    sFifoWrite += n;
}

/* osAiSetNextBuffer(): queue `bytes` of stereo s16 samples. */
void port_audio_push(const s16* samples, u32 bytes) {
    const u32 n = bytes / 4;
    if (n == 0) {
        return;
    }
    if (!sStarted && sOpened) {
        port_log("audio: the game's first buffer %u ms after the device opened (%u empty callbacks before it, not underruns)\n",
                 (unsigned) ((port_time_us() - sOpenUs) / 1000), (unsigned) sDryBeforeStart);
    }
    {   /* the gap since the last top-up sets the cushion (cushion_samples) */
        const u32 now = port_time_us(), gap = sMarkUs ? now - sMarkUs : 0;
        sMarkUs = now;
        sGapMaxUs -= sGapMaxUs >> 6;
        if (gap > sGapMaxUs && gap < 1000000) {
            sGapMaxUs = gap;
        }
    }
    if (sWav != NULL) {
        fwrite(samples, 1, n * 4, sWav);
        sWavBytes += n * 4;
    }
    if (sInRate == OUT_FREQ) {
        fifo_push(samples, n);
        sStarted = 1;
        return;
    }
    {
        /* Linear interpolation between x[i-1] and x[i]; x[-1] = the previous push's last sample. */
        s16 tmp[256 * 2];
        u32 k = 0;
        for (;;) {
            const u32 idx = sPhase >> 16;
            s32 frac, a, b;
            if (idx >= n) {
                break;
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
    sStarted = 1;
}

/* A retrace: headless has no device, so consume one VI of output here. */
void port_audio_vi(void) {
    static u32 sFrac;
    if (!sHeadless) {
        return;
    }
    sFrac += OUT_FREQ;
    {
        const u32 take = sFrac / PORT_VI_HZ;
        const u32 have = fifo_count();
        sFrac -= take * PORT_VI_HZ;
        if (have < take) {
            if (sStarted) {
                sUnderruns++; /* as the callback: not before the game's first buffer */
            }
            sFifoRead += have;
        } else {
            sFifoRead += take;
        }
    }
}
