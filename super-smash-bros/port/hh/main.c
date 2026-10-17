/* main.c -- boot, the 60 Hz frame loop and display-list submission.
 *
 * The game's five N64 OS threads run as coroutines (bs/n64_stubs.c,
 * coroutine.c). Each host tick does what one N64 vertical retrace did:
 *   1. propagate the VI framebuffer swap and deliver gfx completions that
 *      were deferred by one retrace (port_vi_simulate_vblank);
 *   2. post INTR_VRETRACE to the scheduler;
 *   3. resume every waiting thread until none can make progress: the
 *      scheduler ticks its clients, the controller thread polls, the game
 *      runs one logic frame and submits a gfx task, audio mixes a frame.
 * The display list the game submitted (port_submit_display_list) is drawn
 * after the resume loop, from the main thread.
 *
 * Logic always runs 60 times a second. Rendering is what gives when the CPU
 * is short: if a tick starts late, its display list is dropped (the game
 * still thinks it drew), so the match plays at full speed with fewer frames
 * shown -- the opposite of the N64, which slowed down. (SPIKE.md Q3.) */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "port.h"
#include <PR/gbi.h>
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"
#include "coroutine.h"
#include "hh_game.h"
#include "savestate.h"

#define HH_MAX_DROPS 3
static int sRenderEvery = 1; /* SSB64_RENDER_EVERY (tests) */
#include "romdata.h"
#include "audio_out.h"

extern struct GfxRenderingAPI gfx_soft_api;
extern struct GfxWindowManagerAPI gfx_sdl;

/* Decomp / bs/n64_stubs.c. */
extern void syMainLoop(void);
extern void port_vi_simulate_vblank(void);
extern void port_resume_service_threads(void);
typedef void *OSMesg;
typedef struct OSMesgQueue OSMesgQueue;
extern OSMesgQueue gSYSchedulerTaskMesgQueue;
extern int osSendMesg(OSMesgQueue *mq, OSMesg msg, int flag);
#define OS_MESG_NOBLOCK 0
#define INTR_VRETRACE 1

void hh_audio_init(void);
int hh_audio_take_peak(void);
uint32_t hh_audio_underruns(void);
uint32_t hh_audio_take_ring_min(void), hh_audio_gaps(void);
void port_fighter_seed_vanilla(void);   /* decomp ftport.c */

#define TICK_US 16667u          /* NTSC field: 1e6 / 60 */

static volatile int sExitRequested;
static int sFrameCount;         /* host ticks since boot */
static void *sPendingDL;
/* The game's own request to hold back a display list or two (BattleShip's load stall): game
 * state, so a save state carries it (hh_game.h). */
static int sLoadStall HH_GS_BSS;
static int sRenderedFrames, sDroppedFrames;

/* Bench summary (whole run): displayed-frame intervals for the 1% low, and
 * totals for averages. */
#define BENCH_MAX_FRAMES 8192
static uint16_t sIntervalsMs10[BENCH_MAX_FRAMES];   /* 0.1 ms units */
static int sIntervalCount;
static uint32_t sLastPresentUs, sRunStartUs, sRunTicks, sRunRendered, sRunDropped;
static uint64_t sRunLogicUs, sRunGfxUs;

/* ---------------------------------------------------------------- pacer --- */
/* Logic ticks at 60 Hz by the wall clock and never slips; pictures give. The
 * round-3 rule dropped a tick's picture whenever the tick ran more than half a
 * tick late: on the device that drew every 2 or 3 ticks with no pattern (v7:
 * "choppy"). F-Zero X's pacer (ea68b8b, with 06750ab's late-run rule) instead
 * measures what a picture costs (EMA; a sample counts at most twice the
 * average) plus the logic and audio every tick, and draws on a fixed cadence:
 * the fastest that holds them in its ticks (cad_fits). It steps down only after
 * PACER_LATE_RUN pictures in a row miss the cadence (one hitch moves nothing)
 * and back up after 30 pictures with room. A picture slot whose tick started
 * more than half a tick late still waits a tick, at most HH_MAX_DROPS in a row.
 * Modes (SSB64_PACER or bench.cfg pacer=): 0 the old drop rule (the default:
 * bench v14 on the device, BUILDLOG 16c), 1 even cadences (60/30/20/15 fps), 2 also 24 fps (drawn on ticks 2, 3:
 * Smash's pictures sit right at the 30 fps edge on the device), "rotate" (the
 * bench's default) cycles 0, 1, 2 every 5 s through the fight and logs each. */
extern const char *port_save_path(const char *name);
#define PACER_LATE_RUN 4
#define PACER_ROTATE_TICKS 300
static const struct { uint8_t ticks, pics, gap[2]; } kCad[] = {
    { 1, 1, { 1 } }, { 2, 1, { 2 } }, { 5, 2, { 2, 3 } }, { 3, 1, { 3 } }, { 4, 1, { 4 } },
};
#define NCAD ((int)(sizeof kCad / sizeof kCad[0]))
static int sPacerMode = 0, sPacerRotate;
static uint32_t sPicEmaUs, sTickEmaUs = 4000, sSlotLateUs;
static int sCad, sGap, sDivUp, sLateRun, sSkipsInRow;
static const char *const kPacerName[3] = { "drop", "even", "even+24" };
/* per mode: ticks, pictures, gaps between pictures (1..4, 5+ ticks), picture intervals */
#define PACER_MAX_IV 4096
static uint32_t sModeTicks[3], sModePics[3], sModeGaps[3][5], sModeLastTick[3];
static uint16_t sModeIv[3][PACER_MAX_IV];
static int sModeIvN[3];
static uint32_t sModeLastUs;

