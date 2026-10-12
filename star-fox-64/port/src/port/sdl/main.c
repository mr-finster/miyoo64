/**
 * Star Fox 64 -> handheld / desktop port (from the MK64 handheld kit): process entry point and the port_*
 * host hooks that don't belong to a specific subsystem (time, logging, save
 * paths, frame driving). The sceGu/PSP original is src/port/psp/psp_main.c.
 *
 * The game's data is not in the executable: port_rom_open() (sf64/port_rom.c)
 * maps the port ROM image, built from the player's ROM, before the game's
 * first DMA.
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

#include <ultra64.h>
#include "port.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "savestate.h"
#if defined(MIYOO)
#include <SDL/SDL.h>
#include "miyoo_panel.h"
#include "miyoo_worker.h"
#include "miyoo_clock.h"
#endif

/* Backends (defined in the sibling src/port/sdl/*.c files). */
extern struct GfxWindowManagerAPI gfx_sdl;
extern struct GfxRenderingAPI     gfx_soft_api;
extern void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title, bool start_in_fullscreen);
extern void gfx_run(Gfx *commands);
extern void gfx_start_frame(void);
extern void gfx_end_frame(void);

/* Game entry (the decomp's src/main.c, split for the port). */
extern void port_game_init(void);
extern void port_game_loop_one_iteration(void);
extern void port_audio_out_init(void);

static volatile int sRunning SAVESTATE_EXCLUDE = 1; /* process lifetime, not game state */
void port_request_exit(void) { sRunning = 0; }

/* ----------------------------------------------------------------- time --- */
/* Monotonic microseconds. musl 1.2's clock_gettime tries the time64 syscall
 * first; the handheld's 4.14 kernel lacks it, so every read cost two kernel
 * entries. SYS_clock_gettime32 goes straight to the one that exists (MK64
 * perf round 24, via ocarina-of-time d267ca3). Every port clock reads this. */
/* SF64_VCLOCK=1 (tests): the GAME's time advances 1/30 s per iteration, not
 * with the wall clock, so its timers, RNG seed and every frame are the same
 * on every run (tools/handheld/sweep.sh: bit-exact renderer sweeps). The PERF
 * profile keeps the real clock. */
static int sVclock SAVESTATE_EXCLUDE = -1;
static u64 sVnow SAVESTATE_EXCLUDE = 1000000;
static inline int vclock_on(void) {
    if (sVclock < 0) { const char *e = getenv("SF64_VCLOCK"); sVclock = e && e[0] == '1'; }
    return sVclock;
}
/* The game's clock (libultra osGetTime/timers): virtual under SF64_VCLOCK. */
u64 port_time_us64(void);
u64 port_game_time_us64(void) { return vclock_on() ? sVnow : port_time_us64(); }
/* The port's own clock (profiling, pacing): always real. */
/* SF64_VDSOCLOCK (default 1; 0 = the syscall always, as before; or vdsoclock=0 in sf64_display.cfg): the kernel's own __vdso_clock_gettime, the
 * same CLOCK_MONOTONIC read in user space, instead of a syscall. musl 1.2.1 never looks the vDSO up.
 * GoldenEye's device Bench (goldeneye-007 X37, hh_os.c): 157-167 ns a read against 598-611 ns for the syscall.
 * Taken from it (as mario-kart-64's MK64_VDSOCLOCK): the vDSO is found through AT_SYSINFO_EHDR's symbol table and used
 * only if, best of three trials of 256 reads each, its readings lie between the syscall's around them and cost under
 * half as much; one CLOCK line says which. Only the cost of a read changes, never the clock read. */
/* SF64_VDSOCLOCK, else a line vdsoclock=0|1 in sf64_display.cfg, else on */
static int port_vdsoclock_switch(void) {
    const char *e = getenv("SF64_VDSOCLOCK");
    int v = 1;
    if (e && e[0]) return e[0] != '0';
    {
        const char *dir = getenv("SF64_DATA");
        char path[512], line[64];
        FILE *f;
        snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
        f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f))
                if (strncmp(line, "vdsoclock=", 10) == 0 && (line[10] == '0' || line[10] == '1')) v = line[10] - '0';
            fclose(f);
        }
    }
    return v;
}
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* the address is this process's, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO */

static u64 port_clock_us_sys(void)
{
#if defined(SYS_clock_gettime32)
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0)
        return (u64)t.s * 1000000u + (u64)(t.ns / 1000);
#endif
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
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
        port_log_always("CLOCK syscall (%s)\n", on ? "no vDSO clock_gettime" : "SF64_VDSOCLOCK=0");
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
u32 port_time_us(void) { return (u32)port_time_us64(); }

/* Frame profile (see port.h). The game's PORT_PROFILE hooks report through
 * port_profile_add: slot 0 = game logic, 1 = gfx run, 2 = gfx end, 3 = audio. */
