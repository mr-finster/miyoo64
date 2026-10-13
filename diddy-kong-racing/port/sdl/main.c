/**
 * Diddy Kong Racing -> Miyoo Mini / desktop port: process entry point, the host
 * main loop and the port_* hooks that belong to no particular subsystem.
 * Structure and the PERF log follow the MK64 port's main.c.
 *
 * One iteration = one pass of the game's main_game_loop() (thread3_main.c,
 * split by #ifdef PORT into port_game_init / port_game_iteration). That pass
 * builds a display list, draws the previous one (gfxtask_run_xbus ->
 * port_gfx_run_task below), and paces itself in fb_update() by counting 60 Hz
 * fields off the wall clock. Audio frames are produced here on wall-clock time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <ultra64.h>
#include "port.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "savestate.h"
#include "handheld_menu.h" /* handheld_power_pending (0 off handheld) */
#if defined(MIYOO)
#include "miyoo_clock.h"
#endif

extern struct GfxWindowManagerAPI gfx_sdl;
extern struct GfxRenderingAPI gfx_soft_api;
extern void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title, bool start_in_fullscreen);
extern void gfx_run(Gfx *commands);
extern void gfx_start_frame(void);
extern void gfx_end_frame(void);

extern void port_game_init(void);
extern void port_game_iteration(void);
extern void port_audiomgr_frame(void);
extern void port_audio_out_init(void);
extern void port_assets_load(void);
extern void port_eeprom_flush(void);

static volatile int sRunning = 1;
void port_request_exit(void) { sRunning = 0; }

/* ----------------------------------------------------------------- time */
/* Monotonic microseconds; every port clock reads this. */
/* DKR_VDSOCLOCK (default 1; 0 = the syscall always, as before; or vdsoclock=0 in dkr_display.cfg): the kernel's own __vdso_clock_gettime, the
 * same CLOCK_MONOTONIC read in user space, instead of a syscall. The handheld's musl 1.2.1 never looks the vDSO up.
 * GoldenEye's device Bench (the GoldenEye port X37, hh_os.c): 157-167 ns a read against 598-611 ns for the syscall.
 * Taken from it (as the MK64 port's MK64_VDSOCLOCK): the vDSO is found through AT_SYSINFO_EHDR's symbol table and used
 * only if, best of three trials of 256 reads each, its readings lie between the syscall's around them and cost under
 * half as much; one CLOCK line says which. Only the cost of a read changes, never the clock read. */
/* DKR_VDSOCLOCK, else vdsoclock=0|1 in dkr_display.cfg (handheld_screen.c), else on */
static int port_vdsoclock_switch(void) {
    const char *e = getenv("DKR_VDSOCLOCK");
    if (e && e[0]) return e[0] != '0';
#if defined(HANDHELD)
    {
        extern int handheld_cfg_vdsoclock(void);
        return handheld_cfg_vdsoclock() != 0;
    }
#else
    return 1;
#endif
}
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* the address is this process's, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO */

static u64 port_clock_us_sys(void)
{
#if defined(SYS_clock_gettime32)
    /* musl 1.2's clock_gettime tries the time64 syscall first and the handheld's 4.14 kernel lacks it, so every read
     * cost two kernel entries; SYS_clock_gettime32 goes straight to the one that exists (MK64 perf round 24). */
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0) {
        return (u64) t.s * 1000000u + (u64) (t.ns / 1000);
    }
#endif
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (u64) ts.tv_sec * 1000000u + (u64) (ts.tv_nsec / 1000);
    }
}

static uint64_t port_clock_us_vdso(void)
{
    struct { long s, ns; } t;
    if (sVdsoCgt(CLOCK_MONOTONIC, &t) == 0) return (uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000);
    return port_clock_us_sys();
}

#if defined(__linux__) && __SIZEOF_POINTER__ == 4
#include <elf.h>
#include <sys/auxv.h>
/* A symbol of the vDSO image (32-bit ELF), as musl's __vdsosym; NULL if none. */
static void *port_vdso_sym(const char *want)
{
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)(uintptr_t)getauxval(AT_SYSINFO_EHDR);
    const Elf32_Phdr *ph;
    const Elf32_Shdr *sh;
    uintptr_t bias = 0;
    int i, have = 0;
    if (eh == NULL || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32) return NULL;
    ph = (const Elf32_Phdr *)((const char *)eh + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) { bias = (uintptr_t)eh + ph[i].p_offset - ph[i].p_vaddr; have = 1; break; }
    if (!have || eh->e_shoff == 0) return NULL;
    sh = (const Elf32_Shdr *)((const char *)eh + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Sym *sym;
        const char *str;
        unsigned n, k;
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        sym = (const Elf32_Sym *)((const char *)eh + sh[i].sh_offset);
        str = (const char *)eh + sh[sh[i].sh_link].sh_offset;
        n = sh[i].sh_size / sizeof(Elf32_Sym);
        for (k = 0; k < n; k++)
            if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF && strcmp(str + sym[k].st_name, want) == 0)
                return (void *)(bias + sym[k].st_value);
    }
    return NULL;
}
#else
static void *port_vdso_sym(const char *want) { (void)want; return NULL; }
#endif

/* 256 reads of each, the syscall's around the vDSO's: 0 if the vDSO's readings fall outside them */
static int port_clock_trial(uint64_t *vdso_ns, uint64_t *sys_ns)
{
    uint64_t t0, t1, t2, a, b;
    int i;
    t0 = port_clock_us_sys();
    for (i = 0; i < 256; i++) (void)port_clock_us_sys();
    t1 = port_clock_us_sys();
    a = port_clock_us_vdso();
    for (i = 0; i < 256; i++) (void)port_clock_us_vdso();
    b = port_clock_us_vdso();
    t2 = port_clock_us_sys();
    *vdso_ns = (b - a) * 1000u / 257u;
    *sys_ns = (t1 - t0) * 1000u / 257u;
    return a >= t1 && b <= t2;
}

/* Once, at start-up before any other thread (port_time_us64 picks on its first read otherwise). */
void port_clock_pick(void)
{
    uint64_t v = ~0ull, s = ~0ull, tv, ts;
    int i, on, same = 1;
    if (sClockMode >= 0) return;
    on = port_vdsoclock_switch();
    sVdsoCgt = on ? (PortCgt32)port_vdso_sym("__vdso_clock_gettime") : NULL;
    if (sVdsoCgt == NULL) {
        sClockMode = 0;
        port_log_always("CLOCK syscall (%s)\n", on ? "no vDSO clock_gettime" : "DKR_VDSOCLOCK=0");
        return;
    }
    for (i = 0; i < 3; i++) { /* the best of three (a GoldenEye device run once caught an interrupt inside one) */
        same &= port_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    sClockMode = same && v * 2 < s;
    port_log_always("CLOCK %s: vDSO %u ns/read vs syscall %u ns/read\n", sClockMode ? "vdso" : "syscall", (unsigned)v, (unsigned)s);
    if (!sClockMode) sVdsoCgt = NULL;
}

u64 port_time_us64(void)
{
    if (sClockMode > 0) return (u64)port_clock_us_vdso();
    if (sClockMode < 0) port_clock_pick();
    return (u64)(sClockMode > 0 ? port_clock_us_vdso() : port_clock_us_sys());
}
u32 port_time_us(void) { return (u32) port_time_us64(); }

/* ----------------------------------------------------------------- logging */
static int sQuiet = -1;
extern int diag_log(const char *fmt, va_list ap); /* port/sdl/diag.c: the log in RAM when diagnosing */
void port_log(const char *fmt, ...) {
    va_list ap;
    if (sQuiet < 0) sQuiet = getenv("DKR_QUIET") != NULL;
    if (sQuiet) return;
    va_start(ap, fmt);
    if (!diag_log(fmt, ap)) {
        va_end(ap);
        va_start(ap, fmt);
        vfprintf(stdout, fmt, ap);
    }
    va_end(ap);
}
void port_log_always(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!diag_log(fmt, ap)) {
        va_end(ap);
        va_start(ap, fmt);
        vfprintf(stdout, fmt, ap);
    }
    va_end(ap);
}

