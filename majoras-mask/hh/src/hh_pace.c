/* Even frame pacing on the panel's own refresh (ALPHA6).
 *
 * MM's gameplay runs at a divisor of 3 (20 fps on the N64's 60 Hz), so each frame should stay on screen for exactly
 * 3 refreshes. The Mini's panel refreshes at about 59.74 Hz (16.74 ms: the pan waits for it, vf1-miyoo
 * tools/miyoo/panel_fb.h), so 3 refreshes are 50.2 ms, while the scheduler paced frames on a 50.000 ms clock and the
 * present thread flipped at each frame's tick. The two drifted 0.2 ms a frame apart; every ~80 frames a frame
 * missed its refresh and stayed up for 4 (ALPHA4's device log: "shown max 66.9" in 73 of 86 windows, 222 frames
 * over 60 ms): judder at a steady 20 fps.
 *
 * Here the refresh is the clock:
 *  - the present thread pans only at the divisor-th refresh after the last pan: it counts refreshes
 *    (FBIO_WAITFORVSYNC, or the measured period when the driver has none), then flips; the pan itself waits for
 *    the next refresh, so the frame shows exactly divisor refreshes after the last one. A frame ready late shows
 *    at the next refresh, and the count starts again from there (one refresh late, never two dropped);
 *  - the scheduler (hh_sched.c) paces the game at divisor x the measured refresh period and pulls its deadlines
 *    gently onto the refreshes it was shown on, so the game neither runs ahead (frames queued) nor behind;
 *  - the present thread has its own buffers (the staged copy, the panel's back page): the game thread never
 *    waits for a refresh.
 * Every 10 s one line: how long frames stayed on screen, in refreshes.
 *     pace: 10.0 s, 200 frames, divisor 3: 2:0 3:200 4:0 other:0, late 0; refresh 16.742 ms (vsync)
 *
 * MM_PACE=0: the ALPHA5 pacing (A/B). MM_SIMVSYNC=<Hz> (qemu, tests): a simulated panel at that rate (the flip
 * and the refresh wait sleep to its next refresh). MM_PACE_LOG=<s>: the line's period (default 10). */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, uint32_t)
#endif

extern uint32_t port_time_us(void);
extern int sched_yield(void);

enum { PACE_OFF = 0, PACE_WAIT = 1, PACE_TIMED = 2, PACE_SIM = 3 };

static int sMode = -1;           /* PACE_*: the refresh source (PACE_OFF: none known) */
static int sOn;                  /* pacing on (MM_PACE != 0) */
static int sFd = -1;             /* /dev/fb0 for FBIO_WAITFORVSYNC */
static double sPeriod = 16666.7; /* us, measured */
static int sPeriodLocked;        /* the period has been measured */
static double sSimPeriod;        /* MM_SIMVSYNC */
static uint32_t sSimT0;
static volatile int sDivisor = 3;
static uint32_t sLastFlip;       /* us: the refresh the last frame went up at */
static int sHaveFlip;
static uint32_t sFlipSeen;       /* flips measured for the period */
/* the histogram since the last line */
static uint32_t sHist[6]; /* 1, 2, 3, 4, 5+, unused */
static uint32_t sLate, sFrames, sLogT0;
static uint32_t sLogEvery = 10000000u;
/* the guard (hh_pace_flipped) */
static uint32_t sSlotT0;         /* us: the present thread entered the pacing wait */
static double sGuardDt, sGuardHeld;
static int sGuardN;
static int sSimWaits = 1;        /* MM_SIMVSYNC_WAITS: refreshes a simulated vsync wait takes (tests) */

