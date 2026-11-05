/* miyoo_sleepsave: the resume state written as the Miyoo goes to sleep (see miyoo_sleepsave.h). The same file in
 * all ten games. */
#include "miyoo_sleepsave.h"
#if defined(MIYOO)
#if defined(__has_include)
#if __has_include("savestate.h")
#include "savestate.h"
#endif
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <SDL/SDL.h>

/* The log: stderr, or the port's own (-DMIYOO_SLEEPSAVE_PORT_LOG: port_log; F-Zero X, whose binary must not name
 * stderr, as for miyoo_clock.c). */
#if defined(MIYOO_SLEEPSAVE_PORT_LOG)
extern void port_log(const char *fmt, ...);
#define SS_LOG(...) port_log(__VA_ARGS__)
#else
#define SS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define SS_POWER_MAX_AGE_MS 3000.0 /* a press older than this at the boundary (the game was stuck) is dropped */
#define SS_GAP_MS 1500.0           /* boundaries this far apart: the game was stopped (asleep) or loading */
#define SS_IDLE_DEFAULT_S 50       /* Onion's shortest idle sleep is 1 min (keymon checks every 15 s) */

/* Outside the save state (a load must not rewind these). */
static int sStarted SAVESTATE_EXCLUDE;
static int sOn SAVESTATE_EXCLUDE;
static double sIdleMs SAVESTATE_EXCLUDE;
static char sEnv[48] SAVESTATE_EXCLUDE;
static int sFd SAVESTATE_EXCLUDE = -1;
static volatile double sLastInputMs SAVESTATE_EXCLUDE;  /* the last button event (any) */
static volatile double sPowerAtMs SAVESTATE_EXCLUDE;    /* the last POWER press */
static volatile unsigned sPowerSeq SAVESTATE_EXCLUDE;   /* POWER presses seen by the thread */
static unsigned sPowerSeen SAVESTATE_EXCLUDE;           /* ... and handled at a boundary */
static volatile int sMenuHeld SAVESTATE_EXCLUDE;        /* MENU down (MENU+POWER is Onion's screenshot) */
static double sIdleBaseMs SAVESTATE_EXCLUDE;            /* idle time counts from max(last input, this) */
static double sIdleDoneFor SAVESTATE_EXCLUDE = -1;      /* the idle period already saved (its last button's time) */
static double sPrevBoundaryMs SAVESTATE_EXCLUDE;
static volatile int sMenuClosed SAVESTATE_EXCLUDE;      /* the overlay stepped aside for a press: reopen it */
static double sDueAtMs SAVESTATE_EXCLUDE;               /* the press, or the idle boundary, of the save in hand */
static pthread_t sAsync SAVESTATE_EXCLUDE;
static volatile int sAsyncBusy SAVESTATE_EXCLUDE;       /* the helper thread runs */
static int sAsyncJoinable SAVESTATE_EXCLUDE;
static void (*sAsyncFn)(void *) SAVESTATE_EXCLUDE;
static void *sAsyncArg SAVESTATE_EXCLUDE;

double miyoo_sleepsave_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void *reader_main(void *arg) {
    (void) arg;
    struct input_event ev[16];
    for (;;) {
        const ssize_t n = read(sFd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR) continue;
            SS_LOG("sleepsave: button read failed (%s); POWER and idle saves off\n", strerror(errno));
            sOn = 0;
            return NULL;
        }
        if (n == 0) { usleep(100000); continue; } /* a test FIFO with no writer */
        for (int i = 0; i < (int) (n / (ssize_t) sizeof(ev[0])); i++) {
            if (ev[i].type != EV_KEY) continue;
            const double now = miyoo_sleepsave_ms();
            sLastInputMs = now;
            if (ev[i].code == KEY_ESC) sMenuHeld = ev[i].value != 0;
            else if (ev[i].code == KEY_POWER && ev[i].value == 1 && !sMenuHeld) {
                sPowerAtMs = now;
                __sync_synchronize();
                sPowerSeq++;
            }
        }
    }
}

void miyoo_sleepsave_start(const char *env) {
    if (sStarted) return;
    sStarted = 1;
    snprintf(sEnv, sizeof(sEnv), "%s", env ? env : "MIYOO_SLEEPSAVE");
    const char *e = getenv(sEnv);
    if (e && e[0] == '0') { SS_LOG("sleepsave: off (%s=0)\n", sEnv); return; }
    char ienv[64];
    snprintf(ienv, sizeof(ienv), "%s_IDLE", sEnv);
    const char *ie = getenv(ienv);
    const int idle_s = ie && ie[0] ? atoi(ie) : SS_IDLE_DEFAULT_S;
    sIdleMs = idle_s > 0 ? idle_s * 1000.0 : 0;
    const char *dev = getenv("MIYOO_INPUT_DEV");
    if (!dev || !dev[0]) dev = "/dev/input/event0";
    sFd = open(dev, O_RDONLY | O_CLOEXEC);
    if (sFd < 0) { SS_LOG("sleepsave: off (%s: %s)\n", dev, strerror(errno)); return; }
    const double now = miyoo_sleepsave_ms();
    sLastInputMs = now;
    sIdleBaseMs = now;
    sPrevBoundaryMs = now;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 32 * 1024);
    if (pthread_create(&t, &a, reader_main, NULL) == 0) {
        pthread_detach(t);
        sOn = 1;
        atexit(miyoo_sleepsave_join); /* a sleep save still syncing finishes before the game quits */
        if (sIdleMs > 0)
            SS_LOG("sleepsave: on (resume state at a POWER press, and after %d s with no button; %s=0 off, %s=<s>)\n",
                    idle_s, sEnv, ienv);
        else
            SS_LOG("sleepsave: on (resume state at a POWER press; no idle save, %s=0; %s=0 off)\n", ienv, sEnv);
    } else {
        SS_LOG("sleepsave: off (no thread)\n");
        close(sFd);
        sFd = -1;
    }
    pthread_attr_destroy(&a);
}

