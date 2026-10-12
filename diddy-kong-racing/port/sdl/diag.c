/**
 * The 2-second beat diagnostic (2026-09-27). On the device a frame runs 15-25 ms long every ~2.05 s
 * (DKR: every ~61.5 iterations at 30 fps; OoT: every 41 frames at 20 fps), with every phase stretched
 * at once: something else takes the CPU. This names it.
 *
 * Off unless <data dir>/diag exists (or DKR_DIAG=1). When on:
 *  - every log line goes to RAM and is written out at exit (so the log's own SD writes can't make
 *    the beat: if the beat survives, it isn't ours);
 *  - each SLOW line (main.c) gets the main thread's CPU time, its wait on the run queue (time it
 *    was ready but another task ran; /proc/<tid>/schedstat), its context switches, and the tasks
 *    that ran during that iteration;
 *  - every PERF window logs the tasks that used the most CPU in it, and those become the ones
 *    tracked per iteration (kernel threads included: an SD, SPI or GPIO worker counts too);
 *  - at exit, each task's CPU over the whole session.
 * Nothing here touches game state or the frames.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include "savestate.h" /* SAVESTATE_EXCLUDE */

extern const char *port_save_path(const char *name);

static int sOn SAVESTATE_EXCLUDE = -1;
int diag_on(void) {
    if (sOn < 0) {
        const char *e = getenv("DKR_DIAG");
        struct stat st;
        sOn = e ? e[0] != '0' : stat(port_save_path("diag"), &st) == 0;
    }
    return sOn;
}

/* ------------------------------------------------------------ the log, in RAM */
#define LOG_CAP (1u << 20)
static char *sLog SAVESTATE_EXCLUDE;
static size_t sLogLen SAVESTATE_EXCLUDE;
void diag_dump(void) {
    if (sLog && sLogLen) { fwrite(sLog, 1, sLogLen, stdout); fflush(stdout); }
    sLogLen = 0;
}
/* 1 = captured (the caller writes nothing). A full buffer is written out first. */
int diag_log(const char *fmt, va_list ap) {
    if (!diag_on()) return 0;
    if (!sLog && !(sLog = malloc(LOG_CAP))) return 0;
    if (sLogLen > LOG_CAP - 1024) diag_dump();
    const int n = vsnprintf(sLog + sLogLen, LOG_CAP - sLogLen, fmt, ap);
    if (n > 0) sLogLen += (size_t) n < LOG_CAP - sLogLen ? (size_t) n : LOG_CAP - sLogLen - 1;
    return 1;
}

/* ------------------------------------------------------------ tasks' CPU time */
/* ns on the CPU: /proc/.../schedstat (ns) if the kernel has it, else utime+stime ticks from stat. */
static long sTick SAVESTATE_EXCLUDE;
static int read_file(const char *path, char *buf, size_t cap) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    const ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int) n;
}
static int task_cpu(const char *dir, unsigned long long *run_ns, unsigned long long *wait_ns, char *name, size_t ncap) {
    char path[96], buf[512];
    snprintf(path, sizeof(path), "%s/schedstat", dir);
    if (read_file(path, buf, sizeof(buf)) > 0 && sscanf(buf, "%llu %llu", run_ns, wait_ns) == 2) {
        if (name) {
            snprintf(path, sizeof(path), "%s/comm", dir);
            if (read_file(path, buf, sizeof(buf)) > 0) { buf[strcspn(buf, "\n")] = 0; snprintf(name, ncap, "%s", buf); }
            else snprintf(name, ncap, "?");
        }
        return 0;
    }
    snprintf(path, sizeof(path), "%s/stat", dir);
    if (read_file(path, buf, sizeof(buf)) <= 0) return -1;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r) return -1;
    if (name) { *r = 0; snprintf(name, ncap, "%s", l + 1); }
    unsigned long ut = 0, st = 0;
    /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
    if (sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return -1;
    if (!sTick) sTick = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    *run_ns = (unsigned long long) (ut + st) * (1000000000ull / (unsigned long long) sTick);
    *wait_ns = 0;
    return 0;
}

#define MAX_TASKS 256
typedef struct { int pid; char name[24]; unsigned long long run, first; } Task;
static Task sTasks[MAX_TASKS] SAVESTATE_EXCLUDE;
static int sNTasks SAVESTATE_EXCLUDE;
static Task *task_find(int pid) {
    for (int i = 0; i < sNTasks; i++) if (sTasks[i].pid == pid) return &sTasks[i];
    if (sNTasks >= MAX_TASKS) return NULL;
    Task *t = &sTasks[sNTasks++];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->first = ~0ull;
    return t;
}
/* Every task on the system: other processes, plus each of our own threads (/proc/self/task). */
typedef struct { int pid; unsigned long long delta; } Hit;
static int scan_all(Hit *top, int ntop) {
    int n = 0;
    const int self = (int) getpid();
    for (int pass = 0; pass < 2; pass++) {
        DIR *d = opendir(pass ? "/proc/self/task" : "/proc");
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const int pid = atoi(e->d_name);
            if (pid <= 0 || (!pass && pid == self)) continue;
            char dir[64], name[24];
            unsigned long long run, wait;
            snprintf(dir, sizeof(dir), pass ? "/proc/self/task/%d" : "/proc/%d", pid);
            if (task_cpu(dir, &run, &wait, name, sizeof(name)) != 0) continue;
            Task *t = task_find(pass ? -pid : pid); /* our threads as negative ids */
            if (!t) continue;
            snprintf(t->name, sizeof(t->name), "%s%s", pass ? "dkr:" : "", name);
            const unsigned long long delta = (t->first == ~0ull) ? 0 : run - t->run;
            if (t->first == ~0ull) t->first = run;
            t->run = run;
            /* keep the ntop biggest */
            int k;
            if (n < ntop) k = n++;
            else if (delta <= top[ntop - 1].delta) continue;
            else k = ntop - 1;
            while (k > 0 && top[k - 1].delta < delta) { top[k] = top[k - 1]; k--; }
            top[k].pid = t->pid; top[k].delta = delta;
        }
        closedir(d);
    }
    return n;
}