/* ----------------------------------------------------------------- profile */
u32 gPortProfFrame[PROF_COUNT];
u32 gPortProfLast[PROF_COUNT];
volatile u32 gPortProfFlipUs;
void port_profile_add(int slot, u32 us) {
    static const int map[4] = { PROF_LOGIC, PROF_GFX, PROF_GFX, PROF_AUDIO };
    if (slot < 0 || slot >= 4) return;
    gPortProfFrame[map[slot]] += us;
}

/* DKR_LOGVERBOSE=1 (or logverbose=1 in dkr_display.cfg): today's full logging, for debugging. */
int port_logverbose(void) {
    static int sV = -1;
    if (sV < 0) {
        const char *e = getenv("DKR_LOGVERBOSE");
#if defined(HANDHELD)
        extern int handheld_cfg_logverbose(void);
        sV = (e && e[0]) ? e[0] == '1' : handheld_cfg_logverbose() == 1;
#else
        sV = e && e[0] == '1';
#endif
    }
    return sV;
}

extern u32 gfx_soft_frames_rendered;
/* SLOW lines (the user's "micro hitches", 2026-09-27): any iteration whose work (minus sleep) passes
 * DKR_SLOWMS (0 = off) logs its breakdown, the ROM bytes it read (the mmap of dkr_assets.bin: a first
 * touch faults in from the SD card) and its page faults. At most 6 a PERF window, so a heavy stretch
 * can't flood the log. The default is 100 ms in normal play (2026-10-06: one session logged 108 lines
 * of 40-42 ms frames); the Bench, the dev-level mode, the diag mode and DKR_LOGVERBOSE=1
 * (dkr_display.cfg: logverbose=1) keep 40 ms. */
#include <sys/resource.h>
static u32 sFrame; /* the iteration counter (main loop below) */
static void slow_iteration(u32 iter_us, u32 nwin) {
    static int sSlowMs = -1;
    static u32 sLogged, sLastWin = ~0u;
    static long sMaj, sMin;
    extern u32 gPortRomBytes;
    struct rusage ru;
    if (sSlowMs < 0) {
        const char *e = getenv("DKR_SLOWMS");
        extern int port_bench_active(void), diag_on(void);
        sSlowMs = e ? atoi(e) : (port_bench_active() || port_logverbose() || diag_on()) ? 40 : 100;
    }
    const u32 rom = gPortRomBytes;
    gPortRomBytes = 0;
    if (sSlowMs == 0) return;
    long maj = 0, min = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0) { maj = ru.ru_majflt - sMaj; min = ru.ru_minflt - sMin; sMaj = ru.ru_majflt; sMin = ru.ru_minflt; }
    if (nwin != sLastWin) { sLastWin = nwin; sLogged = 0; }
    const u32 work = iter_us - gPortProfFrame[PROF_SLEEP];
    extern int diag_on(void);
    extern void diag_iteration(int slow, char *out, size_t cap);
    char dg[320];
    const int slow = work >= (u32) sSlowMs * 1000u && sLogged < (diag_on() ? 12u : 6u);
    dg[0] = 0;
    diag_iteration(slow, dg, sizeof(dg)); /* every iteration when diagnosing (<data dir>/diag) */
    if (!slow) return;
    sLogged++;
#define MS(i) (gPortProfFrame[i] / 1000.0f)
    port_log_always("SLOW it %u work %.1f ms | logic+dl %.1f audio %.1f gfx %.1f = r3d %.1f r2d %.1f tex %.1f pres %.1f | rom %uk | faults maj %ld min %ld%s\n",
                    (unsigned) sFrame, work / 1000.0f, MS(PROF_LOGIC), MS(PROF_AUDIO), MS(PROF_GFX), MS(PROF_R3D), MS(PROF_R2D), MS(PROF_TEX),
                    MS(PROF_PRES), rom / 1024u, maj, min, dg);
#undef MS
}

static void prof_iteration_end(u32 iter_us) {
    static u32 sSum[PROF_COUNT], sMax, sN, sWin, sIt, sSub;
    slow_iteration(iter_us, sWin);
    extern u32 port_audio_out_underruns(void);
    static u32 sUnderrunsPrev;
    gPortProfFrame[PROF_ITER] = iter_us;
    for (int i = 0; i < PROF_COUNT; i++) { sSum[i] += gPortProfFrame[i]; gPortProfLast[i] = gPortProfFrame[i]; gPortProfFrame[i] = 0; }
    if (iter_us > sMax) sMax = iter_us;
    sN++;
    if (++sIt < 150) return;
    sIt = 0;
    /* Normal play: one PERF line per 5 windows of 150 iterations (the sums keep adding up in between).
     * The Bench, the dev-level mode and DKR_LOGVERBOSE=1 (dkr_display.cfg: logverbose=1) print every window. */
    {
        extern int port_bench_active(void);
        if (!port_bench_active() && !port_logverbose() && ++sSub < 5) {
            sWin++;
            { extern void diag_window(void); diag_window(); }
            return;
        }
        sSub = 0;
    }
    const u32 ur = port_audio_out_underruns();
#define AVG(i) ((float) sSum[i] / (1000.0f * sN))
    extern u32 gfx_soft_phase_switches;
    extern int texman_usage_percent(void);
#if defined(MIYOO) /* the second core: split triangles, worker jobs and sleeps (gfx_soft.c) */
    extern int gfx_soft_mt_line(char *buf, int n);
    char mt[96];
    gfx_soft_mt_line(mt, sizeof(mt));
#define PERF_MT "%s"
#define PERF_MT_ARG , mt
#else
#define PERF_MT ""
#define PERF_MT_ARG
#endif
    char sndq[64];
    { extern void port_sndq_perf(char *buf, size_t len); port_sndq_perf(sndq, sizeof(sndq)); }
    port_log_always("PERF work %.1f ms avg (iter %.1f, max %.1f, sleep %.1f) | logic+dl %.1f audio %.1f gfx %.1f = r3d %.1f r2d %.1f tex %.1f comp %.1f clr %.1f pres %.1f | sw %.1f/f | tex %d%% | fps %.1f | underruns +%u%s" PERF_MT "\n",
                    AVG(PROF_ITER) - AVG(PROF_SLEEP), AVG(PROF_ITER), sMax / 1000.0f, AVG(PROF_SLEEP),
                    AVG(PROF_LOGIC), AVG(PROF_AUDIO), AVG(PROF_GFX), AVG(PROF_R3D), AVG(PROF_R2D), AVG(PROF_TEX), AVG(PROF_COMP),
                    AVG(PROF_CLR), AVG(PROF_PRES), (float) gfx_soft_phase_switches / sN, texman_usage_percent(),
                    1000.0f / (AVG(PROF_ITER) > 0.1f ? AVG(PROF_ITER) : 0.1f), ur - sUnderrunsPrev, sndq PERF_MT_ARG);
#undef PERF_MT
#undef PERF_MT_ARG
    gfx_soft_phase_switches = 0;
#undef AVG
    sUnderrunsPrev = ur;
    memset(sSum, 0, sizeof(sSum)); sMax = 0; sN = 0; sWin++;
    { extern void diag_window(void); diag_window(); }
}

/* ----------------------------------------------------------------- save paths
 * Everything the port writes goes under a data dir: $DKR_DATA if set, else
 * <exe dir>/data (desktop). The Miyoo launch.sh sets $DKR_DATA to the app folder. */
