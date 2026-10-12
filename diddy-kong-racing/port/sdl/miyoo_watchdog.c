/* miyoo_watchdog: the stall report (see miyoo_watchdog.h). */
#if defined(MIYOO)
#define _GNU_SOURCE
#include "miyoo_watchdog.h"
#include "miyoo_worker.h"
/* SAVESTATE_EXCLUDE (the port's savestate.h, as miyoo_worker.c): a port that saves its whole data
 * section must not save these (a state loaded in another session would bring back dead thread ids). */
#if defined(__has_include)
#if __has_include("savestate.h")
#include "savestate.h"
#endif
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* The switch's name: the port's own prefix (-DMIYOO_WD_ENV='"DKR_WATCHDOG"'). */
#ifndef MIYOO_WD_ENV
#define MIYOO_WD_ENV "MIYOO_WATCHDOG"
#endif
#define MIYOO_WD_SECONDS 3
#define WD_REPORTS_MAX 3
#define WD_REPORT_EVERY_NS (10LL * 1000000000LL)
#define WD_RECOVER_NS (6LL * 1000000000LL) /* the recovery step: this long without a picture */
#define WD_MAX_THREADS 8

volatile int gMiyooWdGame SAVESTATE_EXCLUDE, gMiyooWdPresent SAVESTATE_EXCLUDE = WD_PRESENT_IDLE;
static const char *const kWait[WD_COUNT] = { "running", "the worker's rows of a triangle", "the worker's lines of the sky",
                                             "the present thread", "a picture", "drawing a picture", "flipping the panel",
                                             "the menu" };

static volatile unsigned sFrames SAVESTATE_EXCLUDE;
static volatile long long sLastFrameNs SAVESTATE_EXCLUDE;
static long long sStallNs SAVESTATE_EXCLUDE;
static struct { pthread_t t; int tid; const char *name; } sThr[WD_MAX_THREADS] SAVESTATE_EXCLUDE;
static volatile int sNThr SAVESTATE_EXCLUDE;
static volatile int sSampled SAVESTATE_EXCLUDE; /* the sample handler ran (one thread at a time) */
static pthread_t sWorkerT SAVESTATE_EXCLUDE;    /* the kit's worker, named in the sample */
static volatile int sHaveWorkerT SAVESTATE_EXCLUDE;
static volatile int sWorkerTid SAVESTATE_EXCLUDE, sWdTid SAVESTATE_EXCLUDE; /* the worker's task (its sample says), the watchdog's own */

static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int gettid_(void) { return (int) syscall(SYS_gettid); }
static void out(const char *s, int n) { if (n > 0) (void) !write(2, s, (size_t) n); }
/* a snprintf'd line (n may exceed the buffer when it was cut) */
static void outl(const char *s, int n, int cap) { out(s, n < cap ? n : cap - 1); }

/* the signal handler's own formatting: no stdio, no locks */
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_x(char *p, unsigned long v) { for (int s = 28; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(v >> s) & 15]; return p; }
static void sample_handler(int sig, siginfo_t *si, void *ucv) {
    (void) sig; (void) si;
    char buf[160], *p = buf;
    const pthread_t self = pthread_self();
    const char *name = "?";
    for (int i = 0; i < sNThr; i++) if (pthread_equal(sThr[i].t, self)) name = sThr[i].name;
    if (sHaveWorkerT && pthread_equal(sWorkerT, self)) { name = "worker"; sWorkerTid = gettid_(); }
    p = put_s(p, "WATCHDOG pc ");
    p = put_s(p, name);
#if defined(__arm__)
    const ucontext_t *uc = (const ucontext_t *) ucv;
    p = put_s(p, ": pc "); p = put_x(p, uc->uc_mcontext.arm_pc);
    p = put_s(p, " lr "); p = put_x(p, uc->uc_mcontext.arm_lr);
    p = put_s(p, " sp "); p = put_x(p, uc->uc_mcontext.arm_sp);
#else
    (void) ucv;
#endif
    *p++ = '\n';
    out(buf, (int) (p - buf));
    __atomic_store_n(&sSampled, 1, __ATOMIC_SEQ_CST);
}

