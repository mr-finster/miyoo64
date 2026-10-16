/* miyoo_worker: the Mini's second core (see miyoo_worker.h). */
#if defined(MIYOO)
#define _GNU_SOURCE
#include "miyoo_worker.h"
/* The port's savestate.h (every handheld port has one next to its platform files): SAVESTATE_EXCLUDE,
 * and in the kit ports hh_game.h's HH_ENV. A state load rewrites the save-state region; the handoff's
 * counters and flags must never be rolled back under a live worker (it would then wait for a job that
 * already came, or the caller for a worker that never started: a state saved with the worker on and
 * loaded with it off, or before it started). Found by the GoldenEye port, 2026-10-09. */
#if defined(__has_include)
#if __has_include("savestate.h")
#include "savestate.h"
#endif
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The A/B switch's name: the port's own prefix (-DMIYOO_WORKER_ENV='"MK64_THREADS"'), else HH_ENV's. */
#ifndef MIYOO_WORKER_ENV
#if defined(HH_ENV)
#define MIYOO_WORKER_ENV HH_ENV("THREADS")
#else
#define MIYOO_WORKER_ENV "MIYOO_THREADS"
#endif
#endif

/* How long each side spins before sleeping, by the clock (read every 256 spins: a vDSO read is ~130 ns).
 * Big triangles come every ~100-200 us while a frame draws, so the worker keeps spinning through a
 * frame's drawing and sleeps only across the game logic between frames; a futex wake per split (tens of
 * us on the device) would eat the gain (GoldenEye, 2026-10-09). The caller's wait is usually short:
 * both shares end together. */
#define WORKER_SPIN_NS 2000000LL
#define CALLER_SPIN_NS 2000000LL

static volatile int sReady SAVESTATE_EXCLUDE = -1;
static void (*volatile sFn)(void *) SAVESTATE_EXCLUDE;
static void *volatile sArg SAVESTATE_EXCLUDE;
static volatile int sSeq SAVESTATE_EXCLUDE;           /* jobs handed over */
static volatile int sDone SAVESTATE_EXCLUDE;          /* jobs finished */
static volatile int sWorkerAsleep SAVESTATE_EXCLUDE;  /* the worker is (about to be) in futex_wait on sSeq */
static volatile int sCallerAsleep SAVESTATE_EXCLUDE;  /* the caller is (about to be) in futex_wait on sDone */
static unsigned sJobs SAVESTATE_EXCLUDE, sSleeps SAVESTATE_EXCLUDE;
static unsigned long long sSpins SAVESTATE_EXCLUDE;
static pthread_t sWorkerThread SAVESTATE_EXCLUDE;     /* for a stall report's sample (miyoo_worker_thread) */
static void (*volatile sWorkerFn)(void *) SAVESTATE_EXCLUDE; /* the job the worker is running, or NULL */