static char sExeDir[256] = "./";
static char sSaveDir[300] = "./data/";
const char *port_eboot_dir(void) { return sExeDir; }
const char *port_save_dir(void) { return sSaveDir; }
const char *port_save_path(const char *name) {
    static char buf[4][512];
    static int i;
    char *b = buf[i++ & 3];
    snprintf(b, sizeof(buf[0]), "%s%s", sSaveDir, name);
    return b;
}
static void set_dirs(const char *argv0) {
    const char *env = getenv("DKR_DATA");
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash && (size_t) (slash + 1 - argv0) < sizeof(sExeDir)) {
        memcpy(sExeDir, argv0, slash + 1 - argv0);
        sExeDir[slash + 1 - argv0] = 0;
    }
    if (env && env[0]) {
        size_t n;
        snprintf(sSaveDir, sizeof(sSaveDir), "%s", env);
        n = strlen(sSaveDir);
        if (n && sSaveDir[n - 1] != '/' && n + 1 < sizeof(sSaveDir)) { sSaveDir[n] = '/'; sSaveDir[n + 1] = 0; }
    } else {
        snprintf(sSaveDir, sizeof(sSaveDir), "%sdata/", sExeDir);
    }
    mkdir(sSaveDir, 0777);
}

#if defined(MIYOO)
/* Miyoo Mini: the data dir (launch.sh sets DKR_DATA to App/DiddyKongRacing/data; else <exe dir>/data,
 * set_dirs' own default) holds the settings, the save states and the auto save, dkr_assets.bin and
 * the console log, rotated like the handheld's. DKR_LOG=0 leaves stdout/stderr alone (a shell over
 * SSH). No `pid record`: that is the handheld OS's. */
static void miyoo_console_setup(const char *argv0) {
    char dir[256];
    const char *env = getenv("DKR_DATA");
    if (env && env[0]) snprintf(dir, sizeof(dir), "%s", env);
    else {
        const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
        if (slash && (size_t) (slash - argv0) + 6 < sizeof(dir)) { memcpy(dir, argv0, (size_t) (slash - argv0)); dir[slash - argv0] = 0; }
        else snprintf(dir, sizeof(dir), ".");
        strcat(dir, "/data");
        setenv("DKR_DATA", dir, 1);
    }
    mkdir(dir, 0777);
    const char *lg = getenv("DKR_LOG");
    if (lg && lg[0] == '0') return;
    char a[300], b[300];
    snprintf(a, sizeof(a), "%s/console.1.log", dir); snprintf(b, sizeof(b), "%s/console.2.log", dir); rename(a, b);
    snprintf(b, sizeof(b), "%s/console.log", dir); rename(b, a);
    const int fd = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}
#endif

/* ----------------------------------------------------------------- loop note */
/* Where the host loop is, for the watchdog's and the hang trace's log lines: the stage it is in and
 * how many iterations it has run. Written by the main thread only (plain stores), read by other
 * threads. */
enum { STAGE_BOOT, STAGE_LOGIC, STAGE_DRAW, STAGE_AUDIO, STAGE_EEPROM, STAGE_CHECK, STAGE_END };
static volatile int sLoopStage;
unsigned handheld_loop_detail(char *buf, size_t n) {
    static const char *const kStage[] = { "boot", "game logic", "drawing", "audio", "eeprom flush", "save state check",
                                          "end of iteration" };
    extern s32 gGameMode;
    const int st = sLoopStage;
    if (n > 0) {
        snprintf(buf, n, "stage %s, iteration %u, game mode %d, pictures rendered %u", kStage[st >= 0 && st <= STAGE_END ? st : 0],
                 (unsigned) sFrame, (int) gGameMode, (unsigned) gfx_soft_frames_rendered);
    }
    return sFrame + 1u; /* iterations started; never 0 (0 = "keeps no count") */
}
#if defined(MIYOO)
/* the watchdog's game line (miyoo_watchdog.c, from its thread) */
int miyoo_wd_detail_game(char *buf, int n) {
    handheld_loop_detail(buf, (size_t) n);
    return (int) strlen(buf);
}
#endif

#if defined(__arm__) && defined(__linux__)
/* DKR_HANGTRACE=<s> (tests, dkr-miyoo 2026-10-10): when no iteration ends for <s> seconds of real time, a thread
 * samples the game thread five times (a signal; its pc, lr, sp and the 48 stack words above sp), logs them with
 * the loop detail, and ends the process (exit 3). For finding where a run spins (addr2line the pcs). */
#include <pthread.h>
#include <ucontext.h>
static pthread_t sHangGame;
static volatile unsigned sHangPc, sHangLr, sHangSp, sHangWords[48];
static volatile int sHangGot;
static void hang_sample(int sig, siginfo_t *si, void *ucp) {
    const ucontext_t *uc = (const ucontext_t *) ucp;
    (void) sig; (void) si;
    sHangPc = uc->uc_mcontext.arm_pc; sHangLr = uc->uc_mcontext.arm_lr; sHangSp = uc->uc_mcontext.arm_sp;
    for (int i = 0; i < 48; i++) sHangWords[i] = ((const unsigned *) (uintptr_t) sHangSp)[i];
    sHangGot = 1;
}
static void *hang_watch(void *arg) {
    const int secs = (int) (intptr_t) arg;
    u32 last = sFrame; int still = 0;
    for (;;) {
        sleep(1);
        if (sFrame != last) { last = sFrame; still = 0; continue; }
        if (++still < secs) continue;
        char d[160];
        handheld_loop_detail(d, sizeof(d));
        fprintf(stderr, "HANGTRACE no iteration for %d s: %s\n", secs, d);
        for (int k = 0; k < 5; k++) {
            sHangGot = 0;
            pthread_kill(sHangGame, SIGUSR2);
            for (int t = 0; t < 100 && !sHangGot; t++) usleep(10000);
            fprintf(stderr, "HANGTRACE sample %d pc %08x lr %08x sp %08x words", k, sHangPc, sHangLr, sHangSp);
            for (int i = 0; i < 48; i++) fprintf(stderr, " %08x", sHangWords[i]);
            fprintf(stderr, "\n");
            usleep(200000);
        }
        _exit(3);
    }
    return NULL;
}
static void hang_trace_start(void) {
    const char *e = getenv("DKR_HANGTRACE");
    if (!e || atoi(e) <= 0) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = hang_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);
    sHangGame = pthread_self();
    pthread_t t;
    pthread_create(&t, NULL, hang_watch, (void *) (intptr_t) atoi(e));
    pthread_detach(t);
}
#else
static void hang_trace_start(void) { }
#endif

/* ----------------------------------------------------------------- gfx task */
/* gfxtask_run_xbus (rcp_dkr.c, #ifdef PORT): draw one finished display list. */
void port_gfx_run_task(Gfx *dlBegin, Gfx *dlEnd) {
    const u32 t0 = port_time_us();
    (void) dlEnd;
    sLoopStage = STAGE_DRAW;
    gfx_start_frame(); /* gfx_sdl12.c handle_events: the menu, and the power-off request */
    gfx_run(dlBegin);
    gfx_end_frame();
    PORT_PROF_ADD(PROF_GFX, port_time_us() - t0);
    sLoopStage = STAGE_LOGIC;
}

/* ----------------------------------------------------------------- audio */
/* The N64 audio manager ran once per two VI fields (1/30 s). Run as many
 * audio frames as wall-clock time says are due (each sizes itself from the
 * output queue, as on the N64), at most 4 after a stall. */
static int sAudioReady;
extern u64 port_clock_us(void);
static void audio_update(void) {
    static u32 sLast;
    static u32 sDebt;
    const u32 now = (u32) port_clock_us();
    u32 t0;
    int n = 0;
    if (!sAudioReady) return;
    if (sLast == 0) sLast = now - 33367;
    sDebt += now - sLast;
    sLast = now;
    t0 = port_time_us();
    while (sDebt >= 33367 && n < 4) {
        port_audiomgr_frame();
        sDebt -= 33367;
        n++;
    }
    if (sDebt > 4 * 33367) sDebt = 0;
    PORT_PROF_ADD(PROF_AUDIO, port_time_us() - t0);
}