static int cad_allowed(int c) { return kCad[c].pics == 1 || sPacerMode == 2; }
/* Does cadence c hold pictures of pic us with tick us of logic and audio every tick,
 * in its ticks plus slack us? F-Zero X keeps 5% spare; Smash's pictures sit at the
 * 30 fps edge, and its skipped tick has ~14 ms idle to absorb an overrun, so the whole
 * budget counts (the round-16 desktop run, 27 ms pictures: 95% held 20 fps). */
static int cad_fits(int c, uint32_t pic, uint32_t tick, uint32_t slack)
{
    return (uint32_t)kCad[c].pics * pic + (uint32_t)kCad[c].ticks * tick <= (uint32_t)kCad[c].ticks * TICK_US + slack;
}
static int cad_best(void)
{
    int n = 0;
    while (n + 1 < NCAD && (!cad_allowed(n) || !cad_fits(n, sPicEmaUs, sTickEmaUs, 0))) n++;
    return n;
}
static int pacer_should_draw(void)
{
    if (sSkipsInRow >= HH_MAX_DROPS) return 1;
    if (sSkipsInRow + 1 < kCad[sCad].gap[sGap]) return 0;
    return sSlotLateUs <= TICK_US / 2;
}
static void pacer_note_picture(uint32_t us)
{
    const uint32_t sample = sPicEmaUs && us > 2 * sPicEmaUs ? 2 * sPicEmaUs : us;
    sGap = (sGap + 1) % kCad[sCad].pics;
    /* late: over by more than the half tick the next slot still draws with */
    sLateRun = !cad_fits(sCad, us, sTickEmaUs, TICK_US / 2) ? sLateRun + 1 : 0;
    sPicEmaUs = sPicEmaUs ? (sPicEmaUs * 7 + sample) / 8 : us;
    const int n = cad_best();
    if (n > sCad) {
        sDivUp = 0;
        if (sLateRun >= PACER_LATE_RUN) { sCad = n; sGap = 0; sLateRun = 0; }
    } else if (n < sCad) {
        if (++sDivUp >= 30) { sCad = n; sGap = 0; sDivUp = 0; }
    } else {
        sDivUp = 0;
    }
}
static void pacer_set_mode(int m)
{
    if (m == sPacerMode) return;
    sPacerMode = m;
    if (sPicEmaUs) sCad = cad_best(); /* the new mode's own cadence at once, no hysteresis */
    else if (!cad_allowed(sCad)) sCad++;
    sGap = 0; sLateRun = 0; sDivUp = 0;
}
/* The bench's speed A/B (2026-10-07, branch speed-2026-10-07; the 2026-10-03 lerprows A/B's frame): with no pacer=,
 * lerprows=, fastrows=, spanvec= or zskip= line in bench.cfg (and none of their SSB64_ variables) the bench keeps the
 * drop pacer and changes the speed round's renderer switches (gfx_soft.c SSB64_FASTROWS, SSB64_SPANVEC, SSB64_ZSKIP)
 * every 5 s: the even windows all on, the odd ones each switch off in turn, then all of them off ("round-off": the
 * renderer before the round). After the first 5 s each window kind's pictures are counted and the summary prints
 * "BENCH ab <kind> | pictures | render ms/picture". Every window draws the same pixels. pacer=rotate brings back the
 * pacer rotation; any of the switch lines fixes the switches for a whole run (0 = off). */