u32 gPortProfFrame[PROF_COUNT] SAVESTATE_EXCLUDE;
u32 gPortProfLast[PROF_COUNT] SAVESTATE_EXCLUDE;
volatile u32 gPortProfFlipUs SAVESTATE_EXCLUDE;
void port_profile_add(int slot, u32 us) {
    static const int map[4] = { PROF_LOGIC, PROF_GFX, PROF_GFX, PROF_AUDIO };
    if (slot < 0 || slot >= 4) return;
    gPortProfFrame[map[slot]] += us;
}

/* Every 150 iterations: averages of every bucket, the worst iteration, how
 * many iterations actually rendered (the rest were frameskip), the 3D
 * internal size the ladder settled on, and audio underruns in the window.
 * interp = gfx minus the backend buckets = display-list interpretation,
 * vertex transform/clip, batching. All wall-clock on the game thread; flip is
 * the present thread's SDL_Flip time (blocking there costs the game nothing). */
/* SF64_LOGVERBOSE=1 (or logverbose=1 in sf64_display.cfg): the full logging of before 2026-10-05, a PERF
 * line every 150 iterations instead of every 750 in normal play. neon=alt (a device A/B) always prints every window. */
static int port_logverbose(void) {
    static int sV SAVESTATE_EXCLUDE = -1;
    if (sV < 0) {
        const char *e = getenv("SF64_LOGVERBOSE");
        sV = e && e[0] == '1';
        if (!(e && e[0])) {
            const char *dir = getenv("SF64_DATA");
            char path[512], line[64];
            snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f)) if (strncmp(line, "logverbose=1", 12) == 0) sV = 1;
                fclose(f);
            }
        }
    }
    return sV;
}
static void prof_iteration_end(u32 iter_us, int rendered) {
    static u32 sIt SAVESTATE_EXCLUDE, sSub SAVESTATE_EXCLUDE;
    static u32 sSum[PROF_COUNT] SAVESTATE_EXCLUDE, sMax SAVESTATE_EXCLUDE, sN SAVESTATE_EXCLUDE, sRendered SAVESTATE_EXCLUDE;
    static u32 sFlipSum SAVESTATE_EXCLUDE, sUnderrunsPrev SAVESTATE_EXCLUDE;
    gPortProfFrame[PROF_ITER] = iter_us;
    for (int i = 0; i < PROF_COUNT; i++) { sSum[i] += gPortProfFrame[i]; gPortProfLast[i] = gPortProfFrame[i]; gPortProfFrame[i] = 0; }
    if (iter_us > sMax) sMax = iter_us;
    sRendered += rendered ? 1 : 0;
    extern int gfx_soft_neon_alt_active(void);
    const u32 stall_us = (gfx_soft_neon_alt_active() || port_logverbose()) ? 80000 : 100000; /* normal play: over 100 ms only (2026-10-06, the slow-frame lines across the ports); neon=alt and SF64_LOGVERBOSE=1 keep 80 */
    if (iter_us > stall_us + gPortProfLast[PROF_SLEEP]) { /* a stall the audio backstop had to cover: say where */
        extern u32 port_audio_out_underruns(void);
        port_log_always("STALL %u ms: logic %u audio %u gfx %u (tex %u r2d %u r3d %u) underruns %u\n",
                 iter_us / 1000, gPortProfLast[PROF_LOGIC] / 1000, gPortProfLast[PROF_AUDIO] / 1000,
                 gPortProfLast[PROF_GFX] / 1000, gPortProfLast[PROF_TEX] / 1000,
                 gPortProfLast[PROF_R2D] / 1000, gPortProfLast[PROF_R3D] / 1000, port_audio_out_underruns());
    }
    { u32 f = gPortProfFlipUs; gPortProfFlipUs = 0; sFlipSum += f; }
    sN++;
    if (++sIt < 150) return;
    sIt = 0;
    {
        extern int gfx_soft_neon_alt_active(void);
        extern int port_bench_on(void);
        if (!gfx_soft_neon_alt_active() && !port_logverbose() && !port_bench_on() && ++sSub < 5) return; /* normal play: one PERF line per 5 windows, the sums keep adding up */
        sSub = 0;
    }
    extern u32 port_audio_out_underruns(void);
    extern void gfx_soft_get_internal(int *w, int *h);
    int iw = 0, ih = 0; gfx_soft_get_internal(&iw, &ih);
    /* The pacing sleep (and the menu) run inside gfx start_frame, so they sit
     * inside the GFX bucket: take them out before deriving the interpreter. */
    const u32 back = sSum[PROF_R3D] + sSum[PROF_R2D] + sSum[PROF_TEX] + sSum[PROF_COMP] + sSum[PROF_CLR] + sSum[PROF_PRES] + sSum[PROF_SLEEP];
    const u32 interp = sSum[PROF_GFX] > back ? sSum[PROF_GFX] - back : 0;
    extern u32 port_audio_thread_mixes(void);
    extern u32 port_audio_out_take_ring_min(void), port_audio_out_gaps(void); /* statefix B: the ring's least, the gaps */
    static u32 sMixPrev SAVESTATE_EXCLUDE;
    const u32 ur = port_audio_out_underruns();
    const u32 mixes = port_audio_thread_mixes();
#define AVG(i) ((float)sSum[i] / (1000.0f * sN))
    extern int gfx_soft_neon_alt_step(void);
    const int neon = gfx_soft_neon_alt_step(); /* the mode this window used (neon=alt flips it after) */
    port_log_always("PERF work %.1f ms avg (iter %.1f, max %.1f, sleep %.1f) rendered %u/%u | logic %.1f audio %.1f gfx %.1f = interp %.1f + r3d %.1f + r2d %.1f + tex %.1f + comp %.1f + clr %.1f + pres %.1f | flip(thread) %.1f | 3d-res %dx%d | underruns +%u backstop-mixes +%u ring %u gaps %u | neon %d\n",
             AVG(PROF_ITER) - AVG(PROF_SLEEP), AVG(PROF_ITER), sMax / 1000.0f, AVG(PROF_SLEEP), sRendered, sN,
             AVG(PROF_LOGIC), AVG(PROF_AUDIO), AVG(PROF_GFX) - AVG(PROF_SLEEP), (float)interp / (1000.0f * sN),
             AVG(PROF_R3D), AVG(PROF_R2D), AVG(PROF_TEX), AVG(PROF_COMP), AVG(PROF_CLR), AVG(PROF_PRES),
             (float)sFlipSum / (1000.0f * sN), iw, ih, ur - sUnderrunsPrev, mixes - sMixPrev,
             port_audio_out_take_ring_min(), port_audio_out_gaps(), neon);
#undef AVG
    { /* the deferred backdrops (gfx_soft.c defer_batch): how often the gate let a frame defer */
        extern uint32_t gfx_soft_defer_frames, gfx_soft_defer_partials, gfx_soft_defer_capped, gfx_soft_defer_early;
        extern uint32_t gfx_soft_replay_us, gfx_soft_replay_px; /* every replay's time and span pixels (2026-10-11) */
        static uint32_t sDfF SAVESTATE_EXCLUDE, sDfP SAVESTATE_EXCLUDE, sDfC SAVESTATE_EXCLUDE, sDfE SAVESTATE_EXCLUDE;
        static uint32_t sRpU SAVESTATE_EXCLUDE, sRpP SAVESTATE_EXCLUDE;
        const u32 nr = sRendered ? sRendered : 1;
        port_log_always("NOZDEFER %u/%u frames deferred | partial replays %u | capped %u | early %u | replay %.2f ms %.1fk px a picture\n",
                        gfx_soft_defer_frames - sDfF, sN, gfx_soft_defer_partials - sDfP, gfx_soft_defer_capped - sDfC,
                        gfx_soft_defer_early - sDfE, (float)(gfx_soft_replay_us - sRpU) / (1000.0f * nr),
                        (float)(gfx_soft_replay_px - sRpP) / (1000.0f * nr));
        sDfF = gfx_soft_defer_frames; sDfP = gfx_soft_defer_partials; sDfC = gfx_soft_defer_capped; sDfE = gfx_soft_defer_early;
        sRpU = gfx_soft_replay_us; sRpP = gfx_soft_replay_px;
    }
#if defined(MIYOO)
    { /* the second core (gfx_soft.c raster_tri): triangles shared and their pixels a picture, the handoffs and the
       * waits that had to sleep; the CPU clock (miyoo_clock.c) */
        extern uint32_t gfx_soft_split_tris, gfx_soft_split_px;
        static uint32_t sSpT SAVESTATE_EXCLUDE, sSpP SAVESTATE_EXCLUDE;
        unsigned jobs = 0, sleeps = 0; unsigned long long spins = 0;
        miyoo_worker_stats(&jobs, &sleeps, &spins);
        const u32 nr = sRendered ? sRendered : 1;
        port_log_always("MIYOO worker %s | split %.1f tris %.1fk px a picture | handoffs %u, slept %u | clock %d MHz\n",
                        miyoo_worker_ready() ? "on" : "off", (float)(gfx_soft_split_tris - sSpT) / nr,
                        (float)(gfx_soft_split_px - sSpP) / (1000.0f * nr), jobs, sleeps, miyoo_clock_mhz());
        sSpT = gfx_soft_split_tris; sSpP = gfx_soft_split_px;
    }
#endif
    /* Raster shape (device round 10: compute vs memory bound). The span-pixel
     * count needs no PMU and already separates the two cases: a big overdraw
     * factor with low ns/px = overdraw-bound (cut pixels); a small overdraw
     * factor with high ns/px = per-pixel/memory-bound (cut per-pixel cost).
     * The PMU line, when available, confirms it (IPC + L1D-misses/px). */
    {
        extern int pmu_ok(void);
        extern uint64_t gfx_soft_span_px, gfx_soft_tris;
        static uint64_t sCyc SAVESTATE_EXCLUDE, sIns SAVESTATE_EXCLUDE, sMiss SAVESTATE_EXCLUDE, sPx SAVESTATE_EXCLUDE;
        extern uint64_t gPmuCyc, gPmuIns, gPmuMiss; /* accumulated in run_one_iteration */
        const uint64_t dc = gPmuCyc - sCyc, di = gPmuIns - sIns, dm = gPmuMiss - sMiss;
        const uint64_t dpx = gfx_soft_span_px - sPx;
        static uint64_t sTri SAVESTATE_EXCLUDE; const uint64_t dtri = gfx_soft_tris - sTri; sTri = gfx_soft_tris;
        sCyc = gPmuCyc; sIns = gPmuIns; sMiss = gPmuMiss; sPx = gfx_soft_span_px;
        if (dpx > 0 && sRendered > 0) {
            const double px_per_frame = (double)dpx / sRendered;
            const double r3d_us = (double)sSum[PROF_R3D] / sRendered;
            {   /* texture arena and target switches (diddy-kong-racing a0ee8b5's questions) */
                extern size_t gfx_soft_arena16, gfx_soft_arena_peak, gfx_soft_arena_size(void), gfx_soft_arena_used(void);
                extern uint32_t gfx_soft_phase_sw, gfx_soft_phase_sw_max;
                static uint32_t sSwPrev SAVESTATE_EXCLUDE;
                const size_t used = gfx_soft_arena_used(), sz = gfx_soft_arena_size();
                port_log_always("TEXMEM arena %u%% (peak %u%%, 16-bit %u%% of used) of %u MB | phase switches/frame avg %.1f max %u\n",
                         (unsigned) (used * 100 / sz), (unsigned) (gfx_soft_arena_peak * 100 / sz),
                         used ? (unsigned) (gfx_soft_arena16 * 100 / used) : 0u, (unsigned) (sz >> 20),
                         (double) (gfx_soft_phase_sw - sSwPrev) / sRendered, (unsigned) gfx_soft_phase_sw_max);
                sSwPrev = gfx_soft_phase_sw; gfx_soft_arena_peak = used; gfx_soft_phase_sw_max = 0;
            }
            port_log_always("GFX3D %.0fk span-px/frame (overdraw %.1fx) | %.0f tris (%.0f px/tri) | %.1f ns/px | r3d %.1f ms\n",
                     px_per_frame / 1000.0, px_per_frame / ((iw > 0 && ih > 0) ? (double)iw * ih : 240.0 * 240.0),
                     (double)dtri / sRendered, dtri ? (double)dpx / dtri : 0.0,
                     dpx ? r3d_us * 1000.0 / px_per_frame : 0.0, r3d_us / 1000.0);
        }
        if (pmu_ok() && dpx > 0) {
            port_log_always("PMU   IPC %.2f | %.0f cyc/px | %.2f L1Dmiss/px (cyc %.0fM insn %.0fM miss %.0fM)\n",
                     di ? (double)di / (double)dc : 0.0, (double)dc / (double)dpx, (double)dm / (double)dpx,
                     (double)dc / 1e6, (double)di / 1e6, (double)dm / 1e6);
        }
    }
    { extern int port_bench_on(void); extern const char *port_bench_ended(void);
      if (port_bench_on() && port_bench_ended()[0]) port_log_always("BENCH %s\n", port_bench_ended()); }
    sUnderrunsPrev = ur; sMixPrev = mixes;
    memset(sSum, 0, sizeof(sSum)); sMax = 0; sN = 0; sRendered = 0; sFlipSum = 0;
}