/* Round 18: the N64 took about 1.4 s to load a level while its audio thread
 * played on, so when level_free() stopped the music, its 50 ms release, the
 * last sound effects and the reverb rang out before the silence of the load.
 * The port loads at once, and on the device the load also holds up the audio,
 * so that tail was played after the load, over the new level's start.
 * load_level_game() and load_level_menu() call this first: when the music was
 * stopped and the player hasn't finished stopping, the last frame (black: the
 * fade-out is over) stays up and the audio runs at its own pace until the
 * player has stopped and the output is quiet (peak under 1/128 of full scale),
 * for at most 60 fields (1 s). DKR_MUSICHOLD=0: off. */
void port_music_tail(void) {
    extern ALCSPlayer *gMusicPlayer;
    extern u8 gMusicPlaying;
    extern u32 port_ai_peak(int watch);
    extern s32 port_vi_fields(s32 minFields);
    static int sOn = -1;
    int n;
    u32 peak = 0;
    if (sOn < 0) { const char *e = getenv("DKR_MUSICHOLD"); sOn = !(e && e[0] == '0'); }
    if (!sOn || !sAudioReady || gMusicPlaying || alCSPGetState(gMusicPlayer) == AL_STOPPED) return;
    port_ai_peak(1);
    for (n = 0; n < 60; n++) {
        peak = port_ai_peak(1);
        if (alCSPGetState(gMusicPlayer) == AL_STOPPED && peak < 256) break;
        /* a power-off (a signal still in gfx_sdl12.c, or already handed to savestate.c): the save
         * comes first, and it only starts after this load's iteration. */
        if (handheld_power_pending() || savestate_resume_save_pending()) break;
        port_vi_fields(1);
        audio_update();
    }
    port_ai_peak(0);
    port_log("music tail: held %d fields at iteration %u (peak %u)\n", n, (unsigned) sFrame, (unsigned) peak);
}

/* fps=60 / DKR_FPS=60 (decomp/src/video.c fb_update): no 30 fps hold. Off by default (fps=30: the hold, the
 * game's pace as before). Read once, on the game thread; the Bench's "no 30 fps hold" window sets it for its
 * 20 s (bench_xspeed_win). */
static int sFps60 = -1, sFps60Cfg = -1;
int port_fps60(void) {
    if (sFps60Cfg < 0) {
        const char *e = getenv("DKR_FPS");
#if defined(HANDHELD) /* the Mini too (the user, 2026-10-11: the game's own 30 fps hold by default) */
        extern int handheld_cfg_fps(void);
        sFps60Cfg = (e && e[0]) ? atoi(e) == 60 : handheld_cfg_fps() == 60;
#else
        sFps60Cfg = e && atoi(e) == 60;
#endif
        sFps60 = sFps60Cfg;
        port_log_always("fps: %s\n", sFps60Cfg ? "up to 60, no 30 fps hold (fps=60)" : "the game's pace, held to 30 fps between 16.7 and 33 ms (fps=60: no hold)");
    }
    return sFps60;
}
/* hold30 (DKR_HOLD30, default 1; decomp/src/video.c fb_update): with fps=30, every frame waits at least 2 fields,
 * so a light scene does not run at 60 and drop back to 30 (the game's own rule lets gVideoDeltaTime fall to 1
 * after 20 fast frames). DKR_HOLD30=0: the earlier hold (min(gVideoDeltaTime, 2)). */
int port_hold30(void) {
    static int sOn = -1;
    if (sOn < 0) {
        const char *e = getenv("DKR_HOLD30");
        sOn = !(e && e[0] == '0');
        port_log_always("hold30: %s\n", sOn ? "on (a firm 30 fps hold)" : "off (the game may drop to 1 field a frame)");
    }
    return sOn;
}
/* DKR_RATELOG=1: every 300 frames, how many advanced by 1, 2, 3, 4+ fields and how often the step changed. */
void port_rate_log(s32 rate) {
    static int sOn = -1;
    static unsigned n, h[5], flips;
    static unsigned long long fields;
    static u64 t0;
    static s32 last;
    if (sOn < 0) { const char *e = getenv("DKR_RATELOG"); sOn = e && e[0] == '1'; }
    if (!sOn) return;
    if (n == 0) t0 = port_clock_us();
    fields += (unsigned long long) rate;
    h[rate < 1 ? 0 : rate > 4 ? 4 : rate]++;
    if (n && rate != last) flips++;
    last = rate;
    if (++n % 300 == 0) {
        port_log_always("rate: %u frames: 1:%u 2:%u 3:%u 4+:%u, changes %u; game %.2f s, clock %.2f s\n", n, h[1], h[2],
                        h[3], h[4], flips, (double) fields * 0.016683, (double) (port_clock_us() - t0) * 1e-6);
        h[0] = h[1] = h[2] = h[3] = h[4] = 0;
        flips = 0;
    }
}
static void port_fps60_bench(int on) { /* -1: back to the setting */
    port_fps60();
    sFps60 = on < 0 ? sFps60Cfg : on;
}

/* ----------------------------------------------------------------- benchmark
 * `dkr --bench` (the "DKR Bench" launcher): the M3 device gate. Each
 * test level is loaded straight into a 1-player race (thread3_main.c asks
 * port_bench_level_to_load() once per frame) with the autopilot holding A;
 * after a settle period every iteration's work time is recorded, and at the
 * end of each level a BENCH line reports average fps, work and the 1% low.
 * The console log is the report. */
/* vehicle: 0 car, 1 hovercraft, 2 plane -- the adventure-mode vehicle of each. */
static const struct { s16 id, vehicle; const char *name; } kBench[] = {
    { 10, 1, "Crescent Island" }, { 29, 0, "Jungle Falls" }, { 15, 2, "Spaceport Alpha" },
    { 8, 1, "Whale Bay" }, { 18, 2, "Greenwood Village" }, { 0, 0, "Central Area (hub)" }, { 3, 0, "Fossil Canyon" },
};
#define BENCH_LEVELS ((int) (sizeof(kBench) / sizeof(kBench[0])))
#define BENCH_SETTLE_US 8000000u   /* load, intro camera, countdown */
#define BENCH_MEASURE_US 40000000u
#define BENCH_MEASURE_WIN_US 20000000u /* kind 7's windows (five a level) */
static int sBench = -1;      /* -1 off; else index of the level running */
static int sBenchNeon = 1;   /* 0: this run has the NEON paths off (a "neon=0" trigger file) */
static int sBenchBoth;       /* "neon=both": each level twice, NEON on, then off */
/* The speed round (2026-09-26): what a "both" run switches -- 0 the NEON paths (neon=both), 1 the
 * sky's NEON rows (noz=both), 2 the direct present (pres=both), 3 the deferred sky (sky=both),
 * 4 the branch-free blend (bf=both), 5 the load mixer started lazily (lazymix=both: B16, 2026-10-03),
 * 6 the lean rows (lean=both: cm_row_lean, 2026-10-04); sBenchNeon is then that switch.
 * 7 the cross-port speed round's windows (xspeed=all, and `dkr --bench`, the Bench launcher, 2026-10-07): each level five
 * times, a 20 s window each: all on, DKR_FASTROWS off, DKR_SPANVEC off (DKR_ZSKIP lives in its rows: off too),
 * DKR_ZSKIP off, then all three off ("round off": the build before the round); sBenchWin is the window.
 * Since 2026-10-08 (branch dkr-floorflash) kind 7 is the floor-flash round's windows instead: each level three times,
 * all on, DKR_PSHADE off, DKR_FOGSPLIT off (the speed switches stay on); a fourth window since 2026-10-10: fps=60.
 * Since 2026-10-11 (branch dkr-next4, the speed audit) kind 7 is that round's windows: each level five times, the
 * build as it ships (DKR_FOGTEST on, DKR_PSROWS on, DKR_FASTROWS off), DKR_FOGTEST off, DKR_PSROWS off, DKR_FASTROWS
 * on, then all three as v1.3.3 had them ("v1.3.3 rows"). The settings' fps stays as the cfg says in every window. */
static int sBenchKind;
static int sBenchWin;
static const char *const kBenchKindName[8] = { "NEON", "sky NEON", "direct present", "deferred sky", "branch-free blend", "lazy load mixer", "lean rows",
                                               "speed audit (each level: all on, fogtest off, psrows off, fastrows on, v1.3.3 rows)" };
#define XSPEED_WINS 5
static const char *const kXspeedWin[XSPEED_WINS] = { ", all on", ", fogtest-off", ", psrows-off", ", fastrows-on", ", v1.3.3 rows (all three as before)" };
static void bench_xspeed_win(int w) {
    extern void gfx_set_fogtest(int on), gfx_soft_set_rows4(int fastrows, int psrows);
    gfx_set_fogtest(w != 1 && w != 4);
    gfx_soft_set_rows4(w == 3 || w == 4, w != 2 && w != 4);
    port_fps60_bench(-1);
}
static void bench_switch(int on) {
    extern void gfx_soft_set_neon(int on), acmd_set_neon(int on), gfx_soft_set_noz(int on), gfx_wm_set_present_mode(int mode);
    extern void gfx_soft_set_skydefer(int on), gfx_soft_set_blendbf(int on), port_audio_set_lazymix(int on);
    extern void gfx_soft_set_lean(int on);
    if (sBenchKind == 6) gfx_soft_set_lean(on);
    else if (sBenchKind == 5) port_audio_set_lazymix(on);
    else if (sBenchKind == 1) gfx_soft_set_noz(on);
    else if (sBenchKind == 4) gfx_soft_set_blendbf(on);
    else if (sBenchKind == 3) gfx_soft_set_skydefer(on);
    else if (sBenchKind == 2) gfx_wm_set_present_mode(on);
    else { gfx_soft_set_neon(on); acmd_set_neon(on); }
}
static int sBenchPending;    /* a level load has been requested */
static u64 sBenchT0;
static u32 sBenchIter[4096];
static int sBenchN;
/* DKR_LEVEL=<id>[,<players>[,<vehicle>[,<entrance>]]] (developer, the eyeball sweep of round 18):
 * boot straight into that level, as the benchmark loads its levels, once. Players 1-4 (the
 * split screens), vehicle 0 car, 1 hovercraft, 2 plane (default: the level's own), entrance
 * = which door of a hub. The game's AI drives every kart on a race track (DKR_LEVEL_DRIVE=0:
 * hands off). DKR_LEVELS=1 prints the level table and exits. */
static int sDevLevel = -1, sDevPlayers = 1, sDevVehicle = -1, sDevEntrance, sDevPending, sDevDrive = 1;
static void dev_level_parse(void) {
    const char *e = getenv("DKR_LEVEL");
    if (!e || !e[0]) return;
    int v[4] = { -1, 1, -1, 0 }, n = 0;
    while (n < 4 && *e) { v[n++] = atoi(e); while (*e && *e != ',') e++; if (*e == ',') e++; }
    sDevLevel = v[0]; sDevPlayers = v[1] < 1 ? 1 : v[1] > 4 ? 4 : v[1]; sDevVehicle = v[2]; sDevEntrance = v[3];
    sDevPending = sDevLevel >= 0;
    if ((e = getenv("DKR_LEVEL_DRIVE")) && e[0] == '0') sDevDrive = 0;
}
static void dev_levels_list(void) {
    extern void level_count(s32 *, s32 *);
    extern s8 leveltable_type(s32);
    extern s32 leveltable_vehicle_default(s32), leveltable_vehicle_usable(s32); /* Vehicle is an int-sized enum */
    extern char *level_name(s32);
    s32 levels = 0, worlds = 0;
    level_count(&levels, &worlds);
    port_log_always("LEVELS %d (worlds %d)\n", (int) levels, (int) worlds);
    for (s32 i = 0; i < levels; i++) {
        const char *nm = level_name(i);
        port_log_always("LEVEL %2d type %3d vehicle %d usable %d  %s\n", (int) i, (int) leveltable_type(i),
                        (int) leveltable_vehicle_default(i), (int) leveltable_vehicle_usable(i), nm ? nm : "?");
    }
    { extern void port_dev_print_track_grid(void); port_dev_print_track_grid(); }
    exit(0);
}
int port_bench_active(void) { return sBench >= 0 || sDevLevel >= 0; }
s32 port_bench_vehicle(void) {
    if (sDevLevel >= 0) {
        extern s32 leveltable_vehicle_default(s32);
        return sDevVehicle >= 0 ? sDevVehicle : leveltable_vehicle_default(sDevLevel);
    }
    return sBench >= 0 ? kBench[sBench].vehicle : 0;
}
s32 port_bench_players(void) { return sDevLevel >= 0 ? sDevPlayers - 1 : 0; } /* gGameNumPlayers: 0 = one */
s32 port_bench_entrance(void) { return sDevLevel >= 0 ? sDevEntrance : 0; }
/* The AI takes the wheel on race tracks; the hub has no racing line (the AI
 * loops there), so there the pad stays idle at the start position. */
int port_bench_autodrive(void) {
    if (sDevLevel >= 0) {
        extern s8 leveltable_type(s32);
        const s32 t = leveltable_type(sDevLevel);
        return sDevDrive && t != 5 /* RACETYPE_HUBWORLD */ && t != 6 && t != 7 /* the cutscenes */;
    }
    return sBench >= 0 && kBench[sBench].id != 0;
}
s32 port_bench_level_to_load(void) {
    { static int sList = -1; if (sList < 0) { const char *e = getenv("DKR_LEVELS"); sList = e && e[0] == '1'; } if (sList) dev_levels_list(); }
    { /* DKR_SAVE_COMPLETE=<file 1-3>: a finished adventure in that save file, then exit (menu.c) */
        const char *e = getenv("DKR_SAVE_COMPLETE");
        if (e && e[0] >= '1' && e[0] <= '3') {
            extern void port_dev_save_complete(s32 file), port_eeprom_flush(void);
            port_dev_save_complete(e[0] - '1');
            port_eeprom_flush();
            port_log_always("SAVE complete: file %c written\n", e[0]);
            exit(0);
        }
    }
    if (sDevPending) {
        sDevPending = 0;
        port_log_always("LEVEL load %d, %d player(s), vehicle %d, entrance %d\n", sDevLevel, sDevPlayers, (int) port_bench_vehicle(), sDevEntrance);
        return sDevLevel;
    }
    if (sBench < 0 || !sBenchPending) return -1;
    sBenchPending = 0;
    port_log_always("BENCH start %s (level %d)\n", kBench[sBench].name, kBench[sBench].id);
    return kBench[sBench].id;
}
static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *) a, y = *(const u32 *) b;
    return x < y ? -1 : x > y;
}
static void bench_iteration(u32 iter_us, u32 work_us) {
    extern u64 port_clock_us(void);
    const u64 now = port_clock_us();
    if (sBench < 0) return;
    if (sBenchPending) return;
    if (sBenchT0 == 0) { sBenchT0 = now; sBenchN = 0; }
    if (now - sBenchT0 < BENCH_SETTLE_US) return;
    if (sBenchN < (int) (sizeof(sBenchIter) / sizeof(sBenchIter[0]))) sBenchIter[sBenchN++] = iter_us;
    {
        extern u32 port_audio_out_underruns(void);
        static u64 sWorkSum; static u32 sWorkN, sUnder0;
        if (sBenchN == 1) { sWorkSum = 0; sWorkN = 0; sUnder0 = port_audio_out_underruns(); }
        sWorkSum += work_us; sWorkN++;
        if (now - sBenchT0 >= BENCH_SETTLE_US + (sBenchKind == 7 ? BENCH_MEASURE_WIN_US : BENCH_MEASURE_US)) {
            u64 sum = 0;
            for (int i = 0; i < sBenchN; i++) sum += sBenchIter[i];
            qsort(sBenchIter, sBenchN, sizeof(u32), cmp_u32);
            const u32 p99 = sBenchIter[(sBenchN * 99) / 100];
            const float avg_ms = (float) sum / sBenchN / 1000.0f;
            char line[240];
            snprintf(line, sizeof(line),
                     "BENCH result %-20s avg %.1f fps (frame %.1f ms, work %.1f ms), 1%% low %.1f fps (%.1f ms), frames %d, underruns %u%s\n",
                     kBench[sBench].name, 1000.0f / avg_ms, avg_ms, (float) sWorkSum / sWorkN / 1000.0f,
                     1000000.0f / p99, p99 / 1000.0f, sBenchN, (unsigned) (port_audio_out_underruns() - sUnder0),
                     sBenchKind == 7 ? kXspeedWin[sBenchWin] :
                     !sBenchBoth ? "" : sBenchNeon ? (sBenchKind == 6 ? ", lean rows on" : sBenchKind == 5 ? ", lazy load mixer on" : sBenchKind == 1 ? ", sky NEON on" : sBenchKind == 2 ? ", direct present on" : sBenchKind == 3 ? ", deferred sky on" : sBenchKind == 4 ? ", branch-free blend on" : ", NEON on")
                                                   : (sBenchKind == 6 ? ", lean rows off" : sBenchKind == 5 ? ", lazy load mixer off" : sBenchKind == 1 ? ", sky NEON off" : sBenchKind == 2 ? ", direct present off" : sBenchKind == 3 ? ", deferred sky off" : sBenchKind == 4 ? ", branch-free blend off" : ", NEON off"));
            port_log_always("%s", line);
            { /* bench.log: appended, never rotated -- the console log keeps only
               * three launches, and playing afterwards would push it out */
                FILE *f = fopen(port_save_path("bench.log"), "a");
                if (f) {
                    static int sHeader;
                    extern const char port_build_rev[];
                    extern const unsigned int port_build_stamp;
                    extern void gfx_soft_get_internal(int *w, int *h);
                    int iw = 0, ih = 0;
                    gfx_soft_get_internal(&iw, &ih);
                    if (!sHeader++) fprintf(f, "--- benchmark run, build %s (stamp %08X), 3D %dx%d, %s %s\n", port_build_rev, port_build_stamp, iw, ih,
                                            kBenchKindName[sBenchKind], sBenchKind == 7 ? "" : sBenchBoth ? "on, then off (each level twice)" : sBenchNeon ? "on" : "off");
                    fputs(line, f);
                    fflush(f); fsync(fileno(f)); fclose(f);
                }
            }
            sBenchT0 = 0;
            if (sBenchKind == 7) { /* the next window of the same level (reloaded), or the next level all on */
                sBenchWin = sBenchWin + 1 < XSPEED_WINS ? sBenchWin + 1 : 0;
                bench_xspeed_win(sBenchWin);
            }
            if (sBenchBoth) { /* the same level again with the other paths; the level
                               * load in between keeps the passes apart */
                sBenchNeon = !sBenchNeon;
                bench_switch(sBenchNeon);
            }
            if ((sBenchBoth && !sBenchNeon) || (sBenchKind == 7 && sBenchWin != 0)) {
                sBenchPending = 1;
            } else if (++sBench >= BENCH_LEVELS) {
                port_log_always("BENCH done\n");
                sBench = -1;
                port_request_exit();
            } else {
                sBenchPending = 1;
            }
        }
    }
}

/* ----------------------------------------------------------------- main */
static void run_one_iteration(void) {
    const u32 t0 = port_time_us();
    {   /* DKR_RNG_TRACE_AT=<iteration>: log every rand_range caller in it */
        static long at = -2, n = 1;
        extern s32 gPortRngTrace;
        if (at == -2) {
            const char *e = getenv("DKR_RNG_TRACE_AT"), *c = getenv("DKR_RNG_TRACE_N");
            at = e ? atol(e) : -1; n = c ? atol(c) : 1;
        }
        gPortRngTrace = (long) sFrame >= at && (long) sFrame < at + n;
        if (gPortRngTrace) port_log_always("ITER %u\n", (unsigned) sFrame);
    }
    u32 tl;
#if defined(MIYOO)
    {   /* DKR_STALL_TEST=game:<iteration>[:<s>] | worker:<iteration>[:<s>] (tests of the watchdog, miyoo_watchdog.c):
         * at that iteration the game thread spins for <s> seconds (6), or the next split triangle's worker share sleeps */
        static long at = -2; static int kind, secs = 6;
        if (at == -2) {
            const char *e = getenv("DKR_STALL_TEST");
            at = -1;
            if (e && !strncmp(e, "game:", 5)) { kind = 1; at = atol(e + 5); }
            else if (e && !strncmp(e, "worker:", 7)) { kind = 2; at = atol(e + 7); }
            const char *c = e ? strchr(e, ':') : NULL;
            if (c && (c = strchr(c + 1, ':')) && atoi(c + 1) > 0) secs = atoi(c + 1);
        }
        if ((long) sFrame == at) {
            extern volatile int gMiyooStallWorker;
            port_log_always("stall test: %s %d s at iteration %u\n", kind == 1 ? "the game thread spins" : "a worker share sleeps", secs, (unsigned) sFrame);
            if (kind == 2) gMiyooStallWorker = secs;
            else { const u64 end = port_time_us64() + (u64) secs * 1000000u; while (port_time_us64() < end) { } }
        }
    }
#endif
    sLoopStage = STAGE_LOGIC;
    port_game_iteration();
    tl = port_time_us() - t0;
    PORT_PROF_ADD(PROF_LOGIC, tl > gPortProfFrame[PROF_GFX] + gPortProfFrame[PROF_SLEEP]
                                  ? tl - gPortProfFrame[PROF_GFX] - gPortProfFrame[PROF_SLEEP] : 0);
    {   /* DKR_LOGIC_LOG=1: per-iteration game state (finds x86/ARM divergence). */
        static int sLogic = -1;
        if (sLogic < 0) sLogic = getenv("DKR_LOGIC_LOG") != NULL;
        if (sLogic) { extern void port_logic_trace(u32 frame, s32 updateRate); port_logic_trace(sFrame, 0); }
    }
    sLoopStage = STAGE_AUDIO;
    audio_update();
    { extern void port_sndq_sample(void); port_sndq_sample(); } /* the sound queue's low point, for PERF */
    sLoopStage = STAGE_EEPROM;
    port_eeprom_flush();
    sLoopStage = STAGE_CHECK;
    {   /* DKR_SAVE_AT / DKR_LOAD_AT=<iteration>: queue a slot-1 save / load
         * (headless save-state verification). */
        static long save_at = -2, load_at = -2;
        if (save_at == -2) {
            const char *e = getenv("DKR_SAVE_AT"); save_at = e ? atol(e) : -1;
            e = getenv("DKR_LOAD_AT"); load_at = e ? atol(e) : -1;
        }
        if ((long) sFrame == save_at) savestate_request_save(1);
        if ((long) sFrame == load_at) savestate_request_load(1);
    }
    {   /* DKR_SAVEWRITE_AT=<iteration> (tests, statefix A): the game writes every region of its save from
         * RAM on the next frame (thread3_main.c port_dev_mark_all_writes), as the next real save would */
        static long savewrite_at = -2;
        if (savewrite_at == -2) { const char *e = getenv("DKR_SAVEWRITE_AT"); savewrite_at = e ? atol(e) : -1; }
        if ((long) sFrame == savewrite_at) { extern void port_dev_mark_all_writes(void); port_dev_mark_all_writes(); }
    }
    {   /* DKR_FRAMEDELAY=<ms> (tests, PM64's hook): every iteration that much slower, as a heavy scene on
         * the device -- the audio queue's aim (X19) is judged by the underruns it leaves */
        static int sDelay = -1;
        if (sDelay < 0) { const char *e = getenv("DKR_FRAMEDELAY"); sDelay = e ? atoi(e) : 0; }
        if (sDelay > 0) usleep((useconds_t) sDelay * 1000u);
    }
    {
        const u32 ts = port_time_us();
        savestate_check(); /* queued save/load, the power-off resume: at the iteration boundary */
        PORT_PROF_ADD(PROF_SLEEP, port_time_us() - ts);
    }
    sLoopStage = STAGE_END;
    {
        const u32 iter = port_time_us() - t0;
        bench_iteration(iter, iter > gPortProfFrame[PROF_SLEEP] ? iter - gPortProfFrame[PROF_SLEEP] : 0);
        prof_iteration_end(iter);
    }
    sFrame++;
    {
        static long exit_at = -2, exitsave_at = -2;
        if (exit_at == -2) { const char *e = getenv("DKR_EXIT_AT"); exit_at = e ? atol(e) : -1; }
        if ((long) sFrame == exit_at) sRunning = 0;
        /* DKR_EXITSAVE_AT=<iteration> (tests, the resume chain T4/T5): EXIT GAME there -- the
         * exit save, then quit, as the handheld menu's confirm does (MK64_EXITSAVE_AT's twin) */
        if (exitsave_at == -2) { const char *e = getenv("DKR_EXITSAVE_AT"); exitsave_at = e ? atol(e) : -1; }
        if ((long) sFrame == exitsave_at) { extern void savestate_request_exit_save(void); savestate_request_exit_save(); }
    }
}

