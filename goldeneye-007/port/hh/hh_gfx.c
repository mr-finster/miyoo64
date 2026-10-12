/* hh_gfx.c: GoldenEye's glue for the handheld kit renderer (hh/gfx/, copied from
 * the OoT port 0921936: gfx_pc.c display-list interpreter + gfx_soft.c rasterizer +
 * gfx_sdl.c desktop / gfx_sdl12.c handheld). The port.h services the kit expects, the
 * desktop proxy's keyboard, and the RSP-task hook that runs a frame's display list. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ultra64.h>
#include "gfx/gfx_pc.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/port.h"
#include "hh_os.h"
#include "hh_hw.h"
#include "hh_plat.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (port/kit) */

extern struct GfxRenderingAPI gfx_soft_api;
extern struct GfxWindowManagerAPI gfx_sdl; /* gfx_sdl.c (desktop) or gfx_sdl12.c (handheld) */

uint32_t gPortProfFrame[PROF_COUNT];
uint32_t gPortProfLast[PROF_COUNT];
volatile uint32_t gPortProfFlipUs SAVESTATE_EXCLUDE;

/* gfx_pc.c resets the projection and model-view stacks to this. */
float identity_matrix[4][4] = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};

static int sQuiet SAVESTATE_EXCLUDE = -1;

void port_log(const char *fmt, ...)
{
    va_list ap;
    if (sQuiet < 0)
        sQuiet = getenv("GE_QUIET") != NULL;
    if (sQuiet)
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void port_log_always(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

uint64_t port_time_us64(void) { return hh_host_ns() / 1000u; }
uint32_t port_time_us(void) { return (uint32_t)port_time_us64(); }

const char *port_save_dir(void) { return hh_plat_save_dir(); }

const char *port_save_path(const char *name)
{
    static char path[640] SAVESTATE_EXCLUDE; /* host state, as hh_plat_save_dir's folder */
    snprintf(path, sizeof(path), "%s/%s", hh_plat_save_dir(), name);
    return path;
}

int port_fs_mkdir(const char *path) { return mkdir(path, 0755); }

/* A segment address the RSP table does not resolve: segment bases are 0 until the game
 * sets them, so the RSP reads it as a physical address. */
void *port_seg_to_ptr(uintptr_t addr)
{
    if (addr >= 0xA0000000u && addr < 0xC0000000u) /* KSEG1, uncached: same memory */
        return (void *)(uintptr_t)(addr - 0x20000000u);
    if (addr >= PORT_REAL_PTR_MIN)
        return (void *)addr;
    return (void *)(uintptr_t)(0x80000000u + (addr & 0x00FFFFFFu));
}

void port_gfx_overlay(void) {}

/* The keyboard and handheld keys: hh_input.c. */

/* The way out: EXIT GAME (after the kit's exit save), the menu's SDL_QUIT, a failed power-off.
 * SDL comes down first on every exit, not just the bench child's (hh_plat.c GE_EXIT_AT):
 * SDL 1.2's fbcon otherwise leaves the console, keyboard and video modes set up, and the next
 * SDL program (the launcher after EXIT GAME) can hang at its first flip, as the bench's second
 * run did (X50, the OoT port sweep-2026-10-01: OoT froze on EXIT GAME on the device).
 * From inside the menu this thread already holds the present lock: SDL 1.2 mutexes are
 * recursive. The exit line goes to console.log (stdout, hh_card.c) so the log shows we got out.
 * Before SDL: the kit's savestate_shutdown, which every other port runs after its game loop and
 * GoldenEye (which exit()s from here) never did: it marks the game loop as ended. Once only:
 * power_off_now comes back here. */
void port_request_exit(void)
{
    extern int gfx_wm_shutdown(void) __attribute__((weak)); /* gfx_sdl12.c (the device) */
    static int sExiting SAVESTATE_EXCLUDE;
    port_log_always("hh: exit after %u frames\n", hh_frames);
    if (sExiting++ == 0)
        savestate_shutdown();
    if (gfx_wm_shutdown != NULL)
        port_log_always(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
#if defined(TARGET_MIYOO)
    { extern void miyoo_clock_restore(void); miyoo_clock_restore(); } /* the CPU clock as found (kit miyoo_clock.c) */
#endif
    fflush(stdout);
    exit(0);
}

void hh_gfx_init(void)
{
    gfx_init(&gfx_sdl, &gfx_soft_api, "GoldenEye 007", false);
}

/* PERF: one line per 60 rendered frames (MK64/OoT format + the stage and the 3D size):
 * the frame interval the player sees, the render (interpreter + raster + present), the
 * kit's buckets, the time the pacer slept, and 3D triangles / span kilopixels per frame. */
extern s32 g_StageNum;
extern uint64_t gfx_soft_span_px, gfx_soft_tris;
extern void gfx_soft_get_internal_size(int *w, int *h);
static uint32_t sPerfFrames SAVESTATE_EXCLUDE, sPerfRenderUs SAVESTATE_EXCLUDE, sPerfMaxRenderUs SAVESTATE_EXCLUDE,
    sPerfLastUs SAVESTATE_EXCLUDE, sPerfIntervalUs SAVESTATE_EXCLUDE, sPerfMaxIntervalUs SAVESTATE_EXCLUDE;
static uint32_t sPerfIntervals SAVESTATE_EXCLUDE; /* intervals summed this window: 59 in the first, 60 after (the OoT port) */
static uint32_t sPerfBuckets[PROF_COUNT] SAVESTATE_EXCLUDE;

/* The device bench's child runs (hh_bench.c): the frames from VI $GE_BENCH_FROM to the exit,
 * summed, and one BENCH line at the exit (hh_bench_report, from hh_plat.c). */
static int sBench SAVESTATE_EXCLUDE = -1;
static uint32_t sBenchFrom SAVESTATE_EXCLUDE, sBenchFrames SAVESTATE_EXCLUDE, sBenchT0 SAVESTATE_EXCLUDE, sBenchT1 SAVESTATE_EXCLUDE;
static uint64_t sBenchRender SAVESTATE_EXCLUDE, sBenchR3d SAVESTATE_EXCLUDE, sBenchR2d SAVESTATE_EXCLUDE, sBenchPres SAVESTATE_EXCLUDE;
static uint64_t sBenchPx SAVESTATE_EXCLUDE, sBenchTris SAVESTATE_EXCLUDE;

void hh_sprof_start(void);
void hh_sprof_stop(void);

extern volatile uint32_t hh_audio_underruns, hh_audio_overruns; /* hh_audio_sdl.c: running totals */
extern uint32_t hh_rsp_yields; /* hh_os.c (GE_RSPYIELD): times a graphics task handed the host back, running total */
void hh_bench_report(void)
{
    const float n = sBenchFrames > 1 ? (float)(sBenchFrames - 1) : 1.0f;
    if (sBench != 1)
        return;
    hh_sprof_stop(); /* GE_SPROF: the device profile of the window (hh_sprof.c) */
    sBenchTris += gfx_soft_tris; sBenchPx += gfx_soft_span_px; /* since the last PERF line */
    port_log_always("BENCH %s | frames %u | wall %.2f ms | render %.2f r3d %.2f r2d %.2f pres %.2f | tris %.0f kpx %.1f"
                    " | aud dry %u yield %u\n",
                    getenv("GE_BENCH_CHILD"), (unsigned)sBenchFrames, (sBenchT1 - sBenchT0) / n / 1000.0f,
                    sBenchRender / n / 1000.0f, sBenchR3d / n / 1000.0f, sBenchR2d / n / 1000.0f, sBenchPres / n / 1000.0f,
                    sBenchTris / n, sBenchPx / n / 1000.0f, (unsigned)hh_audio_underruns, (unsigned)hh_rsp_yields);
}

extern uint32_t hh_ai_drops;                                      /* hh_hw.c */
uint32_t hh_audio_fill_ms(void);
extern uint32_t hh_rsp_away_us; /* and this task's time spent so */
static uint32_t sPerfYields SAVESTATE_EXCLUDE;
#if defined(MIYOO)
static uint32_t sPerfSteps SAVESTATE_EXCLUDE; /* the window's frame steps (speedgraphframes), summed */
#endif

/* The handheld menu runs inside a frame's render (gfx_sdl12.c handle_events, from gfx_start_frame): its time, set
 * there, is left out of the render figures here (2026-10-02: the PERF line's "render max 3380" at the Dam was the
 * menu open for a volume change, not a slow frame). */
uint32_t hh_menu_us SAVESTATE_EXCLUDE;

/* HITCH: one line for a frame whose render took over 500 ms (wall, menu included), naming where the time went: the
 * menu, the kit's buckets inside the render, and the rest (the display list interpreter, texture hashing, deferred
 * resolves). gPortProfFrame's sleep bucket also holds the pacer's sleeps before this render, so it is not used. */
#define HH_HITCH_US 500000u
static void hh_hitch_line(uint32_t render_us, uint32_t menu_us)
{
    const uint32_t *b = gPortProfFrame;
    const uint32_t named = menu_us + b[PROF_R3D] + b[PROF_R2D] + b[PROF_TEX] + b[PROF_COMP] + b[PROF_CLR] + b[PROF_PRES];
    port_log_always("HITCH stage %d vi %u frame %u: render %.1f ms = menu %.1f + r3d %.1f r2d %.1f tex %.1f comp %.1f "
                    "clr %.1f pres %.1f + other %.1f | aud dry %u | audio and scheduler during it %.1f (not counted)\n",
                    (int)g_StageNum, hh_vicount, hh_frames, render_us / 1000.0f, menu_us / 1000.0f,
                    b[PROF_R3D] / 1000.0f, b[PROF_R2D] / 1000.0f, b[PROF_TEX] / 1000.0f, b[PROF_COMP] / 1000.0f,
                    b[PROF_CLR] / 1000.0f, b[PROF_PRES] / 1000.0f,
                    render_us > named ? (render_us - named) / 1000.0f : 0.0f, (unsigned)hh_audio_underruns,
                    hh_rsp_away_us / 1000.0f);
}

/* STALL: one line for a frame interval over 250 ms (the menu's time left out), split by where the host's time went
 * since the last frame (hh_os.c hh_acct): each game thread (thread_config.h: 3 main = the game's logic and display
 * lists, 4 audio = the mixer's command lists, 2 scheduler; other = the rest), the kernel's service (audio tasks, events), the graphics task
 * (the render), the frame hook (save states), the idle sleeps; the longest single turn of one thread (no OS call: no
 * retrace, no sound delivered in it); the process's own CPU time, page faults (card: waited for a read, the executable's code or the
 * ROM; mem: fresh memory), waits and preemptions, block reads/writes, and the ROM pages read for the first time
 * (hh_hw.c: the ROM is mapped from the card). GE_FRAMELOG=1: the same line for every frame. */
#include <sys/resource.h>
extern u32 hh_rom_cold_pages;
static uint32_t sStallAcct[HH_ACCT_N] SAVESTATE_EXCLUDE, sStallCold SAVESTATE_EXCLUDE, sStallLast SAVESTATE_EXCLUDE;
static struct rusage sStallRu SAVESTATE_EXCLUDE; /* at the last line (or the first frame) */
static int sStallRuOk SAVESTATE_EXCLUDE;
static int sFrameLog SAVESTATE_EXCLUDE = -1;
static float hh_tv_ms(const struct timeval *a, const struct timeval *b)
{
    return (float)(a->tv_sec - b->tv_sec) * 1000.0f + (float)(a->tv_usec - b->tv_usec) / 1000.0f;
}
/* For the STALL line: the system's free and available memory (kB, /proc/meminfo; the device has 64 MB and runs with
 * ~2 MB free: a reclaim, or the executable's code paged out and read back, is a wait) and the runnable tasks (/proc/loadavg's
 * 4th field: another process wanting the one core). -1 where unreadable (the desktop: whatever the host has). */
static void hh_stall_sys(long *free_kb, long *avail_kb, int *running)
{
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");
    *free_kb = *avail_kb = -1;
    *running = -1;
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "MemFree:", 8) == 0)
                *free_kb = atol(line + 8);
            else if (strncmp(line, "MemAvailable:", 13) == 0)
                *avail_kb = atol(line + 13);
        }
        fclose(f);
    }
    f = fopen("/proc/loadavg", "r");
    if (f != NULL) {
        float a, b, c;
        int run = 0, tot = 0;
        if (fscanf(f, "%f %f %f %d/%d", &a, &b, &c, &run, &tot) == 5)
            *running = run;
        fclose(f);
    }
}