static void pace_init(void) {
    if (sMode >= 0) return;
    const char *e = getenv("MM_PACE");
    const char *sim = getenv("MM_SIMVSYNC");
    const char *le = getenv("MM_PACE_LOG");
    if (le != NULL && atoi(le) > 0) sLogEvery = (uint32_t) atoi(le) * 1000000u;
    sOn = !(e != NULL && e[0] == '0');
    if (!sOn) fprintf(stderr, "pace: off (MM_PACE=0): flips at render completion, as ALPHA5; the histogram still kept\n");
    if (sim != NULL && atof(sim) > 1.0) {
        sSimPeriod = 1e6 / atof(sim);
        sPeriod = sSimPeriod;
        sSimT0 = port_time_us();
        sMode = PACE_SIM;
        { const char *w = getenv("MM_SIMVSYNC_WAITS"); if (w != NULL && atoi(w) > 1) sSimWaits = atoi(w); }
        if (sSimWaits > 1) sPeriod = sSimPeriod * sSimWaits; /* what the old startup check measured on such a driver */
        fprintf(stderr, "pace: %s, simulated panel at %.3f Hz (MM_SIMVSYNC), vsync wait %d refreshes\n", sOn ? "on" : "off",
                atof(sim), sSimWaits);
        return;
    }
    sFd = open("/dev/fb0", O_RDWR);
    if (sFd >= 0) {
        uint32_t arg = 0;
        const uint32_t t0 = port_time_us();
        if (ioctl(sFd, FBIO_WAITFORVSYNC, &arg) == 0) {
            /* a real wait: three in a row, each one refresh of a 50-70 Hz panel apart. One that returns at once
             * waits for nothing; one that takes two refreshes or a timeout (1.0.4 and before took up to 40 ms) would
             * hold every frame twice as long: slow motion on the Mini Plus and the Flip (MM_PACE=0 fixed it) */
            uint32_t t[4];
            int ok = 1;
            t[0] = port_time_us();
            for (int i = 1; i < 4; i++) {
                ioctl(sFd, FBIO_WAITFORVSYNC, &arg);
                t[i] = port_time_us();
                if (t[i] - t[i - 1] < 14000 || t[i] - t[i - 1] > 20000) ok = 0;
            }
            if (ok) {
                sMode = PACE_WAIT;
                sPeriod = (double) (t[3] - t[0]) / 3.0;
                fprintf(stderr, "pace: %s, FBIO_WAITFORVSYNC works (%.2f ms, then %.2f %.2f %.2f ms)\n", sOn ? "on" : "off",
                        (t[0] - t0) / 1000.0, (t[1] - t[0]) / 1000.0, (t[2] - t[1]) / 1000.0, (t[3] - t[2]) / 1000.0);
                return;
            }
            fprintf(stderr, "pace: FBIO_WAITFORVSYNC not a 14-20 ms refresh (%.2f %.2f %.2f ms): not used\n",
                    (t[1] - t[0]) / 1000.0, (t[2] - t[1]) / 1000.0, (t[3] - t[2]) / 1000.0);
        }
        close(sFd);
        sFd = -1;
    }
    sMode = PACE_TIMED; /* the period and the phase from the flips (the pan waits for the refresh) */
    fprintf(stderr, "pace: %s, no FBIO_WAITFORVSYNC: refreshes timed from the flips\n", sOn ? "on" : "off");
}

int hh_pace_on(void) {
    pace_init();
    return sOn && sMode != PACE_OFF;
}

/* A precise wait to an absolute time: sleeps cost a kernel tick (~9.5 ms on the Mini), so the last 12 ms spin
 * (yielding). */
static void wait_until(uint32_t t) {
    for (;;) {
        const int32_t left = (int32_t) (t - port_time_us());
        if (left <= 0) return;
        if (left > 12000) usleep((useconds_t) (left - 12000));
        else sched_yield();
    }
}

/* the simulated panel's next refresh after now */
static uint32_t sim_next(void) {
    const uint32_t now = port_time_us();
    const double k = floor((double) (uint32_t) (now - sSimT0) / sSimPeriod) + 1.0;
    return sSimT0 + (uint32_t) (k * sSimPeriod);
}

/* one refresh: returns its time */
static uint32_t wait_refresh(void) {
    if (sMode == PACE_SIM) {
        uint32_t t = 0;
        for (int i = 0; i < sSimWaits; i++) {
            t = sim_next();
            wait_until(t);
        }
        return t;
    }
    if (sMode == PACE_WAIT) {
        uint32_t arg = 0;
        ioctl(sFd, FBIO_WAITFORVSYNC, &arg);
        return port_time_us();
    }
    /* timed: the next refresh on the measured grid */
    const uint32_t now = port_time_us();
    const double k = floor((double) (int32_t) (now - sLastFlip) / sPeriod) + 1.0;
    const uint32_t t = sLastFlip + (uint32_t) (k * sPeriod);
    wait_until(t);
    return t;
}

void hh_pace_set_divisor(int d) {
    if (d >= 1 && d <= 6) sDivisor = d;
}

/* The present thread, before its flip: wait until the flip lands on the divisor-th refresh after the last one. */
void hh_pace_wait_slot(void) {
    if (!hh_pace_on() || !sHaveFlip) return;
    const int d = sDivisor;
    sSlotT0 = port_time_us();
    if (sMode == PACE_TIMED) {
        /* the pan waits for the next refresh: issue it in the middle of the refresh before the slot */
        const uint32_t t = sLastFlip + (uint32_t) (((double) d - 0.5) * sPeriod);
        if ((int32_t) (t - port_time_us()) > 0) wait_until(t);
        return;
    }
    for (;;) {
        /* refreshes since the last flip, counting one that is less than 0.5 ms away as passed (a pan issued then
         * would race it) */
        const int32_t since = (int32_t) (port_time_us() - sLastFlip);
        const int passed = (int) floor(((double) since + 500.0) / sPeriod);
        if (passed >= d - 1) return;
        wait_refresh();
    }
}