void miyoo_wd_register(const char *name) {
    const int i = __atomic_fetch_add(&sNThr, 0, __ATOMIC_SEQ_CST);
    if (i >= WD_MAX_THREADS) return;
    sThr[i].t = pthread_self();
    sThr[i].tid = gettid_();
    sThr[i].name = name;
    __atomic_store_n(&sNThr, i + 1, __ATOMIC_SEQ_CST);
}

void miyoo_wd_frame(void) {
    __atomic_store_n(&sLastFrameNs, now_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&sFrames, sFrames + 1, __ATOMIC_RELAXED);
}

/* Every task of the process, with raw syscalls only (opendir would malloc, and the stuck thread may
 * hold malloc's lock): its tid and name, the run state, user/system time, the CPU it last ran on,
 * and the kernel function it sleeps in. */
struct ldirent { unsigned long long ino; long long off; unsigned short reclen; unsigned char type; char name[]; };
static int read_file(const char *path, char *buf, int n) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    const int r = (int) read(fd, buf, (size_t) n - 1);
    close(fd);
    buf[r > 0 ? r : 0] = 0;
    return r > 0 ? r : 0;
}
static void report_tasks(void) {
    const int dfd = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return;
    char dbuf[2048];
    for (;;) {
        const int nr = (int) syscall(SYS_getdents64, dfd, dbuf, sizeof(dbuf));
        if (nr <= 0) break;
        for (int off = 0; off < nr;) {
            const struct ldirent *d = (const struct ldirent *) (dbuf + off);
            off += d->reclen;
            if (d->name[0] < '0' || d->name[0] > '9') continue;
            char path[64], stat[512], wchan[64], line[400];
            snprintf(path, sizeof(path), "/proc/self/task/%s/stat", d->name);
            if (!read_file(path, stat, sizeof(stat))) continue;
            snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", d->name);
            if (!read_file(path, wchan, sizeof(wchan))) strcpy(wchan, "?");
            /* "tid (comm) S ppid ...": the state after the last ')', then fields 4.. */
            char *rp = strrchr(stat, ')');
            const char *lp = strchr(stat, '(');
            if (!rp || !lp) continue;
            *rp = 0;
            char state = rp[2];
            unsigned long f[42] = { 0 }; /* f[k]: stat field k (1-based); 4 ppid, 14 utime, 15 stime, 39 processor, 40 rt_priority, 41 policy */
            int k = 4;
            for (char *q = rp + 3; *q && k < 42; k++) {
                f[k] = strtoul(q, &q, 10);
                while (*q == ' ') q++;
            }
            const int tid = atoi(d->name);
            const char *name = "";
            for (int i = 0; i < sNThr; i++) if (sThr[i].tid == tid) name = sThr[i].name;
            if (tid == sWorkerTid) name = "worker";
            if (tid == sWdTid) name = "watchdog";
            cpu_set_t cs;
            CPU_ZERO(&cs);
            unsigned mask = 0;
            if (syscall(SYS_sched_getaffinity, tid, sizeof(cs), &cs) > 0)
                for (int c = 0; c < 8; c++) if (CPU_ISSET(c, &cs)) mask |= 1u << c;
            const int n = snprintf(line, sizeof(line), "WATCHDOG task %d (%s)%s%s: state %c, utime %lu stime %lu ticks, cpus %#x (last %lu), policy %lu prio %lu, wchan %s\n",
                                   tid, lp + 1, name[0] ? " = " : "", name, state, f[14], f[15], mask, f[39], f[41], f[40], wchan);
            outl(line, n, (int) sizeof(line));
        }
    }
    close(dfd);
}

/* Every process on the system that is running or has a real-time policy (an RT task spinning on a core
 * starves a thread pinned there), with the load average. */
