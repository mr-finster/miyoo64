/**
 * Mario Kart 64 -> Miyoo Mini / desktop port: process entry point and the port_*
 * host hooks that don't belong to a specific subsystem (time, logging, save
 * paths, frame driving). The sceGu/PSP original is src/port/psp/psp_main.c.
 *
 * The game's data is not in the executable: port_assets_load() (assets_handheld.c)
 * fills the NOLOAD asset region from the assets.bin cache or from the player's
 * ROM before anything touches it (audio init reads the sound tables).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include <ultra64.h>
#include "port.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "savestate.h"
#if defined(MIYOO)
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
/* Device A/B for the speed switches (2026-10-04, the final speed sweep). MK64 has no Bench mode, so a
 * switch set to "ab" (MK64_<NAME>=ab, or <name>=ab in mk64_display.cfg) alternates off/on every PERF
 * window (150 iterations): the phase flips when a PERF line is written, and an "AB ..." line under it
 * names what the window that just ended ran. 0 = off, 1 = on, 2 = ab. */
int gPortAbPhase SAVESTATE_EXCLUDE = 1;
int gPortAbWindow SAVESTATE_EXCLUDE; /* the AB windows so far (gfx_soft.c MK64_SPEEDBENCH's cycle) */
int port_switch_mode(const char *env, int cfg, int dflt) {
    const char *e = getenv(env);
    if (e && e[0]) {
        if (e[0] == 'a') return 2;
        if (e[0] == '0' || e[0] == '1') return e[0] - '0';
    }
    return cfg >= 0 ? cfg : dflt;
}

/* MK64_VDSOCLOCK (default on; 0 = the syscall always, ab = alternate per PERF window; vdsoclock= in
 * mk64_display.cfg): the kernel's own __vdso_clock_gettime, the same CLOCK_MONOTONIC read in user space
 * from the ARM generic timer, instead of a syscall. The handheld's musl 1.2.1 never looks the vDSO up, and
 * this clock is read ~500 times a frame (the backend's profile buckets around every raster and 2D batch,
 * the pacer, audio). GoldenEye's device Bench (the GoldenEye port r34/r37, hh_os.c, X37): 157-167 ns a read
 * against 598-611 ns for the syscall, worth 0.37-0.41 ms a frame there. Taken from it as is: the vDSO is
 * found through AT_SYSINFO_EHDR's symbol table and used only if, best of three trials of 256 reads each,
 * its readings lie between the syscall's around them and cost under half as much; the choice is logged
 * once (CLOCK line). Only the cost of a read changes, never the clock read. */
typedef int (*PortCgt32)(int, void *);
static PortCgt32 sVdsoCgt SAVESTATE_EXCLUDE;    /* the address is this process's, never a saved one */
static int sClockMode SAVESTATE_EXCLUDE = -1;   /* -1 not picked, 0 syscall, 1 vDSO usable */
static int sClockSwitch SAVESTATE_EXCLUDE = 1;  /* MK64_VDSOCLOCK: 0, 1, 2 = ab */
static int sClockForce SAVESTATE_EXCLUDE;       /* MK64_VDSOCLOCK=force (tests): the vDSO whenever it agrees, even if not faster */

static u64 port_clock_us_sys(void) {
#if defined(SYS_clock_gettime32)
    /* musl 1.2's clock_gettime tries the time64 syscall first, and the handheld's
     * 4.14 kernel lacks it, so every read costs two kernel entries. */
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0) {
        return (u64)t.s * 1000000u + (u32)(t.ns / 1000);
    }
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u32)(ts.tv_nsec / 1000);
}

static u64 port_clock_us_vdso(void) {
    struct { long s, ns; } t;
    if (sVdsoCgt(CLOCK_MONOTONIC, &t) == 0) {
        return (u64)t.s * 1000000u + (u32)(t.ns / 1000);
    }
    return port_clock_us_sys();
}

