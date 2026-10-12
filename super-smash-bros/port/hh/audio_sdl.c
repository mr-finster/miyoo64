/* audio_sdl.c -- the N64 audio interface, replaced by an SDL audio device.
 *
 * The game's audio thread mixes one 32 kHz stereo frame per retrace
 * (audio_out.h) and hands it to hh_audio_push; a single-producer/
 * single-consumer ring carries it to SDL's callback thread. The legacy
 * SDL_OpenAudio() API builds against SDL 2 (desktop) and SDL 1.2 (handheld), as
 * MK64's audio_sdl.c does. On underrun the callback plays silence, keeping the
 * stream timed; on overrun (game ahead of the device) the oldest block goes. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(HANDHELD)
#include <SDL/SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#include "audio_out.h"
#include "port.h"
#include "hh_declick.h"

#define OUT_BLOCK 512                   /* SDL callback granularity, frames */
#define RING_FRAMES (OUT_BLOCK * 16)    /* ~256 ms */

/* Shared with SDL's thread: never part of a save state (outside port/gamestate.ld's
 * include list). */
static int16_t sRing[RING_FRAMES * 2];
static volatile uint32_t sRead, sWrite;   /* total frames consumed / produced */
static int sOpened;
static volatile uint32_t sUnderruns;
static int sPeak;          /* loudest sample pushed since the last PERF line */
static volatile int sMuted; /* the handheld menu (and its EXIT GAME: silence through the exit save) */
static int sFlushOnUnlock;  /* a state was loaded: drop the old sound still queued */

/* statefix B (kit k1.8, hh_declick.h): no click after the menu or a load. After any gap (the
 * first sound, a dry ring, a load's flush) the card waits until the ring holds DECLICK_CUSHION again (or
 * the ring stops growing for 100 ms), then fades in over 10 ms; muting (the menu), a load's hold and a
 * ring that runs dry fade out over 3 ms (a dry ring: its last frames; a ring a buffer empties exactly
 * counts as dry). At full gain the samples pass untouched. The game keeps the ring at its aim after each
 * fill (hh_audio_aim_frames: 50-150 ms, 1600-4800 frames) and mixes up to 920 frames a tick while under
 * it, so 1024 (32 ms, two buffers) comes back by the game's second tick after a gap and stays well under
 * the steady fill (the PERF line's "ring min": the least the ring held at a callback that played).
 * SSB64_DECLICK=0: as before (a hard mute, no fades, the ring dropped as the load's lock is released). */
#define DECLICK_CUSHION 1024u
static HhDeclick sDk;
static int sDeclick = 1;
static volatile uint32_t sRingMin = UINT32_MAX; /* least frames queued at a callback that played (PERF) */

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    uint32_t want = (uint32_t)len / 4, have = sWrite - sRead;
    uint32_t n = have < want ? have : want;
    (void)userdata;
    if (n < want) sUnderruns++; /* as before: a callback the ring could not fill */
    if (sDeclick && !hhd_ready(&sDk, have, want)) { /* after a gap: silence until the cushion is back */
        memset(out, 0, (size_t)want * 4);
        return;
    }
    if (have < sRingMin) sRingMin = have;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (sRead + i) % RING_FRAMES;
        out[i * 2] = sRing[idx * 2];
        out[i * 2 + 1] = sRing[idx * 2 + 1];
    }
    if (n < want) memset(out + n * 2, 0, (want - n) * 4);
    if (sDeclick) hhd_apply(&sDk, out, n, want, sMuted || sDk.hold); /* the fades (muted: down to silence) */
    else if (sMuted) memset(out, 0, (size_t)want * 4);
    sRead += n;
}

