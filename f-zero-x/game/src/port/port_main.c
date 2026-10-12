/**
 * The host loop: plays the N64 hardware around the game's own threads.
 *
 * Per retrace: run the N64 threads until all are blocked; execute the SP tasks
 * they started (display list -> renderer, audio command list -> mixer) and raise
 * the SP/DP events, running the threads again after each; then raise the VI.
 * The game logic therefore ticks exactly once per VI, 60 times a second, like
 * the N64 - the renderer may skip drawing a display list when it is behind,
 * but the game never slows down or speeds up.
 *
 * Environment (desktop / tests):
 *   FZX_ROM=<path>        the player's ROM (default: <data>/fzerox.z64)
 *   FZX_DATA=<dir>        where save, log and dumps go (default ./data)
 *   FZX_HEADLESS=1        no window, no pacing (as fast as possible)
 *   FZX_EXIT_AT=<vi>      quit after that many retraces
 *   FZX_DUMP_AT=<vi,...>  write the picture shown at those retraces to <data>/shot_<vi>.ppm
 *   FZX_PERF=1            the once-a-second PERF line in headless runs too
 *   FZX_MAXSKIP=<n>       pictures skipped in a row at most (default 3: 15 fps floor)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#include "port_platform.h"
#include "gfx/port.h"
#include "gfx/savestate.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#include "miyoo_clock.h"
#endif
#if !defined(HANDHELD_DEVICE) && defined(__has_include)
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define FZX_HAVE_CALLGRIND 1
#endif
#endif

extern void bootproc(void);
extern void port_set_data_dir(const char* dir);
extern void port_overlays_init(void);
extern int port_find_and_open_rom(void);

/* Monotonic microseconds. musl 1.2's clock_gettime tries the time64 syscall
 * first and the handheld's 4.14 kernel lacks it, so every read cost two kernel
 * entries; SYS_clock_gettime32 goes straight to the one that exists.
 * (MK64 perf round 24, via ocarina-of-time d267ca3.) */
static uint32_t time_us_syscall(void) {
#if defined(HANDHELD_DEVICE) && defined(SYS_clock_gettime32)
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0)
        return (u32) ((u64) t.s * 1000000u + (u32) (t.ns / 1000));
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32) ((u64) ts.tv_sec * 1000000u + (u32) (ts.tv_nsec / 1000));
}

/* FZX_VDSOCLOCK (default 1; 0 = the syscall always, as before; environment only, as the port's other switches): the kernel's own __vdso_clock_gettime, the
 * same CLOCK_MONOTONIC read in user space, instead of a syscall. musl 1.2.1 never looks the vDSO up.
 * GoldenEye's device Bench (goldeneye-007 X37, hh_os.c): 157-167 ns a read against 598-611 ns for the syscall.
 * Taken from it (as mario-kart-64's MK64_VDSOCLOCK): the vDSO is found through AT_SYSINFO_EHDR's symbol table and used
 * only if, best of three trials of 256 reads each, its readings lie between the syscall's around them and cost under
 * half as much; one CLOCK line says which. Where port_clock_init's generic timer is in use it is read directly and
 * this is not reached; it is the read otherwise (FZX_CLOCK=0, the probe failing, the bench's "syscall clock" runs). Only the cost of a read changes, never the clock read. */
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* the address is this process's, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO */

static uint64_t port_clock_us_sys(void)
{
#if defined(HANDHELD_DEVICE) && defined(SYS_clock_gettime32)
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0)
        return (uint64_t) t.s * 1000000u + (uint32_t) (t.ns / 1000);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint32_t) (ts.tv_nsec / 1000);
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

