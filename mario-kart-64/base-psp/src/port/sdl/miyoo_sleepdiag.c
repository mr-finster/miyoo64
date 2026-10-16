/* miyoo_sleepdiag: the sleep/wake diagnostic (the SLEEPTEST build). After Onion's sleep (keymon: SIGSTOP of
 * the game, the governor to powersave, the panel off; then cpuclock, the governor back, the panel on, SIGCONT)
 * the game was reported slow until restarted. This finds out what changed. Its own thread, every 250 ms:
 *
 *   - a wake: its own tick came more than 1 s late (the whole process was stopped, or the system slept), or
 *     CLOCK_BOOTTIME ran more than 1 s ahead of CLOCK_MONOTONIC (a real suspend). Logged with the state
 *     before (the last line's) and after, and then one DIAG line a second for 30 s;
 *   - otherwise a DIAG line every 10 s (and every second for the first 30 s of the run);
 *   - the clock watch (miyoo_clock_watch) every second and at once on a wake: MK64_WATCH=log (default:
 *     report a drift), fix (re-apply it, and pin the game / worker / present threads back), 0 off.
 *
 * A DIAG line: the governor, scaling_cur/min/max/setspeed, the PLL's MHz, cpu1/online and the online list;
 * frames and the frame time split (logic, render, present on the game thread, the pacer's wait, audio mixing,
 * the present thread's flip = the panel's vsync/pan wait); the audio ring's fill (and its low point) and
 * underruns; the busy share of each core and of each of the game's threads (with its core and affinity), and
 * the busiest other processes. The framebuffer's mode (/dev/fb0 var screeninfo) when it changes and at a wake.
 * MK64_SLEEPDIAG=0: none of it. Built only with -DMIYOO. */
#if defined(MIYOO)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "miyoo_clock.h"
#include "miyoo_worker.h"
#include "savestate.h"

#define DIAG_VERSION "1.1.0-sleeptest"
#define TICK_US 250000
#define WAKE_GAP_NS 1000000000LL
#define FAST_FOR_NS (30LL * 1000000000LL)

extern void port_log_always(const char *fmt, ...);
extern unsigned port_audio_out_underruns(void);
extern unsigned port_audio_thread_mixes(void);
extern void port_audio_diag(unsigned *fill, unsigned *fill_min);
extern unsigned port_audio_diag_rate(void);
extern volatile unsigned gAudioDiagCalls, gAudioDiagOut;

enum { R_GAME, R_PRESENT, R_MIXER, R_DIAG, R_COUNT };
static volatile int sTid[R_COUNT] SAVESTATE_EXCLUDE;
static const char *const kRole[R_COUNT] = { "game", "present", "audio-mixer", "diag" };
volatile unsigned gDiagFlipUs SAVESTATE_EXCLUDE, gDiagFlips SAVESTATE_EXCLUDE; /* the present thread's flips */

static int gettid_(void) { return (int) syscall(SYS_gettid); }
void miyoo_sleepdiag_tid(int role) { if (role >= 0 && role < R_COUNT) sTid[role] = gettid_(); }

/* ---- the game thread's frame times, summed until the next line ---- */
typedef struct { unsigned iters, rendered; unsigned long long logic, render, present, pacer, audio, iter; unsigned max; } Sums;
static Sums sSums SAVESTATE_EXCLUDE;
static pthread_mutex_t sLock SAVESTATE_EXCLUDE = PTHREAD_MUTEX_INITIALIZER;
static int sOn SAVESTATE_EXCLUDE = -1;
static int sMode SAVESTATE_EXCLUDE = 1;          /* MK64_WATCH: 0 off, 1 log, 2 fix */
static volatile long long sLastIterNs SAVESTATE_EXCLUDE;
static volatile long long sFrameGapNs SAVESTATE_EXCLUDE; /* the last >1 s gap between iterations (for the thread) */

static long long ns_of(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }

/* slots as port.h's PROF_*: LOGIC 0, AUDIO 1, GFX 2, ..., PRES 8, SLEEP 9 */
void miyoo_sleepdiag_iter(unsigned iter_us, const unsigned *last, int rendered) {
    if (sOn <= 0) return;
    const long long now = ns_of(CLOCK_MONOTONIC);
    const long long gap = sLastIterNs ? now - sLastIterNs : 0;
    sLastIterNs = now;
    if (gap > WAKE_GAP_NS) { sFrameGapNs = gap; return; } /* the iteration a stop fell into: not in the sums */
    const unsigned gfx = last[2], pres = last[8], sleep = last[9];
    pthread_mutex_lock(&sLock);
    sSums.iters++;
    sSums.rendered += rendered ? 1 : 0;
    sSums.logic += last[0];
    sSums.audio += last[1];
    sSums.present += pres;
    sSums.pacer += sleep;
    sSums.render += gfx > pres + sleep ? gfx - pres - sleep : 0;
    sSums.iter += iter_us;
    if (iter_us > sSums.max) sSums.max = iter_us;
    pthread_mutex_unlock(&sLock);
}