static void pace_log(uint32_t now) {
    const double secs = (double) (uint32_t) (now - sLogT0) / 1e6;
    fprintf(stderr, "pace: %.1f s, %u frames, divisor %d: 2:%u 3:%u 4:%u other:%u, late %u; refresh %.3f ms (%s)\n",
            secs, (unsigned) sFrames, sDivisor, (unsigned) sHist[1], (unsigned) sHist[2], (unsigned) sHist[3],
            (unsigned) (sHist[0] + sHist[4]), (unsigned) sLate, sPeriod / 1000.0,
            sMode == PACE_SIM ? "simulated" : sMode == PACE_WAIT ? "vsync" : "timed from the flips");
    memset(sHist, 0, sizeof(sHist));
    sLate = 0;
    sFrames = 0;
    sLogT0 = now;
}

/* The present thread, after its flip (t0 before it, t1 after): the time the frame went up, the period, the
 * histogram. A simulated panel's flip waits for its refresh here. */
void hh_pace_flipped(uint32_t t0, uint32_t t1) {
    pace_init(); /* the histogram (and a simulated panel's wait) with pacing on or off */
    uint32_t t = t1;
    if (sMode == PACE_SIM) { t = sim_next(); wait_until(t); } /* the pan waits for the refresh */
    (void) t0;
    if (sHaveFlip) {
        const double dt = (double) (uint32_t) (t - sLastFlip);
        int n = (int) floor(dt / sPeriod + 0.5);
        if (n < 1) n = 1;
        /* the period: flips that landed on a refresh (within 1.5 ms of the grid) */
        if (sMode != PACE_SIM && n <= 8 && fabs(dt - n * sPeriod) < 1500.0) {
            const double p = dt / n;
            if (p > 14000.0 && p < 20000.0) {
                sPeriod += (p - sPeriod) / (sFlipSeen < 32 ? 4.0 : 32.0);
                if (++sFlipSeen >= 16) sPeriodLocked = 1;
            }
        }
        sHist[n >= 5 ? 4 : n == 1 ? 0 : n - 1]++;
        if (n > sDivisor) sLate++;
        sFrames++;
        if (sOn && sSlotT0 != 0) {
            /* The guard (1.0.5): the game waits for the present thread, so pacing that holds frames too long slows
             * the game itself (slow motion on the Mini Plus and the Flip in 1.0.4). Every 30 frames: frames shown
             * at under 80% of the target rate, with the pacing wait and the flip taking over half a target frame
             * (a slow render skips the wait, so it can't trip this) or a refresh outside 14-20 ms (the scheduler
             * paces at divisor x that), and pacing goes off for the session, as MM_PACE=0. The target is a nominal
             * 16.74 ms refresh, not the measured one, which may be the thing that's wrong. */
            const double target = sDivisor * 16740.0;
            sGuardDt += dt;
            sGuardHeld += (double) (uint32_t) (t - sSlotT0);
            if (++sGuardN >= 30) {
                const double mdt = sGuardDt / sGuardN, mheld = sGuardHeld / sGuardN;
                if (mdt > target * 1.25 && (mheld > target * 0.5 || sPeriod < 14000.0 || sPeriod > 20000.0)) {
                    sOn = 0;
                    fprintf(stderr, "pace: off for this session: frames shown every %.1f ms (target %.1f), %.1f ms of it in "
                            "the pacing wait and the flip; refresh %.3f ms (%s)\n", mdt / 1000.0, target / 1000.0,
                            mheld / 1000.0, sPeriod / 1000.0,
                            sMode == PACE_SIM ? "simulated" : sMode == PACE_WAIT ? "vsync" : "timed from the flips");
                }
                sGuardDt = sGuardHeld = 0.0;
                sGuardN = 0;
            }
        }
    } else {
        sLogT0 = t;
    }
    sLastFlip = t;
    sHaveFlip = 1;
    if ((uint32_t) (t - sLogT0) >= sLogEvery) pace_log(t);
}

/* The scheduler: the frame period (divisor x the measured refresh), or its own when not measured yet. */
uint32_t hh_pace_frame_usec(int divisor, uint32_t fallback) {
    if (!hh_pace_on() || divisor < 1) return fallback;
    if (sMode != PACE_SIM && !sPeriodLocked) return fallback;
    return (uint32_t) (divisor * sPeriod + 0.5);
}

/* The scheduler: its next deadline pulled an eighth of the way onto the refresh grid of the last flip (at most
 * 1 ms a frame), so the game's clock and the panel's cannot drift apart. */
void hh_pace_adjust_deadline(uint32_t *deadline, int divisor) {
    if (!hh_pace_on() || !sHaveFlip || divisor < 1) return;
    if (sMode != PACE_SIM && !sPeriodLocked) return;
    const double frame = divisor * sPeriod;
    const double off = 500.0; /* just after the refresh the frame went up at */
    double e = fmod((double) (int32_t) (*deadline - sLastFlip) - off, frame);
    if (e > frame / 2) e -= frame;
    if (e < -frame / 2) e += frame;
    double c = e / 8.0;
    if (c > 1000.0) c = 1000.0;
    if (c < -1000.0) c = -1000.0;
    *deadline -= (uint32_t) (int32_t) c;
}
