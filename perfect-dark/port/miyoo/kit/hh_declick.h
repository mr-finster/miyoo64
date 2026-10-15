/* hh_declick.h -- the port kit k1.8 (statefix B): the card never steps between silence and the
 * middle of a waveform. Header only, for a port's audio sink: the SDL callback's side of its ring.
 *
 * Where the clicks came from:
 *  - the menu muted and unmuted the card from one sample to the next;
 *  - a load left the ring holding the old timeline's sound, and the restored game's followed it;
 *  - every time the ring ran dry (the slow frames after a load or the menu, while textures come
 *    back) the card stopped dead and started again on the next burst. A run of those is the short
 *    crackle (MK64 OPEN_ISSUES X47: 81-94 underruns in a resume window).
 *
 * The rules, applied by the callback (one HhDeclick per sink):
 *  - After any gap (the first sound, a dry ring, a flush) playback waits until the ring holds the
 *    port's cushion again (GoldenEye's "primed" rule, round 18), then fades in over HHD_IN_MS.
 *    The wait also ends when the ring has stopped growing for HHD_STALL_MS below the cushion: a
 *    producer that measures the queue (osAiGetLength) stops when it thinks it is full, so the
 *    cushion must never be more than it makes. The port picks a cushion below its steady fill.
 *  - Muting (the menu), a hold (a load in progress) and a ring that runs dry fade out over
 *    HHD_OUT_MS. A dry ring fades out the last frames it did have (a ring a buffer empties
 *    exactly counts as dry: hhd_ready records what the ring held).
 *  - At full gain the samples pass through untouched: steady play is unchanged, bit for bit.
 *
 * The port's side:
 *  - the callback: hhd_ready(&d, have) before taking frames (0: write silence, take nothing);
 *    then copy its n frames as before (muted or not), zero the rest, and hhd_apply(&d, out, n,
 *    want, muted || d.hold), which returns 1 when the ring ran dry (the port's underrun);
 *  - port_audio_out_gap(1) (the kit's savestate.c, before a load): hhd_hold(&d);
 *    port_audio_out_gap(0) (after it): hhd_settle(&d, SDL_Delay-like ms sleep) -- a load read from
 *    the page cache takes under a millisecond, so the hold may not have reached the callback yet,
 *    and dropping the queue under a card still playing at full gain was a step -- then, with the
 *    callback locked out (SDL_LockAudio), empty the ring, hhd_flush(&d), d.hold = 0.
 */
#ifndef HH_DECLICK_H
#define HH_DECLICK_H

#include <stdint.h>
#include <string.h>

#ifndef HHD_IN_MS
#define HHD_IN_MS 10
#endif
#ifndef HHD_OUT_MS
#define HHD_OUT_MS 3
#endif
#ifndef HHD_STALL_MS
#define HHD_STALL_MS 100
#endif
#define HHD_ONE 32768

typedef struct HhDeclick {
    int32_t gain;            /* the card's gain now, 0..HHD_ONE */
    int32_t in_step;         /* gain per frame while fading in */
    uint32_t out_frames;     /* frames a fade out takes */
    uint32_t cushion;        /* frames the ring must hold before playing after a gap */
    uint32_t stall_frames;   /* frames of no growth that end the wait */
    uint32_t last_have, stalled;
    int primed;              /* playing; else waiting for the cushion */
    volatile int hold;       /* a load in progress: fade out, stay silent */
    uint32_t gaps;           /* times the sound restarted after a gap (for a port's log) */
    volatile uint32_t calls; /* callbacks so far (hhd_ready counts them) */
    uint32_t hold_calls;     /* calls when the hold began */
    uint32_t seen;           /* the ring's frames at this callback (hhd_ready) */
} HhDeclick;

static inline void hhd_init(HhDeclick *d, uint32_t rate, uint32_t cushion_frames) {
    memset(d, 0, sizeof(*d));
    d->in_step = (int32_t) (HHD_ONE / (rate * HHD_IN_MS / 1000 > 0 ? rate * HHD_IN_MS / 1000 : 1));
    if (d->in_step < 1) d->in_step = 1;
    d->out_frames = rate * HHD_OUT_MS / 1000;
    if (d->out_frames < 1) d->out_frames = 1;
    d->cushion = cushion_frames;
    d->stall_frames = rate * HHD_STALL_MS / 1000;
}