void hh_audio_init(void)
{
    SDL_AudioSpec want, have;
#if defined(MIYOO)
    /* The Mini's SDL does its MI system init (MI_SYS_Init) in the video init, and its dsp audio driver
     * crashes in SDL_AudioInit without it (MK64/DKR on the device, 2026-10-09). main.c brings up the
     * screen (gfx_init) before this already; kept so no later reorder can bring the crash back. */
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        port_log_always("SSB64: SDL video init failed: %s\n", SDL_GetError());
#endif
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        port_log_always("SSB64: no audio (%s)\n", SDL_GetError());
        return;
    }
    memset(&want, 0, sizeof want);
    want.freq = HH_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = HH_AUDIO_CHANNELS;
    want.samples = OUT_BLOCK;
    want.callback = audio_callback;
    if (SDL_OpenAudio(&want, &have) != 0) {
        port_log_always("SSB64: SDL_OpenAudio failed: %s\n", SDL_GetError());
        return;
    }
    if (have.freq != HH_AUDIO_RATE || have.channels != HH_AUDIO_CHANNELS || have.format != AUDIO_S16SYS) {
        port_log_always("SSB64: audio device gave %d Hz %d ch fmt %x; expected 32000/2/s16\n",
                        have.freq, have.channels, have.format);
#if defined(MIYOO)
        /* The Mini's dsp gives 32000/2/s16 (the other ports' device logs); any other SDL build that grants
         * something else gets SDL's own conversion (no obtained spec), so the game's 32 kHz stays in time. */
        SDL_CloseAudio();
        if (SDL_OpenAudio(&want, NULL) != 0) {
            port_log_always("SSB64: SDL_OpenAudio (converted) failed: %s\n", SDL_GetError());
            return;
        }
        port_log_always("SSB64: audio reopened at 32000/2/s16, SDL converts\n");
#endif
    }
#if defined(MIYOO)
    else port_log_always("SSB64: audio %d Hz, %d ch, %d-frame buffer\n", have.freq, have.channels, have.samples);
#endif
    {
        const char *e = getenv("SSB64_DECLICK");
        sDeclick = !(e && e[0] == '0');
        hhd_init(&sDk, HH_AUDIO_RATE, DECLICK_CUSHION);
    }
    sOpened = 1;
    SDL_PauseAudio(0);
}

#include <stdio.h>
#include <stdlib.h>
/* SSB64_PCM=<file> (debug): append every pushed frame as raw s16le stereo. */
static FILE *pcm_dump(void)
{
    static int sInit;
    static FILE *sFile;
    if (!sInit) {
        const char *p = getenv("SSB64_PCM");
        sInit = 1;
        if (p) sFile = fopen(p, "wb");
    }
    return sFile;
}

void hh_audio_push(const int16_t *samples, int frames)
{
    FILE *dump;
    if (frames <= 0) return;
    if ((dump = pcm_dump()) != NULL) fwrite(samples, 4, (size_t)frames, dump);
    if (!sOpened) {
        /* No device (headless tests): still measure the level. */
        for (int i = 0; i < frames * 2; i++) {
            int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > sPeak) sPeak = v;
        }
        return;
    }
    if (sWrite - sRead + (uint32_t)frames > RING_FRAMES) {
        /* The device fell behind (or is paused): drop the oldest audio so
         * the game never blocks on sound. */
        SDL_LockAudio();
        sRead = sWrite + (uint32_t)frames - RING_FRAMES;
        SDL_UnlockAudio();
    }
    for (int i = 0; i < frames; i++) {
        uint32_t idx = (sWrite + (uint32_t)i) % RING_FRAMES;
        int l = samples[i * 2], r = samples[i * 2 + 1];
        sRing[idx * 2] = (int16_t)l;
        sRing[idx * 2 + 1] = (int16_t)r;
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > sPeak) sPeak = l;
        if (r > sPeak) sPeak = r;
    }
    __sync_synchronize();
    sWrite += (uint32_t)frames;
}

uint32_t hh_audio_underruns(void)
{
    return sUnderruns;
}

/* PERF line: the least the ring held at a callback that played since the last call (0: none played),
 * then reset; and the times the sound came back after a gap (hh_declick.h). */
uint32_t hh_audio_take_ring_min(void)
{
    const uint32_t m = sRingMin;
    sRingMin = UINT32_MAX;
    return m == UINT32_MAX ? 0 : m;
}
uint32_t hh_audio_gaps(void) { return sDk.gaps; }