extern int gfx_soft_lerp_rows, gfx_soft_fastrows, gfx_soft_spanvec, gfx_soft_zskip;
extern int hh_cfg_switch(const char *env, const char *key, int dflt);
static int sAbOn, sBenchRun, sAbKind;
#define AB_KINDS 5
static const char *const kAbName[AB_KINDS] = { "all-on", "fastrows-off", "spanvec-off", "zskip-off", "round-off" };
static uint32_t sAbPics[AB_KINDS];
static uint64_t sAbUs[AB_KINDS];
/* window w (5 s each): even all on, odd 1..AB_KINDS-1 in turn */
static void ab_window(int w)
{
    const int k = (w & 1) ? 1 + ((w >> 1) % (AB_KINDS - 1)) : 0;
    sAbKind = k;
    gfx_soft_fastrows = k != 1 && k != 4;
    gfx_soft_spanvec = k != 2 && k != 4;
    gfx_soft_zskip = k != 3 && k != 4;
}
static void pacer_config(int bench)
{
    char line[64], val[16] = "";
    const char *e = getenv("SSB64_PACER");
    sBenchRun = bench;
    if (e != NULL) { snprintf(val, sizeof val, "%s", e); }
    else {
        FILE *f = fopen(port_save_path("bench.cfg"), "r");
        if (f != NULL) {
            while (fgets(line, sizeof line, f) != NULL)
                if (strncmp(line, "pacer=", 6) == 0) { sscanf(line + 6, "%15s", val); break; }
            fclose(f);
        }
    }
    if (val[0] == '\0' && bench && hh_cfg_switch("SSB64_LERPROWS", "lerprows", -1) < 0 && hh_cfg_switch("SSB64_FASTROWS", "fastrows", -1) < 0 &&
        hh_cfg_switch("SSB64_SPANVEC", "spanvec", -1) < 0 && hh_cfg_switch("SSB64_ZSKIP", "zskip", -1) < 0) sAbOn = 1;
    else if (val[0] == '\0') sPacerRotate = bench;
    else if (strcmp(val, "rotate") == 0) sPacerRotate = 1;
    else { const int m = atoi(val); sPacerMode = m >= 0 && m <= 2 ? m : 0; }
    if (sPacerRotate) sPacerMode = 0;
    port_log_always("SSB64: pacer %s\n", sPacerRotate ? "rotate (drop, even, even+24 every 5 s)" : kPacerName[sPacerMode]);
    if (sAbOn) port_log_always("SSB64: bench A/B every 5 s (drop pacer): all on, then fastrows off, spanvec off, zskip off, all three off, each after an all-on window\n");
    else port_log_always("SSB64: texel-lerp rows %s, fastrows %d spanvec %d zskip %d\n", gfx_soft_lerp_rows ? "on" : "off",
                         gfx_soft_fastrows, gfx_soft_spanvec, gfx_soft_zskip);
}
/* A picture was shown: gap and interval stats for the mode that drew it. */
static void pacer_stats_picture(uint32_t now)
{
    const int m = sPacerMode;
    if (sModeLastTick[m] != 0) {
        const uint32_t g = (uint32_t)sFrameCount - sModeLastTick[m];
        if (g >= 1) sModeGaps[m][g >= 5 ? 4 : g - 1]++;
        if (sModeLastUs != 0 && sModeIvN[m] < PACER_MAX_IV && g < 30) {
            const uint32_t iv = (now - sModeLastUs) / 100;
            sModeIv[m][sModeIvN[m]++] = (uint16_t)(iv > 65535 ? 65535 : iv);
        }
    }
    sModePics[m]++;
    sModeLastTick[m] = (uint32_t)sFrameCount;
    sModeLastUs = now;
}

void port_request_exit(void) { sExitRequested = 1; }
/* gfx_sdl12.c's screen capture (dump.cfg) overrides this on the device. */
__attribute__((weak)) void port_capture_flush(void) { }
int port_get_frame_count(void) { return sFrameCount; }

/* BattleShip holds back a DL or two after a camera cut to mimic a load
 * hitch on real hardware; keep the behaviour (it hides a one-frame glitch). */
void port_sim_load_stall(int n)
{
    if (n > sLoadStall) sLoadStall = n;
}

/* The scheduler calls this from osSpTaskStartGo for every gfx task. */
void port_submit_display_list(void *dl)
{
    if (sLoadStall > 0) {
        sLoadStall--;
        return;
    }
    if (dl != NULL) sPendingDL = dl;
}

/* SP/DP completions arrive one retrace after submission, as on hardware. */
int port_get_last_dl_defer_n(void) { return 1; }

static void game_coroutine_entry(void *arg)
{
    (void)arg;
    port_log("SSB64: boot: syMainLoop\n");
    syMainLoop();
    port_log("SSB64: boot chain complete\n");
}

static void on_signal(int sig)
{
    (void)sig;
    sExitRequested = 1;
}

/* One N64 retrace. Returns 1 if a display list was produced. */
static int run_tick(void)
{
    uint32_t t0 = port_time_us();
    { extern void hh_os_test_clock_tick(void); hh_os_test_clock_tick(); } /* SSB64_TESTCLOCK (tests) */
    gfx_sdl.handle_events();
    port_vi_simulate_vblank();
    osSendMesg(&gSYSchedulerTaskMesgQueue, (OSMesg)(uintptr_t)INTR_VRETRACE, OS_MESG_NOBLOCK);
    port_resume_service_threads();
    {
        uint32_t dt = port_time_us() - t0;
        PORT_PROF_ADD(PROF_LOGIC, dt);
        sRunLogicUs += dt;
        sTickEmaUs = (sTickEmaUs * 15 + dt) / 16;
    }
    sFrameCount++;
    sRunTicks++;
    return sPendingDL != NULL;
}