static void report_system(void) {
    char line[400], buf[512];
    if (read_file("/proc/loadavg", buf, sizeof(buf))) { const int n = snprintf(line, sizeof(line), "WATCHDOG system: loadavg %s", buf); outl(line, n, (int) sizeof(line)); }
    const int dfd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return;
    char dbuf[4096];
    int shown = 0;
    for (;;) {
        const int nr = (int) syscall(SYS_getdents64, dfd, dbuf, sizeof(dbuf));
        if (nr <= 0) break;
        for (int off = 0; off < nr && shown < 40;) {
            const struct ldirent *d = (const struct ldirent *) (dbuf + off);
            off += d->reclen;
            if (d->name[0] < '0' || d->name[0] > '9') continue;
            char path[64];
            snprintf(path, sizeof(path), "/proc/%s/stat", d->name);
            if (!read_file(path, buf, sizeof(buf))) continue;
            char *rp = strrchr(buf, ')');
            const char *lp = strchr(buf, '(');
            if (!rp || !lp) continue;
            *rp = 0;
            const char state = rp[2];
            unsigned long f[42] = { 0 }; /* as in report_tasks; 19 nice, 20 threads */
            int k = 4;
            for (char *q = rp + 3; *q && k < 42; k++) { f[k] = strtoul(q, &q, 10); while (*q == ' ') q++; }
            const unsigned long policy = f[41];
            if (state != 'R' && state != 'D' && policy == 0) continue;
            const int n = snprintf(line, sizeof(line), "WATCHDOG proc %s (%s): state %c, policy %lu prio %lu, utime %lu stime %lu ticks, nice %ld, threads %lu, last cpu %lu\n",
                                   d->name, lp + 1, state, policy, f[40], f[14], f[15], (long) f[19], f[20], f[39]);
            outl(line, n, (int) sizeof(line));
            shown++;
        }
    }
    close(dfd);
}

/* The recovery step, once a stall has lasted WD_RECOVER_NS: wake the worker's futexes (a lost wake-up)
 * and let every task of the process run on every core (a thread pinned to a starved core). Logged, so a
 * freeze that ends here says which it was. */
static void recover(void) {
    static const char kMsg[] = "WATCHDOG recovery: waking the worker's futexes, unpinning every thread of the game\n";
    out(kMsg, (int) sizeof(kMsg) - 1);
    miyoo_worker_kick();
    cpu_set_t all;
    CPU_ZERO(&all);
    const long cores = sysconf(_SC_NPROCESSORS_CONF);
    for (int c = 0; c < cores && c < 8; c++) CPU_SET(c, &all);
    const int dfd = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return;
    char dbuf[2048];
    for (;;) {
        const int nr = (int) syscall(SYS_getdents64, dfd, dbuf, sizeof(dbuf));
        if (nr <= 0) break;
        for (int off = 0; off < nr;) {
            const struct ldirent *d = (const struct ldirent *) (dbuf + off);
            off += d->reclen;
            if (d->name[0] >= '0' && d->name[0] <= '9') syscall(SYS_sched_setaffinity, atoi(d->name), sizeof(all), &all);
        }
    }
    close(dfd);
}

/* a part's length as written into room bytes (snprintf returns what it would have written) */
static int fit(int r, int room) { return r < 0 ? 0 : r >= room ? room - 1 : r; }
static void report(long long stalled_ns, int nth) {
    char line[512];
    int n = snprintf(line, sizeof(line), "WATCHDOG %d: no picture for %lld ms (pictures so far %u)\n", nth, stalled_ns / 1000000,
                     (unsigned) sFrames);
    out(line, n);
    const int gw = gMiyooWdGame, pw = gMiyooWdPresent;
    n = snprintf(line, sizeof(line), "WATCHDOG game: waiting on %s | ", kWait[gw >= 0 && gw < WD_COUNT ? gw : 0]);
    n += fit(miyoo_wd_detail_game(line + n, (int) sizeof(line) - n - 2), (int) sizeof(line) - n - 2);
    line[n++] = '\n';
    out(line, n);
    n = snprintf(line, sizeof(line), "WATCHDOG worker: ");
    n += fit(miyoo_worker_debug(line + n, (int) sizeof(line) - n - 2), (int) sizeof(line) - n - 2);
    line[n++] = '\n';
    out(line, n);
    n = snprintf(line, sizeof(line), "WATCHDOG present: %s | ", kWait[pw >= 0 && pw < WD_COUNT ? pw : 0]);
    n += fit(miyoo_wd_detail_present(line + n, (int) sizeof(line) - n - 2), (int) sizeof(line) - n - 2);
    line[n++] = '\n';
    out(line, n);
    n = snprintf(line, sizeof(line), "WATCHDOG audio: ");
    n += fit(miyoo_wd_detail_audio(line + n, (int) sizeof(line) - n - 2), (int) sizeof(line) - n - 2);
    line[n++] = '\n';
    out(line, n);
    /* the program counters: the registered threads, then the worker (the kit's) */
    pthread_t wt;
    const int have_worker = miyoo_worker_thread(&wt);
    if (have_worker && !sHaveWorkerT) { sWorkerT = wt; sHaveWorkerT = 1; }
    for (int i = -1; i < sNThr; i++) {
        pthread_t t;
        if (i < 0) { if (!have_worker) continue; t = wt; }
        else t = sThr[i].t;
        __atomic_store_n(&sSampled, 0, __ATOMIC_SEQ_CST);
        if (pthread_kill(t, SIGRTMIN + 4) != 0) continue;
        for (int w = 0; w < 100 && !__atomic_load_n(&sSampled, __ATOMIC_SEQ_CST); w++) usleep(1000);
    }
    report_tasks();
    report_system();
    out("WATCHDOG end\n", 13);
    fsync(2);
}

