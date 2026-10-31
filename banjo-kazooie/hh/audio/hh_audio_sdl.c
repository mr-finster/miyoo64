/*
 * The AI's output (osAiSetNextBuffer -> hh_plat_audio_out): stereo s16 frames the audio task
 * (acmd_mixer.c) wrote in host order, at the rate osAiSetFrequency set (~22,050 Hz). They go
 * into a single-producer / single-consumer ring that SDL's audio callback drains; the legacy
 * SDL_OpenAudio API builds against SDL 2 (the desktop proxy) and SDL 1.2 (the handheld).
 *
 * hh_hw.c paces the AI on the game's own clock, as the N64's DAC did, so the game mixes one
 * retrace's worth of audio per retrace -- but in bursts, one per rendered frame (17-25 fps on the
 * device's slower levels), while the card consumes steadily. Playback starts only once the ring
 * holds a cushion (CUSHION_MS), and a ring that runs dry waits for the cushion again rather
 * than clicking through every gap (the user: "sound crackles", round 18). A full ring drops
 * the oldest frames. BK_AUDIOCUSHION=<ms> overrides the cushion.
 * The card is opened at OUT_FREQ (32 kHz, as DKR's port on the same hardware) and the AI's rate
 * resampled here, linear and stateful across buffers: SDL 1.2's own conversion of 22,047 Hz is
 * crude (round 18).
 * BK_AUDIO=0: no device (the frames are mixed and dropped). BK_WAVOUT=<file>: every frame the
 * game hands the AI, as raw s16 stereo (tests: the proxy runs headless).
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (port/kit) */
#if defined(HANDHELD)
#include <SDL/SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#define RING_FRAMES 16384 /* 512 ms at 32 kHz: the cushion (150 ms) plus bursts after a slow frame */

#define CUSHION_MS 150
#define OUT_FREQ 32000

static int16_t sRing[RING_FRAMES * 2] SAVESTATE_EXCLUDE;
static uint32_t sCushion SAVESTATE_EXCLUDE = 3300;        /* frames: CUSHION_MS at the open rate */
static volatile int sPrimed SAVESTATE_EXCLUDE;            /* the callback plays (else it waits for the cushion) */
static volatile uint32_t sRead SAVESTATE_EXCLUDE, sWrite SAVESTATE_EXCLUDE; /* total frames read (callback) / written (game) */
static int sState SAVESTATE_EXCLUDE;                      /* 0 = not tried, 1 = open, -1 = off */
static uint32_t sRate SAVESTATE_EXCLUDE;
/* for the PERF line (hh_gfx.c): times the card ran dry / the ring overflowed, and the fill */
volatile uint32_t hh_audio_underruns SAVESTATE_EXCLUDE, hh_audio_overruns SAVESTATE_EXCLUDE;
uint32_t hh_audio_fill_ms(void) { return sState == 1 ? (sWrite - sRead) * 1000u / OUT_FREQ : 0; }

static volatile int sMuted SAVESTATE_EXCLUDE; /* the kit's EXIT GAME: silence through the exit save */

/* the port kit hooks (port/kit/platform: the menu and save states). The game's mixer runs in its own
 * audio thread (a game coroutine), never on the card's callback, so nothing mixes while a state is
 * captured or restored: the mix lock and the "backstop" (MK64's host-thread mixer) have nothing to
 * hold. Muting silences the card; the ring keeps filling and draining as usual. */
void port_audio_mix_lock(void) {}
void port_audio_mix_unlock(void) {}
void port_audio_backstop_enable(int on) { (void)on; }
void port_audio_out_set_muted(int muted) { sMuted = muted != 0; }

/* statefix B (the port kit k1.8, hh_declick.h): no click after the menu or a load. The primed rule above
 * (round 18) is now hhd_ready's, with the same cushion (sCushion, BK_AUDIOCUSHION); on top of it the card
 * fades in over 10 ms after every gap (the first sound, a dry ring, a load), and muting (the menu, EXIT
 * GAME), a load's hold and a ring running dry fade out over 3 ms (a dry ring fades the last frames it
 * had), where the card used to stop or start mid-waveform. Muted, the ring drains as before; a ring that
 * runs dry while muted (the game is stopped behind the menu) waits for the cushion after it, as any gap.
 * At full gain the samples pass untouched. hhd's 100 ms stall guard is off: this producer is paced by the
 * game's own DAC clock (hh_hw.c's osAiGetLength), never by this ring, so it never stops short of the
 * cushion, and the wait ends where the old rule's did. Rate control and stretch are unchanged.
 * BK_DECLICK=0: as before (tests). */