/* The queue's aim (round 18, PM64 fb7ae20 / the lead's X19): the game mixed a fixed 1/60 s a tick
 * whatever was queued, so a tick later than the queue ran it dry (27-49 underruns in the 65 s device
 * bench). hh_audio_aim_frames() is how many frames the queue should hold after this tick's fill: the
 * longest recent gap between fills, a quarter more and one buffer, 50-150 ms, the longest gap decaying
 * by 1/64 a fill (about a second). Called once per audio tick (syAudioGetPortSampleCount), before the
 * fill; 0 = no device, keep the pace. SSB64_AUDIOAIM=<ms>: a fixed aim; =0: off (the old fixed pace). */
uint32_t hh_audio_aim_frames(uint32_t *queued)
{
    static uint32_t sMarkUs, sGapMaxUs;
    static int sFixedMs = -2;
    extern uint32_t port_time_us(void);
    const uint32_t now = port_time_us(), gap = sMarkUs ? now - sMarkUs : 0;
    uint32_t aim_us;
    if (sFixedMs == -2) { const char *e = getenv("SSB64_AUDIOAIM"); sFixedMs = e && e[0] ? atoi(e) : -1; }
    sMarkUs = now;
    sGapMaxUs -= sGapMaxUs >> 6;
    if (gap > sGapMaxUs && gap < 1000000) sGapMaxUs = gap;
    if (!sOpened || sFixedMs == 0) return 0;
    aim_us = sGapMaxUs + sGapMaxUs / 4 + 16000;
    aim_us = aim_us < 50000 ? 50000 : aim_us > 150000 ? 150000 : aim_us;
    if (sFixedMs > 0) aim_us = (uint32_t)sFixedMs * 1000u;
    *queued = sWrite - sRead;
    return (uint32_t)((uint64_t)HH_AUDIO_RATE * aim_us / 1000000u);
}

/* Peak level since the last call (PERF line), then reset. */
int hh_audio_take_peak(void)
{
    int p = sPeak;
    sPeak = 0;
    return p;
}

/* the kit's hooks (kit savestate.c, miyoo_menu.c). The game mixes on its own audio coroutine,
 * inside the main loop, and a snapshot or a restore runs on the main thread between ticks, so no
 * mixer ever runs on game memory in flux: there is no backstop mixer to park (DKR's way). The mix
 * lock is SDL's audio lock, so the callback thread waits out a restore. */
void port_audio_backstop_enable(int on) { (void)on; }
int port_audio_backstop_swap(int on) { (void)on; return 0; }
void port_audio_mix_lock(void) { if (sOpened) SDL_LockAudio(); }
void port_audio_mix_unlock(void)
{
    FILE *dump = pcm_dump();
    if (sFlushOnUnlock) {
        /* After a load the ring still holds up to 256 ms of the sound before it: drop it, so the
         * restored game's sound starts at once (and a test's PCM dump lines up at the mark). With the
         * declick, port_audio_out_gap(0) drops it once the card has faded out (k1.8). */
        sFlushOnUnlock = 0;
        if (!sDeclick || !sOpened) sRead = sWrite;
        if (dump != NULL) {
            fflush(dump);
            port_log_always("AUDIO savestate mark %ld (load)\n", ftell(dump));
        }
    } else if (dump != NULL) {
        fflush(dump);
        port_log_always("AUDIO savestate mark %ld (save)\n", ftell(dump));
    }
    if (sOpened) SDL_UnlockAudio();
}
void hh_audio_flush_after_load(void) { sFlushOnUnlock = 1; }
void port_audio_out_set_muted(int muted) { sMuted = muted; }

/* The kit's savestate.c around a load (k1.8): 1 before it, the card fades out and holds; 0 after it (the
 * mix lock, SDL's audio lock here, already released), once a whole callback has run under the hold
 * (hhd_settle, at most 50 ms: a load from the page cache can take under a millisecond, and dropping the
 * queue under a buffer still at full gain was a step), the old timeline's queue is dropped with the
 * callback locked out, and the restored game's sound fades in once the cushion is back. */
static void gap_sleep_ms(uint32_t ms) { SDL_Delay(ms); }
void port_audio_out_gap(int begin)
{
    if (!sDeclick || !sOpened) return;
    if (begin) {
        hhd_hold(&sDk);
        return;
    }
    hhd_settle(&sDk, gap_sleep_ms, 50);
    SDL_LockAudio();
    sRead = sWrite;
    hhd_flush(&sDk);
    sDk.hold = 0;
    SDL_UnlockAudio();
}