/* The tracked suspects (re-chosen each window) and the main thread. */
#define NSUS 10
static int sSus[NSUS] SAVESTATE_EXCLUDE, sNSus SAVESTATE_EXCLUDE;
static unsigned long long sSusRun[NSUS] SAVESTATE_EXCLUDE;
static char sMainDir[64] SAVESTATE_EXCLUDE;
static unsigned long long sMainRun SAVESTATE_EXCLUDE, sMainWait SAVESTATE_EXCLUDE;
static long sVcs SAVESTATE_EXCLUDE, sIvcs SAVESTATE_EXCLUDE;
static Task *task_by_id(int id) { for (int i = 0; i < sNTasks; i++) if (sTasks[i].pid == id) return &sTasks[i]; return NULL; }
static void sus_dir(int id, char *dir, size_t cap) {
    if (id < 0) snprintf(dir, cap, "/proc/self/task/%d", -id);
    else snprintf(dir, cap, "/proc/%d", id);
}

/* Every iteration (from main.c, main thread). If slow, appends " | cpu .. wait .. cs .. | ran ..." to out. */
void diag_iteration(int slow, char *out, size_t cap) {
    if (!diag_on()) return;
    if (!sMainDir[0]) snprintf(sMainDir, sizeof(sMainDir), "/proc/self/task/%ld", (long) syscall(SYS_gettid));
    unsigned long long run = 0, wait = 0;
    task_cpu(sMainDir, &run, &wait, NULL, 0);
    const unsigned long long drun = run - sMainRun, dwait = wait - sMainWait;
    sMainRun = run; sMainWait = wait;
    struct rusage ru;
    long dv = 0, di = 0;
#ifdef RUSAGE_THREAD
    if (getrusage(RUSAGE_THREAD, &ru) == 0) { dv = ru.ru_nvcsw - sVcs; di = ru.ru_nivcsw - sIvcs; sVcs = ru.ru_nvcsw; sIvcs = ru.ru_nivcsw; }
#endif
    char ran[256];
    int k = 0;
    ran[0] = 0;
    for (int i = 0; i < sNSus; i++) {
        char dir[64];
        unsigned long long r = 0, w;
        sus_dir(sSus[i], dir, sizeof(dir));
        if (task_cpu(dir, &r, &w, NULL, 0) != 0) continue;
        const unsigned long long d = r - sSusRun[i];
        sSusRun[i] = r;
        if (slow && d >= 500000ull && k < (int) sizeof(ran) - 40) { /* >= 0.5 ms */
            const Task *t = task_by_id(sSus[i]);
            k += snprintf(ran + k, sizeof(ran) - (size_t) k, " %s(%d) %.1f", t ? t->name : "?", sSus[i] < 0 ? -sSus[i] : sSus[i], d / 1e6);
        }
    }
    if (slow && out)
        snprintf(out, cap, " | main cpu %.1f wait %.1f cs %ld/%ld | ran%s", drun / 1e6, dwait / 1e6, dv, di, ran[0] ? ran : " -");
}

/* Every PERF window: the top CPU users of the window; they become the tracked suspects. */
void diag_window(void) {
    if (!diag_on()) return;
    Hit top[NSUS + 1];
    const int n = scan_all(top, NSUS + 1);
    char line[512];
    int k = snprintf(line, sizeof(line), "DIAGWIN top");
    sNSus = 0;
    const int self_main = (int) -syscall(SYS_gettid);
    for (int i = 0; i < n && k < (int) sizeof(line) - 48; i++) {
        const Task *t = task_by_id(top[i].pid);
        k += snprintf(line + k, sizeof(line) - (size_t) k, " %s(%d) %.1f", t ? t->name : "?", top[i].pid < 0 ? -top[i].pid : top[i].pid, top[i].delta / 1e6);
        if (top[i].pid != self_main && sNSus < NSUS) {
            char dir[64];
            unsigned long long r = 0, w;
            sSus[sNSus] = top[i].pid;
            sus_dir(top[i].pid, dir, sizeof(dir));
            sSusRun[sNSus] = task_cpu(dir, &r, &w, NULL, 0) == 0 ? r : 0;
            sNSus++;
        }
    }
    extern void port_log_always(const char *fmt, ...);
    port_log_always("%s (ms of CPU this window)\n", line);
}

/* At exit: each task's CPU over the session, biggest first, then the RAM log goes out. */
void diag_exit(void) {
    if (!diag_on()) return;
    Hit top[16];
    scan_all(top, 16); /* refresh run times */
    extern void port_log_always(const char *fmt, ...);
    port_log_always("DIAGTOTAL (ms of CPU over the session):\n");
    for (int pass = 0; pass < 20; pass++) {
        int best = -1;
        unsigned long long bd = 0;
        for (int i = 0; i < sNTasks; i++) {
            const unsigned long long d = sTasks[i].first == ~0ull ? 0 : sTasks[i].run - sTasks[i].first;
            if (d > bd) { bd = d; best = i; }
        }
        if (best < 0) break;
        port_log_always("  %-22s %6d %10.1f\n", sTasks[best].name, sTasks[best].pid < 0 ? -sTasks[best].pid : sTasks[best].pid, bd / 1e6);
        sTasks[best].first = sTasks[best].run; /* taken */
    }
    diag_dump();
}