/* PMU accumulators, summed per rendered iteration in run_one_iteration. */
uint64_t gPmuCyc SAVESTATE_EXCLUDE, gPmuIns SAVESTATE_EXCLUDE, gPmuMiss SAVESTATE_EXCLUDE;

/* port_log() / port_log_always() are provided by src/port/segments.c. */
extern void port_log_always(const char *fmt, ...);

/* ------------------------------------------------------------ save paths --- */
/* Everything the port writes (save, config, log, debug dumps) goes under a
 * writable data dir: $SF64_DATA if set, else the executable's dir + /data
 * (desktop and the Miyoo Mini). */
static char sEbootDir[256] SAVESTATE_EXCLUDE = "./"; /* this process's, set before any load */
static char sSaveDir[300] SAVESTATE_EXCLUDE  = "./data/";

const char *port_eboot_dir(void) { return sEbootDir; }
const char *port_save_dir(void)  { return sSaveDir; }

const char *port_save_path(const char *name) {
    static char buf[4][512];
    static int i;
    char *b = buf[i++ & 3];
    snprintf(b, sizeof(buf[0]), "%s%s", sSaveDir, name);
    return b;
}

void port_fs_mkdir(const char *path) { mkdir(path, 0777); }

void port_set_save_dir(const char *argv0) {
    const char *env = getenv("SF64_DATA");
#if defined(MIYOO)
    if (!env || !env[0]) env = miyoo_exe_dir(); /* launch.sh sets it to the game's folder; a bare start too */
#endif
    if (env && env[0]) {
        snprintf(sEbootDir, sizeof(sEbootDir), "%s", env);
        size_t n = strlen(sEbootDir);
        if (n && sEbootDir[n - 1] != '/' && n + 1 < sizeof(sEbootDir)) { sEbootDir[n] = '/'; sEbootDir[n + 1] = 0; }
    } else {
        const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
        if (slash && (size_t)(slash + 1 - argv0) < sizeof(sEbootDir)) {
            memcpy(sEbootDir, argv0, slash + 1 - argv0);
            sEbootDir[slash + 1 - argv0] = 0;
        } else {
            snprintf(sEbootDir, sizeof(sEbootDir), "./");
        }
    }
    char dir[300];
    snprintf(dir, sizeof(dir), "%sdata", sEbootDir);
    mkdir(dir, 0777);
    snprintf(sSaveDir, sizeof(sSaveDir), "%s/", dir);
}

void port_fs_init(void) { /* data/ created in port_set_save_dir(); nothing else. */ }

/* ---------------------------------------------------------- debug stubs --- */
void port_screenshot(int index)          { (void)index; }
void port_depthshot(int index)           { (void)index; }
void port_debug_selftest(void)           { }
void port_debug_frame_begin(u32 frame)   { (void)frame; }
void port_debug_frame_end(u32 frame)     { (void)frame; }
void port_gfx_overlay(void)              { } /* FPS overlay: off by default */

/* Referenced by gfx_pc.c's gfx_sp_reset (was defined in the PSP main). */
float identity_matrix[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};

/* --------------------------------------------------------- frame driving --- */
void port_gfx_run(Gfx *dl)        { gfx_run(dl); }
void port_gfx_start_frame(void)   { gfx_start_frame(); }
void port_gfx_end_frame(void)     { gfx_end_frame(); }

static u32 sFrame SAVESTATE_EXCLUDE; /* port-side frame counter: restoring it re-fired the SF64_LOAD_AT hook in a loop */
u32 port_frame_count(void) { return sFrame; } /* gfx_sdl12.c's crash line */
volatile int gPortLoopStage SAVESTATE_EXCLUDE; /* port.h PORT_LOOP_STAGE */
#if defined(HANDHELD)
/* A diagnostic line: where the loop stopped -- the stage (PORT_LOOP_STAGE in sf64_main.c and below),
 * the game state, the game's frame and the pictures drawn -- and how many iterations passed since the
 * last call (the return value: 0 = the main thread is stuck in one). */
unsigned handheld_loop_detail(char *buf, size_t n) {
    static const char *const kStage[PORT_STAGE_COUNT] = { "boot", "timers", "input", "event poll (menu)", "game update",
                                                          "drawing", "audio", "save state check", "iteration end" };
    extern s32 gGameState;
    extern u32 gSysFrameCount, gfx_soft_frames_rendered;
    const int st = gPortLoopStage;
    if (n > 0) {
        snprintf(buf, n, "stage %s, game state %d, game frame %u, pictures %u", kStage[st >= 0 && st < PORT_STAGE_COUNT ? st : 0],
                 (int) gGameState, (unsigned) gSysFrameCount, (unsigned) gfx_soft_frames_rendered);
    }
    return sFrame;
}
#endif
/* The Bench (2026-10-11, the speed audit's bomb items; an empty file `bench` next to the game, or SF64_BENCH=1):
 * one stretch of Corneria replayed from a save state for every timed window, so each window draws the same frames
 * (the 2026-10-07 Bench timed a level moving on, window by window, and read as noise). The autopilot (SF64_AUTO=2,
 * shields kept full) on the virtual game clock with every frame drawn (SF64_VCLOCK=1, SF64_FAST=1: the frames are
 * the qemu and desktop sweeps' frames, unpaced), so a window's work is its frames' whole cost. At iteration
 * SF64_BENCH_SEG (default 2849, in Corneria) slot 1 is saved; the window after it is "settle" (no load before it:
 * not read); then every 150 iterations (one PERF window) the state is loaded again and the next window runs:
 *   steady (no bombs) x BENCH_STEADY cycles, then bomb (a smart bomb every 240 polls, so the blast and the water's
 *   splash sprays: SF64_AUTOBOMB's cadence from the same poll count every time) x BENCH_BOMB cycles;
 *   each cycle all-on, earlyflush-off, cellskip-off, both-off, the order turned by one each cycle.
 * After every PERF window (all printed) a line "BENCH <phase> <window> (n of N)" names the one just timed; then exit.
 * No resume prompt, no auto save; its files stay in handheld/.sf64-test. */
#define BENCH_PERIOD 150
#define BENCH_STEADY 4
#define BENCH_BOMB 6
#define BENCH_BOMB_EVERY 70 /* polls: bombs 10 and 80 polls into each window */
#define BENCH_BOMB_DET 16 /* B again 16 polls after each bomb: detonated close, so the blast wraps the camera */
static int sBench SAVESTATE_EXCLUDE = -1;
static long sBenchSeg SAVESTATE_EXCLUDE = 2849;
static unsigned sBenchPolls SAVESTATE_EXCLUDE;
static int sBenchBombEvery SAVESTATE_EXCLUDE = BENCH_BOMB_EVERY, sBenchBombDet SAVESTATE_EXCLUDE = BENCH_BOMB_DET; /* SF64_BENCH_BOMB=every[:det] */
static char sBenchEnded[64] SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* The Miyoo's Bench (an empty file `bench` in the game's folder, miyoo_console_setup): the same windows, every switch on,
 * the second core on and off in turn -- one core against two on the same frames. */
static const char *const kBenchCfg[4] = { "two-cores", "one-core", "two-cores", "one-core" };
#else
static const char *const kBenchCfg[4] = { "all-on", "earlyflush-off", "cellskip-off", "both-off" };
#endif
int port_bench_on(void) {
    if (sBench < 0) {
        const char *e = getenv("SF64_BENCH"); sBench = e && e[0] == '1';
        const char *g = getenv("SF64_BENCH_SEG");
        const char *bb = getenv("SF64_BENCH_BOMB");
        if (bb && atoi(bb) > 0) { sBenchBombEvery = atoi(bb); const char *c = strchr(bb, ':'); sBenchBombDet = c ? atoi(c + 1) : 0; }
        if (g && atol(g) > 0) sBenchSeg = atol(g) - (atol(g) + 1) % BENCH_PERIOD; /* (n + 1) % 150 == 0: PERF windows line up */
        if (sBench) port_log_always("bench: Corneria from a state saved at iteration %ld, every %d-iteration window from it: "
                                    "settle, steady x %d, bomb x %d (a bomb every %d polls from 10 in, B again %d after; all-on, earlyflush-off, "
                                    "cellskip-off, both-off)\n", sBenchSeg, BENCH_PERIOD, BENCH_STEADY, BENCH_BOMB, sBenchBombEvery, sBenchBombDet);
    }
    return sBench;
}
const char *port_bench_ended(void) { return sBenchEnded; }
static void bench_hook(long frame) {
    extern unsigned port_auto_polls(void);
    extern void port_auto_polls_set(unsigned n), port_auto_bomb_set(int every, int det, unsigned phase), gfx_soft_bench_set(int early, int cell);
    if (frame == sBenchSeg) {
        sBenchPolls = port_auto_polls();
        savestate_request_save(1);
        gfx_soft_bench_set(1, 1);
        port_log_always("bench: segment saved at iteration %ld (poll %u)\n", frame, sBenchPolls);
        return;
    }
    if (frame <= sBenchSeg || (frame - sBenchSeg) % BENCH_PERIOD) return;
    const int k = (int) ((frame - sBenchSeg) / BENCH_PERIOD); /* window k just ended: 1 settle, 2.. the timed ones */
    const int n = 4 * (BENCH_STEADY + BENCH_BOMB);
    if (k == 1) snprintf(sBenchEnded, sizeof(sBenchEnded), "settle all-on (not read)");
    else {
        const int w = k - 2, c = w / 4, cfg = (w % 4 + c) % 4;
        snprintf(sBenchEnded, sizeof(sBenchEnded), "%s %s (%d of %d)", w < 4 * BENCH_STEADY ? "steady" : "bomb", kBenchCfg[cfg], w + 1, n);
    }
    if (k - 1 >= n) { port_request_exit(); return; }
    const int w = k - 1, c = w / 4, cfg = (w % 4 + c) % 4; /* the next window */
#if defined(MIYOO)
    { extern void gfx_soft_split_set(int on); gfx_soft_bench_set(1, 1); gfx_soft_split_set(cfg % 2 == 0); }
#else
    gfx_soft_bench_set(cfg == 0 || cfg == 2, cfg == 0 || cfg == 1);
#endif
    port_auto_bomb_set(w < 4 * BENCH_STEADY ? 0 : sBenchBombEvery, sBenchBombDet, sBenchPolls + 10);
    port_auto_polls_set(sBenchPolls);
    savestate_request_load(1);
}
/* Headless save/load verification: SF64_SAVE_AT / SF64_LOAD_AT frame numbers
 * queue a slot-1 save/load, so a scripted run can save mid-race, keep playing,
 * load, and confirm the game rewound to the saved state. */
static void savestate_test_hook(long frame) {
    static long save_at SAVESTATE_EXCLUDE = -2, load_at SAVESTATE_EXCLUDE = -2;
    if (save_at == -2) {
        const char *s = getenv("SF64_SAVE_AT"); save_at = s ? atol(s) : -1;
        const char *l = getenv("SF64_LOAD_AT"); load_at = l ? atol(l) : -1;
    }
    if (port_bench_on()) bench_hook(frame);
    if (frame == save_at) savestate_request_save(1);
    if (frame == load_at) savestate_request_load(1);
    { /* SF64_SAVEWRITE_AT=<frame> (statefix tests): the game's own save write at a level start from the map
       * (fox_save.c Save_PortTestWrite: Katina's played flag, then Save_Write), so a headless run shows what
       * the game's next write after a LOAD puts on the card. */
        static long sw_at SAVESTATE_EXCLUDE = -2;
        if (sw_at == -2) { const char *e = getenv("SF64_SAVEWRITE_AT"); sw_at = e ? atol(e) : -1; }
        if (frame == sw_at) {
            extern void Save_PortTestWrite(void);
            Save_PortTestWrite();
            port_log_always("savewrite: the game's save write (Katina played) at frame %ld\n", frame);
        }
    }
    { /* SF64_SOUNDPICK_AT=<frame> (statefix tests): the sound screen's music slider moved to 10, not yet written
       * (fox_save.c Save_PortTestSoundPick), so a state can hold an unsaved sound pick. */
        static long sp_at SAVESTATE_EXCLUDE = -2;
        if (sp_at == -2) { const char *e = getenv("SF64_SOUNDPICK_AT"); sp_at = e ? atol(e) : -1; }
        if (frame == sp_at) {
            extern void Save_PortTestSoundPick(void);
            Save_PortTestSoundPick();
            port_log_always("soundpick: music volume 10 picked, not written, at frame %ld\n", frame);
        }
    }
#if defined(HANDHELD)
    { /* SF64_MENU_AT=<frame> (test hook, statefix B): MENU pressed twice at that frame (gfx_sdl12.c), so the
       * menu opens and closes at once: a headless run goes through its mute and unmute. */
        static long menu_at SAVESTATE_EXCLUDE = -2;
        if (menu_at == -2) { const char *e = getenv("SF64_MENU_AT"); menu_at = e ? atol(e) : -1; }
        if (frame == menu_at) { extern void port_test_menu_press(void); port_test_menu_press(); }
    }
#endif
    { /* SF64_EXITSAVE_AT=<frame>: EXIT GAME from the menu (quicksave, then quit). */
        static long exitsave_at SAVESTATE_EXCLUDE = -2;
        if (exitsave_at == -2) { const char *e = getenv("SF64_EXITSAVE_AT"); exitsave_at = e ? atol(e) : -1; }
        if (frame == exitsave_at) savestate_request_exit_save();
    }
    { /* SF64_EXIT_AT=<frame>: clean exit (gprof needs exit(), not a kill). */
        static long exit_at SAVESTATE_EXCLUDE = -2;
        if (exit_at == -2) { const char *f = getenv("SF64_EXIT_AT"); exit_at = f ? atol(f) : -1; }
        if (frame == exit_at) exit(0);
    }
}

static void run_one_iteration(void) {
    if (vclock_on()) sVnow += 33333;
    extern u32 gfx_soft_frames_rendered;
    extern void pmu_read(uint64_t out[3]);
    extern int pmu_ok(void);
    uint64_t p0[3] = {0,0,0};
    const u32 t0 = port_time_us();
    const u32 rendered0 = gfx_soft_frames_rendered;
    if (pmu_ok()) pmu_read(p0);
    port_debug_frame_begin(sFrame);
    port_game_loop_one_iteration();
    PORT_LOOP_STAGE(PORT_STAGE_CHECK);
    savestate_test_hook(sFrame);
    { const u32 ts = port_time_us();
      savestate_check(); /* apply any queued save/load at the iteration boundary */
      PORT_PROF_ADD(PROF_SLEEP, port_time_us() - ts); }
    PORT_LOOP_STAGE(PORT_STAGE_END);
    port_debug_frame_end(sFrame);
    const int did_render = gfx_soft_frames_rendered != rendered0;
    if (pmu_ok() && did_render) { /* only rendered frames feed the raster PMU stats */
        uint64_t p1[3]; pmu_read(p1);
        gPmuCyc += p1[0] - p0[0]; gPmuIns += p1[1] - p0[1]; gPmuMiss += p1[2] - p0[2];
    }
    prof_iteration_end(port_time_us() - t0, did_render);
    sFrame++;
}

#if defined(MIYOO)
/* The Miyoo Mini: every OS's entry runs App/StarFox64/launch.sh, which sets SF64_DATA to the game's folder
 * (miyoo/game.json "data": "."); the save dir is its data/ (port_set_save_dir), where the console log goes too,
 * the last two kept as console.1.log and console.2.log. SF64_LOG=- (tests): leave stdout/stderr alone. */
static void miyoo_console_setup(void) {
    const char *keep = getenv("SF64_LOG");
    { /* The Bench (main.c bench_hook): an empty file `bench` next to the game
       * runs it instead of the game, in its own folder (bench/: its state and log, never the player's data/), and it
       * quits by itself after its 41 windows. Delete the file to play again. */
        char b[480];
        snprintf(b, sizeof(b), "%s/bench", miyoo_exe_dir());
        struct stat st;
        if (stat(b, &st) == 0 && S_ISREG(st.st_mode)) {
            setenv("SF64_BENCH", "1", 0); setenv("SF64_AUTO", "2", 0); setenv("SF64_GOD", "1", 0);
            setenv("SF64_GODKILL", "300", 0); setenv("SF64_VCLOCK", "1", 0); setenv("SF64_FAST", "1", 0);
            snprintf(b, sizeof(b), "%s/benchrun", miyoo_exe_dir());
            mkdir(b, 0777);
            setenv("SF64_DATA", b, 1);
        }
    }
    if (keep && keep[0] == '-') return;
    if (!getenv("SF64_DATA")) setenv("SF64_DATA", miyoo_exe_dir(), 1);
    char dir[400], p1[460], p2[460];
    snprintf(dir, sizeof(dir), "%s/data", getenv("SF64_DATA"));
    mkdir(dir, 0777);
    snprintf(p1, sizeof(p1), "%s/console.1.log", dir); snprintf(p2, sizeof(p2), "%s/console.2.log", dir);
    rename(p1, p2);
    snprintf(p2, sizeof(p2), "%s/console.log", dir);
    rename(p2, p1);
    const int fd = open(p2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}
/* The CPU clock (kit miyoo_clock): SF64_CPUCLOCK=<MHz> wins, then cpuclock=<MHz> in sf64_display.cfg, else the
 * model's default (1500 MHz Mini, 1800 Mini Plus); 0 leaves it alone. Put back at exit and on SIGTERM's path. */
static void miyoo_clock_setup(void) {
    int mhz = -1;
    const char *dir = getenv("SF64_DATA");
    char path[512], line[64];
    snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (strncmp(line, "cpuclock=", 9) == 0 && line[9] >= '0' && line[9] <= '9') mhz = atoi(line + 9);
        fclose(f);
    }
    miyoo_clock_boost_to(mhz);
}
#endif

int main(int argc, char **argv) {
#if defined(MIYOO)
    miyoo_console_setup();
#endif
    { /* nice -10 (kit k1.6): an OS battery check that forks every ~2 s stole a frame each time.
       * Straight after the console redirect so the "priority:" line reaches console.log; every thread
       * (audio, present) and later child inherits it. SF64_PRIO=0: as before. */
        extern void hh_game_priority(void);
        hh_game_priority();
    }
    { /* SF64_HEAPSHIFT=<bytes>: debug -- start the heap somewhere else, as the
       * device's address randomisation does every boot, so the desktop can
       * catch a heap pointer leaking into a save state. */
        const char *hs = getenv("SF64_HEAPSHIFT");
        if (hs && atol(hs) > 0) { volatile char *p = malloc((size_t) atol(hs)); if (p) p[0] = 1; }
    }
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: progress is visible up to any crash */
    setvbuf(stderr, NULL, _IONBF, 0);
    { /* the build in every console log (BUILDLOG notes each build's stamp), so a device log names its build */
        extern const uint32_t port_build_stamp;
        port_log_always("sf64 build %08X\n", (unsigned) port_build_stamp);
    }
    port_set_save_dir(argc > 0 ? argv[0] : NULL);
#if defined(MIYOO)
    miyoo_clock_setup();
    /* Video before anything else SDL: the Mini's SDL does its MI system init (MI_SYS_Init) in the video init, and its
     * dsp audio driver crashes in SDL_AudioInit without it (MK64 and DKR, 2026-10-09: signal 11 at iteration 0 on
     * every start with the game data already built). The first-start screens and the game draw on this panel. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) port_log_always("miyoo: SDL video init failed: %s\n", SDL_GetError());
    SDL_ShowCursor(SDL_DISABLE);
    miyoo_panel_open();
#endif
    port_clock_pick(); /* SF64_VDSOCLOCK: before any other thread reads the clock */
    savestate_parse_args(argc, argv); /* --loadStateFile <path> (tests) */
    { /* SF64: the port ROM image (built from the player's ROM) and the overlays' initial data. */
        extern int port_rom_open(void);
        extern void port_overlay_init(void);
        extern int port_romimage_ensure(const char* argv0);
        port_romimage_ensure(argc > 0 ? argv[0] : NULL); /* first start: build sf64.rom from the player's ROM */
        if (!port_rom_open()) { port_log_always("no port ROM image: nothing to run\n"); return 1; }
        { extern void port_radio_fill(const char* argv0); port_radio_fill(argc > 0 ? argv[0] : NULL); }
        { extern void port_embedded_fill(const char* argv0); port_embedded_fill(argc > 0 ? argv[0] : NULL); }
        port_overlay_init();
    }
    port_fs_init();
    port_audio_out_init();
    PORT_LOG("boot\n");

    gfx_init(&gfx_sdl, &gfx_soft_api, "Star Fox 64", false);
    savestate_boot_prompt(); /* resume an auto save, or start new (handheld) */
    port_debug_selftest();

    port_game_init();
    PORT_LOG("game init done\n");
    { extern void port_audio_backstop_enable(int on); port_audio_backstop_enable(1); }
    { extern void pmu_init(void); pmu_init(); } /* Cortex-A7 counters (compute vs memory bound) */

    while (sRunning) {
        run_one_iteration();
    }
    savestate_shutdown();
    port_log_always("SESSION END (clean exit)\n");
    fflush(NULL);
    sync();
#if defined(HANDHELD)
    { /* SDL down before the launcher comes back (X50, gfx_sdl12.c); the line after it says
       * the shutdown finished rather than hung. */
        extern int gfx_wm_shutdown(void);
        port_log_always(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
    }
#endif
    return 0;
}