static void *wd_main(void *unused) {
    (void) unused;
    sWdTid = gettid_();
    {   /* every core: the game thread made it and pinned itself first, and a stall on its core must not stop the report */
        cpu_set_t all;
        CPU_ZERO(&all);
        const long cores = sysconf(_SC_NPROCESSORS_CONF);
        for (int c = 0; c < cores && c < 8; c++) CPU_SET(c, &all);
        sched_setaffinity(0, sizeof(all), &all);
    }
    int reports = 0, recovered = 0;
    long long last_report = 0, stall_start = 0, recover_at = 0;
    for (;;) {
        usleep(250000);
        const long long t = now_ns();
        if (!sFrames) continue; /* not armed before the first picture */
        if (gMiyooWdGame == WD_MENU) { __atomic_store_n(&sLastFrameNs, t, __ATOMIC_RELAXED); continue; }
        const long long last = __atomic_load_n(&sLastFrameNs, __ATOMIC_RELAXED);
        const long long stalled = t - last;
        if (stalled < sStallNs) {
            if (reports) { /* the stall is over: say so, and when against the recovery step (right after it: it helped) */
                char line[200];
                int n = snprintf(line, sizeof(line), "WATCHDOG over: pictures again after %lld ms", (last - stall_start) / 1000000);
                if (recovered) n += snprintf(line + n, sizeof(line) - (size_t) n, ", %lld ms after the recovery step", (last - recover_at) / 1000000);
                line[n++] = '\n';
                out(line, n);
                fsync(2);
            }
            reports = recovered = 0;
            continue;
        }
        if (!reports) stall_start = last;
        if (!recovered && reports && stalled >= WD_RECOVER_NS) { recover(); recovered = 1; recover_at = now_ns(); fsync(2); continue; }
        if (reports >= WD_REPORTS_MAX || (reports && t - last_report < WD_REPORT_EVERY_NS)) continue;
        report(stalled, ++reports);
        last_report = t;
    }
    return NULL;
}

void miyoo_wd_start(void) {
    static int started SAVESTATE_EXCLUDE;
    if (started) return;
    started = 1;
    const char *e = getenv(MIYOO_WD_ENV);
    const int s = e && e[0] ? atoi(e) : MIYOO_WD_SECONDS;
    if (s <= 0) { fprintf(stderr, "watchdog: off (%s=0)\n", MIYOO_WD_ENV); return; }
    sStallNs = (long long) s * 1000000000LL;
    miyoo_wd_register("game");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sample_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 4, &sa, NULL);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    if (pthread_create(&t, &a, wd_main, NULL) == 0) {
        pthread_detach(t);
        fprintf(stderr, "watchdog: on (a stall report after %d s without a picture)\n", s);
    } else fprintf(stderr, "watchdog: off (pthread_create failed)\n");
    pthread_attr_destroy(&a);
}
#endif