static void render_pending(void)
{
    void *dl = sPendingDL;
    uint32_t t0 = port_time_us();
    sPendingDL = NULL;
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
    {
        /* SSB64_SLOWGFX=<us>: a device-slow picture on the desktop (pacer tests) */
        static long sSlow = -1;
        if (sSlow < 0) { const char *e = getenv("SSB64_SLOWGFX"); sSlow = e ? atol(e) : 0; }
        if (sSlow > 0) while ((int32_t)(port_time_us() - t0) < (int32_t)sSlow) { }
    }
    {
        uint32_t now = port_time_us(), dt = now - t0;
        PORT_PROF_ADD(PROF_GFX, dt);
        sRunGfxUs += dt;
        if (sLastPresentUs != 0 && sIntervalCount < BENCH_MAX_FRAMES) {
            uint32_t iv = (now - sLastPresentUs) / 100;
            sIntervalsMs10[sIntervalCount++] = (uint16_t)(iv > 65535 ? 65535 : iv);
        }
        sLastPresentUs = now;
        pacer_stats_picture(now);
        if (sAbOn && sFrameCount >= PACER_ROTATE_TICKS) { sAbPics[sAbKind]++; sAbUs[sAbKind] += dt; }
        if (sPacerMode != 0) pacer_note_picture(dt);
    }
    sRenderedFrames++;
    sRunRendered++;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* Resident memory from /proc (KiB): peak total (VmHWM), and now split into
 * anonymous (the game's own) and file-backed (the mmap'd ROM slice: page
 * cache the kernel can drop). */
static void rss_kb(long *hwm, long *anon, long *file)
{
    char line[128];
    FILE *f = fopen("/proc/self/status", "r");
    *hwm = *anon = *file = 0;
    if (f == NULL) return;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "VmHWM:", 6) == 0) *hwm = atol(line + 6);
        else if (strncmp(line, "RssAnon:", 8) == 0) *anon = atol(line + 8);
        else if (strncmp(line, "RssFile:", 8) == 0) *file = atol(line + 8);
    }
    fclose(f);
}

/* One line per bench stage: what the M3 go/no-go gate reads. */
extern uint64_t gPortAudioUsTotal;
static uint64_t sRunAudioUs0;

static void bench_summary(const char *label)
{
    float secs = (port_time_us() - sRunStartUs) / 1e6f;
    float low = 0.0f;
    long hwm, anon, file;
    rss_kb(&hwm, &anon, &file);
    if (sIntervalCount > 20) {
        qsort(sIntervalsMs10, (size_t)sIntervalCount, sizeof sIntervalsMs10[0], cmp_u16);
        uint16_t p99 = sIntervalsMs10[(sIntervalCount * 99) / 100];
        low = p99 ? 10000.0f / p99 : 0.0f;
    }
    port_log_always("BENCH %s | game speed %.1f%% (%.1f ticks/s) | fps avg %.1f, 1%% low %.1f | dropped %u | logic %.2f ms/tick (audio %.2f) | render %.2f ms/frame | RSS peak %ld KB (now %ld anon + %ld file) | audio underruns %u\n",
                    label, 100.0f * sRunTicks / (secs * 60.0f), sRunTicks / secs,
                    sRunRendered / secs, low, (unsigned)sRunDropped,
                    sRunTicks ? sRunLogicUs / 1000.0f / sRunTicks : 0.0f,
                    sRunTicks ? (gPortAudioUsTotal - sRunAudioUs0) / 1000.0f / sRunTicks : 0.0f,
                    sRunRendered ? sRunGfxUs / 1000.0f / sRunRendered : 0.0f,
                    hwm, anon, file, (unsigned)hh_audio_underruns());
    for (int m = 0; m < 3; m++) {
        if (sModeTicks[m] == 0) continue;
        float mlow = 0.0f;
        if (sModeIvN[m] > 20) {
            qsort(sModeIv[m], (size_t)sModeIvN[m], sizeof sModeIv[m][0], cmp_u16);
            const uint16_t p99 = sModeIv[m][(sModeIvN[m] * 99) / 100];
            mlow = p99 ? 10000.0f / p99 : 0.0f;
        }
        port_log_always("BENCH pacer %-7s | %u ticks | fps %.1f, 1%% low %.1f | gaps 1:%u 2:%u 3:%u 4:%u 5+:%u ticks\n",
                        kPacerName[m], sModeTicks[m], 60.0f * sModePics[m] / sModeTicks[m], mlow,
                        sModeGaps[m][0], sModeGaps[m][1], sModeGaps[m][2], sModeGaps[m][3], sModeGaps[m][4]);
    }
    for (int k = 0; sAbOn && k < AB_KINDS; k++)
        port_log_always("BENCH ab %-12s | %u pictures | render %.2f ms/picture\n", kAbName[k],
                        sAbPics[k], sAbPics[k] ? sAbUs[k] / 1000.0 / sAbPics[k] : 0.0);
}

/* PERF line every 5 s of game time: rendered fps, logic (all threads incl.
 * audio mixing) and render cost per rendered frame. */
extern uint32_t gfx_soft_phase_switches;
extern int texman_peak_percent(void);

/* PERF2 (round 12 diagnostics): where the logic time goes and what memory
 * does. reloc = reloc file loads (read, vpk0, byte swap, relocation) and
 * evict = the texture-cache range scans inside them, both inside logic;
 * comp/clr = the 85% composite and the clears, inside render but outside
 * r3d/r2d; faults = page faults (major = read from the SD card); texdrop =
 * uploads refused (arena full / no id left); texreset = full cache resets;
 * avail = the kernel's MemAvailable. */
extern uint32_t gPortRelocUs, gPortRelocLoads, gPortEvictUs, gPortEvictCalls;
extern uint32_t gfx_soft_upload_drops, gfx_soft_upload_noid, gfx_soft_tex_resets;
extern uint32_t gfx_texture_pool_used(void);

