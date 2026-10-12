/* declick_test.c -- hh_declick.h against the sink it replaces (k1.8, statefix B). A model of a
 * port's audio sink (a ring the game fills in bursts, a callback that drains 512 frames at a time
 * at 32 kHz) plays a 440 Hz sine, once through the old callback and once through the declicked
 * one, in the cases that clicked:
 *  1. steady play: after the first fade in, the output is the old output bit for bit;
 *  2. the menu: muted for 2 s while the ring drains, unmuted, then slow frames: no step;
 *  3. a load: held (the read itself instant, as from the page cache), the queue dropped, the restored
 *     game's stream in another phase: no step;
 *  4. a ring that runs dry between late bursts: no step (old: a click at each edge);
 *  5. a producer that stops below the cushion: playback starts within HHD_STALL_MS;
 *  6. the fade in reaches full gain in HHD_IN_MS.
 * A "step" is a jump between two output samples larger than twice the sine's own largest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hh_declick.h"

#define RATE 32000
#define RING 16384
#define BLOCK 512
#define AMP 20000.0

static int sFails;
#define CHECK(c, ...) do { if (!(c)) { sFails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct {
    int16_t ring[RING * 2];
    uint32_t rd, wr;
    int muted, declick;
    HhDeclick dk;
    double phase;       /* the game's stream: radians */
    int16_t last;       /* the last sample the card played (left) */
    int have_last;
    long steps, frames_out, nonzero_at;
    int16_t *capture;   /* every sample the card played (left), when set */
    long cap_n, cap_max;
} Sink;

static void sink_init(Sink *s, int declick, uint32_t cushion) {
    memset(s, 0, sizeof(*s));
    s->declick = declick;
    hhd_init(&s->dk, RATE, cushion);
    s->nonzero_at = -1;
}

static void produce(Sink *s, uint32_t frames) {
    for (uint32_t i = 0; i < frames && s->wr - s->rd < RING; i++) {
        const int16_t v = (int16_t) lrint(AMP * sin(s->phase));
        const uint32_t k = s->wr % RING;
        s->ring[k * 2] = v;
        s->ring[k * 2 + 1] = (int16_t) -v;
        s->wr++;
        s->phase += 2.0 * M_PI * 440.0 / RATE;
    }
}

static void callback(Sink *s) {
    int16_t out[BLOCK * 2];
    const uint32_t want = BLOCK, have = s->wr - s->rd;
    uint32_t n, i;
    if (s->declick && !hhd_ready(&s->dk, have, want)) {
        memset(out, 0, sizeof(out));
        n = 0;
    } else {
        n = have < want ? have : want;
        for (i = 0; i < n; i++) {
            const uint32_t k = (s->rd + i) % RING;
            out[i * 2] = s->ring[k * 2];
            out[i * 2 + 1] = s->ring[k * 2 + 1];
        }
        s->rd += n;
        for (i = n; i < want; i++) out[i * 2] = out[i * 2 + 1] = 0;
        if (s->declick) hhd_apply(&s->dk, out, n, want, s->muted || s->dk.hold);
        else if (s->muted) memset(out, 0, sizeof(out));
    }
    const int lim = (int) (2.0 * AMP * 2.0 * M_PI * 440.0 / RATE);
    for (i = 0; i < want; i++) {
        const int16_t v = out[i * 2];
        if (s->have_last && abs(v - s->last) > lim) s->steps++;
        if (abs(out[i * 2 + 1] + v) > 1) { sFails++; fprintf(stderr, "FAIL: the channels are faded apart\n"); return; }
        if (v != 0 && s->nonzero_at < 0) s->nonzero_at = s->frames_out + i;
        s->last = v;
        s->have_last = 1;
        if (s->capture && s->cap_n < s->cap_max) s->capture[s->cap_n++] = v;
    }
    s->frames_out += want;
}

/* Runs ms of time: the callback every BLOCK frames; the game pushes frame_ms of audio every frame_ms
 * (or every late_ms for the first `late` frames), only while `game` is set. */
typedef struct { double frame_ms, late_ms; int late; int game; } Pace;
static void run(Sink *s, double ms, Pace *p, double *t_game, double *t_cb, double *t) {
    const double end = *t + ms;
    while (*t < end) {
        const double next_cb = *t_cb, next_game = p->game ? *t_game : 1e18;
        if (next_cb <= next_game) {
            *t = next_cb;
            callback(s);
            *t_cb += BLOCK * 1000.0 / RATE;
        } else {
            *t = next_game;
            produce(s, (uint32_t) (RATE * p->frame_ms / 1000.0));
            *t_game += p->late > 0 ? p->late_ms : p->frame_ms;
            if (p->late > 0) p->late--;
        }
    }
}

