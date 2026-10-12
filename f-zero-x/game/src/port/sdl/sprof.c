/*
 * The device sampling profiler (round 15, from GoldenEye's hh_sprof.c, its round 30): where the
 * handheld's time really goes. tools/qprof.sh counts instructions under qemu, so a cache miss, a VFP
 * divide or a pipeline stall costs nothing there; this samples the program counter on the device.
 *
 * The Bench's "ship, profiled" runs (bench.c): over each run's measured window a CLOCK_MONOTONIC
 * timer raises SIGPROF every FZX_SPROF_US microseconds (default 500) and the handler stores the
 * interrupted pc and lr. Wall-clock sampling: time spent blocked (a sleep, a flip) lands in the
 * kernel or the libc call it waited in. Each run appends one block to <data>/bench_prof.bin:
 * "FZXSPROF1 <tag>", the sample count and the samples lost, the (pc, lr) pairs, then the executable
 * mappings of /proc/self/maps, then "END". tools/sprof.py names them with the unstripped binary.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include "savestate.h"

void port_log(const char* fmt, ...);

#define SPROF_MAX (1u << 17) /* 131k samples: 65 s at the default rate */

static uint32_t (*sSamples)[2] SAVESTATE_EXCLUDE;
static volatile uint32_t sCount SAVESTATE_EXCLUDE;
static volatile uint32_t sLost SAVESTATE_EXCLUDE;
static timer_t sTimer SAVESTATE_EXCLUDE;
static int sOn SAVESTATE_EXCLUDE;

static void sprof_handler(int sig, siginfo_t* si, void* ctx) {
    const ucontext_t* uc = ctx;
    uint32_t pc = 0, lr = 0;
    (void) sig;
    (void) si;
#if defined(__arm__)
    pc = (uint32_t) uc->uc_mcontext.arm_pc;
    lr = (uint32_t) uc->uc_mcontext.arm_lr;
#else /* the desktop build: samples without addresses (the profile is for the device) */
    (void) uc;
#endif
    if (sCount < SPROF_MAX) {
        sSamples[sCount][0] = pc;
        sSamples[sCount][1] = lr;
        sCount++;
    } else {
        sLost++;
    }
}

/* Start sampling (a no-op if it runs already, or with no memory for the samples). */
void port_sprof_start(void) {
    const char* us = getenv("FZX_SPROF_US");
    long period = us ? atol(us) : 500;
    struct sigaction sa;
    struct sigevent sev;
    struct itimerspec its;
    if (sOn) return;
    if (sSamples == NULL) sSamples = calloc(SPROF_MAX, sizeof(*sSamples));
    if (sSamples == NULL) return;
    if (period < 100) period = 100;
    sCount = 0;
    sLost = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sprof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGPROF;
    if (timer_create(CLOCK_MONOTONIC, &sev, &sTimer) != 0) {
        port_log("sprof: timer_create failed\n");
        return;
    }
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = period * 1000;
    its.it_value = its.it_interval;
    timer_settime(sTimer, 0, &its, NULL);
    sOn = 1;
}

/* Stop, and append the samples to path as one block tagged tag. */
void port_sprof_stop(const char* path, const char* tag) {
    FILE *f, *m;
    uint32_t n;
    char line[512];
    if (!sOn) return;
    timer_delete(sTimer);
    signal(SIGPROF, SIG_IGN);
    sOn = 0;
    n = sCount;
    f = fopen(path, "ab");
    if (f == NULL) {
        port_log("sprof: cannot open %s\n", path);
        return;
    }
    fprintf(f, "FZXSPROF1 %s\n%u %u\n", tag, (unsigned) n, (unsigned) sLost);
    fwrite(sSamples, sizeof(*sSamples), n, f);
    m = fopen("/proc/self/maps", "r");
    if (m != NULL) {
        while (fgets(line, sizeof(line), m) != NULL)
            if (strstr(line, " r-x") != NULL) fputs(line, f);
        fclose(m);
    }
    fputs("END\n", f);
    fclose(f);
    port_log("sprof: %u samples (%u lost) -> %s\n", (unsigned) n, (unsigned) sLost, path);
}