static void proc_faults(unsigned long *minflt, unsigned long *majflt)
{
    char buf[512];
    unsigned long skip;
    int i1, i2, i3, i4, i5;
    unsigned int flags;
    FILE *f = fopen("/proc/self/stat", "r");
    *minflt = *majflt = 0;
    if (f == NULL) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *p = strrchr(buf, ')'); /* the command name may hold spaces */
    if (p == NULL || p[1] == '\0' || p[2] == '\0') return;
    /* after ") S": ppid pgrp session tty_nr tpgid flags minflt cminflt majflt */
    sscanf(p + 3, " %d %d %d %d %d %u %lu %lu %lu", &i1, &i2, &i3, &i4, &i5, &flags, minflt, &skip, majflt);
}

static long mem_available_kb(void)
{
    char line[128];
    long kb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) return -1;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "MemAvailable:", 13) == 0) { kb = atol(line + 13); break; }
    }
    fclose(f);
    return kb;
}

static void perf_report(void)
{
    static uint32_t sLast;
    static int sLastTick;
    uint32_t now = port_time_us();
    if (sLast == 0) { sLast = now; sLastTick = sFrameCount; return; }
    /* Normal play: one PERF/PERF2 pair per 600 ticks (~10 s), not 300. A bench run, a pacer rotation, the speed A/B
     * and SSB64_LOGVERBOSE=1 (bench.cfg/ssb64 cfg: logverbose=1) keep 300 (the 5 s phases line up with it). */
    static int sWin;
    if (sWin == 0)
        sWin = (sBenchRun || sPacerRotate || sAbOn || hh_cfg_switch("SSB64_LOGVERBOSE", "logverbose", 0) > 0) ? 300 : 600;
    if (sFrameCount - sLastTick < sWin) return;
    float secs = (now - sLast) / 1e6f;
    int ticks = sFrameCount - sLastTick;
#if defined(MIYOO) /* the second core: this window's split triangles and their share of the 3D pixels (gfx_soft.c) */
    extern int gfx_soft_mt_line(char *buf, int n);
    char mt[128];
    gfx_soft_mt_line(mt, sizeof mt);
#define PERF_MT "%s"
#define PERF_MT_ARG , mt
#else
#define PERF_MT ""
#define PERF_MT_ARG
#endif
    port_log_always("PERF ticks/s %.1f fps %.1f dropped %d | logic %.2f ms/tick (audio %.2f) | render %.2f ms/frame (r3d %.2f r2d %.2f tex %.2f pres %.2f) | sw %.1f/f | tex %d%% | audio peak %d underruns %u ring min %u gaps %u" PERF_MT "\n",
                    ticks / secs, sRenderedFrames / secs, sDroppedFrames,
                    gPortProfFrame[PROF_LOGIC] / 1000.0f / ticks,
                    gPortProfFrame[PROF_AUDIO] / 1000.0f / ticks,
                    sRenderedFrames ? gPortProfFrame[PROF_GFX] / 1000.0f / sRenderedFrames : 0.0f,
                    sRenderedFrames ? gPortProfFrame[PROF_R3D] / 1000.0f / sRenderedFrames : 0.0f,
                    sRenderedFrames ? gPortProfFrame[PROF_R2D] / 1000.0f / sRenderedFrames : 0.0f,
                    sRenderedFrames ? gPortProfFrame[PROF_TEX] / 1000.0f / sRenderedFrames : 0.0f,
                    sRenderedFrames ? gPortProfFrame[PROF_PRES] / 1000.0f / sRenderedFrames : 0.0f,
                    sRenderedFrames ? (float)gfx_soft_phase_switches / sRenderedFrames : 0.0f, texman_peak_percent(),
                    hh_audio_take_peak(), (unsigned)hh_audio_underruns(), (unsigned)hh_audio_take_ring_min(),
                    (unsigned)hh_audio_gaps() PERF_MT_ARG);
#undef PERF_MT
#undef PERF_MT_ARG
    {
        static unsigned long sMin0, sMaj0;
        unsigned long mn, mj;
        proc_faults(&mn, &mj);
        port_log_always("PERF2 reloc %u loads %.2f ms/tick (evict %u %.3f ms/tick, pool %u) | comp %.2f clr %.2f ms/frame | faults %lu (major %lu) | texdrop %u noid %u texreset %u | avail %ld KB\n",
                        gPortRelocLoads, gPortRelocUs / 1000.0f / ticks, gPortEvictCalls, gPortEvictUs / 1000.0f / ticks,
                        gfx_texture_pool_used(),
                        sRenderedFrames ? gPortProfFrame[PROF_COMP] / 1000.0f / sRenderedFrames : 0.0f,
                        sRenderedFrames ? gPortProfFrame[PROF_CLR] / 1000.0f / sRenderedFrames : 0.0f,
                        mn - sMin0, mj - sMaj0, gfx_soft_upload_drops, gfx_soft_upload_noid, gfx_soft_tex_resets,
                        mem_available_kb());
        sMin0 = mn; sMaj0 = mj;
        gPortRelocUs = gPortRelocLoads = gPortEvictUs = gPortEvictCalls = 0;
        gfx_soft_upload_drops = gfx_soft_upload_noid = gfx_soft_tex_resets = 0;
    }
    memset(gPortProfFrame, 0, sizeof(gPortProfFrame));
    gfx_soft_phase_switches = 0;
    sLast = now;
    sLastTick = sFrameCount;
    sRenderedFrames = sDroppedFrames = 0;
}