u32 port_frame_count(void) { return sFrame; }

static void on_signal(int sig) {
    port_log_always("signal %d at iteration %u\n", sig, (unsigned) sFrame);
    fflush(NULL);
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock the OS had, as on every way out */
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

extern void game_priority(void); /* port/sdl/priority.c: the 2-second beat */

static void game_session(void) {
    hang_trace_start(); /* DKR_HANGTRACE (tests): on the game thread's stack, whichever it is */
    port_game_init();
    sAudioReady = 1;
    port_log("game init done\n");

    while (sRunning) {
        run_one_iteration();
    }
}

#if defined(HANDHELD) && defined(__arm__)
/* The game on a stack at a fixed address (from the Miyoo build, the DKR port e7c4463). A state holds a few
 * pointers into the game thread's stack: Shield and MagnetFX objects keep the stack-built LevelObjectEntry
 * they were spawned from as their level_entry (in every state, menus and races), and the audio heap keeps
 * the odd stale stack address. The binary is not PIE and the main pool is a static array, so every other
 * pointer in a state means the same in any process, but the handheld's kernel puts the process stack
 * somewhere else at every start (ASLR: the [stack] mapping differs in every device log that lists it), so
 * after an resume those few point at random. qemu's stack is always at the same place, and a
 * load within one session doesn't move it, so neither ever shows it. The loop runs on its own 8 MB stack
 * at a fixed address, guard page below, in the same thread: a saved stack address then points into the
 * same stack in every process. musl has no swapcontext, so dkr_call_on_stack (below) makes the one switch:
 * it calls game_session with sp at the top of that stack and puts the process's sp back when it returns.
 * If the address is taken the loop stays on the process stack, as before, and says so. DKR_FIXEDSTACK=0:
 * off. Only new states benefit. */
#include <stdint.h>
#include <sys/mman.h>
#define GAME_STACK_ADDR 0x50000000u
#define GAME_STACK_SIZE (8u << 20)
/* void dkr_call_on_stack(void (*fn)(void), void *top): r4 (callee-saved) keeps the old sp across the call */
void dkr_call_on_stack(void (*fn)(void), void *top);
__asm__(".text\n"
        ".align 2\n"
        ".arm\n"
        ".global dkr_call_on_stack\n"
        ".type dkr_call_on_stack, %function\n"
        "dkr_call_on_stack:\n"
        "    push {r4, lr}\n"
        "    mov r4, sp\n"
        "    mov sp, r1\n"
        "    blx r0\n"
        "    mov sp, r4\n"
        "    pop {r4, pc}\n"
        ".size dkr_call_on_stack, . - dkr_call_on_stack\n");
static void game_session_fixed_stack(void) {
    const char *e = getenv("DKR_FIXEDSTACK");
    void *want = (void *) (uintptr_t) (GAME_STACK_ADDR - 4096u), *got = MAP_FAILED;
    if (!(e && e[0] == '0')) {
        got = mmap(want, GAME_STACK_SIZE + 4096u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (got != MAP_FAILED && got != want) { munmap(got, GAME_STACK_SIZE + 4096u); got = MAP_FAILED; }
    }
    if (got == MAP_FAILED) {
        port_log_always("stack: the process's own (%s)\n", e && e[0] == '0' ? "DKR_FIXEDSTACK=0" : "the fixed address is taken");
        game_session();
        return;
    }
    mprotect(got, 4096u, PROT_NONE); /* the guard page: an overflow faults instead of running into whatever is below */
    port_log_always("stack: fixed at %#x (%u MB; a save's stack addresses mean the same in every process)\n",
                    (unsigned) GAME_STACK_ADDR, GAME_STACK_SIZE >> 20);
    dkr_call_on_stack(game_session, (void *) (uintptr_t) (GAME_STACK_ADDR + GAME_STACK_SIZE));
}
#endif

int main(int argc, char **argv) {
#if defined(MIYOO)
    miyoo_console_setup(argc > 0 ? argv[0] : NULL);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGABRT, on_signal);
    set_dirs(argc > 0 ? argv[0] : NULL);
    port_clock_pick(); /* DKR_VDSOCLOCK: after the data dir (its cfg), before any other thread reads the clock */
    game_priority();
#if defined(MIYOO)
    {   /* The CPU clock (miyoo_clock.c, the shared kit 27a1311): 1500 MHz on the Mini, 1800 on the Mini Plus
         * by default, cpuclock=<MHz> in dkr_display.cfg (0: as the OS set it), DKR_CPUCLOCK=<MHz> wins. Put back
         * at every way out (the exit below, atexit, the crash handlers). The clock goes in the log either way. */
        extern int handheld_cfg_cpuclock(void);
        if (!miyoo_clock_boost_to(handheld_cfg_cpuclock())) { /* left alone: what cpufreq says (never /dev/mem off a Mini) */
            FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
            long khz = 0;
            if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
            if (khz > 0) port_log_always("clock: %ld MHz (cpufreq)\n", khz / 1000);
        }
    }
#endif
    savestate_parse_args(argc, argv); /* --loadStateFile <path> */
    for (int i = 1; i < argc; i++) { /* the Bench launcher: the speed round's windows (kind 7) on this build */
        if (!strcmp(argv[i], "--bench")) { sBench = 0; sBenchPending = 1; sBenchKind = 7; }
    }
    if (getenv("DKR_BENCH")) { sBench = 0; sBenchPending = 1; if (!strcmp(getenv("DKR_BENCH"), "xspeed")) sBenchKind = 7; }
    dev_level_parse();
    { /* Fallback trigger: an empty file named "benchmark" in the data dir runs
       * the benchmark once (and is removed), for launchers that drop Exec args.
       * A file holding "neon=0" runs it with the NEON rows and audio kernels off,
       * the device A/B against the scalar loops (the cross-port lead's round 2);
       * "neon=both" measures each level twice, NEON on and then off, so one run
       * answers it (twice as long: ~12 minutes). */
        const char *flag = port_save_path("benchmark");
        FILE *f = fopen(flag, "rb");
        if (f) {
            char buf[64];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f); remove(flag); sBench = 0; sBenchPending = 1;
            if (strstr(buf, "neon=0")) sBenchNeon = 0;
            if (strstr(buf, "neon=both")) sBenchBoth = 1;
            if (strstr(buf, "noz=both")) { sBenchBoth = 1; sBenchKind = 1; }   /* the speed round's device A/Bs */
            if (strstr(buf, "pres=both")) { sBenchBoth = 1; sBenchKind = 2; }
            if (strstr(buf, "sky=both")) { sBenchBoth = 1; sBenchKind = 3; }
            if (strstr(buf, "bf=both")) { sBenchBoth = 1; sBenchKind = 4; }
            if (strstr(buf, "lazymix=both")) { sBenchBoth = 1; sBenchKind = 5; }
            if (strstr(buf, "lean=both")) { sBenchBoth = 1; sBenchKind = 6; }
            if (strstr(buf, "xspeed=all")) { sBenchBoth = 0; sBenchKind = 7; }
        }
    }
    if (sBench >= 0 && !sBenchNeon) {
        extern void gfx_soft_set_neon(int on), acmd_set_neon(int on);
        gfx_soft_set_neon(0);
        acmd_set_neon(0);
    }
    if (sBench >= 0 && getenv("DKR_BENCH_FROM")) { /* test hook: start at level index n */
        const int n = atoi(getenv("DKR_BENCH_FROM"));
        if (n > 0 && n < BENCH_LEVELS) sBench = n;
    }
    if (sBench >= 0) port_log_always("BENCH mode: %d levels, %u s each after %u s to settle, %s %s\n", BENCH_LEVELS,
                                     (sBenchKind == 7 ? BENCH_MEASURE_WIN_US : BENCH_MEASURE_US) / 1000000u, BENCH_SETTLE_US / 1000000u,
                                     kBenchKindName[sBenchKind], sBenchKind == 7 ? "" : sBenchBoth ? "on, then off (each level twice)" : sBenchNeon ? "on" : "off");
    port_assets_load();
    if (getenv("DKR_ASSETS_ONLY")) return 0;
    port_audio_out_init();
    gfx_init(&gfx_sdl, &gfx_soft_api, "Diddy Kong Racing", false);
    savestate_boot_prompt(); /* resume an auto save, or start new (handheld) */
    { extern const char port_build_rev[]; extern const unsigned int port_build_stamp;
      port_log_always("boot: build %s (stamp %08X)\n", port_build_rev, port_build_stamp); }

#if defined(HANDHELD) && defined(__arm__)
    game_session_fixed_stack();
#else
    game_session();
#endif
    savestate_shutdown();
    port_eeprom_flush();
    port_log_always("SESSION END (clean exit) after %u iterations\n", (unsigned) sFrame);
#if defined(MIYOO)
    { extern void port_audio_evtq_summary(void); port_audio_evtq_summary(); } /* the game's audio event queues: 0 and 0 */
    miyoo_clock_restore(); /* before the SDL shutdown, whose 5 s alarm could end the process */
#endif
    { extern void diag_exit(void); diag_exit(); } /* diagnosing: the session's CPU per task, then the RAM log */
    fflush(NULL);
    {   /* X50 (the OoT port, the user's EXIT GAME freeze): SDL down on every exit, after the logs are out.
         * SDL 1.2's fbcon otherwise leaves the console, keyboard and video modes set up, and the
         * launcher after us can hang at its first flip (gfx_sdl12.c). The line after it (stderr, also
         * console.log; past the RAM log) shows the shutdown itself got through. */
        extern int gfx_wm_shutdown(void);
        fprintf(stderr, gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
    }
    return 0;
}

#include "../../decomp/src/audiosfx.h"
/* DKR_SNDPGUARD (decomp/src/audiosfx.c sndp_voice_handler): an empty sound queue gets its API event back
 * instead of spinning forever. On by default; DKR_SNDPGUARD=0 off. */
int port_sndp_guard(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DKR_SNDPGUARD");
        on = !(e && e[0] == '0');
    }
    return on;
}
/* DKR_SNDPQUEUE=<150..600> (decomp/src/audio.c): the sound player's event queue, 600 by default (the Miyoo
 * build's size; 150 = the game's own). */