#if defined(__linux__) && __SIZEOF_POINTER__ == 4
#include <elf.h>
#include <sys/auxv.h>
/* A symbol of the vDSO image (32-bit ELF), as musl's __vdsosym; NULL if none. */
static void *port_vdso_sym(const char *want) {
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
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf32_Sym *sym = (const Elf32_Sym *)((const char *)eh + sh[i].sh_offset);
        const char *str = (const char *)eh + sh[sh[i].sh_link].sh_offset;
        const unsigned n = sh[i].sh_size / sizeof(Elf32_Sym);
        for (unsigned k = 0; k < n; k++)
            if (ELF32_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_shndx != SHN_UNDEF && strcmp(str + sym[k].st_name, want) == 0)
                return (void *)(bias + sym[k].st_value);
    }
    return NULL;
}
#else
static void *port_vdso_sym(const char *want) { (void)want; return NULL; }
#endif

/* 256 reads of each, the syscall's around the vDSO's: 0 if the vDSO's readings fall outside them */
static int port_clock_trial(u64 *vdso_ns, u64 *sys_ns) {
    u64 t0, t1, t2, a, b;
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

/* Once, from main() before any other thread exists (port_time_us picks on its first read otherwise). */
void port_clock_pick(void) {
    extern int handheld_cfg_vdsoclock(void);
    u64 v = ~0ull, s = ~0ull, tv, ts;
    int i, same = 1;
    if (sClockMode >= 0) return;
#if defined(HANDHELD)
    sClockSwitch = port_switch_mode("MK64_VDSOCLOCK", handheld_cfg_vdsoclock(), 1);
#else
    sClockSwitch = port_switch_mode("MK64_VDSOCLOCK", -1, 1);
#endif
    { const char *e = getenv("MK64_VDSOCLOCK"); sClockForce = e && e[0] == 'f'; if (sClockForce) sClockSwitch = 1; }
    sVdsoCgt = sClockSwitch ? (PortCgt32)port_vdso_sym("__vdso_clock_gettime") : NULL;
    if (sVdsoCgt == NULL) {
        sClockMode = 0;
        fprintf(stderr, "CLOCK syscall (%s)\n", sClockSwitch ? "no vDSO clock_gettime" : "MK64_VDSOCLOCK=0");
        return;
    }
    /* the best of three (a GoldenEye device run once caught an interrupt inside the vDSO's 256 reads) */
    for (i = 0; i < 3; i++) {
        same &= port_clock_trial(&tv, &ts);
        if (tv < v) v = tv;
        if (ts < s) s = ts;
    }
    sClockMode = same && (v * 2 < s || sClockForce);
    fprintf(stderr, "CLOCK %s: vDSO %u ns/read vs syscall %u ns/read%s\n", sClockMode ? "vdso" : "syscall", (unsigned)v,
            (unsigned)s, sClockMode && sClockSwitch == 2 ? " (vdsoclock=ab: alternating per PERF window)" : "");
    if (!sClockMode) sVdsoCgt = NULL;
}
int port_clock_ab_state(void) { return sClockSwitch == 2 ? (sClockMode > 0 && gPortAbPhase) : -1; }

u32 port_time_us(void) {
    if (sClockMode > 0 && (sClockSwitch == 1 || gPortAbPhase)) return (u32)port_clock_us_vdso();
    if (sClockMode < 0) { port_clock_pick(); return port_time_us(); }
    return (u32)port_clock_us_sys();
}

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
static void prof_iteration_end(u32 iter_us, int rendered) {
    static u32 sIt SAVESTATE_EXCLUDE, sSub SAVESTATE_EXCLUDE;
    static u32 sSum[PROF_COUNT] SAVESTATE_EXCLUDE, sMax SAVESTATE_EXCLUDE, sN SAVESTATE_EXCLUDE, sRendered SAVESTATE_EXCLUDE;
    static u32 sFlipSum SAVESTATE_EXCLUDE, sUnderrunsPrev SAVESTATE_EXCLUDE;
    static int sAbWin SAVESTATE_EXCLUDE; /* the last window was an ab run (STALL keeps 80 ms) */
    gPortProfFrame[PROF_ITER] = iter_us;
    for (int i = 0; i < PROF_COUNT; i++) { sSum[i] += gPortProfFrame[i]; gPortProfLast[i] = gPortProfFrame[i]; gPortProfFrame[i] = 0; }
    if (iter_us > sMax) sMax = iter_us;
    sRendered += rendered ? 1 : 0;
    const u32 stall_us = (sAbWin || port_logverbose()) ? 80000 : 100000; /* normal play: over 100 ms only (2026-10-06, the slow-frame lines across the ports); an ab run and MK64_LOGVERBOSE=1 keep 80 */
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
    { /* Normal play: one PERF line per 5 windows of 150 iterations (the sums keep adding up in between).
       * An "ab" run (a speed switch alternating per window, see port_switch_mode) and MK64_LOGVERBOSE=1
       * print every window, as before. */
        extern int gfx_speed_ab_line(char *buf, int n), gfx_soft_ab_line(char *buf, int n);
        char ab[128];
        int n = gfx_speed_ab_line(ab, 48);
        if (n >= 0 && n < 48) n += gfx_soft_ab_line(ab + n, 64 - n);
        if (port_clock_ab_state() >= 0 && n >= 0 && n < 64) n++;
        sAbWin = n > 0;
        if (n <= 0 && !port_logverbose() && ++sSub < 5) return;
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
    static u32 sMixPrev SAVESTATE_EXCLUDE;
    const u32 ur = port_audio_out_underruns();
    const u32 mixes = port_audio_thread_mixes();
#define AVG(i) ((float)sSum[i] / (1000.0f * sN))
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
    port_log_always("PERF work %.1f ms avg (iter %.1f, max %.1f, sleep %.1f) rendered %u/%u | logic %.1f audio %.1f gfx %.1f = interp %.1f + r3d %.1f + r2d %.1f + tex %.1f + comp %.1f + clr %.1f + pres %.1f | flip(thread) %.1f | 3d-res %dx%d | underruns +%u backstop-mixes +%u" PERF_MT "\n",
             AVG(PROF_ITER) - AVG(PROF_SLEEP), AVG(PROF_ITER), sMax / 1000.0f, AVG(PROF_SLEEP), sRendered, sN,
             AVG(PROF_LOGIC), AVG(PROF_AUDIO), AVG(PROF_GFX) - AVG(PROF_SLEEP), (float)interp / (1000.0f * sN),
             AVG(PROF_R3D), AVG(PROF_R2D), AVG(PROF_TEX), AVG(PROF_COMP), AVG(PROF_CLR), AVG(PROF_PRES),
             (float)sFlipSum / (1000.0f * sN), iw, ih, ur - sUnderrunsPrev, mixes - sMixPrev PERF_MT_ARG);
#undef PERF_MT
#undef PERF_MT_ARG
#undef AVG
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
    { /* the speed switches set to ab: what the window that just ended ran, then the other half */
        extern int gfx_speed_ab_line(char *buf, int n), gfx_soft_ab_line(char *buf, int n);
        char ab[128];
        int n = gfx_speed_ab_line(ab, 48);
        if (n >= 0 && n < 48) n += gfx_soft_ab_line(ab + n, 64 - n);
        const int c = port_clock_ab_state();
        if (c >= 0 && n >= 0 && n < 64) n += snprintf(ab + n, sizeof(ab) - n, " vdsoclock=%d", c);
        if (n > 0) { port_log_always("AB%s\n", ab); gPortAbPhase ^= 1; gPortAbWindow++; }
    }
    sUnderrunsPrev = ur; sMixPrev = mixes;
    memset(sSum, 0, sizeof(sSum)); sMax = 0; sN = 0; sRendered = 0; sFlipSum = 0;
}

/* PMU accumulators, summed per rendered iteration in run_one_iteration. */
uint64_t gPmuCyc SAVESTATE_EXCLUDE, gPmuIns SAVESTATE_EXCLUDE, gPmuMiss SAVESTATE_EXCLUDE;

/* port_log() / port_log_always() are provided by src/port/segments.c. */
extern void port_log_always(const char *fmt, ...);

/* ------------------------------------------------------------ save paths --- */
/* Everything the port writes (save, config, log, debug dumps) goes under a
 * writable data dir: $MK64_DATA if set, else the executable's dir + /data
 * (desktop). The Miyoo launch.sh sets $MK64_DATA to the app folder. */
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
    const char *env = getenv("MK64_DATA");
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
void port_debug_frame_end(u32 frame)     { extern void port_state_hash(u32 frame); port_state_hash(frame); } /* MK64_STATEHASH (statehash.c) */
void port_gfx_overlay(void)              { } /* FPS overlay: off by default */

/* Referenced by gfx_pc.c's gfx_sp_reset (was defined in the PSP main). */
float identity_matrix[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};

/* --------------------------------------------------------- frame driving --- */
/* Where the host loop is, for the watchdog's log line: the stage it is in and how many iterations
 * it has run. Stores to a port variable on the
 * main thread only, outside the save states. The game's own work (update_gamestate's course loads and a
 * held audio reset, game_state_handler) is "game logic"; display_and_vsync calls the three below, then
 * port_audio_frame ("audio"). */
enum { STAGE_BOOT, STAGE_GAME, STAGE_EVENTS, STAGE_DRAW, STAGE_AUDIO, STAGE_CHECK, STAGE_END };
static volatile int sLoopStage SAVESTATE_EXCLUDE;
void port_gfx_run(Gfx *dl)        { sLoopStage = STAGE_DRAW; gfx_run(dl); }  /* pacer sleep, display list, present */
void port_gfx_start_frame(void)   { sLoopStage = STAGE_EVENTS; gfx_start_frame(); } /* handle_events: keys, the menu */
void port_gfx_end_frame(void)     { gfx_end_frame(); sLoopStage = STAGE_AUDIO; }

static u32 sFrame SAVESTATE_EXCLUDE; /* port-side frame counter: restoring it re-fired the MK64_LOAD_AT hook in a loop */
u32 port_frame_count(void) { return sFrame; } /* the crash line's iteration (gfx_sdl12.c fatal_handler) */
/* The loop note, read by other threads: the stage, the iteration, the game state and
 * the frames rendered; returns the iterations started (sFrame counts finished ones). */
unsigned handheld_loop_detail(char *buf, size_t n) {
    static const char *const kStage[] = { "boot", "game logic", "frame start (events, menu)", "drawing", "audio",
                                          "save state check", "iteration end" };
    extern s32 gGamestate;
    extern u32 gfx_soft_frames_rendered;
    const int st = sLoopStage;
    const u32 frame = *(volatile u32 *) &sFrame;
    if (n > 0) {
        snprintf(buf, n, "stage %s, iteration %u, gamestate %d, frames rendered %u", kStage[st >= 0 && st <= STAGE_END ? st : 0],
                 (unsigned) frame, (int) gGamestate, (unsigned) *(volatile u32 *) &gfx_soft_frames_rendered);
    }
    return frame + 1;
}
#if defined(MIYOO)
/* the watchdog's game line (miyoo_watchdog.c, from its thread) */
int miyoo_wd_detail_game(char *buf, int n) {
    handheld_loop_detail(buf, (size_t) n);
    return (int) strlen(buf);
}
#endif
/* Headless save/load verification: MK64_SAVE_AT / MK64_LOAD_AT frame numbers
 * queue a slot-1 save/load, so a scripted run can save mid-race, keep playing,
 * load, and confirm the game rewound to the saved state. */
static void savestate_test_hook(long frame) {
    static long save_at SAVESTATE_EXCLUDE = -2, load_at SAVESTATE_EXCLUDE = -2;
    if (save_at == -2) {
        const char *s = getenv("MK64_SAVE_AT"); save_at = s ? atol(s) : -1;
        const char *l = getenv("MK64_LOAD_AT"); load_at = l ? atol(l) : -1;
    }
    if (frame == save_at) savestate_request_save(1);
    if (frame == load_at) savestate_request_load(1);
    { /* MK64_EXITSAVE_AT=<frame>: EXIT GAME from the menu (quicksave, then quit). */
        static long exitsave_at SAVESTATE_EXCLUDE = -2;
        if (exitsave_at == -2) { const char *e = getenv("MK64_EXITSAVE_AT"); exitsave_at = e ? atol(e) : -1; }
        if (frame == exitsave_at) savestate_request_exit_save();
    }
    { /* MK64_SAVEWRITE_AT=<frame> (statefix tests): the game's own EEPROM write of its GP points and sound mode,
       * as the options menu makes it on a sound-mode change (save.c: the main block, then the backup). Each writes
       * the whole 512-byte image, so a test can show what a game write after a LOAD puts on the card. */
        static long sw_at SAVESTATE_EXCLUDE = -2;
        if (sw_at == -2) { const char *e = getenv("MK64_SAVEWRITE_AT"); sw_at = e ? atol(e) : -1; }
        if (frame == sw_at) {
            extern void write_save_data_grand_prix_points_and_sound_mode(void), update_save_data_backup(void);
            write_save_data_grand_prix_points_and_sound_mode();
            update_save_data_backup();
            port_log_always("savewrite: the game's GP points and sound mode written at frame %ld\n", frame);
        }
    }
    { /* MK64_FINISH_AT=<frame>: put P1 on its last lap so the autopilot's
       * straight-line crossing of the start line ends the race -- the only
       * headless way to reach the results / driver's-points screens. */
        static long finish_at SAVESTATE_EXCLUDE = -2;
        extern s32 gLapCountByPlayerId[];
        if (finish_at == -2) { const char *f = getenv("MK64_FINISH_AT"); finish_at = f ? atol(f) : -1; }
        if (frame == finish_at) gLapCountByPlayerId[0] = 2;
    }
    { /* MK64_LAP_AT=<frame>: P1 onto lap 2, and MK64_ITEM_AT=<frame>: P1 through an item box --
       * HUD screenshots of the lap counter and a full item window (MK64_HUD). */
        static long lap_at SAVESTATE_EXCLUDE = -2, item_at SAVESTATE_EXCLUDE = -2;
        extern s32 gLapCountByPlayerId[];
        extern void func_8007ABFC(s32 playerId, bool arg1);
        if (lap_at == -2) { const char *f = getenv("MK64_LAP_AT"); lap_at = f ? atol(f) : -1; const char *g = getenv("MK64_ITEM_AT"); item_at = g ? atol(g) : -1; }
        if (frame == lap_at) gLapCountByPlayerId[0] = 1;
        if (frame == item_at) func_8007ABFC(0, 0);
    }
    { /* MK64_FINISH_ALL=1: the same on every race, so the autopilot wins a
       * whole cup in a few minutes -- the headless way to the podium ceremony. */
        static int finish_all SAVESTATE_EXCLUDE = -1;
        extern s32 gLapCountByPlayerId[], gGamestate;
        if (finish_all < 0) { const char *f = getenv("MK64_FINISH_ALL"); finish_all = f && f[0] == '1'; }
        static s32 last SAVESTATE_EXCLUDE = -1;
        if (finish_all && gGamestate == 4 /* RACING */ && gLapCountByPlayerId[0] < 2) gLapCountByPlayerId[0] = 2;
        if (finish_all && gGamestate != last) { port_log_always("gamestate %d at frame %ld\n", (int) gGamestate, frame); last = gGamestate; }
    }
    { extern void debug_penguin_cam(long frame); debug_penguin_cam(frame); }
    { extern void debug_behind_test(long frame); debug_behind_test(frame); }
    { /* MK64_ASSETDUMP_AT=<f1>,<f2>,...: write the asset region to
       * data/assetdump_<frame>.bin -- debug, finds runtime writes into ROM data. */
        static char spec[160] SAVESTATE_EXCLUDE; static int init SAVESTATE_EXCLUDE;
        if (!init) { const char *e = getenv("MK64_ASSETDUMP_AT"); if (e) snprintf(spec, sizeof(spec), ",%s,", e); init = 1; }
        if (spec[0]) {
            char key[24]; snprintf(key, sizeof(key), ",%ld,", frame);
            if (strstr(spec, key)) {
                extern char __assets_start[], __assets_end[];
                char name[48]; snprintf(name, sizeof(name), "assetdump_%ld.bin", frame);
                FILE *f = fopen(port_save_path(name), "wb");
                if (f) { fwrite(__assets_start, 1, (size_t) (__assets_end - __assets_start), f); fclose(f); }
            }
        }
    }
    { /* MK64_EXIT_AT=<frame>: clean exit (gprof needs exit(), not a kill). */
        static long exit_at SAVESTATE_EXCLUDE = -2;
        if (exit_at == -2) { const char *f = getenv("MK64_EXIT_AT"); exit_at = f ? atol(f) : -1; }
        if (frame == exit_at) exit(0);
    }
}

static void run_one_iteration(void) {
    extern u32 gfx_soft_frames_rendered;
    extern void pmu_read(uint64_t out[3]);
    extern int pmu_ok(void);
    uint64_t p0[3] = {0,0,0};
    const u32 t0 = port_time_us();
    const u32 rendered0 = gfx_soft_frames_rendered;
    if (pmu_ok()) pmu_read(p0);
    port_debug_frame_begin(sFrame);
#if defined(MIYOO)
    {   /* MK64_STALL_TEST=game:<iteration>[:<s>] | worker:<iteration>[:<s>] (tests of the watchdog, miyoo_watchdog.c):
         * at that iteration the game thread spins for <s> seconds (6), or the next split triangle's worker share sleeps */
        static long at SAVESTATE_EXCLUDE = -2; static int kind SAVESTATE_EXCLUDE, secs SAVESTATE_EXCLUDE = 6;
        if (at == -2) {
            const char *e = getenv("MK64_STALL_TEST");
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
            else { const u32 ts = port_time_us(); while (port_time_us() - ts < (u32) secs * 1000000u) { } }
        }
    }
#endif
    sLoopStage = STAGE_GAME;
    port_game_loop_one_iteration();
    sLoopStage = STAGE_CHECK;
    savestate_test_hook(sFrame);
    { const u32 ts = port_time_us();
      savestate_check(); /* apply any queued save/load at the iteration boundary */
      PORT_PROF_ADD(PROF_SLEEP, port_time_us() - ts); }
    sLoopStage = STAGE_END;
    port_debug_frame_end(sFrame);
    const int did_render = gfx_soft_frames_rendered != rendered0;
    if (pmu_ok() && did_render) { /* only rendered frames feed the raster PMU stats */
        uint64_t p1[3]; pmu_read(p1);
        gPmuCyc += p1[0] - p0[0]; gPmuIns += p1[1] - p0[1]; gPmuMiss += p1[2] - p0[2];
    }
    prof_iteration_end(port_time_us() - t0, did_render);
#if defined(MIYOO)
    { extern void miyoo_sleepdiag_iter(unsigned iter_us, const unsigned *last, int rendered); miyoo_sleepdiag_iter(gPortProfLast[PROF_ITER], gPortProfLast, did_render); }
#endif
    sFrame++;
}

#if defined(MIYOO)
/* Miyoo Mini: the app folder (launch.sh sets MK64_DATA to it; else the binary's own folder)
 * holds the settings, the auto save and save states (in data/), assets.bin and the console
 * log, rotated (console.1.log, console.2.log). MK64_LOG=0 leaves stdout/stderr alone (a shell over SSH). */
static void miyoo_console_setup(const char *argv0) {
    char dir[256];
    const char *env = getenv("MK64_DATA");
    if (env && env[0]) snprintf(dir, sizeof(dir), "%s", env);
    else {
        const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
        if (slash && (size_t) (slash - argv0) < sizeof(dir)) { memcpy(dir, argv0, (size_t) (slash - argv0)); dir[slash - argv0] = 0; }
        else snprintf(dir, sizeof(dir), ".");
        setenv("MK64_DATA", dir, 1);
    }
    mkdir(dir, 0777);
    const char *lg = getenv("MK64_LOG");
    if (lg && lg[0] == '0') return;
    char a[300], b[300];
    snprintf(a, sizeof(a), "%s/console.1.log", dir); snprintf(b, sizeof(b), "%s/console.2.log", dir); rename(a, b);
    snprintf(b, sizeof(b), "%s/console.log", dir); rename(b, a);
    const int fd = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}
#endif

/* The game's priority (kit k1.6 hh_priority.c, from DKR b37c2ef; the 2-second beat, 2026-09-27).
 * an OS helper script forks every ~2.05 s, ~15-25 ms of CPU on the single
 * core; at the game's priority they land inside one frame, which runs late: a hitch every 2 s in every
 * port (DKR device: 0.40 -> 0.05 frames/s losing >= 15 ms, worst frame 55.7 -> 43.1 ms, underruns 1 -> 0).
 * A modest nice (not SCHED_RR/FIFO: nothing in the OS can starve) lets the script run in the game's
 * per-frame gaps. Set before any thread or lasting child exists, so the audio, present and worker
 * threads inherit it. MK64_PRIO=<nice> (default -10; 0 = off,
 * the old behaviour). The desktop needs root or CAP_SYS_NICE to go below 0: it logs and keeps what it had. */
#include <sys/resource.h>
#include <errno.h>
static void port_set_priority(void) {
    const char *e = getenv("MK64_PRIO");
    const int want = e ? atoi(e) : -10;
    const int before = getpriority(PRIO_PROCESS, 0);
    if (want != 0 && setpriority(PRIO_PROCESS, 0, want) != 0)
        fprintf(stderr, "priority: nice %d refused (%s), kept %d\n", want, strerror(errno), before);
    fprintf(stderr, "priority: nice %d (asked %d)\n", getpriority(PRIO_PROCESS, 0), want);
    fflush(stderr);
}

int main(int argc, char **argv) {
#if defined(MIYOO)
    miyoo_console_setup(argc > 0 ? argv[0] : NULL);
    { extern const char *miyoo_sleepdiag_version(void); fprintf(stderr, "Mario Kart 64 %s (the sleep/wake diagnostic build: DIAG and WAKE lines; MK64_WATCH=log|fix|0)\n", miyoo_sleepdiag_version()); }
#endif
    port_set_priority();
    port_clock_pick(); /* MK64_VDSOCLOCK: before any other thread reads the clock */
#if defined(MIYOO)
    {   /* The CPU clock (miyoo_clock.c, the shared kit 27a1311): 1500 MHz on the Mini, 1800 on the Mini Plus
         * by default, cpuclock=<MHz> in mk64_display.cfg (0: as the OS set it), MK64_CPUCLOCK=<MHz> wins. Put back
         * at every way out (the exit below, atexit, the crash handler). The clock goes in the log either way. */
        extern int handheld_cfg_cpuclock(void);
        if (!miyoo_clock_boost_to(handheld_cfg_cpuclock())) { /* left alone: what cpufreq says (never /dev/mem off a Mini) */
            FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
            long khz = 0;
            if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
            if (khz > 0) fprintf(stderr, "clock: %ld MHz (cpufreq)\n", khz / 1000);
        }
        { extern void miyoo_sleepdiag_start(void); miyoo_sleepdiag_start(); } /* after the boost: the watch keeps what it set */
    }
#endif
    { /* MK64_HEAPSHIFT=<bytes>: debug -- start the heap somewhere else, as the
       * device's address randomisation does every boot, so the desktop can
       * catch a heap pointer leaking into a save state. */
        const char *hs = getenv("MK64_HEAPSHIFT");
        if (hs && atol(hs) > 0) { volatile char *p = malloc((size_t) atol(hs)); if (p) p[0] = 1; }
    }
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: progress is visible up to any crash */
    setvbuf(stderr, NULL, _IONBF, 0);
    port_set_save_dir(argc > 0 ? argv[0] : NULL);
    savestate_parse_args(argc, argv); /* --loadStateFile <path> */
    port_assets_load(argc > 0 ? argv[0] : NULL);
    if (getenv("MK64_ASSETS_ONLY")) return 0; /* build/verify the asset cache and stop (bitexact_sweep.sh prewarm) */
    port_fs_init();
    port_audio_out_init();
    PORT_LOG("boot\n");

    gfx_init(&gfx_sdl, &gfx_soft_api, "Mario Kart 64", false);
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
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock the OS had, before anything that could end the process */
#endif
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