#include "hh_declick.h"
static HhDeclick sDk SAVESTATE_EXCLUDE;
static int sDeclick SAVESTATE_EXCLUDE = 1;
static int primed(void) { return sDeclick ? sDk.primed : sPrimed; }

static void ring_copy(int16_t *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        const uint32_t k = (sRead + i) % RING_FRAMES;
        out[i * 2] = sRing[k * 2];
        out[i * 2 + 1] = sRing[k * 2 + 1];
    }
    sRead += n;
}

static void callback(void *user, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    uint32_t want = (uint32_t)len / 4, have = sWrite - sRead, n;
    (void)user;
    if (sDeclick) {
        const int silent = sMuted || sDk.hold;
        if (!hhd_ready(&sDk, have, want)) { /* after a gap: wait for the cushion */
            memset(out, 0, (size_t)len);
            return;
        }
        n = have < want ? have : want;
        ring_copy(out, n);
        if (n < want)
            memset(out + n * 2, 0, (want - n) * 4);
        if (hhd_apply(&sDk, out, n, want, silent) && !silent)
            hh_audio_underruns++;
        return;
    }
    if (!sPrimed && have >= sCushion)
        sPrimed = 1;
    if (!sPrimed) {
        memset(out, 0, (size_t)len);
        return;
    }
    n = have < want ? have : want;
    if (sMuted) { /* drain as usual, play silence */
        sRead += n;
        memset(out, 0, (size_t)len);
        return;
    }
    if (n < want) {
        sPrimed = 0; /* ran dry: refill the cushion before playing on */
        hh_audio_underruns++;
    }
    ring_copy(out, n);
    if (n < want)
        memset(out + n * 2, 0, (want - n) * 4);
}

/* The kit's savestate.c around a load (k1.8): 1 before it, the card fades out and holds; 0 after it, once
 * a whole callback has run under the hold (a load from the page cache takes under a millisecond: without
 * the wait the drop cut a buffer playing at full gain), the old timeline's queue is dropped with the
 * callback locked out, and the restored game's sound fades in once the cushion is back. The mix lock is a
 * no-op here (the game mixes on its own audio thread, parked through a load), so nothing else is held
 * under SDL_LockAudio. The resampler's carry (prevL/R, phase: not in a state) lands in the fade in. */
static void declick_sleep(uint32_t ms) { SDL_Delay(ms); }
void port_audio_out_gap(int begin)
{
    if (!sDeclick || sState != 1)
        return;
    if (begin) {
        hhd_hold(&sDk);
        return;
    }
    if (!hhd_settle(&sDk, declick_sleep, 50))
        fprintf(stderr, "[audio] load: no card buffer under the hold in 50 ms; the queue dropped anyway\n");
    SDL_LockAudio();
    sRead = sWrite;
    hhd_flush(&sDk);
    sDk.hold = 0;
    SDL_UnlockAudio();
}

static void open_device(uint32_t rate)
{
    SDL_AudioSpec want;
    const char *e = getenv("BK_AUDIO");
    if (e && e[0] == '0') {
        sState = -1;
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
        sState = -1;
        return;
    }
    memset(&want, 0, sizeof want);
    want.freq = OUT_FREQ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = callback;
    if (SDL_OpenAudio(&want, NULL) != 0) { /* NULL: SDL converts to what the card takes */
        fprintf(stderr, "[audio] SDL_OpenAudio(%u Hz) failed: %s\n", (unsigned)rate, SDL_GetError());
        sState = -1;
        return;
    }
    sRate = rate;
    {
        const char *c = getenv("BK_AUDIOCUSHION");
        const uint32_t ms = (c && atoi(c) > 0) ? (uint32_t)atoi(c) : CUSHION_MS;
        sCushion = OUT_FREQ * ms / 1000;
        if (sCushion > RING_FRAMES / 2)
            sCushion = RING_FRAMES / 2;
    }
    {
        const char *d = getenv("BK_DECLICK");
        sDeclick = !(d && d[0] == '0');
        hhd_init(&sDk, OUT_FREQ, sCushion);
        sDk.stall_frames = 0xFFFFFFFFu; /* no stall guard: see sDk */
    }
    sPrimed = 0;
    sState = 1;
    SDL_PauseAudio(0);
    fprintf(stderr, "[audio] %u Hz in, %d Hz out\n", (unsigned)rate, OUT_FREQ);
}