static void hh_stall_frame(uint32_t now, uint32_t menu, uint32_t render_us)
{
    const uint32_t iv = sStallLast ? now - sStallLast : 0;
    uint32_t d[HH_ACCT_N];
    struct rusage ru;
    int i;
    if (sFrameLog < 0)
        sFrameLog = getenv("GE_FRAMELOG") != NULL && atoi(getenv("GE_FRAMELOG")) > 0;
    sStallLast = now;
    for (i = 0; i < HH_ACCT_N; i++) {
        d[i] = hh_os_acct_us[i] - sStallAcct[i];
        sStallAcct[i] = hh_os_acct_us[i];
    }
    /* not a stall: an interval the kernel mostly slept through (every thread waiting on the clock: the boot's black
     * frames, the logos' pace) */
    if (((iv > menu && iv - menu >= 250000u && d[HH_ACCT_SLEEP] < (iv - menu) / 2) || sFrameLog) && sStallRuOk &&
        getrusage(RUSAGE_SELF, &ru) == 0) {
        long free_kb, avail_kb;
        int running;
        hh_stall_sys(&free_kb, &avail_kb, &running);
        /* cpu: this process's own time (user + system) in the interval: far below the interval = it waited (the
         * card, or another process had the CPU: "switches" forced = preempted, waits = blocked) */
        port_log_always("%s stage %d vi %u frame %u: %.1f ms = render %.1f menu %.1f | main %.1f audio %.1f sched %.1f "
                        "other %.1f | service %.1f draw %.1f hook %.1f sleep %.1f | longest turn %.1f (thread %d) "
                        "| cpu user %.1f sys %.1f | faults card %ld mem %ld | waits %ld preempted %ld | blocks in %ld out %ld "
                        "| rom pages %u | mem free %ld kB avail %ld kB, running %d | aud dry %u\n",
                        sFrameLog ? "FRAME" : "STALL", (int)g_StageNum, hh_vicount, hh_frames, iv / 1000.0f,
                        render_us / 1000.0f, menu / 1000.0f, d[3] / 1000.0f, d[4] / 1000.0f, d[2] / 1000.0f,
                        (d[0] + d[1] + d[5] + d[6] + d[7]) / 1000.0f,
                        d[HH_ACCT_SERVICE] / 1000.0f, d[HH_ACCT_DRAW] / 1000.0f, d[HH_ACCT_HOOK] / 1000.0f,
                        d[HH_ACCT_SLEEP] / 1000.0f, hh_os_slice_max_us / 1000.0f, hh_os_slice_max_id,
                        hh_tv_ms(&ru.ru_utime, &sStallRu.ru_utime), hh_tv_ms(&ru.ru_stime, &sStallRu.ru_stime),
                        ru.ru_majflt - sStallRu.ru_majflt, ru.ru_minflt - sStallRu.ru_minflt,
                        ru.ru_nvcsw - sStallRu.ru_nvcsw, ru.ru_nivcsw - sStallRu.ru_nivcsw,
                        ru.ru_inblock - sStallRu.ru_inblock, ru.ru_oublock - sStallRu.ru_oublock,
                        (unsigned)(hh_rom_cold_pages - sStallCold), free_kb, avail_kb, running,
                        (unsigned)hh_audio_underruns);
        sStallRu = ru;
    } else if (getrusage(RUSAGE_SELF, &ru) == 0) {
        sStallRu = ru; /* one syscall a frame (~2 us): the line covers exactly the long interval */
        sStallRuOk = 1;
    }
    sStallCold = hh_rom_cold_pages;
    hh_os_slice_max_us = 0;
}