/* --bench 6,1,3: the M3 benchmark. Plays stage 6 (bench.c), then re-execs
 * itself with the remaining stages, one fresh process per stage, and exits
 * after the last. Each stage: 65 s (a 1-minute match plus its intro), then
 * a BENCH summary line in console.log. */
#define BENCH_TICKS (65 * 60)
static const char *sBenchRest;   /* stages still to run after this one */
static char sBenchLabel[32];

static void parse_args(int argc, char **argv)
{
#ifdef HH_BENCH_DEFAULT
    int have_bench = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--bench") == 0) have_bench = 1;
    if (!have_bench) {
        /* The benchmark build: no arguments needed from the launcher. */
        static char stages[64];
        char *comma;
        snprintf(stages, sizeof stages, "%s", HH_BENCH_DEFAULT);
        comma = strchr(stages, ',');
        if (comma != NULL) { *comma = '\0'; sBenchRest = comma + 1; }
        setenv("SSB64_BENCH", stages, 1);
        snprintf(sBenchLabel, sizeof sBenchLabel, "stage %s", stages);
    }
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            static char stages[64];
            char *comma;
            snprintf(stages, sizeof stages, "%s", argv[++i]);
            comma = strchr(stages, ',');
            if (comma != NULL) { *comma = '\0'; sBenchRest = comma + 1; }
            setenv("SSB64_BENCH", stages, 1);
            snprintf(sBenchLabel, sizeof sBenchLabel, "stage %s", stages);
        } else if (strcmp(argv[i], "--intres") == 0 && i + 1 < argc) {
            setenv("SSB64_INTRES", argv[++i], 1);
        } else if (strcmp(argv[i], "--append-log") == 0) {
            setenv("SSB64_LOG_APPEND", "1", 1);
        }
    }
}

static void bench_next(char **argv)
{
    char *args[8];
    int n = 0;
    args[n++] = argv[0];
    args[n++] = "--bench";
    args[n++] = (char *)sBenchRest;
    args[n++] = "--append-log";
    if (getenv("SSB64_INTRES")) { args[n++] = "--intres"; args[n++] = getenv("SSB64_INTRES"); }
    args[n] = NULL;
    execv("/proc/self/exe", args);
    execv(argv[0], args);
    port_log_always("SSB64: bench: exec failed\n");
}

/* Save-state test hooks (desktop and qemu; the handheld menu does this on the device), at the top of
 * loop iteration n = sFrameCount: SSB64_SAVE_AT=<n> saves slot 1, SSB64_LOAD_AT=<n> loads it,
 * SSB64_EXITSAVE_AT=<n> is EXIT GAME (the auto save, then quit), SSB64_POWER_AT=<n> is the OS's
 * power-off (SIGTERM: the auto save, then quit), SSB64_FIXUP_AT=<n> runs only the renderer reset a load
 * does (gfx_savestate_fixup), to tell a cache-history difference from a state one.
 * statefix (2026-10-09): SSB64_SAVEWRITE_AT=<n> is a game save write: the title screen's (the boot count
 * +1, then lbBackupWrite, the routine every record, unlock and option write ends in: gSCManagerBackupData,
 * whole, both copies, to sram.bin), so a headless run shows what the game's next write after a load puts on
 * the card (a write that changes nothing is skipped by hh_sram_write); SSB64_MENU_AT=<n> (device
 * build) presses MENU twice (gfx_sdl12.c), so the handheld menu opens and closes at once: its mute and
 * unmute, headless. */
static void savestate_test_hooks(void)
{
    static long sSaveAt = -2, sLoadAt, sExitSaveAt, sPowerAt, sFixupAt, sSaveWriteAt, sMenuAt;
    if (sSaveAt == -2) {
        const char *e;
        e = getenv("SSB64_SAVE_AT");     sSaveAt = e ? atol(e) : -1;
        e = getenv("SSB64_LOAD_AT");     sLoadAt = e ? atol(e) : -1;
        e = getenv("SSB64_EXITSAVE_AT"); sExitSaveAt = e ? atol(e) : -1;
        e = getenv("SSB64_POWER_AT");    sPowerAt = e ? atol(e) : -1;
        e = getenv("SSB64_FIXUP_AT");    sFixupAt = e ? atol(e) : -1;
        e = getenv("SSB64_SAVEWRITE_AT"); sSaveWriteAt = e ? atol(e) : -1;
        e = getenv("SSB64_MENU_AT");     sMenuAt = e ? atol(e) : -1;
    }
    if (sFrameCount == sSaveWriteAt) {
        extern void lbBackupPortTestWrite(void);
        port_log_always("SSB64: SSB64_SAVEWRITE_AT: the title's save write (boot count +1, lbBackupWrite)\n");
        lbBackupPortTestWrite();
    }
#if defined(HANDHELD)
    if (sFrameCount == sMenuAt) { extern void port_test_menu_press(void); port_test_menu_press(); }
#else
    (void)sMenuAt;
#endif
    if (sFrameCount == sFixupAt) { extern void gfx_savestate_fixup(void); gfx_savestate_fixup(); } /* a load's renderer reset alone */
    if (sFrameCount == sSaveAt) savestate_request_save(1);
    if (sFrameCount == sLoadAt) savestate_request_load(1);
    if (sFrameCount == sExitSaveAt) savestate_request_exit_save();
    if (sFrameCount == sPowerAt) raise(SIGTERM);
}

