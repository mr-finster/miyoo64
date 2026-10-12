/* qsample.c: a sampling profile of the device build under qemu-arm, for an image whose qemu has no TCG plugins
 * (2026-10-04). Built as a shared object with the Miyoo toolchain gcc and preloaded into the game:
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o qsample.so tools/qsample.c
 *   qemu-arm -L <sysroot> -E LD_PRELOAD=/abs/qsample.so -E QSAMPLE_OUT=/tmp/s.txt build/miyoo/dkr
 * ITIMER_PROF ticks on the qemu process's CPU time; each SIGPROF records the guest pc it interrupted (qemu delivers
 * it between translation blocks), so the counts follow qemu's time, roughly the guest's instructions -- a ranking,
 * not device time (qemu misses the A7's memory latency, divides and VFP stalls). QSAMPLE_FROM=<n>: start after n
 * seconds of process time (skips the boot). At exit: "<hex pc> <count>" lines and "total <n>" (tools/qsample.py, or
 * the OoT port's qprof symprof.py, maps them to functions). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define NB (1u << 16)
static uint32_t sPc[NB], sCnt[NB];
static uint32_t sTotal, sLost;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    const ucontext_t *uc = (const ucontext_t *)uc_;
    const uint32_t pc = (uint32_t)uc->uc_mcontext.arm_pc;
    uint32_t h = (pc * 2654435761u) >> 16;
    for (int i = 0; i < 64; i++, h = (h + 1) & (NB - 1)) {
        if (sPc[h] == pc) { sCnt[h]++; sTotal++; return; }
        if (sPc[h] == 0) { sPc[h] = pc; sCnt[h] = 1; sTotal++; return; }
    }
    sLost++;
}
static void dump(void) {
    const char *p = getenv("QSAMPLE_OUT");
    FILE *f = fopen(p ? p : "/tmp/qsample.txt", "w");
    if (!f) return;
    for (uint32_t i = 0; i < NB; i++) if (sPc[i]) fprintf(f, "%x %u\n", sPc[i], sCnt[i]);
    fprintf(f, "total %u\n", sTotal);
    fclose(f);
}
static void start(int sig) {
    (void)sig;
    struct itimerval it = { { 0, 2000 }, { 0, 2000 } };
    setitimer(ITIMER_PROF, &it, NULL);
}
__attribute__((constructor)) static void qsample_init(void) {
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    atexit(dump);
    const char *e = getenv("QSAMPLE_FROM");
    if (e && atoi(e) > 0) { /* a one-shot ITIMER_VIRTUAL... simpler: a process-time timer that arms the sampler */
        struct sigaction sv; memset(&sv, 0, sizeof(sv)); sv.sa_handler = start; sv.sa_flags = SA_RESTART;
        sigaction(SIGVTALRM, &sv, NULL);
        struct itimerval it = { { 0, 0 }, { atoi(e), 0 } };
        setitimer(ITIMER_VIRTUAL, &it, NULL);
    } else start(0);
}