/* The old sink and the new through one scenario; returns the steps of each. */
typedef void (*Scenario)(Sink *s);
static void both(Scenario f, long *old_steps, long *new_steps) {
    Sink *a = calloc(1, sizeof(Sink)), *b = calloc(1, sizeof(Sink));
    sink_init(a, 0, 2400);
    sink_init(b, 1, 2400);
    f(a);
    f(b);
    *old_steps = a->steps;
    *new_steps = b->steps;
    free(a);
    free(b);
}

static void sc_menu(Sink *s) {
    double t = 0, tg = 0, tc = 0;
    Pace p = { 33.3, 0, 0, 1 };
    run(s, 2000, &p, &tg, &tc, &t);          /* playing */
    s->muted = 1; p.game = 0;                /* the menu: the game stops, the ring drains */
    run(s, 2000, &p, &tg, &tc, &t);
    s->muted = 0; p.game = 1; tg = t; p.late = 8; p.late_ms = 90; /* resume: slow frames first */
    run(s, 3000, &p, &tg, &tc, &t);
}

static void sc_load(Sink *s) {
    double t = 0, tg = 0, tc = 0;
    Pace p = { 33.3, 0, 0, 1 };
    run(s, 2000, &p, &tg, &tc, &t);
    if (s->declick) hhd_hold(&s->dk);        /* port_audio_out_gap(1); the read itself takes no time (page cache) */
    p.game = 0;
    if (s->declick) {                        /* port_audio_out_gap(0): hhd_settle, then the flush */
        while (s->dk.calls - s->dk.hold_calls < 2) run(s, 1, &p, &tg, &tc, &t);
        s->rd = s->wr; hhd_flush(&s->dk); s->dk.hold = 0;
    } else {
        s->rd = s->wr;                       /* the old sink: the queue simply continued from elsewhere */
    }
    s->phase += 2.1;                         /* the restored game's stream: another phase */
    p.game = 1; tg = t; p.late = 6; p.late_ms = 70;
    run(s, 3000, &p, &tg, &tc, &t);
}

static void sc_dry(Sink *s) {
    double t = 0, tg = 0, tc = 0;
    Pace p = { 33.3, 0, 0, 1 };
    run(s, 1000, &p, &tg, &tc, &t);
    p.late = 40; p.late_ms = 55;             /* a heavy scene: the ring runs dry again and again */
    run(s, 4000, &p, &tg, &tc, &t);
}

static uint32_t sSlept;
static void sleep_none(uint32_t ms) { sSlept += ms; }

/* 8. a producer that pushes whole buffers and falls behind: the ring empties exactly at a buffer's end */
static void sc_exact(Sink *s) {
    double t = 0, tg = 0, tc = 0;
    Pace p = { BLOCK * 1000.0 / RATE, 0, 0, 1 };  /* one 512-frame buffer each 16 ms: exact multiples */
    run(s, 1000, &p, &tg, &tc, &t);
    p.late = 30; p.late_ms = 3 * BLOCK * 1000.0 / RATE; /* then one buffer every 48 ms */
    run(s, 2000, &p, &tg, &tc, &t);
}

