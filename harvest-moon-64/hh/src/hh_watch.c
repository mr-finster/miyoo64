/* The device watchdog (after the Majora's Mask port's hh_mm_watch.inc). Host side.
 *
 * A thread of its own, on unless HM64_WATCHDOG=0 (HM64_WATCHDOG=<s>: the stall time, default 5):
 *  - the game loop beats at every retrace (hh_watch_beat). No beat for <s> seconds (the MENU overlay, which holds the
 *    loop by design, and a stopped process -- Onion's sleep -- do not count) and console.log gets one report: the
 *    loop's stage, the retrace and frame, memory (VmRSS, MemAvailable, swap), the main thread's pc/lr/sp and the code
 *    addresses on its stack (`addr2line -f -e hm64 <addr>...` with the unstripped build), every thread's state and
 *    wchan. Again every 30 s while the stall lasts; one line when retraces come back.
 *  - MENU (read from the input device as Onion's keymon does, /dev/input/event0; MIYOO_INPUT_DEV for tests) with the
 *    loop stalled for 2 s or more, or the OS's SIGTERM while it is stalled: the process quits at once (the clock put
 *    back, SDL shut down with a 5 s alarm behind it), so a black screen never needs a reset. With the loop running,
 *    MENU is the game's own (port_sdl.c: the overlay; EXIT GAME quits).
 * The thread and its state are host side: outside the save states (hh/gamestate.ld). */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "port.h"

extern char __executable_start[], etext[];
extern uint32_t hh_retraces;
void port_log_always(const char *fmt, ...);

static volatile uint64_t sBeatUs;
static volatile int sMenuDepth;
static pthread_t sMainThread;
static uintptr_t sMainStackHi;
static volatile int sSnapReady;
static volatile unsigned long sSnapPc, sSnapLr, sSnapSp;
static int sStallS = 5;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000u;
}

void hh_watch_beat(void) { sBeatUs = now_us(); }

/* port_sdl.c around the MENU overlay and the resume prompt (modal: no retraces while it is up) */
void hh_watch_menu(int d) {
    sMenuDepth += d;
    hh_watch_beat();
}

static long field(const char *path, const char *key) {
    char line[160];
    long v = -1;
    const size_t kl = strlen(key);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0) { v = strtol(line + kl, NULL, 10); break; }
    }
    fclose(f);
    return v;
}

static void mem(char *buf, size_t n) {
    snprintf(buf, n, "rss %ldk vsz %ldk, system avail %ldk free %ldk swapfree %ldk", field("/proc/self/status", "VmRSS:"),
             field("/proc/self/status", "VmSize:"), field("/proc/meminfo", "MemAvailable:"), field("/proc/meminfo", "MemFree:"),
             field("/proc/meminfo", "SwapFree:"));
}

static void snap(int sig, siginfo_t *si, void *ucv) {
    (void) sig; (void) si;
#if defined(__arm__)
    ucontext_t *u = ucv;
    sSnapPc = u->uc_mcontext.arm_pc;
    sSnapLr = u->uc_mcontext.arm_lr;
    sSnapSp = u->uc_mcontext.arm_sp;
#else
    (void) ucv;
#endif
    __sync_synchronize();
    sSnapReady = 1;
}

/* one thread's pc/lr/sp (SIGUSR1 to it) and the code addresses on its stack (read through /proc/self/mem, so an
 * unmapped page ends the scan instead of faulting). tid 0: the main thread. */
static void snap_thread(pid_t tid, const char *what) {
    char w[480];
    size_t o = 0;
    sSnapReady = 0;
    if (tid == 0) pthread_kill(sMainThread, SIGUSR1);
    else syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
    for (int i = 0; i < 50 && !sSnapReady; i++) usleep(10000);
    if (!sSnapReady) { port_log_always("watchdog: %s did not take the signal\n", what); return; }
    w[0] = 0;
    int fd = open("/proc/self/mem", O_RDONLY);
    uint32_t buf[512];
    uintptr_t at = sSnapSp, end = sSnapSp + 8192;
    int n = 0;
    if (tid == 0 && sMainStackHi && end > sMainStackHi) end = sMainStackHi;
    while (fd >= 0 && at < end && n < 40) {
        const size_t want = (end - at) < sizeof buf ? (end - at) & ~3u : sizeof buf;
        const ssize_t r = pread(fd, buf, want, (off_t) at);
        if (r <= 0) break;
        for (int k = 0; k < (int) (r / 4) && n < 40; k++)
            if (buf[k] >= (uintptr_t) __executable_start && buf[k] < (uintptr_t) etext) {
                o += (size_t) snprintf(w + o, sizeof w - o, " %08x", (unsigned) buf[k]);
                n++;
            }
        at += (uintptr_t) r;
    }
    if (fd >= 0) close(fd);
    port_log_always("watchdog: %s pc %08lx lr %08lx sp %08lx, code on its stack:%s\n", what, sSnapPc, sSnapLr, sSnapSp, w);
}