/* Once, at start-up before any other thread (time_us_base picks on its first read otherwise). */
static void port_clock_pick(void)
{
    uint64_t v = ~0ull, s = ~0ull, tv, ts;
    int i, on, same = 1;
    if (sClockMode >= 0) return;
    on = !(getenv("FZX_VDSOCLOCK") && getenv("FZX_VDSOCLOCK")[0] == '0');
    sVdsoCgt = on ? (PortCgt32)port_vdso_sym("__vdso_clock_gettime") : NULL;
    if (sVdsoCgt == NULL) {
        sClockMode = 0;
        port_log("CLOCK syscall (%s)\n", on ? "no vDSO clock_gettime" : "FZX_VDSOCLOCK=0");
        return;
    }
    for (i = 0; i < 3; i++) { /* the best of three (a GoldenEye device run once caught an interrupt inside one) */
        same &= port_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    sClockMode = same && v * 2 < s;
    port_log("CLOCK %s: vDSO %u ns/read vs syscall %u ns/read\n", sClockMode ? "vdso" : "syscall", (unsigned)v, (unsigned)s);
    if (!sClockMode) sVdsoCgt = NULL;
}

static uint32_t time_us_base(void)
{
    if (sClockMode > 0) return (uint32_t)port_clock_us_vdso();
    if (sClockMode < 0) port_clock_pick();
    return (uint32_t)(sClockMode > 0 ? port_clock_us_vdso() : port_clock_us_sys());
}

#if defined(HANDHELD_DEVICE) && defined(__arm__)
/* Still a kernel entry per read, and the profile buckets and the pacer read the clock a
 * few hundred times a picture. The Cortex-A7's generic timer counts in user space when
 * the kernel allows it (CNTKCTL.PL0VCTEN, which Linux sets unless an erratum needs
 * trapping): port_clock_init probes CNTFRQ/CNTVCT under a SIGILL guard, checks the count
 * against CLOCK_MONOTONIC over 20 ms, and only then switches; the offset keeps the two
 * clocks continuous, so the bench can switch between them (port_clock_fast). FZX_CLOCK=0
 * keeps the syscall. */
static volatile int sClockFast;       /* 1: CNTVCT */
static int sClockOk;                  /* the probe passed */
static uint32_t sCntMult, sCntOffset; /* us = (count * sCntMult) >> 32, + offset */
static inline uint64_t cntvct(void) {
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi) : : "memory");
    return ((uint64_t) hi << 32) | lo;
}
static inline uint32_t cnt_us(uint64_t c) {
    return (uint32_t) ((uint64_t) (uint32_t) (c >> 32) * sCntMult + (((uint64_t) (uint32_t) c * sCntMult) >> 32));
}
static sigjmp_buf sClockJmp;
static void clock_sigill(int sig) {
    (void) sig;
    siglongjmp(sClockJmp, 1);
}
static void port_clock_init(void) {
    const char* e = getenv("FZX_CLOCK");
    port_clock_pick(); /* FZX_VDSOCLOCK */
    struct sigaction sa, old;
    volatile uint32_t freq = 0;
    volatile int ok = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = clock_sigill;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old);
    if (sigsetjmp(sClockJmp, 1) == 0) {
        uint32_t f;
        __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
        freq = f;
        (void) cntvct();
        ok = 1;
    }
    sigaction(SIGILL, &old, NULL);
    if (!ok || freq < 1000000u || freq > 200000000u) {
        port_log("clock: syscall (the generic timer %s)\n", ok ? "reports no usable frequency" : "traps in user space");
        return;
    }
    {
        const uint32_t t0 = time_us_syscall();
        const uint64_t k0 = cntvct();
        struct timespec ts = { 0, 20000000 };
        nanosleep(&ts, NULL);
        const uint32_t t1 = time_us_syscall();
        const uint64_t k1 = cntvct();
        const double cus = (double) (k1 - k0) * 1e6 / freq, sus = (double) (u32) (t1 - t0);
        if (cus < sus * 0.99 - 50.0 || cus > sus * 1.01 + 50.0) {
            port_log("clock: syscall (the generic timer at %u Hz ran %.0f us against %.0f)\n", (unsigned) freq, cus, sus);
            return;
        }
    }
    sCntMult = (uint32_t) (((uint64_t) 1000000u << 32) / freq);
    sCntOffset = time_us_syscall() - cnt_us(cntvct());
    sClockOk = 1;
    sClockFast = !(e && e[0] == '0');
    port_log("clock: generic timer %u Hz, read in user space%s\n", (unsigned) freq, sClockFast ? "" : " (off: FZX_CLOCK=0)");
}
/* bench.c: 1 = the generic timer (when the probe passed), 0 = the syscall; returns the mode. */
int port_clock_fast(int on) {
    sClockFast = on && sClockOk;
    return sClockFast;
}
uint32_t port_time_us(void) {
    if (sClockFast) {
        return cnt_us(cntvct()) + sCntOffset;
    }
    return time_us_base(); /* the vDSO or the syscall (FZX_VDSOCLOCK) */
}
#else
static void port_clock_init(void) {
    port_clock_pick(); /* FZX_VDSOCLOCK */
}
int port_clock_fast(int on) {
    (void) on;
    return 0;
}
uint32_t port_time_us(void) {
    return time_us_base(); /* the vDSO or the syscall (FZX_VDSOCLOCK) */
}
#endif

/* To an absolute deadline, again after a signal: a relative nanosleep returns early on any
 * handled signal, and the Bench's profiled runs (sdl/sprof.c) raise SIGPROF every 500 us, so their
 * pacer never slept more than 0.5 ms and the game ran ahead of the clock (a host check: 15 ms asked,
 * 0.5 ms slept). Retrying with the same deadline doesn't add the timer slack once per signal. */
static void sleep_us(u32 us) {
    struct timespec dl;
    int i;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += (time_t) (us / 1000000u);
    dl.tv_nsec += (long) (us % 1000000u) * 1000;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    for (i = 0; i < 1000 && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) != 0; i++) {
    }
}

static long env_long(const char* name, long def) {
    const char* v = getenv(name);
    return (v && v[0]) ? atol(v) : def;
}

static int dump_wanted(u32 vi) {
    static char spec[16384]; /* 512 cut a list of ~100 VIs short without a word (round 11) */
    static int init;
    if (!init) {
        const char* e = getenv("FZX_DUMP_AT");
        if (e) {
            if (snprintf(spec, sizeof(spec), ",%s,", e) >= (int) sizeof(spec)) {
                port_log("FZX_DUMP_AT: the list is longer than %u characters; the rest is ignored\n",
                         (unsigned) sizeof(spec) - 3);
            }
        }
        init = 1;
    }
    if (!spec[0]) {
        return 0;
    }
    char key[24];
    snprintf(key, sizeof(key), ",%u,", (unsigned) vi);
    return strstr(spec, key) != NULL;
}

PortStats gPortStats;
int gPortStateLoaded; /* a state was loaded in this process (savestate.c; FZX_LOADPIC) */

/* Profile buckets (gfx/port.h) shared with the renderer: the host fills LOGIC,
 * AUDIO, GFX, SLEEP and ITER; the renderer its own raster buckets. */

/* ---------------------------------------------------------------- pacer --- */
/* The logic ticks at 60 Hz by the wall clock and never slips; pictures give.
 * Device round 4: skipping whenever a retrace started late gave pictures at
 * irregular 2-4 tick gaps once a picture cost ~45 ms - it looked choppier than
 * its 15-20 fps. Now the pacer measures what a picture costs (EMA, plus the
 * per-tick logic and audio) and draws on every Nth tick, N = the fewest ticks
 * that hold one: an even 60, 30, 20 or 15 fps. It steps down once pictures
 * stop fitting and back up after ~30 pictures with room to spare.
 * A slot that starts more than half a tick late still waits a tick (at most
 * sMaxSkips in a row), so the ticks never fall behind the wall clock.
 * Round 11: one slow picture (textures re-imported after a load, an OS call)
 * moves neither the average nor the cadence. A sample counts at most twice the
 * average, and the cadence only drops after PACER_LATE_RUN pictures in a row
 * that miss it. One 53-99 ms hitch used to hold 15 fps for about a second
 * (the round-9 device bench, after every caption). FZX_PACER=0: as before (A/B).
 * Round 12: two uneven cadences between 60 and 30 as well: 45 fps drawn on ticks 1, 1, 2
 * and 40 fps on 1, 2. The steps between shown pictures then differ (16.7 or 33.3 ms of
 * game time), where every even cadence keeps them equal. On by default since round 13d
 * (the 13c device bench: ~40 fps against 29.5 in the same race; the user: "Yes");
 * FZX_CADENCE=0 keeps the even cadences only. */
#define VI_US 16667u
#define PACER_LATE_RUN 4
static u32 sSlotLateUs; /* how late this retrace started */
static int sSkipsInRow, sMaxSkips = 3;
static u32 sSlowGfxUs;
static u32 sHitchFrom, sHitchTo, sHitchUs; /* FZX_HITCH (pacer test) */
static int sHitchDone;
static u32 sPicEmaUs;             /* a drawn picture's cost */
static u32 sTickEmaUs = 1500;     /* logic + audio per tick */
/* Cadences, fastest first: ticks and pictures per repeat, and the tick gaps in it. */
/* Round 15 (the user, 2026-09-29: "gain some speed somehow without downscaling anything"): finer
 * steps between 60 and 45 -- 52.5, 50 and 48 fps, one repeated tick in 8, 6 or 5 instead of one in
 * 4 (fine: FZX_FINECAD=0 leaves them out). A race picture of ~16.3 ms (the 1df5e94 device run) fits
 * 7 in 8 ticks where 60 needs <= 14.4 ms, so the pacer held 45. The pictures are the same. */
static const struct {
    u8 ticks, pics, fine, gap[7];
    const char* name;
} kCad[] = {
    { 1, 1, 0, { 1 }, "1" },
    { 8, 7, 1, { 1, 1, 1, 1, 1, 1, 2 }, "8/7" },
    { 6, 5, 1, { 1, 1, 1, 1, 2 }, "6/5" },
    { 5, 4, 1, { 1, 1, 1, 2 }, "5/4" },
    { 4, 3, 0, { 1, 1, 2 }, "4/3" },
    { 3, 2, 0, { 1, 2 }, "3/2" },
    { 2, 1, 0, { 2 }, "2" },
    { 3, 1, 0, { 3 }, "3" },
    { 4, 1, 0, { 4 }, "4" },
};
#define NCAD ((int) (sizeof(kCad) / sizeof(kCad[0])))
static int sCad, sDivDown;        /* the cadence (kCad), and pictures in a row that fit a faster one */
static int sGap;                  /* this picture's place in the cadence's gaps */
static int sUneven = -1;          /* the uneven cadences too (FZX_CADENCE=0: off) */
static int sFineCad = -1;         /* the fine uneven cadences too (FZX_FINECAD=0: off) */
static int sLateRun;              /* pictures in a row that missed the cadence */
static int sPacerRobust = -1;     /* FZX_PACER=0: the round-1 pacer */

/* bench.c: the uneven cadences on (1) or off (0) for a run */
void port_pacer_set_uneven(int on) {
    sUneven = on ? 1 : 0;
}
/* bench.c: the fine cadences on (1) or off (0) for a run */
void port_pacer_set_fine(int on) {
    sFineCad = on ? 1 : 0;
}
/* may cadence c be used: even, or uneven with the uneven ones on (and fine with the fine ones) */
static int cad_allowed(int c) {
    return kCad[c].pics == 1 || (sUneven && (!kCad[c].fine || sFineCad));
}

/* bench.c: each setting starts the pacer afresh. */
void port_pacer_reset(void) {
    sPicEmaUs = 0;
    sCad = 0;
    sGap = 0;
    sDivDown = 0;
    sSkipsInRow = 0;
    sLateRun = 0;
}

#if defined(MIYOO)
/* The Miyoo Mini's pacing (the user's rule for the Miyoo builds, 2026-10-11: no smoothing, no even
 * "SMOOTH" cadence by default): FAST, every picture drawn as soon as its tick comes, the game's own
 * 60 a second when it fits; a tick that starts more than half a VI late skips its picture (at most
 * sMaxSkips in a row), so the logic stays on the wall clock, as the N64's 60 Hz does. No prediction
 * and no cadence: kCad[0] ("1") throughout. FZX_PACE=cadence: the handheld's cadence pacer (A/B). */
static int sPaceFast = -1;
static int pace_fast(void) {
    if (sPaceFast < 0) {
        const char* e = getenv("FZX_PACE");
        sPaceFast = !(e && e[0] == 'c');
    }
    return sPaceFast;
}
#endif

static int pacer_should_draw(void) {
    if (sSkipsInRow >= sMaxSkips) return 1;
#if defined(MIYOO)
    if (pace_fast()) return sSlotLateUs <= VI_US / 2;
#endif
    if (sSkipsInRow + 1 < kCad[sCad].gap[sGap]) return 0;
    return sSlotLateUs <= VI_US / 2;
}

/* Does cadence c hold pictures of pic us, with tick us of logic and audio every tick? A fine one
 * must fit 92% of its ticks, not 95%: its run of drawn ticks piles up more lateness before its one
 * gap, and at 95% it took pictures that then missed their slots (tools/pacer_sim.sh, +-10% costs:
 * at 16.3 ms 50.0 fps and 1.4 skips a second off the pattern, at 92% 49.2 fps and 1.0). */
static int cad_fits(int c, u32 pic, u32 tick) {
    return (u32) kCad[c].pics * pic + (u32) kCad[c].ticks * tick <= (u32) kCad[c].ticks * VI_US * (kCad[c].fine ? 92u : 95u) / 100;
}

static void pacer_note_picture(u32 us) {
    int n = 0;
#if defined(MIYOO)
    if (pace_fast()) { /* FAST: only the average, for the PERF line */
        sPicEmaUs = sPicEmaUs ? (sPicEmaUs * 7 + us) / 8 : us;
        sCad = 0;
        return;
    }
#endif
    if (sPacerRobust < 0) {
        const char *e = getenv("FZX_PACER");
        sPacerRobust = !(e && e[0] == '0');
    }
    if (sUneven < 0) {
        const char *e = getenv("FZX_CADENCE");
        sUneven = !(e && e[0] == '0');
    }
    if (sFineCad < 0) {
        const char *e = getenv("FZX_FINECAD");
        sFineCad = !(e && e[0] == '0');
    }
    sGap = (sGap + 1) % kCad[sCad].pics;
    if (sPacerRobust) {
        const u32 sample = sPicEmaUs && us > 2 * sPicEmaUs ? 2 * sPicEmaUs : us;
        sLateRun = !cad_fits(sCad, us, sTickEmaUs) ? sLateRun + 1 : 0;
        sPicEmaUs = sPicEmaUs ? (sPicEmaUs * 7 + sample) / 8 : us;
    } else {
        sPicEmaUs = sPicEmaUs ? (sPicEmaUs * 7 + us) / 8 : us;
    }
    /* the fastest allowed cadence that holds the average, down to one picture in
     * sMaxSkips + 1 ticks */
    for (;;) {
        int m = n + 1;
        if (cad_allowed(n) && cad_fits(n, sPicEmaUs, sTickEmaUs)) break;
        while (m < NCAD && !cad_allowed(m)) m++;
        if (m >= NCAD || kCad[m].ticks > (sMaxSkips + 1) * kCad[m].pics) break;
        n = m;
    }
    if (n > sCad) {
        sDivDown = 0;
        if (!sPacerRobust || sLateRun >= PACER_LATE_RUN) {
            sCad = n;
            sGap = 0;
            sLateRun = 0;
        }
    } else if (n < sCad) {
        if (++sDivDown >= 30) {
            sCad = n;
            sGap = 0;
            sDivDown = 0;
        }
    } else {
        sDivDown = 0;
    }
}

/* One line a second: what the device must hold for the M3 gate. */
static struct {
    u32 vis, pics, skips, slips, maxLateUs, underruns0;
    u32 sum[PROF_COUNT];
} sPerf;

int port_bench_active(void);
void port_bench_parse(int argc, char** argv);
void port_bench_check(void);
void port_bench_note_second(u32 pics, u32 skips, u32 slips, u32 lateMaxUs, u32 underruns, const u32* sum, u32 pxPerPic);

/* bench.c: start a fresh one-second window (its measured seconds line up with it). */
void port_perf_restart(void) {
    extern uint64_t gfx_soft_span_px, gfx_soft_tris;
    const u32 u = port_audio_underruns();
    memset(&sPerf, 0, sizeof(sPerf));
    sPerf.underruns0 = u;
    gfx_soft_span_px = 0;
    gfx_soft_tris = 0;
}

static void perf_vi(u32 lateUs) {
    int i;
    sPerf.vis++;
    if (lateUs > sPerf.maxLateUs) {
        sPerf.maxLateUs = lateUs;
    }
    for (i = 0; i < PROF_COUNT; i++) {
        sPerf.sum[i] += gPortProfFrame[i];
        gPortProfLast[i] = gPortProfFrame[i];
        gPortProfFrame[i] = 0;
    }
    if (sPerf.vis == PORT_VI_HZ) {
        const u32 u = port_audio_underruns();
        const float n = (float) sPerf.vis * 1000.0f; /* per-VI ms */
        extern uint64_t gfx_soft_span_px, gfx_soft_tris;
        extern uint32_t gfx_soft_phase_switches, gfx_soft_arena_peak;
        const u32 pics = sPerf.pics ? sPerf.pics : 1;
        const u32 flipUs = gPortProfFlipUs;
        gPortProfFlipUs = 0;
        /* Normal play: the PERF line of every 10th second only. The Bench, and FZX_LOGVERBOSE=1 (today's logging, for
         * debugging), write all of them. port_bench_note_second below sees every second either way. */
        static int sVerbose = -1;
        static u32 sSec;
        if (sVerbose < 0) { const char* e = getenv("FZX_LOGVERBOSE"); sVerbose = e != NULL && e[0] == '1'; }
        if (sVerbose || port_bench_active() || ++sSec % 10 == 0)
        port_log("PERF vi %u pics %u skip %u slip %u late %.1f div %s | res %d filt %d | logic %.1f gfx %.1f (3d %.1f 2d %.1f tex %.1f comp %.1f pres %.1f) "
                 "audio %.1f sleep %.1f | underruns %u | tris %u px %u phase %u arena %u%% | pwait %.2f clr %.2f flip %.1f"
                 " | ring min %u gaps %u\n",
                 (unsigned) port_os_vi_count(), (unsigned) sPerf.pics, (unsigned) sPerf.skips, (unsigned) sPerf.slips,
                 sPerf.maxLateUs / 1000.0f, kCad[sCad].name, port_res3d_pct(), port_filter3d(), sPerf.sum[PROF_LOGIC] / n, sPerf.sum[PROF_GFX] / n, sPerf.sum[PROF_R3D] / n,
                 sPerf.sum[PROF_R2D] / n, sPerf.sum[PROF_TEX] / n, sPerf.sum[PROF_COMP] / n, sPerf.sum[PROF_PRES] / n,
                 sPerf.sum[PROF_AUDIO] / n, sPerf.sum[PROF_SLEEP] / n, (unsigned) (u - sPerf.underruns0),
                 (unsigned) (gfx_soft_tris / pics), (unsigned) (gfx_soft_span_px / pics), /* per picture */
                 (unsigned) (gfx_soft_phase_switches / pics), (unsigned) gfx_soft_arena_peak,
                 sPerf.sum[PROF_PWAIT] / n, sPerf.sum[PROF_CLR] / n, flipUs / n, (unsigned) port_audio_take_ring_min(),
                 (unsigned) port_audio_gaps());
#if defined(MIYOO)
        if (sVerbose || port_bench_active() || sSec % 10 == 0) { /* the second core, with the PERF line */
            extern void gfx_soft_split_counts(unsigned * split, unsigned * flushes);
            extern void miyoo_worker_stats(unsigned * jobs, unsigned * sleeps, unsigned long long * spins);
            extern int miyoo_worker_ready(void), miyoo_clock_mhz(void);
            unsigned split, flushes, jobs, sleeps;
            unsigned long long spins;
            gfx_soft_split_counts(&split, &flushes);
            miyoo_worker_stats(&jobs, &sleeps, &spins);
            port_log("MT %s: 3D flushes split %u/%u, jobs %u, sleeps %u | clock %d MHz\n", miyoo_worker_ready() ? "on" : "off",
                     split, flushes, jobs, sleeps, miyoo_clock_mhz());
            if (getenv("FZX_MTPROF")) { /* thread CPU ms per second: one core's 3D, the shares, the slower shares summed */
                extern void gfx_soft_mtprof(uint64_t * one, uint64_t * even, uint64_t * odd, uint64_t * mx);
                uint64_t one, even, odd, mx;
                gfx_soft_mtprof(&one, &even, &odd, &mx);
                port_log("MTPROF one %.1f even %.1f odd %.1f slower %.1f ms\n", one / 1e6, even / 1e6, odd / 1e6, mx / 1e6);
            }
        }
#endif
        port_bench_note_second(sPerf.pics, sPerf.skips, sPerf.slips, sPerf.maxLateUs, u - sPerf.underruns0, sPerf.sum,
                               (u32) (gfx_soft_span_px / pics));
        gfx_soft_phase_switches = 0;
        gfx_soft_arena_peak = 0;
        gfx_soft_tris = 0;
        gfx_soft_span_px = 0;
        memset(&sPerf, 0, sizeof(sPerf));
        sPerf.underruns0 = u;
    }
}

/* Where the host loop is, for a diagnostic log line: whether the loop still turns (iterations since
 * the last call) and, if not, in which stage and game thread it stopped. Written by the main thread only. */
enum { STAGE_BOOT, STAGE_THREADS, STAGE_DRAW, STAGE_AUDIO, STAGE_CHECK, STAGE_SLEEP, STAGE_RETRACE };
static volatile u32 sLoopIter;
static volatile int sLoopStage;
unsigned handheld_loop_detail(char* buf, size_t n) {
    static const char* const kStage[] = { "boot", "game threads", "drawing", "audio task", "save state check", "pacer sleep", "retrace" };
    extern s32 gGameMode;
    const int st = sLoopStage;
    if (n > 0) {
        snprintf(buf, n, "stage %s, game thread %d, vi %u, game mode %d, display lists %u (drawn %u)",
                 kStage[st >= 0 && st <= STAGE_RETRACE ? st : 0], port_os_running_id(), (unsigned) port_os_vi_count(),
                 (int) gGameMode, (unsigned) gPortStats.gfxTasks, (unsigned) gPortStats.gfxRendered);
    }
    return sLoopIter;
}

/* sdl/priority.c: nice -10 first thing in main (the 2-second hitch of an OS helper), logged later */
void port_game_priority(void);
void port_game_priority_log(void);

int main(int argc, char** argv) {
    char err[256];
    const char* data;
    port_game_priority(); /* before anything else, so every thread and child inherits it (sdl/priority.c) */
    data = getenv("FZX_DATA");
    (void) err;
    {
        /* FZX_HEAPSHIFT=<bytes>: move the heap, as the device's ASLR does every boot, to
         * test that a state from another process loads (MK64 v25). */
        const char* e = getenv("FZX_HEAPSHIFT");
        if (e && atol(e) > 0) {
            volatile char* p = malloc((size_t) atol(e));
            if (p) p[0] = 1;
        }
    }
    savestate_parse_args(argc, argv);
    port_bench_parse(argc, argv); /* fzerox_bench / FZX_BENCH=1: the on-device benchmark */
#if defined(MIYOO)
    /* the Miyoo Mini: data/ next to the executable (App/FZeroX/data), or FZX_DATA (launch.sh sets it) */
    if (!data || !data[0]) {
        data = miyoo_data_dir();
    }
#else
    if (!data || !data[0]) {
        data = "./data";
    }
#endif
    mkdir(data, 0777);
    port_set_data_dir(data);

#ifndef FZX_BUILD_ID
#define FZX_BUILD_ID "dev"
#endif
    /* "SESSION <game> build ..." first, as OoT's console.log (the lead's consistency list) */
    port_log("SESSION fzerox build %s (%s)\n", FZX_BUILD_ID,
#if defined(MIYOO)
             "Miyoo Mini"
#else
             "desktop"
#endif
    );
    port_game_priority_log();
#if defined(MIYOO)
    miyoo_clock_boost(); /* 1500 MHz Mini, 1800 Mini Plus (FZX_CPUCLOCK=<MHz>, 0 = leave it); put back at exit (atexit) */
#endif
    port_clock_init(); /* before any thread starts */
    if (!port_find_and_open_rom()) {
        return 1;
    }

    const int headless = env_long("FZX_HEADLESS", 0) != 0;
    const long exitAt = env_long("FZX_EXIT_AT", -1);
    port_platform_init(headless);
    if (!port_bench_active()) {
        savestate_boot_prompt(); /* after the menu is up: RESUME GAME / NEW GAME */
    }
    port_overlays_init();
    port_os_init();

    bootproc();

    const int perf = !headless || env_long("FZX_PERF", 0) != 0;
    sMaxSkips = (int) env_long("FZX_MAXSKIP", 3);
    sSlowGfxUs = (u32) env_long("FZX_SLOWGFX", 0);
    {
        const char* h = getenv("FZX_HITCH");
        unsigned a, b, c;
        if (h && sscanf(h, "%u-%u:%u", &a, &b, &c) == 3) {
            sHitchFrom = a;
            sHitchTo = b;
            sHitchUs = c;
        }
    }
    u32 nextVi = port_time_us();
    for (;;) {
        u32 t0 = port_time_us(), t1;
        sLoopIter++;
        sLoopStage = STAGE_THREADS;
        port_os_run();
        gPortProfFrame[PROF_LOGIC] += port_time_us() - t0;
        for (;;) {
            OSTask* task = port_os_take_sp_task();
            if (task == NULL) {
                break;
            }
            t1 = port_time_us();
            sLoopStage = task->t.type == M_AUDTASK ? STAGE_AUDIO : STAGE_DRAW;
            if (task->t.type == M_GFXTASK) {
                if (headless || pacer_should_draw()) {
                    port_platform_gfx_task((Gfx*) task->t.data_ptr);
                    if (sSlowGfxUs != 0 || sHitchUs != 0) {
                        /* FZX_SLOWGFX=<us>: a device-slow renderer on the desktop (pacer test).
                         * FZX_HITCH=<from>-<to>:<us>: the pictures drawn from VI <from> (at least
                         * one) to <to> take <us> instead (a hitch, or a slow stretch). */
                        const u32 vi = port_os_vi_count();
                        u32 us = sSlowGfxUs;
                        if (sHitchUs != 0 && vi >= sHitchFrom && (vi <= sHitchTo || !sHitchDone)) {
                            us = sHitchUs;
                            sHitchDone = 1;
                        }
                        const u32 until = t1 + us;
                        while ((s32) (until - port_time_us()) > 0) {
                        }
                    }
                    sSkipsInRow = 0;
                    sPerf.pics++;
                    if (!headless) {
                        pacer_note_picture(port_time_us() - t1);
                    }
                } else {
                    extern void port_platform_gfx_skipped(Gfx * dl);
                    port_platform_gfx_skipped((Gfx*) task->t.data_ptr); /* for port_gfx_readback */
                    sSkipsInRow++; /* not drawn: the game only needs to hear it finished */
                    sPerf.skips++;
                }
                gPortStats.gfxTasks++;
                gPortProfFrame[PROF_GFX] += port_time_us() - t1;
                port_os_event(OS_EVENT_SP);
                port_os_event(OS_EVENT_DP);
            } else if (task->t.type == M_AUDTASK) {
                port_platform_audio_task((u64*) task->t.data_ptr, task->t.data_size);
                gPortStats.audioTasks++;
                gPortProfFrame[PROF_AUDIO] += port_time_us() - t1;
                port_os_event(OS_EVENT_SP);
            } else {
                port_log("sp: unknown task type %u\n", task->t.type);
                port_os_event(OS_EVENT_SP);
            }
            t1 = port_time_us();
            sLoopStage = STAGE_THREADS;
            port_os_run();
            gPortProfFrame[PROF_LOGIC] += port_time_us() - t1;
        }
        gPortStats.workUs += port_time_us() - t0;

        sLoopStage = STAGE_CHECK;
        if (!port_platform_poll()) {
            break;
        }
        savestate_check(); /* every fiber is blocked, no RSP task is pending */
        port_bench_check();
        sLoopStage = STAGE_SLEEP;
        u32 lateUs = 0;
        if (!headless) {
            /* 60 Hz by the wall clock; never make up lost time by running fast.
             * A "wait" over a second is no tick's remainder (at most one VI): the 32-bit clock
             * moved more than 2^31 us (35.8 min) since the last tick and the difference wrapped
             * positive. The handheld menu runs inside a tick, so leaving it open that long did it:
             * closed after 40 minutes, the game slept 31.6 more on its last picture with the
             * music playing (ocarina-of-time 1138e29, the cross-port list's C). It is a hitch. */
            nextVi += 1000000u / PORT_VI_HZ;
            s32 wait = (s32) (nextVi - port_time_us());
            if (wait > 0 && wait <= 1000000) {
                t1 = port_time_us();
                sleep_us((u32) wait);
                gPortProfFrame[PROF_SLEEP] += port_time_us() - t1;
            } else if (wait < -100000 || wait > 0) {
                nextVi = port_time_us(); /* a hitch (load, pause, a long menu): resume from now -- a slip */
                sPerf.slips++;
            } else {
                lateUs = (u32) -wait;
            }
        }
        sSlotLateUs = lateUs;
        {
            const u32 tickUs = gPortProfFrame[PROF_LOGIC] + gPortProfFrame[PROF_AUDIO];
            sTickEmaUs = (sTickEmaUs * 15 + tickUs) / 16;
        }
        gPortProfFrame[PROF_ITER] += port_time_us() - t0;
        sLoopStage = STAGE_RETRACE;
        if (perf) {
            perf_vi(lateUs);
        } else {
            int i;
            for (i = 0; i < PROF_COUNT; i++) {
                gPortProfLast[i] = gPortProfFrame[i];
                gPortProfFrame[i] = 0;
            }
        }
        port_os_vi();
#ifdef FZX_HAVE_CALLGRIND
        {
            /* FZX_CGSTART=<vi>: under valgrind --instr-atstart=no, profile from that retrace on (desktop). */
            static long sCgStart = -2;
            if (sCgStart == -2) sCgStart = env_long("FZX_CGSTART", -1);
            if (sCgStart >= 0 && (long) port_os_vi_count() == sCgStart) CALLGRIND_START_INSTRUMENTATION;
        }
#endif
        /* FZX_LOADPIC (platform_sdl.c): a process that resumes a state at boot (--loadStateFile, RESUME GAME) shows
         * no N64 logo while it loads, and after the load the picture is the saving process's last frame, drawn
         * again at the load, not the CPU's framebuffer from the restored RDRAM (the boot's logo). */
        extern int port_platform_loadpic_on(void), savestate_boot_load_pending(void);
        if (gPortStats.gfxRendered == 0 &&
            !((gPortStateLoaded || savestate_boot_load_pending()) && port_platform_loadpic_on())) {
            /* Boot: until the first display list the N64 shows what the CPU put
             * in the framebuffer, the N64 logo (sys_main.c), for about 4 s. */
            static const void* sShownFb;
            const void* fb = port_os_vi_framebuffer();
            if (fb != NULL && fb != sShownFb) {
                port_platform_show_framebuffer(fb);
                sShownFb = fb;
            }
        }
        port_platform_vi(port_os_vi_count());
        {
            /* FZX_STATEHASH=1: one line per VI hashing the RNG seeds and every
             * racer's velocity, speed and position (floats only, no pointers),
             * to find where two builds' game state first differs. */
            static int sHash = -1;
            if (sHash < 0) {
                const char* e = getenv("FZX_STATEHASH");
                sHash = e != NULL && e[0] == '1';
            }
            if (sHash) {
                extern s32 gRandSeed1, gRandSeed2;
                extern u8 gRacers[];
                u32 h = 2166136261u, i, k;
                const u32 seeds[2] = { (u32) gRandSeed1, (u32) gRandSeed2 };
                for (k = 0; k < 8; k++) {
                    h = (h ^ ((const u8*) seeds)[k]) * 16777619u;
                }
                for (i = 0; i < 30; i++) {
                    const u8* r = gRacers + i * 936;
                    for (k = 0; k < 12; k++) h = (h ^ r[116 + k]) * 16777619u; /* velocity */
                    for (k = 0; k < 4; k++) h = (h ^ r[152 + k]) * 16777619u;  /* speed */
                    for (k = 0; k < 12; k++) h = (h ^ r[268 + k]) * 16777619u; /* modelPos */
                }
                port_log("sh %u %08X %08X %08X\n", (unsigned) port_os_vi_count(), (unsigned) seeds[0], (unsigned) seeds[1], (unsigned) h);
            }
        }
        if (dump_wanted(port_os_vi_count())) {
            char name[64];
            snprintf(name, sizeof(name), "shot_%05u.ppm", (unsigned) port_os_vi_count());
            port_platform_dump(port_data_path(name));
        }
        {
            /* FZX_DUMP_POLLS=<p1>,<p2>,...: the picture after controller read p (input.c's
             * gPortPolls, the clock of FZX_PKEYS and of m64ref.sh's shot lines) as
             * poll_<p>.ppm, for tools/bestmatch.py (T13). */
            extern u32 gPortPolls;
            static char sPolls[16384];
            static int sInit;
            static u32 sLast;
            if (!sInit) {
                const char* e = getenv("FZX_DUMP_POLLS");
                if (e && snprintf(sPolls, sizeof(sPolls), ",%s,", e) >= (int) sizeof(sPolls)) {
                    port_log("FZX_DUMP_POLLS: the list is longer than %u characters; the rest is ignored\n",
                             (unsigned) sizeof(sPolls) - 3);
                }
                sInit = 1;
            }
            if (sPolls[0] && gPortPolls != sLast) {
                char key[24];
                sLast = gPortPolls;
                snprintf(key, sizeof(key), ",%u,", (unsigned) gPortPolls);
                if (strstr(sPolls, key) != NULL) {
                    char name[64];
                    snprintf(name, sizeof(name), "poll_%06u.ppm", (unsigned) gPortPolls);
                    port_platform_dump(port_data_path(name));
                }
            }
        }
        if (exitAt >= 0 && (long) port_os_vi_count() >= exitAt) {
            break;
        }
    }
    savestate_shutdown();
    port_log("exit at vi %u: %u gfx tasks, %u audio tasks\n", port_os_vi_count(), gPortStats.gfxTasks,
             gPortStats.audioTasks);
    port_platform_quit();
    return 0;
}