/* ---- the system's side ---- */
static int read_file(const char *path, char *buf, int n) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) { buf[0] = 0; return 0; }
    const int r = (int) read(fd, buf, (size_t) n - 1);
    close(fd);
    buf[r > 0 ? r : 0] = 0;
    return r > 0 ? r : 0;
}

/* /proc/stat: busy and total ticks per core (cpu0, cpu1; -1 if the line is missing: offline) */
static void cpu_ticks(long long busy[2], long long total[2]) {
    char buf[2048];
    busy[0] = busy[1] = total[0] = total[1] = -1;
    if (!read_file("/proc/stat", buf, sizeof(buf))) return;
    for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (strncmp(l, "cpu", 3) != 0 || l[3] < '0' || l[3] > '1' || l[4] != ' ') continue;
        const int c = l[3] - '0';
        unsigned long long v[10] = { 0 };
        sscanf(l + 4, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        long long t = 0;
        for (int i = 0; i < 8; i++) t += (long long) v[i];
        total[c] = t;
        busy[c] = t - (long long) (v[3] + v[4]); /* idle + iowait */
    }
}

/* a task's utime+stime and last core, from a stat line */
static int parse_stat(const char *path, unsigned long *ticks, int *cpu, char *comm, int ncomm) {
    char buf[512];
    if (!read_file(path, buf, sizeof(buf))) return 0;
    char *rp = strrchr(buf, ')'), *lp = strchr(buf, '(');
    if (!rp || !lp) return 0;
    if (comm) { int k = (int) (rp - lp - 1); if (k >= ncomm) k = ncomm - 1; memcpy(comm, lp + 1, (size_t) k); comm[k] = 0; }
    unsigned long f[42] = { 0 };
    int k = 3; /* rp+2 is field 3 (state) */
    char *q = rp + 2;
    while (*q && *q != ' ') q++;
    for (k = 4; *q && k < 42; k++) { f[k] = strtoul(q, &q, 10); while (*q == ' ') q++; }
    *ticks = f[14] + f[15];
    *cpu = (int) f[39];
    return 1;
}

#define MAXT 24
typedef struct { int tid; unsigned long ticks; } TPrev;
static TPrev sTPrev[MAXT] SAVESTATE_EXCLUDE;
static int sNTPrev SAVESTATE_EXCLUDE;
#define MAXP 160
static TPrev sPPrev[MAXP] SAVESTATE_EXCLUDE;
static int sNPPrev SAVESTATE_EXCLUDE;

static unsigned long prev_of(const TPrev *t, int n, int id) { for (int i = 0; i < n; i++) if (t[i].tid == id) return t[i].ticks; return (unsigned long) -1; }

static int tid_role(int tid) {
    for (int r = 0; r < R_COUNT; r++) if (sTid[r] == tid) return r;
    return -1;
}
static unsigned aff_mask(int tid) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    unsigned m = 0;
    if (sched_getaffinity(tid, sizeof(cs), &cs) == 0) for (int c = 0; c < 8; c++) if (CPU_ISSET(c, &cs)) m |= 1u << c;
    return m;
}

/* "game 812 47% c0 {0} | worker 815 39% c1 {1} | ..." */
static int threads_line(char *b, int cap, double secs, long hz) {
    int k = 0;
    TPrev now[MAXT];
    int nn = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return snprintf(b, cap, "(no /proc/self/task)");
    const int wtid = miyoo_worker_tid();
    struct dirent *e;
    while ((e = readdir(d)) && nn < MAXT) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const int tid = atoi(e->d_name);
        char path[64], comm[24];
        unsigned long t;
        int cpu;
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        if (!parse_stat(path, &t, &cpu, comm, sizeof(comm))) continue;
        now[nn].tid = tid; now[nn].ticks = t; nn++;
        const unsigned long p = prev_of(sTPrev, sNTPrev, tid);
        const int r = tid_role(tid);
        const char *name = r >= 0 ? kRole[r] : tid == wtid ? "worker" : comm;
        if (k < cap) k += snprintf(b + k, (size_t) (cap - k), "%s%s %d ", k ? " | " : "", name, tid);
        if (k < cap) {
            if (p == (unsigned long) -1 || secs <= 0) k += snprintf(b + k, (size_t) (cap - k), "-%%");
            else k += snprintf(b + k, (size_t) (cap - k), "%.0f%%", (double) (t - p) * 100.0 / ((double) hz * secs));
        }
        if (k < cap) k += snprintf(b + k, (size_t) (cap - k), " c%d {%x}", cpu, aff_mask(tid));
    }
    closedir(d);
    memcpy(sTPrev, now, sizeof(TPrev) * (size_t) nn);
    sNTPrev = nn;
    return k;
}

/* the busiest other processes over the window: "keymon(301) 3% audioserver(288) 1%" (1% or more, up to 4) */
static int procs_line(char *b, int cap, double secs, long hz) {
    TPrev now[MAXP];
    struct { int pid; double pct; char comm[20]; } all[MAXP];
    int nn = 0, na = 0;
    const int self = getpid();
    DIR *d = opendir("/proc");
    if (!d) return snprintf(b, cap, "?");
    struct dirent *e;
    while ((e = readdir(d)) && nn < MAXP) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const int pid = atoi(e->d_name);
        if (pid == self) continue;
        char path[64], comm[20];
        unsigned long t;
        int cpu;
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (!parse_stat(path, &t, &cpu, comm, sizeof(comm))) continue;
        now[nn].tid = pid; now[nn].ticks = t; nn++;
        const unsigned long p = prev_of(sPPrev, sNPPrev, pid);
        if (p == (unsigned long) -1 || secs <= 0 || t < p) continue;
        const double pct = (double) (t - p) * 100.0 / ((double) hz * secs);
        if (pct < 1.0) continue;
        all[na].pid = pid; all[na].pct = pct; snprintf(all[na].comm, sizeof(all[na].comm), "%s", comm); na++;
    }
    closedir(d);
    memcpy(sPPrev, now, sizeof(TPrev) * (size_t) nn);
    sNPPrev = nn;
    int k = 0;
    for (int i = 0; i < 4 && i < na && k < cap; i++) {
        int best = i;
        for (int j = i + 1; j < na; j++) if (all[j].pct > all[best].pct) best = j;
        if (best != i) { const __typeof__(all[0]) x = all[i]; all[i] = all[best]; all[best] = x; }
        k += snprintf(b + k, (size_t) (cap - k), "%s%s(%d) %.0f%%", i ? " " : "", all[i].comm, all[i].pid, all[i].pct);
    }
    if (!na) k = snprintf(b, cap, "none at 1%%+");
    return k;
}

/* the framebuffer's mode; key: the same without the pan offset (it moves every flip) */
static int fb_line(char *b, int cap, char *key, int nkey) {
    if (key) key[0] = 0;
    const int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) { if (key) snprintf(key, nkey, "none"); return snprintf(b, cap, "no /dev/fb0"); }
    struct fb_var_screeninfo v;
    const int ok = ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0;
    close(fd);
    if (!ok) { if (key) snprintf(key, nkey, "fail"); return snprintf(b, cap, "FBIOGET_VSCREENINFO failed"); }
    const double ht = (double) v.xres + v.left_margin + v.right_margin + v.hsync_len, vt = (double) v.yres + v.upper_margin + v.lower_margin + v.vsync_len;
    const double hz = v.pixclock ? 1e12 / ((double) v.pixclock * ht * vt) : 0.0;
    char rest[200];
    snprintf(rest, sizeof(rest), "%u bpp, pixclock %u ps, margins l%u r%u u%u lo%u, sync %u/%u (%.2f Hz), activate %u vmode %u", v.bits_per_pixel,
             v.pixclock, v.left_margin, v.right_margin, v.upper_margin, v.lower_margin, v.hsync_len, v.vsync_len, hz, v.activate, v.vmode);
    if (key) snprintf(key, nkey, "%ux%u %ux%u %s", v.xres, v.yres, v.xres_virtual, v.yres_virtual, rest);
    return snprintf(b, cap, "%ux%u virtual %ux%u offset %u,%u %s", v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.xoffset, v.yoffset, rest);
}

static int state_line(char *b, int cap) {
    MiyooClockState s;
    miyoo_clock_read_state(&s);
    return snprintf(b, cap, "gov %s cur %ld min %ld max %ld setspeed %ld kHz, PLL %d MHz, cpu1 %d, online %s", s.gov, s.cur_khz, s.min_khz,
                    s.max_khz, s.setspeed_khz, s.pll_mhz, s.cpu1, s.online);
}

/* MK64_WATCH=fix: the game on core 0, the worker and the present thread on core 1, as at the start */
static void repin(void) {
    const int want[3] = { sTid[R_GAME], miyoo_worker_tid(), sTid[R_PRESENT] };
    const int core[3] = { 0, 1, 1 };
    const char *const nm[3] = { "game", "worker", "present" };
    char line[200];
    int k = 0;
    for (int i = 0; i < 3; i++) {
        if (!want[i]) continue;
        const unsigned m = aff_mask(want[i]);
        if (m == (1u << core[i])) continue;
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(core[i], &cs);
        const int r = sched_setaffinity(want[i], sizeof(cs), &cs);
        k += snprintf(line + k, sizeof(line) - (size_t) k, " %s {%x}->{%x}%s,", nm[i], m, aff_mask(want[i]), r ? " (refused)" : "");
    }
    if (k) { line[k - 1] = 0; port_log_always("wake: re-pinned%s\n", line); }
}

static char sLastState[200] SAVESTATE_EXCLUDE, sLastFb[300] SAVESTATE_EXCLUDE, sLastFbKey[300] SAVESTATE_EXCLUDE;

static void diag_line(long long now, long long t0, long long wake_at, double secs, long hz, long long busy0[2], long long tot0[2]) {
    Sums s;
    pthread_mutex_lock(&sLock);
    s = sSums;
    memset(&sSums, 0, sizeof(sSums));
    pthread_mutex_unlock(&sLock);
    static unsigned sFlipUs0 SAVESTATE_EXCLUDE, sFlips0 SAVESTATE_EXCLUDE, sUr0 SAVESTATE_EXCLUDE, sMix0 SAVESTATE_EXCLUDE, sCb0 SAVESTATE_EXCLUDE, sOut0 SAVESTATE_EXCLUDE;
    const unsigned cb = gAudioDiagCalls, outs = gAudioDiagOut;
    const unsigned fu = gDiagFlipUs, fl = gDiagFlips, ur = port_audio_out_underruns(), mx = port_audio_thread_mixes();
    unsigned fill = 0, fill_min = 0;
    port_audio_diag(&fill, &fill_min);
    long long busy[2], tot[2];
    cpu_ticks(busy, tot);
    char cores[64];
    int ck = 0;
    for (int c = 0; c < 2; c++) {
        if (tot[c] < 0) ck += snprintf(cores + ck, sizeof(cores) - (size_t) ck, " cpu%d off", c);
        else if (tot0[c] < 0 || tot[c] <= tot0[c]) ck += snprintf(cores + ck, sizeof(cores) - (size_t) ck, " cpu%d -", c);
        else ck += snprintf(cores + ck, sizeof(cores) - (size_t) ck, " cpu%d %.0f%%", c, (double) (busy[c] - busy0[c]) * 100.0 / (double) (tot[c] - tot0[c]));
        busy0[c] = busy[c]; tot0[c] = tot[c];
    }
    char st[200];
    state_line(st, sizeof(st));
    snprintf(sLastState, sizeof(sLastState), "%s", st);
    const double n = s.iters ? (double) s.iters : 1.0;
    char when[48];
    if (wake_at) snprintf(when, sizeof(when), "+%llds, wake+%llds", (now - t0) / 1000000000LL, (now - wake_at) / 1000000000LL);
    else snprintf(when, sizeof(when), "+%llds", (now - t0) / 1000000000LL);
    port_log_always("DIAG %s: %s\n", when, st);
    port_log_always("DIAG %s: %.1f fps (%u of %u iterations drawn in %.1f s) | per iteration ms: logic %.1f render %.1f present %.1f pacer %.1f audio %.1f = iter %.1f (max %.1f) | flip(thread) %.1f ms x %u | audio ring %u (low %u) underruns +%u backstop +%u, SDL callbacks %.1f/s pulling %.0f samples/s (output %u Hz) |%s\n",
                    when, secs > 0 ? s.rendered / secs : 0.0, s.rendered, s.iters, secs, s.logic / n / 1000.0, s.render / n / 1000.0,
                    s.present / n / 1000.0, s.pacer / n / 1000.0, s.audio / n / 1000.0, s.iter / n / 1000.0, s.max / 1000.0,
                    fl - sFlips0 ? (double) (fu - sFlipUs0) / (fl - sFlips0) / 1000.0 : 0.0, fl - sFlips0, fill, fill_min, ur - sUr0, mx - sMix0,
                    secs > 0 ? (cb - sCb0) / secs : 0.0, secs > 0 ? (outs - sOut0) / secs : 0.0, port_audio_diag_rate(), cores);
    sFlipUs0 = fu; sFlips0 = fl; sUr0 = ur; sMix0 = mx; sCb0 = cb; sOut0 = outs;
    char th[700], pr[200];
    threads_line(th, sizeof(th), secs, hz);
    procs_line(pr, sizeof(pr), secs, hz);
    port_log_always("DIAG %s: threads %s | others %s\n", when, th, pr);
    char fb[300], key[300];
    fb_line(fb, sizeof(fb), key, sizeof(key));
    if (strcmp(key, sLastFbKey) != 0) {
        port_log_always("DIAG %s: fb %s%s\n", when, fb, sLastFbKey[0] ? " (changed)" : "");
        snprintf(sLastFbKey, sizeof(sLastFbKey), "%s", key);
    }
    snprintf(sLastFb, sizeof(sLastFb), "%s", fb);
}

static void *diag_main(void *unused) {
    (void) unused;
    sTid[R_DIAG] = gettid_();
    {   /* every core: the game pins itself to core 0 later */
        cpu_set_t all;
        CPU_ZERO(&all);
        for (int c = 0; c < 2; c++) CPU_SET(c, &all);
        sched_setaffinity(0, sizeof(all), &all);
    }
    const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    const long long t0 = ns_of(CLOCK_MONOTONIC);
    long long prev = t0, prev_boot = ns_of(CLOCK_BOOTTIME), last_line = t0, next_line = t0 + 1000000000LL, next_watch = t0;
    long long fast_until = t0 + FAST_FOR_NS, wake_at = 0;
    long long busy0[2], tot0[2];
    cpu_ticks(busy0, tot0);
    long long seen_frame_gap = 0;
    for (;;) {
        usleep(TICK_US);
        const long long now = ns_of(CLOCK_MONOTONIC), boot = ns_of(CLOCK_BOOTTIME);
        const long long gap = now - prev, boot_jump = (boot - prev_boot) - gap;
        prev = now; prev_boot = boot;
        const long long fg = sFrameGapNs;
        if (fg != seen_frame_gap) { seen_frame_gap = fg; port_log_always("DIAG: the game thread had a %lld ms gap between iterations\n", fg / 1000000); }
        if (gap - TICK_US * 1000LL > WAKE_GAP_NS || boot_jump > WAKE_GAP_NS) {
            wake_at = now;
            port_log_always("WAKE: the diag thread's tick came %lld ms late (the process was stopped or the system slept); boottime ran %lld ms ahead of monotonic\n",
                            (gap - TICK_US * 1000LL) / 1000000, boot_jump / 1000000);
            port_log_always("WAKE before: %s\n", sLastState);
            port_log_always("WAKE before: fb %s\n", sLastFb);
            char st[200], fb[300];
            state_line(st, sizeof(st));
            fb_line(fb, sizeof(fb), NULL, 0);
            port_log_always("WAKE after:  %s\n", st);
            port_log_always("WAKE after:  fb %s\n", fb);
            fast_until = now + FAST_FOR_NS;
            next_line = now; /* a line now (the window since the last line has the stop in it: its fps says so) */
            next_watch = now;
        }
        if (sMode > 0 && now >= next_watch) {
            char line[200];
            if (miyoo_clock_watch(sMode, line, sizeof(line)) && line[0]) port_log_always("%s\n", line);
            if (sMode >= 2 && wake_at == now) repin();
            next_watch = now + 1000000000LL;
        }
        if (now >= next_line) {
            diag_line(now, t0, wake_at, (double) (now - last_line) / 1e9, hz, busy0, tot0);
            last_line = now;
            next_line = now + (now < fast_until ? 1000000000LL : 10000000000LL);
        }
    }
    return NULL;
}

void miyoo_sleepdiag_start(void) {
    if (sOn >= 0) return;
    const char *e = getenv("MK64_SLEEPDIAG");
    sOn = !(e && e[0] == '0');
    const char *w = getenv("MK64_WATCH");
    sMode = !w || !w[0] || w[0] == 'l' ? 1 : w[0] == 'f' ? 2 : 0;
    sTid[R_GAME] = gettid_();
    if (!sOn) { port_log_always("sleepdiag: off (MK64_SLEEPDIAG=0)\n"); return; }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    if (pthread_create(&t, &a, diag_main, NULL) == 0) {
        pthread_detach(t);
        port_log_always("sleepdiag: on (%s), watch %s (MK64_WATCH=log|fix|0)\n", DIAG_VERSION, sMode == 2 ? "fix" : sMode == 1 ? "log" : "off");
    } else {
        sOn = 0;
        port_log_always("sleepdiag: off (pthread_create failed)\n");
    }
    pthread_attr_destroy(&a);
}
const char *miyoo_sleepdiag_version(void) { return DIAG_VERSION; }
#endif