/* A pacing wait. Any sleep on the Miyoo Mini costs a whole kernel tick, 9.2-9.6 ms however short the request
 * (measured on the Mini Plus), so a frame's remainder under ~10 ms slept was a frame late. Only the part of a wait
 * above 12 ms is slept; the rest is a sched_yield loop on the monotonic clock, to the same deadline. On a kernel whose
 * sleeps are exact the timing is the same. SSB64_SPINWAIT=0: one sleep, as before. Returns the microseconds waited. */
extern int sched_yield(void); /* not <sched.h>: the game has a sched.h of its own on the include path */
static unsigned int spin_wait_us(unsigned int us) {
    static int spin = -1;
    struct timespec ts;
    unsigned long long t0, now, until;
    if (spin < 0) { const char *e = getenv("SSB64_SPINWAIT"); spin = !(e && e[0] == '0'); }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    until = t0 + (unsigned long long)us * 1000ull;
    for (now = t0; now < until;) {
        const unsigned long long left = until - now;
        if (!spin || left > 12000000ull) {
            const unsigned long long s = spin ? left - 12000000ull : left;
            struct timespec d = { (time_t)(s / 1000000000ull), (long)(s % 1000000000ull) };
            nanosleep(&d, NULL); /* a signal cutting it short: the loop goes on to the deadline */
        } else {
            sched_yield();
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    }
    return (unsigned int)((now - t0) / 1000u);
}

int main(int argc, char **argv)
{
    char err[512];
    const char *env;
    long exit_after = -1;       /* SSB64_FRAMES=<n>: quit after n ticks (tests) */
    int free_run;               /* SSB64_FREERUN=1: no pacing (headless tests) */
    extern void hh_game_priority(void), hh_game_priority_log(void);

    hh_game_priority();         /* first, before any thread or child: all inherit it (priority.c) */
    parse_args(argc, argv);
    savestate_parse_args(argc, argv); /* a relaunch: --loadStateFile <path> */
    if (sBenchLabel[0] != '\0' && getenv("SSB64_FRAMES") == NULL) exit_after = BENCH_TICKS;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if ((env = getenv("SSB64_FRAMES")) != NULL) exit_after = atol(env);
    { extern void port_clock_pick(void); port_clock_pick(); } /* SSB64_VDSOCLOCK: before any thread reads the clock */
    sRunStartUs = port_time_us();
    sRunAudioUs0 = gPortAudioUsTotal;
    free_run = getenv("SSB64_FREERUN") != NULL;
    /* SSB64_RENDER_EVERY=<n> (tests, with SSB64_FREERUN): only every nth tick's picture is drawn, the game itself
     * runs every tick, so a long qemu capture (the attract run against the N64) takes a fraction of the time */
    { const char *e = getenv("SSB64_RENDER_EVERY"); sRenderEvery = (e != NULL && atoi(e) > 1) ? atoi(e) : 1; }

    port_log_always("SSB64: Super Smash Bros., save dir %s\n", port_save_dir());
#if defined(HANDHELD)
    { /* the kit's save-state and power lines (stderr) go to console.log too */
        extern int port_log_get_fd(void);
        const int fd = port_log_get_fd();
        if (fd >= 0) dup2(fd, 2);
    }
#endif
    hh_game_priority_log();
#if defined(MIYOO)
    {   /* The Miyoo Mini's CPU clock (kit miyoo_clock.c): 1500 MHz on the Mini, 1800 on the Mini Plus by default,
         * before the first start's ROM read; SSB64_CPUCLOCK or cpuclock= in <data>/bench.cfg (MHz, 0 = leave it).
         * Put back at every exit (port_platform_quiesce, atexit). threads=1 in bench.cfg = SSB64_THREADS=1: the
         * renderer on one core (kit miyoo_worker.c reads the variable). */
        extern int miyoo_clock_boost_to(int mhz);
        miyoo_clock_boost_to(hh_cfg_switch("SSB64_CPUCLOCK", "cpuclock", -1));
        if (hh_cfg_switch("SSB64_THREADS", "threads", 0) == 1) setenv("SSB64_THREADS", "1", 1);
    }
#endif
    if (romdata_init(port_save_dir(), err, sizeof err) != 0) {
        port_log_always("SSB64: %s\n", err);
        return 1;
    }
    /* A .sra/.srm/.sav the player dropped into the data folder becomes sram.bin (sram.c), before the boot
     * resume can load a state (whose save then gives way to it: hh_game_save_reapply) and before the game
     * first reads its save. */
    { extern void hh_sram_import(void); hh_sram_import(); }
    gfx_init(&gfx_sdl, &gfx_soft_api, "Super Smash Bros.", false);
    hh_audio_init();
    /* Save states need the ROM slice at its fixed address: the restored game memory points into it. */
    if (!romdata_at_fixed_address()) port_log_always("SSB64: the ROM data is not at its fixed address: save states disabled\n");
    else savestate_boot_prompt(); /* an auto save on the card: RESUME GAME / NEW GAME (handheld) */

    /* The fork looks fighters up through a per-kind registry (BattleShip's
     * mod hook); seed it with the 27 vanilla kinds before the game starts. */
    port_fighter_seed_vanilla();

    port_coroutine_init_main();
    PortCoroutine *game = port_coroutine_create(game_coroutine_entry, NULL, 128 * 1024);
    if (game == NULL) return 1;
    port_coroutine_resume(game);   /* boot chain runs until every thread waits */
    port_log("SSB64: boot yielded, entering frame loop\n");

    pacer_config(sBenchLabel[0] != '\0');
    uint32_t deadline = port_time_us();
    while (!sExitRequested) {
        /* The iteration boundary: no coroutine runs (the game's threads are all suspended in
         * their pool stacks) and no display list is pending, so a save or load queued by the
         * menu, a power-off or the boot resume happens here, between two whole ticks. */
        if (romdata_at_fixed_address()) {
            savestate_test_hooks();
            savestate_check();
            if (sExitRequested) break; /* EXIT GAME's auto save is done */
        }
        if (sPacerRotate) pacer_set_mode((sFrameCount / PACER_ROTATE_TICKS) % 3);
        if (sAbOn) ab_window(sFrameCount / PACER_ROTATE_TICKS);
        sModeTicks[sPacerMode]++;
        run_tick();
        deadline += TICK_US;
        int32_t slack = (int32_t)(deadline - port_time_us());

        if (sPendingDL != NULL) {
            /* Late by more than half a tick: drop this frame's picture, keep
             * the game at full speed. Up to HH_MAX_DROPS in a row (device
             * round 5: allowing only one held the match at 60% speed, since
             * a 32 ms render needs two ticks of slack), so the screen still
             * updates at least every HH_MAX_DROPS + 1 ticks (15 fps floor). */
            static int sDroppedRun;
            if (!free_run && sPacerMode != 0) {
                if (pacer_should_draw()) { render_pending(); sSkipsInRow = 0; }
                else { sPendingDL = NULL; sDroppedFrames++; sRunDropped++; sSkipsInRow++; }
            } else if (!free_run && slack < -(int32_t)TICK_US / 2 && sDroppedRun < HH_MAX_DROPS) {
                sPendingDL = NULL;
                sDroppedFrames++;
                sRunDropped++;
                sDroppedRun++;
            } else if (free_run && sRenderEvery > 1 && sFrameCount % sRenderEvery != 0) {
                sPendingDL = NULL; /* SSB64_RENDER_EVERY: not drawn */
            } else {
                render_pending();
                sDroppedRun = 0;
            }
        }
        perf_report();
        if (exit_after >= 0 && sFrameCount >= exit_after) break;

        slack = (int32_t)(deadline - port_time_us());
        sSlotLateUs = 0;
        /* No tick's remainder is over a second (at most two ticks, after a sleep a signal cut
         * short). A longer "wait" is the 32-bit clock wrapping: a tick that took over 2^31 us
         * (35.8 min) makes deadline - now positive again, and the sleep would hold the last picture
         * for up to 35 more minutes (ocarina-of-time 1138e29, pm64 a4441db, where the handheld menu runs
         * inside a frame). Nothing blocks a Smash tick that long today (MENU quits; the handheld menu
         * comes with M7); it is resynced like a load. */
        if (free_run) {
            deadline = port_time_us();
        } else if (slack > 0 && slack <= 1000000) {
            (void)spin_wait_us((unsigned int)slack);
        } else if (slack > 0 || slack < -(int32_t)(TICK_US * 6)) {
            deadline = port_time_us();   /* hopelessly behind (a load), or the clock wrapped: resync */
        } else {
            sSlotLateUs = (uint32_t)-slack; /* the next tick starts this late (the pacer) */
        }
    }
    port_capture_flush();
    { extern void gfx_fix_probe_report(void); gfx_fix_probe_report(); } /* SSB64_PROBE=1 */
    port_log_always("SSB64: exit after %d ticks\n", sFrameCount);
    if (sBenchLabel[0] != '\0') {
        bench_summary(sBenchLabel);
        if (sBenchRest != NULL && !sExitRequested) {
#if defined(HANDHELD)
            /* Never re-exec on the device (round 15b). execv keeps this image's open framebuffer
             * console, tty and sound device but skips everything that releases them, and the
             * new process blocked in its device setup before its first frame: a black screen
             * until a hard reboot (the user, bench v12; v10's log stops the same way, after the
             * exec'd process's first line). One stage per launch on the device; the desktop
             * still runs the chain. */
            port_log_always("SSB64: bench: stages %s not run (one stage per launch on the device)\n", sBenchRest);
#else
            fflush(NULL);
            bench_next(argv);
#endif
        }
    }
    savestate_shutdown();
    port_log_always("SSB64: leaving for the launcher\n");
    port_exit(0);
}
