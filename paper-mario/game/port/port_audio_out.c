/* The native port's sound output (M2). Host side. The game's mixed frames (port_audio.c) go into a ring the device's
 * SDL audio callback drains (stereo s16 at the game's 32 kHz); the ring's fill decides how many samples the next
 * frame makes (AlFrameSize or AlMinFrameSize, as nusys did from the AI's buffer), aiming at ~50 ms queued. The
 * desktop proxy has no device: it makes 32000/60 samples a retrace on average and writes them to PM64_WAV=<file>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#ifdef PM64_SDL12
#include <SDL/SDL.h>
#endif

#define RING (1u << 15) /* stereo frames (~1 s at 32 kHz) */
static int16_t sRing[RING * 2];
static volatile uint32_t sHead, sTail; /* frames written / read */
static uint32_t sRate;
static int sOpen;
static FILE *sWav;
static uint32_t sWavFrames;
static uint32_t sDue; /* desktop: frames owed so far, in 1/60 steps */
/* The queue's aim grows with the time between top-ups (port_audio_out_mark, once per retrace's fill): 50 ms, or
 * the longest recent gap plus a margin, up to 150 ms. At a flat 50 ms every game frame over ~60 ms left the device
 * with nothing to play (the prologue's castle scenes: frames of 40-70 ms, the user heard the sound skip); a deeper
 * queue all the time would put every sound effect late. The longest gap decays by 1/64 a top-up. */
static uint32_t sMarkUs, sGapMaxUs;
uint32_t port_audio_out_underruns; /* callbacks that ran dry (the PERF line) */
static volatile int sMuted; /* the kit's menu: silence while it is open (and through EXIT GAME's save) */
void port_audio_out_set_muted(int muted) { sMuted = muted; }

#ifdef PM64_SDL12
static void callback(void *user, Uint8 *stream, int len) {
    (void) user;
    int16_t *out = (int16_t *) stream;
    if (sMuted) { memset(stream, 0, (size_t) len); return; }
    uint32_t want = (uint32_t) len / 4, have = sHead - sTail, n = want < have ? want : have;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t i = (sTail + k) & (RING - 1);
        out[k * 2] = sRing[i * 2];
        out[k * 2 + 1] = sRing[i * 2 + 1];
    }
    sTail += n;
    if (n < want) { memset(out + n * 2, 0, (want - n) * 4); port_audio_out_underruns++; } /* underrun: silence */
}
#endif

static void wav_header(FILE *f, uint32_t frames) {
    const uint32_t bytes = frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    const uint32_t v[] = { 36 + bytes, 0x45564157u /* WAVE */, 0x20746D66u /* "fmt " */, 16, 0x00020001u /* PCM, 2 ch */,
                           sRate, sRate * 4, 0x00100004u /* align 4, 16 bits */, 0x61746164u /* data */, bytes };
    memcpy(h + 4, v, sizeof v);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static void wav_close(void) {
    if (!sWav) return;
    wav_header(sWav, sWavFrames);
    fclose(sWav);
    sWav = NULL;
}

void port_audio_out_init(uint32_t rate) {
    sRate = rate;
#ifdef PM64_SDL12
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = (int) rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = callback;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 && SDL_OpenAudio(&want, &have) == 0) {
        sOpen = 1;
        port_log("audio out: %d Hz, %d channels, %d-sample buffer\n", have.freq, have.channels, have.samples);
        SDL_PauseAudio(0);
    } else {
        port_log("audio out: SDL audio failed: %s\n", SDL_GetError());
    }
#endif
    const char *w = getenv("PM64_WAV");
    if (w && w[0] && (sWav = fopen(w, "wb"))) { wav_header(sWav, 0); atexit(wav_close); }
}

/* once per retrace's fill (port_audio.c): the time since the last one sets how deep the queue aims */
void port_audio_out_mark(void) {
    const uint32_t now = port_time_us(), gap = sMarkUs ? now - sMarkUs : 0;
    sMarkUs = now;
    sGapMaxUs -= sGapMaxUs >> 6;
    if (gap > sGapMaxUs && gap < 1000000) sGapMaxUs = gap;
}

/* samples for the next audio frame: 0 when enough is queued */
int32_t port_audio_out_want(int32_t frame, int32_t min_frame) {
    if (sOpen) {
        uint32_t aim_us = sGapMaxUs + sGapMaxUs / 4 + 16000; /* the longest recent gap, a quarter more, one buffer */
        aim_us = aim_us < 50000 ? 50000 : aim_us > 150000 ? 150000 : aim_us;
        static int sFixedMs = -1; /* PM64_AUDIOAIM=<ms>: a fixed aim (50: as before), the A/B */
        if (sFixedMs < 0) { const char *e = getenv("PM64_AUDIOAIM"); sFixedMs = e && e[0] ? atoi(e) : 0; }
        if (sFixedMs > 0) aim_us = (uint32_t) sFixedMs * 1000u;
        const uint32_t queued = sHead - sTail, target = (uint32_t) ((uint64_t) sRate * aim_us / 1000000);
        if (queued >= target) return 0;
        return queued + (uint32_t) min_frame >= target ? min_frame : frame;
    }
    /* no device: keep pace with the retraces (port_audio_out_retrace counts them) */
    if (sDue < (uint32_t) min_frame * 60u) return 0;
    return sDue >= (uint32_t) frame * 60u ? frame : min_frame;
}

void port_audio_out_retrace(void) {
    if (!sOpen) sDue += sRate; /* in 1/60 units: sRate / 60 frames per retrace */
}

void port_audio_out_push(const int16_t *samples, int32_t frames) {
    if (!sOpen) sDue -= sDue >= (uint32_t) frames * 60u ? (uint32_t) frames * 60u : sDue;
    if (sWav) { fwrite(samples, 4, (size_t) frames, sWav); sWavFrames += (uint32_t) frames; }
    if (!sOpen) return;
    for (int32_t k = 0; k < frames; k++) {
        if (sHead - sTail >= RING) break; /* full: drop */
        const uint32_t i = sHead & (RING - 1);
        sRing[i * 2] = samples[k * 2];
        sRing[i * 2 + 1] = samples[k * 2 + 1];
        __sync_synchronize();
        sHead++;
    }
}