static void threads(void) {
    char out[1024];
    size_t o = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    out[0] = 0;
    if (!d) return;
    while ((e = readdir(d)) != NULL && o < sizeof out - 80) {
        char path[96], buf[256], wchan[64] = "", comm[24] = "?";
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        buf[0] = 0;
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
        snprintf(path, sizeof path, "/proc/self/task/%s/wchan", e->d_name);
        f = fopen(path, "r");
        if (f) { if (!fgets(wchan, sizeof wchan, f)) wchan[0] = 0; fclose(f); }
        char *rp = strrchr(buf, ')'), *lp = strchr(buf, '(');
        if (lp && rp && rp > lp) snprintf(comm, sizeof comm, "%.*s", (int) (rp - lp - 1), lp + 1);
        o += (size_t) snprintf(out + o, sizeof out - o, " %s %s %c %s;", e->d_name, comm, rp && rp[1] == ' ' ? rp[2] : '?',
                               wchan[0] ? wchan : "-");
    }
    closedir(d);
    port_log_always("watchdog: threads:%s\n", out);
}

static const char *stage_name(int s) {
    static const char *const k[] = { "boot", "input", "check", "game", "flip", "save state", "pacing" };
    return s >= 0 && s < (int) (sizeof k / sizeof *k) ? k[s] : "?";
}

static void report(uint64_t gap) {
    char m[200];
    mem(m, sizeof m);
    port_log_always("watchdog: no retrace for %.1f s: stage %s, retrace %u, frame %u; %s\n", gap / 1e6,
                    stage_name(port_loop_stage), (unsigned) hh_retraces, (unsigned) port_frame, m);
    snap_thread(0, "main");
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    const pid_t self = (pid_t) syscall(SYS_gettid), mainTid = getpid();
    while (d && (e = readdir(d)) != NULL) {
        const pid_t t = (pid_t) atoi(e->d_name);
        char what[24];
        if (t <= 0 || t == self || t == mainTid) continue;
        snprintf(what, sizeof what, "thread %d", (int) t);
        snap_thread(t, what);
    }
    if (d) closedir(d);
    threads();
}

static void quit_now(const char *why, uint64_t gap) {
    port_log_always("watchdog: %s with the game stuck %.1f s: quitting\n", why, gap / 1e6);
#ifdef PM64_SDL12
    port_log_always(port_video_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up\n");
#endif
    fflush(NULL);
    _exit(0);
}

int handheld_power_pending(void);

static void *watch_main(void *arg) {
    (void) arg;
    const char *dev = getenv("MIYOO_INPUT_DEV");
    if (!dev || !dev[0]) dev = "/dev/input/event0";
    const int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    uint64_t prev = now_us(), stallFrom = 0, lastReport = 0;
    port_log_always("watchdog: on (a report after %d s with no retrace; MENU quits a stuck game; buttons: %s%s)\n", sStallS,
                    dev, fd >= 0 ? "" : " not readable");
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int menu = 0;
        if (fd >= 0) poll(&pf, 1, 250);
        else usleep(250000);
        const uint64_t now = now_us();
        if (now - prev > 1500000) hh_watch_beat(); /* this thread was stopped too (a sleep): not a stall */
        prev = now;
        if (fd >= 0 && (pf.revents & POLLIN)) {
            struct input_event ev[16];
            ssize_t r;
            while ((r = read(fd, ev, sizeof ev)) > 0)
                for (int k = 0; k < (int) (r / (ssize_t) sizeof ev[0]); k++)
                    if (ev[k].type == EV_KEY && ev[k].code == KEY_ESC && ev[k].value == 1) menu = 1;
            if (r == 0) usleep(100000); /* a test FIFO with no writer */
        }
        if (sMenuDepth > 0) { hh_watch_beat(); continue; }
        const uint64_t beat = sBeatUs; /* one read: the game thread may move it past `now` meanwhile */
        const uint64_t gap = beat && beat < now ? now - beat : 0;
        if (beat && gap >= (uint64_t) sStallS * 1000000ull) {
            if (!stallFrom || now - lastReport >= 30000000ull) { report(gap); lastReport = now; }
            if (!stallFrom) stallFrom = beat;
        } else if (stallFrom && gap < 1000000ull) {
            port_log_always("watchdog: retraces again after %.1f s\n", (sBeatUs - stallFrom) / 1e6);
            stallFrom = 0;
        }
        if (beat && gap >= 2000000ull) {
            if (menu) { if (!stallFrom) report(gap); quit_now("MENU", gap); }
#ifdef PM64_SDL12
            if (handheld_power_pending()) quit_now("SIGTERM", gap);
#endif
        }
    }
    return NULL;
}

void hh_watch_start(void) {
    const char *e = getenv("HM64_WATCHDOG");
    if (e && e[0] == '0') { port_log_always("watchdog: off (HM64_WATCHDOG=0)\n"); return; }
    if (e && atoi(e) > 0) sStallS = atoi(e);
    sMainThread = pthread_self();
    FILE *f = fopen("/proc/self/maps", "r"); /* the main stack's top: the stack scan stops there */
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (strstr(line, "[stack]") && sscanf(line, "%lx-%lx", &lo, &hi) == 2) sMainStackHi = hi;
    }
    if (f) fclose(f);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = snap;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&t, &a, watch_main, NULL) == 0) pthread_detach(t);
    else port_log_always("watchdog: off (no thread)\n");
    pthread_attr_destroy(&a);
}