static uint32_t sPerfWin SAVESTATE_EXCLUDE; /* frames per PERF line: 60 (Bench, GE_LOGVERBOSE) or 300 */

static void hh_perf_frame(uint32_t render_us)
{
    uint32_t now = port_time_us();
    int i;
    {   /* the menu's time out of the render (see hh_menu_us); a frame over 500 ms says where its time went */
        const uint32_t menu = hh_menu_us;
        hh_menu_us = 0;
        if (render_us >= HH_HITCH_US)
            hh_hitch_line(render_us, menu);
        hh_stall_frame(now, menu, render_us);
        render_us = render_us > menu ? render_us - menu : 0;
    }
    if (sBench < 0) {
        sBench = getenv("GE_BENCH_CHILD") != NULL;
        sBenchFrom = getenv("GE_BENCH_FROM") ? (uint32_t)atoi(getenv("GE_BENCH_FROM")) : 0;
    }
    if (sBench && hh_vicount >= sBenchFrom) {
        if (sBenchFrames++ == 0) {
            sBenchT0 = now; /* the window opens at this frame's end: the next frames are counted */
            hh_sprof_start();
            gfx_soft_tris = gfx_soft_span_px = 0;
        } else {
            sBenchRender += render_us;
            sBenchR3d += gPortProfFrame[PROF_R3D];
            sBenchR2d += gPortProfFrame[PROF_R2D];
            sBenchPres += gPortProfFrame[PROF_PRES];
        }
        sBenchT1 = now;
    }
    if (sPerfLastUs != 0) {
        uint32_t iv = now - sPerfLastUs;
        sPerfIntervalUs += iv;
        sPerfIntervals++;
        if (iv > sPerfMaxIntervalUs)
            sPerfMaxIntervalUs = iv;
    }
    sPerfLastUs = now;
#if defined(MIYOO)
    { extern s32 speedgraphframes; sPerfSteps += (uint32_t)speedgraphframes; }
#endif
    sPerfRenderUs += render_us;
    if (render_us > sPerfMaxRenderUs)
        sPerfMaxRenderUs = render_us;
    for (i = 0; i < PROF_COUNT; i++) {
        sPerfBuckets[i] += gPortProfFrame[i];
        gPortProfLast[i] = gPortProfFrame[i];
        gPortProfFrame[i] = 0;
    }
    /* Normal play writes one PERF line per 300 frames (~10 s) instead of per 60; a Bench child and
     * GE_LOGVERBOSE=1 (debug.cfg: logverbose=1) keep the 60-frame window. */
    if (sPerfWin == 0) sPerfWin = (sBench || getenv("GE_LOGVERBOSE") != NULL) ? 60 : 300;
    if (++sPerfFrames == sPerfWin) {
        const float n = (float)sPerfWin, iv = sPerfIntervals ? sPerfIntervalUs / (float)sPerfIntervals / 1000.0f : 0.0f;
        int w = 0, h = 0;
        gfx_soft_get_internal_size(&w, &h);
#if defined(MIYOO)
        /* the second core (gfx_soft.c SplitJob), a frame: triangles split, jobs handed over, waits that slept */
        extern void miyoo_worker_stats(unsigned *jobs, unsigned *sleeps, unsigned long long *spins);
        extern int miyoo_worker_ready(void);
        extern unsigned gfx_soft_split_tris, gfx_soft_batch_splits;
        extern uint32_t gfx_soft_split_wpx;
        unsigned wk_jobs = 0, wk_sleeps = 0;
        miyoo_worker_stats(&wk_jobs, &wk_sleeps, NULL);
        const float wk_split = (float)gfx_soft_split_tris, wk_kpx = (float)gfx_soft_split_wpx / 1000.0f;
        const float wk_batch = (float)gfx_soft_batch_splits;
        gfx_soft_split_tris = 0;
        gfx_soft_batch_splits = 0;
        gfx_soft_split_wpx = 0;
        /* the clock as the PLL says now, the pacing, and the frame outside the render: the audio tasks */
        extern int miyoo_clock_mhz(void), hh_uncap(void), hh_minstep(void);
        /* the minimum frame step in force here (hh_plat.c: minstep=1 in the missions, 2 on the title stage) and
         * the mean step of the window's frames (retraces a frame: 2.00 under the cap, 1.00 to 2.00 with minstep 1) */
        const int ms_now = (hh_minstep() == 1 && (int)g_StageNum != 90 /* LEVELID_TITLE */) ? 1 : 2;
        port_log_always("PERF stage %d 3d %dx%d | fps %.1f iv %.1f max %.1f | render %.1f max %.1f | r3d %.1f r2d %.1f "
                        "tex %.1f clr %.1f pres %.1f gfx %.1f | sleep %.1f | tris %u kpx %.1f | aud dry %u over %u drop %u fill %u yield %u"
                        " | wk %s batch %.1f split %.1f kpx %.1f jobs %.1f sleeps %.2f | cpu %d MHz uncap %s aud %.1f"
                        " | minstep %d step %.2f\n",
                        (int)g_StageNum, w, h, iv > 0.0f ? 1000.0f / iv : 0.0f, iv, sPerfMaxIntervalUs / 1000.0f,
                        sPerfRenderUs / n / 1000.0f, sPerfMaxRenderUs / 1000.0f, sPerfBuckets[PROF_R3D] / n / 1000.0f,
                        sPerfBuckets[PROF_R2D] / n / 1000.0f, sPerfBuckets[PROF_TEX] / n / 1000.0f,
                        sPerfBuckets[PROF_CLR] / n / 1000.0f, sPerfBuckets[PROF_PRES] / n / 1000.0f,
                        sPerfBuckets[PROF_GFX] / n / 1000.0f, sPerfBuckets[PROF_SLEEP] / n / 1000.0f,
                        (unsigned)(gfx_soft_tris / sPerfWin), (float)gfx_soft_span_px / n / 1000.0f,
                        (unsigned)hh_audio_underruns, (unsigned)hh_audio_overruns, (unsigned)hh_ai_drops,
                        (unsigned)hh_audio_fill_ms(), (unsigned)(hh_rsp_yields - sPerfYields),
                        miyoo_worker_ready() ? "on" : "off", wk_batch / n, wk_split / n, wk_kpx / n, wk_jobs / n, wk_sleeps / n,
                        miyoo_clock_mhz(), hh_uncap() ? "on" : "off", sPerfBuckets[PROF_AUDIO] / n / 1000.0f,
                        ms_now, sPerfSteps / n);
#else
        port_log_always("PERF stage %d 3d %dx%d | fps %.1f iv %.1f max %.1f | render %.1f max %.1f | r3d %.1f r2d %.1f "
                        "tex %.1f clr %.1f pres %.1f gfx %.1f | sleep %.1f | tris %u kpx %.1f | aud dry %u over %u drop %u fill %u yield %u\n",
                        (int)g_StageNum, w, h, iv > 0.0f ? 1000.0f / iv : 0.0f, iv, sPerfMaxIntervalUs / 1000.0f,
                        sPerfRenderUs / n / 1000.0f, sPerfMaxRenderUs / 1000.0f, sPerfBuckets[PROF_R3D] / n / 1000.0f,
                        sPerfBuckets[PROF_R2D] / n / 1000.0f, sPerfBuckets[PROF_TEX] / n / 1000.0f,
                        sPerfBuckets[PROF_CLR] / n / 1000.0f, sPerfBuckets[PROF_PRES] / n / 1000.0f,
                        sPerfBuckets[PROF_GFX] / n / 1000.0f, sPerfBuckets[PROF_SLEEP] / n / 1000.0f,
                        (unsigned)(gfx_soft_tris / sPerfWin), (float)gfx_soft_span_px / n / 1000.0f,
                        (unsigned)hh_audio_underruns, (unsigned)hh_audio_overruns, (unsigned)hh_ai_drops,
                        (unsigned)hh_audio_fill_ms(), (unsigned)(hh_rsp_yields - sPerfYields));
#endif
        sPerfYields = hh_rsp_yields;
#if defined(MIYOO)
        sPerfSteps = 0;
#endif
        if (sBench && sBenchFrames > 1) { sBenchTris += gfx_soft_tris; sBenchPx += gfx_soft_span_px; }
        gfx_soft_tris = gfx_soft_span_px = 0;
        sPerfFrames = sPerfRenderUs = sPerfMaxRenderUs = sPerfIntervalUs = sPerfMaxIntervalUs = sPerfIntervals = 0;
        for (i = 0; i < PROF_COUNT; i++)
            sPerfBuckets[i] = 0;
    }
}