static inline void cpu_relax(void) {
#if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}
static inline int ld(volatile int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void st(volatile int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static void fwait(volatile int *addr, int val) { syscall(SYS_futex, (int *) addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0); }
static void fwake(volatile int *addr) { syscall(SYS_futex, (int *) addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); }
static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
/* Spin until *p != v or about `ns` have passed; returns the spins. */
static unsigned spin_while_eq(volatile int *p, int v, long long ns) {
    unsigned n = 0;
    long long end = 0;
    while (ld(p) == v) {
        cpu_relax();
        if ((++n & 255) == 0) {
            const long long t = now_ns();
            if (!end) end = t + ns;
            else if (t >= end) break;
        }
    }
    return n;
}

void miyoo_worker_pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

static volatile int sWorkerTid SAVESTATE_EXCLUDE; /* the worker's task id (miyoo_worker_tid) */
int miyoo_worker_tid(void) { return sReady == 1 ? sWorkerTid : 0; }

static void *worker_main(void *unused) {
    (void) unused;
    sWorkerTid = (int) syscall(SYS_gettid);
    miyoo_worker_pin_self(1);
    int seen = 0;
    for (;;) {
        spin_while_eq(&sSeq, seen, WORKER_SPIN_NS);
        while (ld(&sSeq) == seen) {   /* idle: sleep until the next run (Dekker with run's store) */
            st(&sWorkerAsleep, 1);
            if (ld(&sSeq) == seen) fwait(&sSeq, seen);
            st(&sWorkerAsleep, 0);
        }
        seen = ld(&sSeq);
        sWorkerFn = sFn;
        sFn(sArg);
        sWorkerFn = NULL;
        st(&sDone, seen);
        if (ld(&sCallerAsleep)) fwake(&sDone);
    }
    return NULL;
}

int miyoo_worker_start(void) {
    if (sReady >= 0) return sReady;
    sReady = 0;
    const char *e = getenv(MIYOO_WORKER_ENV);
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (e && e[0] == '1') { fprintf(stderr, "worker: off (%s=1), %ld cores online\n", MIYOO_WORKER_ENV, cores); return 0; }
    if (cores < 2) { fprintf(stderr, "worker: off, %ld core online\n", cores); return 0; }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    if (pthread_create(&t, &a, worker_main, NULL) != 0) { fprintf(stderr, "worker: off, pthread_create failed\n"); return 0; }
    pthread_attr_destroy(&a);
    pthread_detach(t);
    sWorkerThread = t;
    miyoo_worker_pin_self(0);  /* the caller (the game thread) on core 0, the worker on core 1 */
    sReady = 1;
    fprintf(stderr, "worker: on, %ld cores online (game thread on core 0, worker on core 1)\n", cores);
    return 1;
}

int miyoo_worker_ready(void) { return sReady == 1; }

void miyoo_worker_run(void (*fn)(void *), void *arg) {
    if (sReady != 1) { fn(arg); return; }  /* no worker: the job runs here, and wait returns at once */
    sFn = fn;
    sArg = arg;
    st(&sSeq, ld(&sSeq) + 1);
    if (ld(&sWorkerAsleep)) fwake(&sSeq);
    sJobs++;
}

void miyoo_worker_wait(void) {
    if (sReady != 1) return;
    const int want = ld(&sSeq);
    if (ld(&sDone) == want) return;
    /* sDone only moves up to want: spin while it still holds the value before */
    sSpins += spin_while_eq(&sDone, want - 1, CALLER_SPIN_NS);
    if (ld(&sDone) == want) return;
    sSleeps++;
    while (ld(&sDone) != want) {
        st(&sCallerAsleep, 1);
        const int d = ld(&sDone);
        if (d != want) fwait(&sDone, d);
        st(&sCallerAsleep, 0);
    }
}

void miyoo_worker_stats(unsigned *jobs, unsigned *sleeps, unsigned long long *spins) {
    if (jobs) *jobs = sJobs;
    if (sleeps) *sleeps = sSleeps;
    if (spins) *spins = sSpins;
    sJobs = sSleeps = 0;
    sSpins = 0;
}
/* For a stall report (miyoo_watchdog.c): plain reads of the handoff's state, made from another thread. */
int miyoo_worker_debug(char *buf, int n) {
    return snprintf(buf, n, "ready %d, jobs handed %d, done %d, worker %s, caller %s, running job %p (last %p arg %p), %u jobs %u sleeps since the last PERF line",
                    sReady, ld(&sSeq), ld(&sDone), ld(&sWorkerAsleep) ? "asleep" : "awake", ld(&sCallerAsleep) ? "asleep" : "awake",
                    (void *) sWorkerFn, (void *) sFn, sArg, sJobs, sSleeps);
}
void miyoo_worker_kick(void) {
    if (sReady != 1) return;
    fwake(&sSeq);
    fwake(&sDone);
}
int miyoo_worker_thread(pthread_t *t) {
    if (sReady != 1) return 0;
    *t = sWorkerThread;
    return 1;
}
#endif