void hh_audio_push(const void *pcm, uint32_t bytes, uint32_t rate)
{
    const int16_t *s = (const int16_t *)pcm;
    uint32_t n = bytes / 4, i;
    {
        static FILE *wav SAVESTATE_EXCLUDE;
        static int tried SAVESTATE_EXCLUDE;
        if (!tried) { const char *w = getenv("BK_WAVOUT"); tried = 1; if (w) wav = fopen(w, "wb"); }
        if (wav && s) { fwrite(s, 4, n, wav); fflush(wav); }
    }
    if (sState == 0)
        open_device(rate);
    if (sState != 1 || s == NULL)
        return;
    if (rate != sRate && rate >= 8000 && rate <= 48000) { /* the game changed the AI rate */
        SDL_CloseAudio();
        sRead = sWrite = 0;
        open_device(rate);
        if (sState != 1)
            return;
    }
    {
        /* linear resample rate -> OUT_FREQ: sPhase is the 16.16 position of the next output frame
         * after sPrev (the last input frame of the previous buffer) */
        static int16_t prevL SAVESTATE_EXCLUDE, prevR SAVESTATE_EXCLUDE;
        static uint32_t phase SAVESTATE_EXCLUDE = 0x10000;
        static int sRateCtl SAVESTATE_EXCLUDE = -1;
        uint32_t step = (uint32_t)(((uint64_t)sRate << 16) / OUT_FREQ);
        uint32_t out = 0, room, fill;
        SDL_LockAudio();
        fill = sWrite - sRead;
        room = RING_FRAMES - fill;
        SDL_UnlockAudio();
        /* Rate control (round 19, the user: "still skips every few seconds"): the game's audio
         * and the card's clock never match exactly, and any small difference drains or fills
         * the cushion until it runs dry. The step moves by up to 0.5% (RetroArch's default dynamic rate
         * control delta, inaudible)
         * toward holding the ring at the cushion: below it each input frame makes slightly
         * more output. BK_AUDIORATECTL=0: a fixed step. */
        if (sRateCtl < 0) { const char *e = getenv("BK_AUDIORATECTL"); sRateCtl = !(e && e[0] == '0'); }
        if (sRateCtl && primed()) {
            const int32_t err = (int32_t)fill - (int32_t)sCushion;       /* frames above the target */
            int32_t ppm = (int32_t)((int64_t)err * 5000 / (int32_t)sCushion); /* +-100% of the cushion = +-0.5% */
            if (ppm > 5000) ppm = 5000;
            if (ppm < -5000) ppm = -5000;
            /* Stretch before running dry (round 42, the user: in heavy smoke "the sound dropped out to
             * silence back and forth quickly like audio buffer lag"). Under sustained slow frames the
             * game's mixer can't lengthen its frames enough to cover real time, the ring drains, runs dry
             * and waits out a 150 ms refill in silence -- again and again. Below half the cushion the
             * step keeps shrinking, to -4% at empty: the sound slows by a few percent while the game lags
             * instead of cutting out. BK_AUDIOSTRETCH=0: +-0.5% only. */
            {
                static int sStretch SAVESTATE_EXCLUDE = -1;
                if (sStretch < 0) { const char *e = getenv("BK_AUDIOSTRETCH"); sStretch = !(e && e[0] == '0'); }
                const int32_t half = (int32_t)sCushion / 2;
                if (sStretch && err < -half && half > 0) {
                    int32_t deep = -err - half; /* 0 .. half */
                    if (deep > half) deep = half;
                    ppm = -5000 - (int32_t)((int64_t)deep * 35000 / half);
                }
            }
            step = (uint32_t)((int64_t)step * (1000000 + ppm) / 1000000);
        }
        for (;;) {
            const uint32_t k = phase >> 16; /* 0 = between prev and s[0], else s[k-1]..s[k] */
            if (k >= n + 0)
                break;
            {
                const int32_t f = (int32_t)(phase & 0xFFFF);
                const int32_t l0 = k == 0 ? prevL : s[(k - 1) * 2], r0 = k == 0 ? prevR : s[(k - 1) * 2 + 1];
                const int32_t l1 = s[k * 2], r1 = s[k * 2 + 1];
                const uint32_t w = (sWrite + out) % RING_FRAMES;
                sRing[w * 2] = (int16_t)(l0 + (((l1 - l0) * f) >> 16));
                sRing[w * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * f) >> 16));
            }
            out++;
            phase += step;
            if (out >= RING_FRAMES)
                break;
        }
        phase -= n << 16;
        if (n > 0) { prevL = s[(n - 1) * 2]; prevR = s[(n - 1) * 2 + 1]; }
        if (out > room) { /* full: drop the oldest */
            hh_audio_overruns++;
            SDL_LockAudio();
            sRead += out - room;
            SDL_UnlockAudio();
        }
        sWrite += out;
    }
}