/* One gfx task = one frame's display list (rspGfxTaskStart: data_ptr = the first Gfx,
 * a KSEG0 pointer). */
void hh_gfx_run_task(OSTask *task)
{
    Gfx *dl = (Gfx *)task->t.data_ptr;
    uint32_t t0;
    if (dl == NULL)
        return;
    t0 = port_time_us();
    gfx_start_frame();
    {   /* the folder screens and the legal page: the N64's texture filter (gfx_soft.c
         * sN64Filter); GE_N64FILTER=0 draws them like the world (A/B) */
        extern int gfx_front_n64filter;
        int hhFrontFolderMenu(void);
        static int sKnob = -1;
        if (sKnob < 0) { const char *e = getenv("GE_N64FILTER"); sKnob = !(e && e[0] == '0'); }
        gfx_front_n64filter = sKnob && hhFrontFolderMenu();
    }
    {   /* the intro's GoldenEye logo: 3D at 2x, box-filtered (gfx_soft.c gfx_soft_ss2x);
         * GE_LOGOSS=0: before */
        extern int gfx_soft_ss2x;
        int hhFrontSupersample(void);
        static int sKnob = -1;
        if (sKnob < 0) { const char *e = getenv("GE_LOGOSS"); sKnob = !(e && e[0] == '0'); }
        gfx_soft_ss2x = sKnob && hhFrontSupersample();
    }
    gfx_run(dl);
    gfx_end_frame();
    {   /* GE_RSPYIELD (hh_os.c): the time the kernel ran the audio and the scheduler during this task is not render */
        extern uint32_t hh_rsp_away_us;
        const uint32_t all = port_time_us() - t0;
        hh_perf_frame(all > hh_rsp_away_us ? all - hh_rsp_away_us : 0);
    }
}