s32 port_sndp_queue_size(void) {
    static s32 n = -1;
    if (n < 0) {
        const char *e = getenv("DKR_SNDPQUEUE");
        n = e && e[0] ? atoi(e) : 600;
        if (n < 150) n = 150;
        if (n > 600) n = 600;
        port_log_always("sound queue: %d events%s\n", (int) n, n == 150 ? " (the game's own size)" : "");
    }
    return n;
}
/* Posts dropped by a full event queue (libultra event.c) and lost API events re-posted by the guard
 * (audiosfx.c, csplayer.c): counted for PERF, the first of each logged. */
static u32 sEvtqDrops, sEvtqLost;
void port_audio_evtq_full(void *evtq) {
    extern SoundPlayer *gSoundPlayerPtr;
    if (!sEvtqDrops++) port_log_always("sound queue: full, a post dropped (%s, iteration %u)\n",
                                       gSoundPlayerPtr && evtq == (void *) &gSoundPlayerPtr->evtq ? "sound player" : "music/jingle player", (unsigned) sFrame);
}
void port_audio_evtq_lost(int player) {
    if (!sEvtqLost++) port_log_always("sound queue: ran empty, the %s's API event put back (iteration %u)\n",
                                      player ? "music/jingle player" : "sound player", (unsigned) sFrame);
}
#if defined(MIYOO)
/* the totals at the end of console.log, and for the watchdog's audio line */
void port_audio_evtq_summary(void) { port_log_always("audio: event queues: %u posts dropped, %u lost API events\n", (unsigned) sEvtqDrops, (unsigned) sEvtqLost); }
void port_audio_evtq_counts(unsigned *drops, unsigned *lost) { *drops = sEvtqDrops; *lost = sEvtqLost; }
#endif
/* the sound player's lowest free count since the last PERF line (sampled after each iteration's audio) */
static int sSndqMinFree = 1 << 30;
void port_sndq_sample(void) {
    extern SoundPlayer *gSoundPlayerPtr;
    int n = 0;
    if (!gSoundPlayerPtr) return;
    for (ALLink *l = gSoundPlayerPtr->evtq.freeList.next; l && n < 4096; l = l->next) n++;
    if (n < sSndqMinFree) sSndqMinFree = n;
}
void port_sndq_perf(char *buf, size_t len) {
    static u32 sDrops0;
    snprintf(buf, len, " | sndq min free %d of %d, drops +%u", sSndqMinFree == (1 << 30) ? -1 : sSndqMinFree, (int) port_sndp_queue_size(),
             (unsigned) (sEvtqDrops - sDrops0));
    sDrops0 = sEvtqDrops; sSndqMinFree = 1 << 30;
}
/* DKR_HANGTRACE (tests, dkr-miyoo 2026-10-10): the sound player's spin report (decomp/src/audiosfx.c). */
int port_hangtrace_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DKR_HANGTRACE"); on = e && atoi(e) > 0; }
    return on;
}
void port_sndp_spin_report(void *p) {
    SoundPlayer *sndp = (SoundPlayer *) p;
    ALSndpEvent *ev = (ALSndpEvent *) &sndp->nextEvent;
    fprintf(stderr, "SNDPSPIN iteration %u: next type %d state %p curTime %d frameTime %d events %d\n", (unsigned) sFrame,
            (int) ev->common.type, (void *) ev->common.state, (int) sndp->curTime, (int) sndp->frameTime, (int) sndp->evtq.eventCount);
    ALLink *l = sndp->evtq.allocList.next;
    for (int i = 0; l && i < 24; i++, l = l->next) {
        ALEventListItem *it = (ALEventListItem *) l;
        ALSndpEvent *e = (ALSndpEvent *) &it->evt;
        ALSoundState *st = e->common.state;
        fprintf(stderr, "SNDPSPIN q%-2d item %p delta %d type %d state %p", i, (void *) it, (int) it->delta, (int) e->common.type, (void *) st);
        if (st) fprintf(stderr, " [sound %p next %p prev %p flags %02x state %d]", (void *) st->sound, (void *) st->next, (void *) st->prev, st->flags, st->state);
        fprintf(stderr, "\n");
    }
    _exit(4);
}
