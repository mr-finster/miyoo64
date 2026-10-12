/* On-device sampling profiler (debug builds and debug.cfg "prof=1").
 *
 * ITIMER_PROF fires every millisecond of process CPU time, on whichever thread
 * is running; the handler bumps a histogram of the interrupted PC and of the
 * link register (the caller, for time spent in libc / libm / SDL leaf code).
 * Every 30 s the top entries plus /proc/self/maps go to <save>/prof.txt
 * (.tmp + rename). tools/handheld/prof_report.py maps them to functions with the
 * unstripped build/hh-miyoo/oot. Off unless OOT_PROF is set: zero cost.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

const char* port_save_dir(void);
void HhProf_Exit(void);

#define HH_PROF_SLOTS 16384 /* power of two */
#define HH_PROF_PROBE 32

typedef struct {
    uint32_t pc;
    uint32_t count;
} HhProfSlot;

static HhProfSlot sPcHist[HH_PROF_SLOTS];
static HhProfSlot sLrHist[HH_PROF_SLOTS];
static volatile uint32_t sSamples, sDropped;
static int sEnabled;
static long sFrames;
static volatile int sCounting = 1; /* OOT_PROF_FROM=<frame>: samples only from that rendered frame on */
static long sFrom;

static void HhProf_Add(HhProfSlot* hist, uint32_t pc) {
    uint32_t h = (pc >> 2) * 2654435761u;
    int i;

    for (i = 0; i < HH_PROF_PROBE; i++) {
        HhProfSlot* s = &hist[(h + i) & (HH_PROF_SLOTS - 1)];

        if (s->pc == pc) {
            s->count++;
            return;
        }
        if (s->pc == 0) {
            s->pc = pc;
            s->count = 1;
            return;
        }
    }
    sDropped++;
}

static void HhProf_Handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = ctx;
    uint32_t pc, lr;

    (void)sig;
    (void)info;
    if (!sCounting) {
        return;
    }
#if defined(__arm__)
    pc = (uint32_t)uc->uc_mcontext.arm_pc;
    lr = (uint32_t)uc->uc_mcontext.arm_lr;
#elif defined(__i386__)
    pc = (uint32_t)uc->uc_mcontext.gregs[14]; /* REG_EIP */
    lr = 0;
#else
    pc = lr = 0;
    (void)uc;
#endif
    sSamples++;
    HhProf_Add(sPcHist, pc);
    if (lr != 0) {
        HhProf_Add(sLrHist, lr & ~1u);
    }
}

static int HhProf_Cmp(const void* a, const void* b) {
    const HhProfSlot *x = a, *y = b;

    return (x->count < y->count) - (x->count > y->count);
}

static void HhProf_DumpHist(FILE* f, const char* tag, const HhProfSlot* hist, int top) {
    static HhProfSlot sorted[HH_PROF_SLOTS];
    int n = 0, i;

    for (i = 0; i < HH_PROF_SLOTS; i++) {
        if (hist[i].pc != 0) {
            sorted[n++] = hist[i];
        }
    }
    qsort(sorted, n, sizeof(sorted[0]), HhProf_Cmp);
    for (i = 0; i < n && i < top; i++) {
        fprintf(f, "%s %08x %u\n", tag, sorted[i].pc, sorted[i].count);
    }
}

static void HhProf_Dump(void) {
    char path[600], tmp[610], line[512];
    FILE *f, *maps;

    if (getenv("OOT_PROF_OUT") != NULL) {
        snprintf(path, sizeof(path), "%s", getenv("OOT_PROF_OUT")); /* the Bench's profiled view: on the card */
    } else {
        snprintf(path, sizeof(path), "%s/prof.txt", port_save_dir());
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f == NULL) {
        return;
    }
    {
        extern const uint32_t hh_build_stamp;
        fprintf(f, "samples %u dropped %u frames %ld build %08x\n", sSamples, sDropped, sFrames, (unsigned)hh_build_stamp);
    }
    HhProf_DumpHist(f, "pc", sPcHist, 2000);
    HhProf_DumpHist(f, "lr", sLrHist, 500);
    maps = fopen("/proc/self/maps", "r");
    if (maps != NULL) {
        while (fgets(line, sizeof(line), maps) != NULL) {
            fprintf(f, "map %s", line);
        }
        fclose(maps);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, path);
}

void HhProf_Init(void) {
    struct sigaction sa;
    struct itimerval it;

    if (getenv("OOT_PROF") == NULL) {
        return;
    }
    if (getenv("OOT_PROF_FROM") != NULL) {
        sFrom = atol(getenv("OOT_PROF_FROM"));
        sCounting = sFrom <= 0;
    }
    if (getenv("OOT_PROF_OUT") == NULL) { /* keep the previous session's profile as prof.1.txt */
        char a[600], b[600];

        snprintf(a, sizeof(a), "%s/prof.txt", port_save_dir());
        snprintf(b, sizeof(b), "%s/prof.1.txt", port_save_dir());
        rename(a, b);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = HhProf_Handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
    sEnabled = 1;
    atexit(HhProf_Exit); /* a Bench view ends at its exit frame, before the 30 s write */
    printf("hh: profiler on (1 kHz), writing %s/prof.txt every 30 s\n", port_save_dir());
}

/* Once per rendered frame from the main loop. */
void HhProf_Frame(void) {
    if (!sEnabled) {
        return;
    }
    if (++sFrames >= sFrom) {
        sCounting = 1;
    }
    if (sFrames % 600 == 0) {
        HhProf_Dump();
    }
}

void HhProf_Exit(void) {
    if (sEnabled) {
        HhProf_Dump();
    }
}