/* The callback, before it takes any frames: 1 = play; 0 = write silence and take nothing yet. */
static inline int hhd_ready(HhDeclick *d, uint32_t have, uint32_t want) {
    d->calls++;
    d->seen = have;
    if (d->primed) return 1;
    if (have >= d->cushion) {
        d->primed = 1;
    } else if (have > 0 && have == d->last_have) {
        d->stalled += want;
        if (d->stalled >= d->stall_frames) d->primed = 1;
    } else {
        d->stalled = 0;
    }
    d->last_have = have;
    if (d->primed) {
        d->stalled = 0;
        d->gain = 0; /* fade in from silence */
        d->gaps++;
    }
    return d->primed;
}

/* The callback, after writing n real frames (stereo s16) and zeros for the rest of want. silent:
 * muted or held. Returns 1 when the ring ran dry (n < want while it was playing).
 * A ring this buffer empties exactly (it held want frames: GoldenEye's qemu card dump, one gap in
 * four runs) is treated as running dry too: the next callback would find nothing and stop a
 * full-gain buffer dead. It only happens while starving (every port's steady fill is several
 * buffers), so at worst a gap that was coming anyway starts one buffer early, faded. */
static inline int hhd_apply(HhDeclick *d, int16_t *out, uint32_t n, uint32_t want, int silent) {
    const int32_t target = silent ? 0 : HHD_ONE;
    const int ran_dry = n < want;
    const int dry = ran_dry || (n > 0 && n == want && d->seen == n);
    uint32_t fade_from = n; /* a dry ring: the last frames it had fade out from here */
    uint32_t i;
    if (dry) {
        const uint32_t k = d->out_frames < n ? d->out_frames : n;
        fade_from = n - k;
    }
    if (d->gain == HHD_ONE && target == HHD_ONE && !dry) return 0; /* steady play: untouched */
    for (i = 0; i < fade_from; i++) {
        if (d->gain < target) {
            d->gain += d->in_step;
            if (d->gain > target) d->gain = target;
        } else if (d->gain > target) {
            d->gain -= HHD_ONE / (int32_t) d->out_frames;
            if (d->gain < target) d->gain = target;
        }
        if (d->gain == HHD_ONE) continue;
        if (d->gain == 0) {
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
            continue;
        }
        out[i * 2] = (int16_t) ((out[i * 2] * d->gain) >> 15);
        out[i * 2 + 1] = (int16_t) ((out[i * 2 + 1] * d->gain) >> 15);
    }
    if (dry) {
        const uint32_t k = n - fade_from;
        const int32_t g0 = d->gain;
        for (i = 0; i < k; i++) { /* from the gain it had down to 0 at the last frame it has */
            const int32_t g = (int32_t) ((int64_t) g0 * (int64_t) (k - 1 - i) / (int64_t) k);
            out[(fade_from + i) * 2] = (int16_t) ((out[(fade_from + i) * 2] * g) >> 15);
            out[(fade_from + i) * 2 + 1] = (int16_t) ((out[(fade_from + i) * 2 + 1] * g) >> 15);
        }
        for (i = n; i < want; i++) out[i * 2] = out[i * 2 + 1] = 0;
        d->gain = 0;
        d->primed = 0; /* wait for the cushion again */
        d->last_have = 0;
        d->stalled = 0;
    }
    return ran_dry;
}

/* port_audio_out_gap(1): the card fades out at its next buffer and stays silent. */
static inline void hhd_hold(HhDeclick *d) {
    d->hold_calls = d->calls;
    d->hold = 1;
}

/* port_audio_out_gap(0), before the flush: waits until a whole callback has run under the hold (so
 * its fade out is in a buffer the card plays), at most max_ms. 1 = settled; 0 = the card isn't
 * calling back (closed, paused, a stuck driver): flush anyway. */
static inline int hhd_settle(HhDeclick *d, void (*sleep_ms)(uint32_t), uint32_t max_ms) {
    uint32_t waited = 0;
    while (d->hold && d->calls - d->hold_calls < 2) {
        if (waited >= max_ms) return 0;
        sleep_ms(1);
        waited++;
    }
    return 1;
}

/* The game thread, with the callback locked out, after the port emptied its ring (a load: what
 * was queued belongs to the old timeline). The next sound waits for the cushion and fades in. */
static inline void hhd_flush(HhDeclick *d) {
    d->gain = 0;
    d->primed = 0;
    d->last_have = 0;
    d->stalled = 0;
}

#endif