int main(void) {
    long o, n;

    /* 1. steady play is untouched once faded in */
    {
        Sink *a = calloc(1, sizeof(Sink)), *b = calloc(1, sizeof(Sink));
        static int16_t ca[RATE * 4], cb[RATE * 4];
        double ta = 0, tga = 0, tca = 0, tb = 0, tgb = 0, tcb = 0;
        Pace pa = { 16.7, 0, 0, 1 }, pb = pa;
        sink_init(a, 0, 2400);
        sink_init(b, 1, 2400);
        produce(a, 4000); produce(b, 4000);  /* both start primed with the same audio */
        a->capture = ca; a->cap_max = RATE * 4;
        b->capture = cb; b->cap_max = RATE * 4;
        run(a, 3000, &pa, &tga, &tca, &ta);
        run(b, 3000, &pb, &tgb, &tcb, &tb);
        const long fade = RATE * HHD_IN_MS / 1000 + 1;
        long diff = 0;
        for (long i = fade; i < a->cap_n && i < b->cap_n; i++) diff += ca[i] != cb[i];
        CHECK(a->cap_n == b->cap_n && a->cap_n > RATE * 2, "capture lengths %ld %ld", a->cap_n, b->cap_n);
        CHECK(diff == 0, "steady play differs from the old sink in %ld samples", diff);
        CHECK(b->steps == 0 && a->steps <= 1, "steady steps old %ld new %ld", a->steps, b->steps);
        free(a); free(b);
    }

    /* 2-4. the cases that clicked */
    both(sc_menu, &o, &n);
    printf("declick: menu  steps old %ld new %ld\n", o, n);
    CHECK(o > 0 && n == 0, "menu: old %ld new %ld", o, n);
    both(sc_load, &o, &n);
    printf("declick: load  steps old %ld new %ld\n", o, n);
    CHECK(o > 0 && n == 0, "load: old %ld new %ld", o, n);
    both(sc_dry, &o, &n);
    printf("declick: dry   steps old %ld new %ld\n", o, n);
    CHECK(o > 0 && n == 0, "dry: old %ld new %ld", o, n);

    both(sc_exact, &o, &n);
    printf("declick: exact steps old %ld new %ld\n", o, n);
    CHECK(o > 0 && n == 0, "exact: old %ld new %ld", o, n);

    /* 5. a producer that stops below the cushion still gets played */
    {
        Sink *s = calloc(1, sizeof(Sink));
        double t = 0, tg = 0, tc = 0;
        Pace p = { 33.3, 0, 0, 0 };
        sink_init(s, 1, 2400);
        produce(s, 1000);
        run(s, 300, &p, &tg, &tc, &t);
        const long limit = RATE * HHD_STALL_MS / 1000 + 2 * BLOCK;
        CHECK(s->nonzero_at >= 0 && s->nonzero_at <= limit, "stalled producer first sound at %ld (limit %ld)", s->nonzero_at, limit);
        CHECK(s->rd == s->wr, "stalled producer's frames not played (%u left)", s->wr - s->rd);
        free(s);
    }

    /* 6. the fade in: from 0 to full gain in HHD_IN_MS */
    {
        HhDeclick d;
        static int16_t buf[BLOCK * 2];
        hhd_init(&d, RATE, 100);
        CHECK(hhd_ready(&d, 200, BLOCK) == 1, "not primed at the cushion");
        CHECK(d.gain == 0, "fade in does not start at 0 (%d)", d.gain);
        for (int i = 0; i < BLOCK * 2; i++) buf[i] = 10000;
        hhd_apply(&d, buf, BLOCK, BLOCK, 0);
        const int full = RATE * HHD_IN_MS / 1000 + 1;
        CHECK(buf[0] < 100 && buf[(full + 1) * 2] == 10000 && d.gain == HHD_ONE, "fade in: %d .. %d gain %d", buf[0], buf[(full + 1) * 2], d.gain);
        for (int i = 1; i < BLOCK; i++) CHECK(buf[i * 2] >= buf[(i - 1) * 2], "fade in not monotonic at %d", i);
        /* muted: down to silence within HHD_OUT_MS, still consuming */
        for (int i = 0; i < BLOCK * 2; i++) buf[i] = 10000;
        hhd_apply(&d, buf, BLOCK, BLOCK, 1);
        const int out = RATE * HHD_OUT_MS / 1000 + 1;
        CHECK(buf[(out + 1) * 2] == 0 && buf[(BLOCK - 1) * 2] == 0 && d.gain == 0, "fade out: %d gain %d", buf[(out + 1) * 2], d.gain);
        CHECK(d.primed == 1, "muting re-primed a ring that had sound");
    }

    /* 7. hhd_settle: returns at once without a hold, times out when the card isn't calling back */
    {
        HhDeclick d;
        hhd_init(&d, RATE, 100);
        CHECK(hhd_settle(&d, sleep_none, 50) == 1, "settle waited without a hold");
        hhd_hold(&d);
        sSlept = 0;
        CHECK(hhd_settle(&d, sleep_none, 50) == 0 && sSlept == 50, "settle with no callbacks: %u ms", sSlept);
        d.calls += 2;
        CHECK(hhd_settle(&d, sleep_none, 50) == 1, "settle after two callbacks");
    }

    printf("declick: %s\n", sFails ? "FAILED" : "ok");
    return sFails ? 1 : 0;
}