static void reopen_menu(void) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = SDLK_ESCAPE;
    SDL_PushEvent(&ev);
}

int miyoo_sleepsave_due(void) {
    if (!sOn) return MIYOO_SLEEPSAVE_NONE;
    const double now = miyoo_sleepsave_ms();
    if (now - sPrevBoundaryMs > SS_GAP_MS) sIdleBaseMs = now; /* stopped (asleep) or loading: no idle save on a wake */
    sPrevBoundaryMs = now;
    const unsigned seq = sPowerSeq;
    __sync_synchronize();
    if (seq != sPowerSeen) {
        sPowerSeen = seq;
        const double at = sPowerAtMs, age = now - at;
        const int reopen = sMenuClosed;
        sMenuClosed = 0;
        if (age <= SS_POWER_MAX_AGE_MS) {
            sDueAtMs = at;
            sIdleDoneFor = sLastInputMs; /* this idle period is saved (the press is its last button) */
            if (reopen) reopen_menu(); /* opens at the next frame's events, after this save */
            return MIYOO_SLEEPSAVE_POWER;
        }
        SS_LOG("sleepsave: power press %.0f ms ago (the game was busy or stopped); not saved\n", age);
        if (reopen) reopen_menu();
    }
    if (sIdleMs > 0 && sLastInputMs != sIdleDoneFor) { /* one save per idle period: until the next button */
        const double start = sLastInputMs > sIdleBaseMs ? sLastInputMs : sIdleBaseMs;
        if (now - start >= sIdleMs) {
            sIdleDoneFor = sLastInputMs;
            sDueAtMs = now;
            return MIYOO_SLEEPSAVE_IDLE;
        }
    }
    return MIYOO_SLEEPSAVE_NONE;
}

void miyoo_sleepsave_done(int kind, int ok) {
    const double ms = miyoo_sleepsave_ms() - sDueAtMs;
    char what[48];
    if (kind == MIYOO_SLEEPSAVE_POWER) snprintf(what, sizeof(what), "power press");
    else snprintf(what, sizeof(what), "idle %.0f s", sIdleMs / 1000.0);
    if (ok) SS_LOG("sleepsave: %s, resume state written (%.0f ms)\n", what, ms);
    else SS_LOG("sleepsave: %s, resume state NOT written (%.0f ms; the previous one is kept)\n", what, ms);
}

int miyoo_sleepsave_menu_should_close(void) {
    if (!sOn || sPowerSeq == sPowerSeen) return 0;
    if (miyoo_sleepsave_ms() - sPowerAtMs > SS_POWER_MAX_AGE_MS) return 0;
    sMenuClosed = 1;
    return 1;
}

static void *async_main(void *arg) {
    (void) arg;
    sAsyncFn(sAsyncArg);
    __sync_synchronize();
    sAsyncBusy = 0;
    return NULL;
}

void miyoo_sleepsave_join(void) {
    if (sAsyncJoinable) {
        pthread_join(sAsync, NULL);
        sAsyncJoinable = 0;
    }
}

void miyoo_sleepsave_async(void (*fn)(void *), void *arg) {
    miyoo_sleepsave_join();
    sAsyncFn = fn;
    sAsyncArg = arg;
    sAsyncBusy = 1;
    __sync_synchronize();
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&sAsync, &a, async_main, NULL) == 0) sAsyncJoinable = 1;
    else { fn(arg); sAsyncBusy = 0; }
    pthread_attr_destroy(&a);
}

int miyoo_sleepsave_busy(void) { return sAsyncBusy; }

#else /* not a Miyoo build: nothing to watch */
#include <time.h>
void miyoo_sleepsave_start(const char *env) { (void) env; }
int miyoo_sleepsave_due(void) { return MIYOO_SLEEPSAVE_NONE; }
void miyoo_sleepsave_done(int kind, int ok) { (void) kind; (void) ok; }
int miyoo_sleepsave_menu_should_close(void) { return 0; }
void miyoo_sleepsave_async(void (*fn)(void *), void *arg) { fn(arg); }
void miyoo_sleepsave_join(void) {}
int miyoo_sleepsave_busy(void) { return 0; }
double miyoo_sleepsave_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif
