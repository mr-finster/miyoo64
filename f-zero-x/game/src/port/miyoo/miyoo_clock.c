/* miyoo_clock: the Mini's CPU clock (see miyoo_clock.h). The PLL programming follows the register
 * sequence Onion's cpuclock tool uses (OnionUI src/cpuclock): the CPU clock comes from the MPLL's
 * low-pass-filter (LPF) frequency synthesiser, LPF = 432 MHz * 2^19 / (clk / 16 * post_div / 2). */
#if defined(MIYOO)
#include "miyoo_clock.h"
#if defined(__has_include)
#if __has_include("savestate.h")
#include "savestate.h"
#endif
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MIYOO_CLOCK_ENV
#if defined(HH_ENV)
#define MIYOO_CLOCK_ENV HH_ENV("CPUCLOCK")
#else
#define MIYOO_CLOCK_ENV "MIYOO_CPUCLOCK"
#endif
#endif

#define RIU_BASE 0x1F000000u
#define MPLL_BASE (RIU_BASE + 0x103000u * 2)
#define MPLL_SIZE 0x1000u
/* The log: stderr, or the port's own (-DMIYOO_CLOCK_LOG=port_log). */
extern void port_log(const char *fmt, ...); /* F-Zero X: the console log, not stderr (naming stderr copies it into the executable: build gate 2b) */
#define MIYOO_CLOCK_LOG(...) port_log(__VA_ARGS__)
#ifndef MIYOO_CLOCK_LOG
#define MIYOO_CLOCK_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define GOV "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
#define SETSPEED "/sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed"
#define CPU1_ONLINE "/sys/devices/system/cpu/cpu1/online"
static const uint64_t kDivSrc = 432000000ull * 524288;
/* Defaults with margin, not Onion's ceilings (the user, 2026-10-10: "don't overclock too much to the
 * limit"; then 1800 on the Plus, the user's pick): Onion asks for up to 1900 (Plus) / 1600 (Mini) for its
 * heaviest core. */
#define MIYOO_CLOCK_PLUS 1800
#define MIYOO_CLOCK_MINI 1500

static int sSavedMHz SAVESTATE_EXCLUDE;         /* 0: nothing changed */
static char sSavedGov[32] SAVESTATE_EXCLUDE;
static int sBoostMHz SAVESTATE_EXCLUDE;         /* the clock the boost set (0: none): what the watch keeps */
static pthread_mutex_t sLock SAVESTATE_EXCLUDE = PTHREAD_MUTEX_INITIALIZER; /* the boost, the watch's re-apply, the restore */
static volatile int sWatchOff SAVESTATE_EXCLUDE; /* the restore ran: the watch keeps its hands off */

/* MIYOO_SYSFS_ROOT=<dir>: every sysfs path under <dir> (tests with a fake tree; off the device only). */
static const char *sys(char *buf, size_t n, const char *path) {
    const char *r = getenv("MIYOO_SYSFS_ROOT");
    if (!r || !r[0]) return path;
    snprintf(buf, n, "%s%s", r, path);
    return buf;
}
static int fake_sys(void) { const char *r = getenv("MIYOO_SYSFS_ROOT"); return r && r[0]; }

/* 283 Mini, 354 Mini Plus, 0 unknown (left alone). */
static int model(void) {
    int id = 0;
    FILE *f = fopen("/tmp/deviceModel", "r");   /* Onion writes it at boot */
    if (f) { if (fscanf(f, "%d", &id) != 1) id = 0; fclose(f); }
    if (id == 283 || id == 354) return id;
    struct stat st;
    if (stat("/customer/app/axp_test", &st) == 0) return 354;     /* the Plus's PMIC tool */
    if (stat("/customer/app/MainUI", &st) == 0) return 283;       /* a stock Mini without it */
    return 0;
}

static volatile uint16_t *map_pll(int *fd) {
    *fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (*fd < 0) return NULL;
    void *p = mmap(NULL, MPLL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, MPLL_BASE);
    if (p == MAP_FAILED) { close(*fd); return NULL; }
    return (volatile uint16_t *) p;
}
static void unmap_pll(volatile uint16_t *p, int fd) { munmap((void *) p, MPLL_SIZE); close(fd); }

static int read_mhz(volatile uint16_t *p16) {
    volatile uint8_t *p8 = (volatile uint8_t *) p16;
    uint32_t lpf = p16[0x2A4] + ((uint32_t) p16[0x2A6] << 16);
    const uint32_t post_div = (p16[0x232] & 0x0F) + 1;
    if (!lpf) lpf = ((uint32_t) p8[0x2C2 << 1] << 16) + ((uint32_t) p8[0x2C1 << 1] << 8) + p8[0x2C0 << 1];
    if (!lpf) return 0;
    return (int) (kDivSrc / lpf * 2 / post_div * 16 / 1000000);
}

static void set_khz(volatile uint16_t *p16, uint32_t khz) {
    const uint32_t post_div = khz >= 800000 ? 2 : khz >= 400000 ? 4 : khz >= 200000 ? 8 : 16;
    const uint32_t rate = khz * 1000 / 16 * post_div / 2;
    const uint32_t lpf = (uint32_t) (kDivSrc / rate);
    uint32_t cur = (p16[0x232] & 0x0F) + 1, d = cur;
    while (post_div > d) { d <<= 1; p16[0x232] = (p16[0x232] & 0xF0) | ((d - 1) & 0x0F); }  /* slower first */
    p16[0x2A8] = 0x0000;              /* LPF off */
    p16[0x2AE] = 0x000F;              /* update count */
    p16[0x2A4] = lpf & 0xFFFF;        /* target, high slot */
    p16[0x2A6] = lpf >> 16;
    p16[0x2B0] = 0x0001;              /* LPF control */
    p16[0x2B2] |= 0x1000;             /* low -> high */
    p16[0x2A8] = 0x0001;              /* LPF on */
    for (int i = 0; i < 10000000 && !(p16[0x2BA] & 1); i++) { }   /* done (bounded) */
    p16[0x2A0] = lpf & 0xFFFF;        /* and the low slot */
    p16[0x2A2] = lpf >> 16;
    while (post_div < d) { d >>= 1; p16[0x232] = (p16[0x232] & 0xF0) | ((d - 1) & 0x0F); }  /* faster last */
}

static void put(const char *path, const char *s) {
    char b[300];
    const int fd = open(sys(b, sizeof(b), path), O_WRONLY | O_TRUNC); /* O_TRUNC: a no-op on sysfs, needed in a fake tree */
    if (fd >= 0) { (void) !write(fd, s, strlen(s)); close(fd); }
}
/* the file's first word ("" if unreadable) */
static int get(const char *path, char *out, int n) {
    char b[300];
    out[0] = 0;
    const int fd = open(sys(b, sizeof(b), path), O_RDONLY);
    if (fd < 0) return 0;
    const int r = (int) read(fd, out, (size_t) n - 1);
    close(fd);
    out[r > 0 ? r : 0] = 0;
    for (int i = 0; out[i]; i++) if (out[i] == '\n' || out[i] == ' ') { out[i] = 0; break; }
    return out[0] != 0;
}
static long get_long(const char *path) {
    char s[32];
    return get(path, s, sizeof(s)) && s[0] >= '0' && s[0] <= '9' ? atol(s) : -1;
}

int miyoo_clock_mhz(void) {
    int fd;
    volatile uint16_t *p = map_pll(&fd);
    if (!p) return 0;
    const int m = read_mhz(p);
    unmap_pll(p, fd);
    return m;
}

static void watch_start(void);
static long sWantSetspeed SAVESTATE_EXCLUDE;    /* scaling_setspeed as the boost left it (the driver clamps it) */

static int boost_to(int mhz) {
    const int id = model();
    const char *e = getenv(MIYOO_CLOCK_ENV);
    if (e && e[0]) mhz = atoi(e);
    else if (mhz < 0) mhz = id == 354 ? MIYOO_CLOCK_PLUS : MIYOO_CLOCK_MINI;
    if (mhz == 0) { MIYOO_CLOCK_LOG("clock: left alone (%s=0)\n", MIYOO_CLOCK_ENV); return 0; }
    if (!id && !fake_sys()) { MIYOO_CLOCK_LOG("clock: left alone (not a Mini / Mini Plus)\n"); return 0; }
    if (mhz < 400) mhz = 400;
    if (mhz > 2000) mhz = 2000;
    int fd = -1;
    volatile uint16_t *p = id ? map_pll(&fd) : NULL; /* never /dev/mem off a Mini */
    if (!p && !fake_sys()) { MIYOO_CLOCK_LOG("clock: left alone (no /dev/mem)\n"); return 0; }
    const int was = p ? read_mhz(p) : 0;
    if (!sSavedMHz) {
        sSavedMHz = was > 0 ? was : 1200;
        if (!get(GOV, sSavedGov, sizeof(sSavedGov))) sSavedGov[0] = 0;
        atexit(miyoo_clock_restore);
    }
    char khz[16];
    snprintf(khz, sizeof(khz), "%d", mhz * 1000);
    put(GOV, "userspace");            /* cpufreq keeps its hands off the PLL */
    put(SETSPEED, khz);
    sWantSetspeed = get_long(SETSPEED);
    sBoostMHz = mhz;
    if (!p) { MIYOO_CLOCK_LOG("clock: governor and setspeed only (MIYOO_SYSFS_ROOT test, no PLL)\n"); return mhz; }
    set_khz(p, (uint32_t) mhz * 1000);
    const int now = read_mhz(p);
    unmap_pll(p, fd);
    MIYOO_CLOCK_LOG("clock: %d -> %d MHz (model %d; %s=<MHz> to change, 0 to leave it)\n", was, now, id, MIYOO_CLOCK_ENV);
    return now;
}

int miyoo_clock_boost_to(int mhz) {
    pthread_mutex_lock(&sLock);
    const int r = boost_to(mhz);
    pthread_mutex_unlock(&sLock);
    watch_start();
    return r;
}

int miyoo_clock_boost(void) { return miyoo_clock_boost_to(-1); }

void miyoo_clock_restore(void) {
    sWatchOff = 1; /* the watch leaves the clock and governor alone from here on */
    if (!sSavedMHz) return;
    pthread_mutex_lock(&sLock);
    const int mhz = sSavedMHz;
    sSavedMHz = 0;
    sBoostMHz = 0;
    int fd;
    volatile uint16_t *p = map_pll(&fd);
    if (p) { set_khz(p, (uint32_t) mhz * 1000); unmap_pll(p, fd); }
    char khz[16];
    snprintf(khz, sizeof(khz), "%d", mhz * 1000);
    put(SETSPEED, khz);
    if (sSavedGov[0]) put(GOV, sSavedGov);
    pthread_mutex_unlock(&sLock);
}

/* ---- The watch (miyoo_clock.h): what the boost (or launch.sh) set, put back after a sleep. ----
 * Onion's sleep (keymon) stops the game, sets the governor to powersave, and on wake runs its cpuclock with
 * the saved clock and writes the governor back -- but a Mini came back with scaling_setspeed at 400000 and
 * the PLL at 400 MHz under userspace (the owner's console.log, 2026-10-16: render 9 -> 25 ms, both cores at
 * 100%, the sound starved). A thread ticks every 250 ms; once a second, and at once when its tick came over
 * 1 s late (the process was stopped) or CLOCK_BOOTTIME ran over 1 s ahead of CLOCK_MONOTONIC (the system
 * slept), it checks the governor, scaling_setspeed, the PLL and cpu1/online against what was set, and
 * re-applies what drifted in the boost's order (governor, setspeed, then the PLL), with one log line. */
#define WATCH_TICK_US 250000
#define WATCH_WAKE_NS 1000000000LL
static int sWatchMode SAVESTATE_EXCLUDE = -1;   /* <GAME>_WATCH: 0 off, 1 report only, 2 re-apply (default) */
static char sWantGov[32] SAVESTATE_EXCLUDE;      /* the governor to keep ("" none) */
static int sWantCpu1 SAVESTATE_EXCLUDE;          /* cpu1/online exists: keep it 1 */
static int sLastDrift SAVESTATE_EXCLUDE;
static volatile unsigned sWakes SAVESTATE_EXCLUDE;
static volatile long long sWakeAtNs SAVESTATE_EXCLUDE;
unsigned miyoo_clock_wakes(void) { return sWakes; }
long long miyoo_clock_last_wake_ns(void) { return sWakeAtNs; }

static long long ns_of(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (long long) t.tv_sec * 1000000000LL + t.tv_nsec; }

/* <GAME>_CPUCLOCK -> <GAME>_WATCH */
static const char *watch_env(char *buf, size_t n) {
    const char *c = MIYOO_CLOCK_ENV;
    const char *suf = strstr(c, "CPUCLOCK");
    snprintf(buf, n, "%.*sWATCH", suf ? (int) (suf - c) : (int) strlen(c), c);
    return buf;
}

static int pll_mhz_now(void) {
    int fd;
    volatile uint16_t *p = model() ? map_pll(&fd) : NULL;
    if (!p) return 0;
    const int m = read_mhz(p);
    unmap_pll(p, fd);
    return m;
}

static void watch_check(int woke) {
    pthread_mutex_lock(&sLock);
    if (sWatchOff) { pthread_mutex_unlock(&sLock); return; }
    char gov[32];
    const int have_gov = get(GOV, gov, sizeof(gov));
    const long sp = sBoostMHz ? get_long(SETSPEED) : -1;
    const int mhz = sBoostMHz ? pll_mhz_now() : 0;
    const int cpu1 = sWantCpu1 ? (int) get_long(CPU1_ONLINE) : 1;
    int drift = 0;
    if (sWantGov[0] && have_gov && strcmp(gov, sWantGov) != 0) drift |= 1;
    if (sBoostMHz && sWantSetspeed > 0 && sp >= 0 && sp != sWantSetspeed) drift |= 2;
    if (sBoostMHz && mhz > 0 && (mhz < sBoostMHz - 10 || mhz > sBoostMHz + 10)) drift |= 4;
    if (sWantCpu1 && cpu1 == 0) drift |= 8;
    char line[240];
    int k = 0;
#define ADD(...) do { if (k < (int) sizeof(line)) k += snprintf(line + k, sizeof(line) - (size_t) k, __VA_ARGS__); } while (0)
    if (drift && sWatchMode >= 2) {
        char khz[16];
        snprintf(khz, sizeof(khz), "%d", sBoostMHz * 1000);
        if (drift & 8) put(CPU1_ONLINE, "1");
        if (drift & 1) put(GOV, sWantGov);
        if (sBoostMHz && (drift & 7)) {
            put(SETSPEED, khz);   /* cpufreq's own step first (its voltage for the top of its table)... */
            int fd;
            volatile uint16_t *p = model() ? map_pll(&fd) : NULL;
            if (p) { set_khz(p, (uint32_t) sBoostMHz * 1000); unmap_pll(p, fd); } /* ...then the PLL past it */
        }
        ADD("wake:");
        char g2[32];
        if (drift & 1) { if (!get(GOV, g2, sizeof(g2))) strcpy(g2, "?"); ADD(" governor %s->%s,", gov, g2); }
        if (sBoostMHz && (drift & 7)) ADD(" setspeed %ld->%ld kHz,", sp, get_long(SETSPEED));
        if (sBoostMHz && (drift & 7) && mhz > 0) ADD(" clock %d->%d MHz,", mhz, pll_mhz_now());
        if (drift & 8) ADD(" cpu1 %d->%ld,", cpu1, get_long(CPU1_ONLINE));
    } else if (drift && drift != sLastDrift) { /* report only: once per change */
        ADD("watch: drift, left as is (report mode):");
        if (drift & 1) ADD(" governor %s (set %s),", gov, sWantGov);
        if (drift & 2) ADD(" setspeed %ld kHz (set %ld),", sp, sWantSetspeed);
        if (drift & 4) ADD(" clock %d MHz (set %d),", mhz, sBoostMHz);
        if (drift & 8) ADD(" cpu1 %d,", cpu1);
    }
#undef ADD
    if (woke && !drift && sWatchMode >= 1) k = snprintf(line, sizeof(line), "wake: clock, governor and cores as set");
    sLastDrift = drift;
    pthread_mutex_unlock(&sLock);
    if (k > 0) { if (k < (int) sizeof(line) && line[k - 1] == ',') line[k - 1] = 0; MIYOO_CLOCK_LOG("%s\n", line); }
}

static void *watch_main(void *unused) {
    (void) unused;
    long long prev = ns_of(CLOCK_MONOTONIC), prev_boot = ns_of(CLOCK_BOOTTIME), next = prev;
    for (;;) {
        usleep(WATCH_TICK_US);
        if (sWatchOff) return NULL;
        const long long now = ns_of(CLOCK_MONOTONIC), boot = ns_of(CLOCK_BOOTTIME);
        const long long late = now - prev - WATCH_TICK_US * 1000LL, jump = (boot - prev_boot) - (now - prev);
        prev = now;
        prev_boot = boot;
        const int woke = late > WATCH_WAKE_NS || jump > WATCH_WAKE_NS;
        if (woke) { sWakeAtNs = now; sWakes++; }
        if (woke || now >= next) {
            watch_check(woke);
            next = now + 1000000000LL;
        }
    }
    return NULL;
}

static void watch_start(void) {
    static int started SAVESTATE_EXCLUDE;
    if (started) return;
    started = 1;
    char env[48];
    watch_env(env, sizeof(env));
    const char *e = getenv(env);
    sWatchMode = !e || !e[0] ? 2 : (e[0] == '0' ? 0 : (e[0] == '1' || e[0] == 'r' || e[0] == 'l') ? 1 : 2);
    if (!model() && !fake_sys()) return; /* off a Mini: nothing to keep */
    if (!sWatchMode) { MIYOO_CLOCK_LOG("watch: off (%s=0)\n", env); return; }
    /* what to keep: the boost's governor (userspace) and setspeed, else the governor launch.sh left (performance) */
    if (sBoostMHz) strcpy(sWantGov, "userspace");
    else if (!get(GOV, sWantGov, sizeof(sWantGov))) sWantGov[0] = 0;
    sWantCpu1 = get_long(CPU1_ONLINE) >= 0;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&t, &a, watch_main, NULL) == 0) {
        pthread_detach(t);
        MIYOO_CLOCK_LOG("watch: keeping governor %s, setspeed %ld kHz, clock %d MHz%s (%s; %s=1 reports only, 0 off)\n",
                        sWantGov[0] ? sWantGov : "(none)", sBoostMHz ? sWantSetspeed : -1L, sBoostMHz, sWantCpu1 ? ", cpu1 online" : "",
                        sWatchMode >= 2 ? "re-applied after a sleep" : "reported only", env);
    } else MIYOO_CLOCK_LOG("watch: off (pthread_create failed)\n");
    pthread_attr_destroy(&a);
}
#endif
