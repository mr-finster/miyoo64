/*
 * The device sampling profiler (round 30): where the handheld's time really goes. tools/qprof counts
 * instructions under qemu, so a cache miss, a VFP divide or a pipeline stall costs nothing there;
 * this samples the program counter on the device itself.
 *
 * BK_SPROF=<file> (the bench's "all on, profiled" run): from the bench window's first frame to its
 * BENCH line, a CLOCK_MONOTONIC timer raises SIGPROF every BK_SPROF_US microseconds (default 500)
 * and the handler stores the interrupted pc and lr. Wall-clock sampling, so time the process spends
 * blocked (a present, a sleep) lands in the kernel or the libc wrapper it waited in. At the end the
 * samples are appended to <file> as one block: "GESPROF1", the run's tag, the sample count, the
 * (pc, lr) pairs, then /proc/self/maps so shared-library samples can be named off the device.
 * tools/sprof.py turns the file into a per-function and per-line table with the unstripped binary.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include "../kit/platform/savestate.h"

#define SPROF_MAX (1u << 17) /* 131k samples: 65 s at the default rate */

static uint32_t (*sSamples)[2] SAVESTATE_EXCLUDE;
static volatile uint32_t sCount SAVESTATE_EXCLUDE;
static uint32_t sLost SAVESTATE_EXCLUDE;
static timer_t sTimer SAVESTATE_EXCLUDE;
static int sState SAVESTATE_EXCLUDE; /* 0 idle, 1 sampling, 2 written */

static void sprof_handler(int sig, siginfo_t *si, void *ctx)
{
    const ucontext_t *uc = ctx;
    uint32_t pc = 0, lr = 0;
    (void)sig; (void)si;
#if defined(__arm__)
    pc = (uint32_t)uc->uc_mcontext.arm_pc;
    lr = (uint32_t)uc->uc_mcontext.arm_lr;
#elif defined(__i386__)
    pc = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
#else
    (void)uc;
#endif
    if (sCount < SPROF_MAX) {
        sSamples[sCount][0] = pc;
        sSamples[sCount][1] = lr;
        sCount++;
    } else {
        sLost++;
    }
}

void hh_sprof_start(void)
{
    const char *path = getenv("BK_SPROF");
    const char *us = getenv("BK_SPROF_US");
    long period = us ? atol(us) : 500;
    struct sigaction sa;
    struct sigevent sev;
    struct itimerspec its;
    if (path == NULL || sState != 0)
        return;
    sSamples = calloc(SPROF_MAX, sizeof(*sSamples));
    if (sSamples == NULL)
        return;
    if (period < 100)
        period = 100;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sprof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGPROF;
    if (timer_create(CLOCK_MONOTONIC, &sev, &sTimer) != 0) {
        printf("[sprof] timer_create failed\n");
        return;
    }
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = period * 1000;
    its.it_value = its.it_interval;
    timer_settime(sTimer, 0, &its, NULL);
    sState = 1;
}

void hh_sprof_stop(void)
{
    const char *path = getenv("BK_SPROF");
    const char *tag = getenv("BK_BENCH_CHILD");
    FILE *f, *m;
    uint32_t n;
    char line[512];
    if (sState != 1)
        return;
    timer_delete(sTimer);
    signal(SIGPROF, SIG_IGN);
    sState = 2;
    n = sCount;
    f = fopen(path, "ab");
    if (f == NULL) {
        printf("[sprof] cannot open %s\n", path);
        return;
    }
    fprintf(f, "GESPROF1 %s\n%u %u\n", tag ? tag : "?", (unsigned)n, (unsigned)sLost);
    fwrite(sSamples, sizeof(*sSamples), n, f);
    m = fopen("/proc/self/maps", "r");
    if (m != NULL) {
        while (fgets(line, sizeof(line), m) != NULL)
            if (strstr(line, " r-xp ") != NULL || strstr(line, " r-x") != NULL)
                fputs(line, f);
        fclose(m);
    }
    fputs("END\n", f);
    fclose(f);
    printf("[sprof] %u samples (%u lost) -> %s\n", (unsigned)n, (unsigned)sLost, path);
    fflush(stdout);
}
