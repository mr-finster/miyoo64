/* hh_plat.c: platform glue shared by both targets -- save directory, debug knobs, and
 * (for now) placeholder task execution until the renderer and mixer are wired in. */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "hh_os.h"
#include "hh_hw.h"
#include "hh_plat.h"
#if defined(TARGET_MIYOO)
#include "gfx/port.h" /* PORT_PROF_ADD: the audio tasks' time (PERF aud) */
#endif
#include "savestate.h" /* SAVESTATE_EXCLUDE: host state a state load must not overwrite (port/kit) */

unsigned int hh_frames SAVESTATE_EXCLUDE;
unsigned int hh_vicount;         /* the last VI tick: GE_EXIT_AT's and GE_PADSCRIPT's clock */
static unsigned int g_exit_at SAVESTATE_EXCLUDE;   /* GE_EXIT_AT=<frames> */
static unsigned int g_gfx_tasks SAVESTATE_EXCLUDE, g_audio_tasks SAVESTATE_EXCLUDE;

const char *hh_last_error(void) { return strerror(errno); }

#if defined(TARGET_MIYOO)
/* GE_DATA (launch.sh sets it: miyoo/game.json "data"), else "<the executable's folder>/data" */
const char *hh_miyoo_home(void)
{
    static char home[512] SAVESTATE_EXCLUDE;
    if (home[0] == 0) {
        char exe[480];
        const char *e = getenv("GE_DATA");
        if (e != NULL && e[0] == '/') {
            snprintf(home, sizeof(home), "%s", e);
            return home;
        }
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        char *slash;
        exe[n > 0 ? n : 0] = 0;
        slash = strrchr(exe, '/');
        if (n <= 0 || slash == NULL)
            snprintf(home, sizeof(home), "/mnt/SDCARD/App/GoldenEye/data");
        else {
            *slash = 0;
            snprintf(home, sizeof(home), "%s/data", exe);
        }
    }
    return home;
}
#endif

/* This process's save folder (GE_SAVE_DIR, else the card's), never game state: a load from a state
 * written with another GE_SAVE_DIR (desktop tests) handed this process that folder, and every later
 * save went there (the Star Fox 64 port 626adc7's sSaveDir; the 2026-10-01 consistency pass). */
const char *hh_plat_save_dir(void)
{
    static char dir[512] SAVESTATE_EXCLUDE;
    if (dir[0] == 0) {
        const char *e = getenv("GE_SAVE_DIR");
        snprintf(dir, sizeof(dir), "%s", e ? e : HH_DEFAULT_SAVE_DIR);
        mkdir(dir, 0755);
    }
    return dir;
}

void hh_gfx_init(void);
void hh_gfx_run_task(OSTask *task);

void hh_plat_init(void)
{
    const char *e = getenv("GE_EXIT_AT");
    g_exit_at = e ? (unsigned)strtoul(e, NULL, 0) : 0;
    hh_gfx_init();
}

void hh_plat_rsp_task(OSTask *task)
{
    if (task->t.type == M_GFXTASK) {
        g_gfx_tasks++;
        hh_gfx_run_task(task);
    } else if (task->t.type == M_AUDTASK) {
        /* the RSP audio microcode's list, run on the CPU (hh/audio/acmd_mixer.c, DKR's) */
        extern void port_audio_run_cmds(const void *cmdList, unsigned int sizeBytes);
        uintptr_t p = (uintptr_t)task->t.data_ptr;
        if (p != 0 && p < 0x20000000u)
            p |= 0x80000000u;
        g_audio_tasks++;
#if defined(TARGET_MIYOO)
        { /* its time, for the PERF line's aud (the Mini's frame outside the render) */
            extern uint32_t port_time_us(void);
            const uint32_t t0 = port_time_us();
            if (p != 0)
                port_audio_run_cmds((const void *)p, task->t.data_size);
            PORT_PROF_ADD(PROF_AUDIO, port_time_us() - t0);
        }
#else
        if (p != 0)
            port_audio_run_cmds((const void *)p, task->t.data_size);
#endif
    }
}

static unsigned int sLastFrameVi SAVESTATE_EXCLUDE; /* the retrace of the last new frame */

/* After a state load (hh_os_state_loaded): the VI count is the state's, sLastFrameVi this session's. A
 * resume at start-up loads a state from thousands of retraces later than the few frames of the fresh
 * boot, and the first retrace after it read as "no new frame since vi 51" with a thread dump (device log
 * 2026-10-01: the game ran on at full rate). Disarmed (0) until the next frame. */
void hh_plat_state_loaded(void)
{
    sLastFrameVi = 0;
}

void hh_plat_vi(u32 vicount, int new_frame)
{
    hh_vicount = vicount;
    static unsigned int sStallDumped SAVESTATE_EXCLUDE;
    if (vicount < sLastFrameVi) sLastFrameVi = vicount; /* a state load took the VI count back */
    if (new_frame) {
        extern int hh_frame_boundary;
        hh_frames++;
        hh_frame_boundary = 1; /* hh_os_run: the save-state check runs before the next event */
        sLastFrameVi = vicount;
        sStallDumped = 0;
    } else if (!sStallDumped && sLastFrameVi != 0 && vicount - sLastFrameVi >= 600) {
        /* Stall report (round 25, a Cradle save left the game running without frames: music on,
         * controls dead): no new picture for 10 s of game time -- every thread's state and the
         * queue it waits on, once, into the log. */
        extern void hh_os_dump(const char *why);
        sStallDumped = 1;
        printf("[plat] no new frame since vi %u (vi %u, frames %u, gfx tasks %u, audio tasks %u)\n", sLastFrameVi,
               vicount, hh_frames, g_gfx_tasks, g_audio_tasks);
        fflush(stdout);
        hh_os_dump("stall");
        fflush(stderr);
    }
    if (vicount % 60 == 0 && (getenv("GE_LOGVERBOSE") != NULL || getenv("GE_BENCH_CHILD") != NULL)) /* per-second chatter: Bench, or GE_LOGVERBOSE=1 (debug.cfg logverbose=1) */
    {
        extern u64 hh_ai_samples;
        printf("[plat] vi %u: frames %u, gfx tasks %u, audio tasks %u, ai samples %llu%s\n", vicount, hh_frames,
               g_gfx_tasks, g_audio_tasks, (unsigned long long)hh_ai_samples, hh_vi_black ? " (black)" : "");
    }
    if (g_exit_at && vicount >= g_exit_at) {
        extern void hh_bench_report(void);
        extern int gfx_wm_shutdown(void) __attribute__((weak)); /* gfx_sdl12.c (the device) */
        hh_bench_report();
        if (getenv("GE_BENCH_CHILD") != NULL && gfx_wm_shutdown != NULL) {
            fflush(stdout);
            gfx_wm_shutdown(); /* the next bench run gets a clean screen */
        }
        printf("[plat] GE_EXIT_AT %u reached\n", g_exit_at);
        fflush(stdout);
        exit(0);
    }
}

/* The loop note, read from another thread (the watchdog): where the kernel loop
 * is (hh_os.c), the game thread it runs (-1 = the kernel), the retrace, the frames shown and the
 * retrace of the last, the gfx and audio tasks run, and the level. The count is the kernel loop's
 * turns (0 since the signal: a game thread that makes no OS call, or the kernel stuck in a task or a
 * flip; more: the loop turns but no new frame comes). No handheld_log_fd here: stderr is console.log
 * (hh_card.c), where the line lands as it is. */
unsigned handheld_loop_detail(char *buf, size_t n)
{
    static const char *const kWhere[] = { "boot", "game thread", "save state check", "drawing", "audio task",
                                          "retrace and timers", "pacer sleep" };
    extern s32 g_StageNum;
    int where = 0, thread = -1;
    const u32 turns = hh_os_where(&where, &thread);
    if (n > 0)
        snprintf(buf, n, "stage %s, game thread %d, vi %u, frames %u (the last at vi %u), gfx tasks %u, audio tasks %u, level %d",
                 kWhere[where >= HH_WHERE_BOOT && where <= HH_WHERE_SLEEP ? where : HH_WHERE_BOOT], thread, hh_vicount,
                 hh_frames, sLastFrameVi, g_gfx_tasks, g_audio_tasks, (int)g_StageNum);
    return turns;
}

void hh_plat_audio_out(const void *pcm, u32 len, u32 rate)
{
    extern void hh_audio_push(const void *pcm, u32 bytes, u32 rate);
    hh_audio_push(pcm, len, rate);
}

/* boss.c (PORT): minimum time from one frame's start to the next, in count units --
 * (n - 1) retraces + the N64's own half-retrace margin (387937). 30 fps unless GE_FPS=60. */
u32 hhMainLoopTickInterval = 781250u + 387937u;

__attribute__((constructor)) static void hh_plat_fps_knob(void)
{
    const char *e = getenv("GE_FPS");
    if (e != NULL && atoi(e) >= 60)
        hhMainLoopTickInterval = 387937u;
    e = getenv("GE_FRAMEVIS"); /* tests: a frame every n retraces (n >= 2) --
                                 * the deltas of a slow device, deterministic under GE_NOPACE */
    if (e != NULL && atoi(e) >= 2)
        hhMainLoopTickInterval = (u32)(atoi(e) - 1) * 781250u + 387937u;
}

#if defined(TARGET_MIYOO)
/* The Miyoo Mini's minimum frame step (GE_MINSTEP, debug.cfg minstep=1|2, default 2; read on first use, after
 * debug.cfg is in the environment). The 30 fps cap is the port's, not the N64's: GoldenEye's own threshold is half a retrace (boss.c,
 * 387937), so on the console a frame done inside one retrace steps 1 (the logos at 60), and the game code steps by
 * time (g_ClockTimer, g_GlobalTimerDelta, loops of one retrace per step, the half-frame counters). 2: a frame waits
 * at least two retraces (the handheld's cap, 30 fps at most). 1: in a mission a frame starts on the first retrace
 * after the last one is done -- steps 1, 2, 1, 2 ... at 1.5 retraces of work a frame, adding up to the real time.
 * The title stage keeps 2 (the gun barrel's walk and the menus step per frame: round 18; the logos and the dots
 * are 60 either way, hhFront60). GE_FPS / GE_FRAMEVIS (tests) still decide. */
int hh_minstep(void)
{
    static int step SAVESTATE_EXCLUDE;
    if (step == 0) {
        const char *e = getenv("GE_MINSTEP");
        step = (e && e[0] == '1') ? 1 : 2; /* default 2 (the user, 2026-10-11: 1 felt uneven on the device) */
        if (getenv("GE_FPS") || getenv("GE_FRAMEVIS"))
            step = 2; /* the tests' own interval (hhMainLoopTickInterval) */
        fprintf(stderr, "minstep: %d (debug.cfg minstep=%d: %s)\n", step, 3 - step,
                step == 1 ? "a frame every second retrace at most, 30 fps" : "a frame on every retrace one is ready for");
    }
    return step;
}
#endif

/* The fractional frame step (GE_FRACSTEP=1, debug.cfg fracstep=1; default off, opt-in for testing). The game's step is
 * whole retraces (g_ClockTimer: the retraces osGetCount crossed since the last frame, rounded at half a retrace), so
 * frames that take 2.4 retraces of real time step 2, 2, 3, 2, 3 and motion judders though the frames come evenly; with
 * minstep=1 it is 1, 2, 1, 2. With this on, the motion step (g_GlobalTimerDelta: Bond's walk and turn, the camera, guns,
 * props, the float timers -- the EU build already runs it at 1.2x the count, so a non-whole step is one the game knows)
 * is the real time since the last frame, in retraces, read off the host clock. The whole-retrace count (g_ClockTimer,
 * g_GlobalTimer: the AI's frame counters, the 30 Hz tick, sound) is untouched. The fractional total is held within one
 * retrace of the whole one (and resynced on a jump: a pause, a state load, a long stall), so the two never drift apart
 * and game time still equals real time. A step of 0 (paused, controls locked) stays 0. Unpaced (GE_NOPACE): off. */
f32 hhFracStep(s32 clk)
{
    static int on SAVESTATE_EXCLUDE = -1, have SAVESTATE_EXCLUDE, logn SAVESTATE_EXCLUDE;
    static double base SAVESTATE_EXCLUDE, sum SAVESTATE_EXCLUDE, isum SAVESTATE_EXCLUDE;
    static double lsum SAVESTATE_EXCLUDE, lisum SAVESTATE_EXCLUDE, lreal SAVESTATE_EXCLUDE;
    static unsigned lframes SAVESTATE_EXCLUDE, lresync SAVESTATE_EXCLUDE;
    static double lwall0 SAVESTATE_EXCLUDE;
    extern double hh_os_real_retraces(void);
    double r, raw, d, lo, hi;
    if (on < 0) {
        const char *e = getenv("GE_FRACSTEP");
        on = (e && e[0] == '1') && hh_os_real_retraces() >= 0.0;
        logn = getenv("GE_FRACLOG") != NULL;
        fprintf(stderr, "fracstep: %s (debug.cfg fracstep=1: the motion step follows the real frame time)\n", on ? "on" : "off");
    }
    if (!on)
        return (f32)clk;
    if (clk <= 0) { /* paused or locked: no motion, and the next step starts the timeline again */
        have = 0;
        return 0.0f;
    }
    r = hh_os_real_retraces();
    if (!have) {
        have = 1;
        base = r - (double)clk;
        sum = isum = 0.0;
    }
    isum += (double)clk;
    raw = r - base;
    if (raw - isum > 3.0 || isum - raw > 3.0) { /* the clock base moved (pause, state load) or a long stall: resync */
        base = r - isum;
        raw = isum;
        lresync++;
    }
    if (raw < isum - 1.0) raw = isum - 1.0;
    if (raw > isum + 1.0) raw = isum + 1.0;
    d = raw - sum;
    lo = (double)clk - 1.0;
    if (lo < 0.25) lo = 0.25;
    hi = (double)clk + 1.0;
    if (d < lo) d = lo;
    if (d > hi) d = hi;
    sum += d;
    if (isum > 1048576.0) { /* keep the doubles small: the same offsets, rebased */
        base += isum;
        sum -= isum;
        isum = 0.0;
    }
    if (logn) { /* GE_FRACLOG=1: every 300 frames, the fractional and whole game time against the wall clock */
        struct timespec ts;
        double wall;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wall = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
        if (lframes == 0)
            lwall0 = wall;
        lsum += d;
        lisum += (double)clk;
        lreal = r;
        if (++lframes % 300 == 0)
            fprintf(stderr, "fracstep: %u frames, wall %.2f s = %.1f retraces; game time frac %.1f whole %.0f (%+.2f); "
                            "resyncs %u; this step %.3f of %d; host clock %.1f\n",
                    lframes, wall - lwall0, (wall - lwall0) * 60.0, lsum, lisum, lsum - lisum, lresync, d, clk, lreal);
    }
    return (f32)d;
}

/* The cap per retrace (boss.c): 60 on the intro screens the N64 draws at 60 (front.c hhFront60, round 27:
 * the logos and the barrel's dots move twice as smoothly, as on the console), hhMainLoopTickInterval
 * elsewhere. GE_INTRO60=0: 30 everywhere. Miyoo: minstep=1 lifts it in the missions. */
u32 hh_main_loop_tick(void)
{
    extern int hhFront60(void);
    static int sIntro60 SAVESTATE_EXCLUDE = -1;
    if (sIntro60 < 0) { const char *e = getenv("GE_INTRO60"); sIntro60 = !(e && e[0] == '0'); }
#if defined(TARGET_MIYOO)
    extern s32 bossGetStageNum(void);
    if (hh_minstep() == 1 && bossGetStageNum() != 90 /* LEVELID_TITLE */)
        return 387937u;
#endif
    return (sIntro60 && hhFront60()) ? 387937u : hhMainLoopTickInterval;
}

/* hh_os_run, once a frame with the game threads parked: the kit's save/load/the power-off resume requests. */
#if defined(HANDHELD)
#include <pthread.h>
#include <SDL/SDL.h>
/* tests: GE_QMENU_AT=<vi> presses keys into the kit menu the way the player does: MENU, down to SAVE,
 * A (slot 1, empty) -- GE_QMENU_KEYS overrides the sequence ("q d a": SDL key letters, ~0.4 s apart,
 * the down scroll given 1.5 s). The menu runs inside the frame's present, as on the device. */
static void qmenu_key(SDLKey k, int down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type = ev.type;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.sym = k;
    SDL_PushEvent(&ev);
}
static void *qmenu_thread(void *arg)
{
    const char *keys = (const char *)arg, *c;
    for (c = keys; *c; c++) {
        if (*c == ' ')
            continue;
        qmenu_key((SDLKey)*c, 1);
        usleep(50000);
        qmenu_key((SDLKey)*c, 0);
        usleep(*c == 'd' || *c == 'u' ? 1500000 : 400000);
    }
    return NULL;
}
#endif

/* The intro logos' pace (front.c, title.c): 1 = the N64's (a step per retrace), GE_LOGOPACE=0: a step a frame */
s32 hh_logo_pace(void)
{
    static int sPace SAVESTATE_EXCLUDE = -1;
    if (sPace < 0) { const char *e = getenv("GE_LOGOPACE"); sPace = !(e && e[0] == '0'); }
    return sPace;
}

void hh_frame_hook(void)
{
#if defined(HANDHELD)
    {
        static int sQm SAVESTATE_EXCLUDE = -1;
        if (sQm < 0) sQm = getenv("GE_QMENU_AT") ? atoi(getenv("GE_QMENU_AT")) : 0;
        if (sQm > 0 && hh_vicount >= (unsigned)sQm) {
            pthread_t t;
            const char *k = getenv("GE_QMENU_KEYS");
            printf("[plat] test menu keys at vi %u\n", hh_vicount);
            sQm = 0;
            pthread_create(&t, NULL, qmenu_thread, (void *)(k ? k : "qda"));
            pthread_detach(t);
        }
    }
#endif
    extern void savestate_check(void);
    extern void savestate_request_save(int slot), savestate_request_load(int slot);
    /* tests: GE_QSAVE_AT=<vi>[:<slot>][,<vi>[:<slot>]...] saves (slot 1 by default) at the first frame at or
     * after each VI, GE_QLOAD_AT the same for loads -- e.g. a state written by one process and read by a
     * fresh one, or save / load / save again in one session */
    static char sQsList[128] SAVESTATE_EXCLUDE, sQlList[128] SAVESTATE_EXCLUDE;
    static int sQInit SAVESTATE_EXCLUDE;
    if (!sQInit) {
        const char *e = getenv("GE_QSAVE_AT"), *l = getenv("GE_QLOAD_AT");
        sQInit = 1;
        snprintf(sQsList, sizeof(sQsList), "%s", e ? e : "");
        snprintf(sQlList, sizeof(sQlList), "%s", l ? l : "");
    }
    {
        char *lists[2] = { sQsList, sQlList };
        int k;
        for (k = 0; k < 2; k++) {
            char *p = lists[k];
            if (p[0] && hh_vicount >= (unsigned)atoi(p)) {
                const char *c = strchr(p, ':'), *comma = strchr(p, ',');
                const int slot = (c && (!comma || c < comma)) ? atoi(c + 1) : 1;
                printf("[plat] test %s, slot %d, vi %u\n", k ? "load" : "save", slot, hh_vicount);
                if (k) savestate_request_load(slot); else savestate_request_save(slot);
                memmove(p, comma ? comma + 1 : p + strlen(p), strlen(comma ? comma + 1 : "") + 1);
                break; /* one request a frame */
            }
        }
    }
    {   /* a save's (or load's) time on the card is not game time: no catch-up burst after it */
        extern void hh_os_pause_begin(void), hh_os_pause_end(void);
        /* statefix: never in the middle of a game save write (file2.c hh_game_save_busy: the game thread
         * waits on the controller thread inside one, and a frame can be shown meanwhile). The menu, a save,
         * a load or the power-off resume waits for the next frame after it, a few retraces. GE_SAVEWAIT=0: as before. */
        extern s32 hh_game_save_busy;
        static int sWait SAVESTATE_EXCLUDE = -1, sWaiting SAVESTATE_EXCLUDE;
        if (sWait < 0) sWait = !(getenv("GE_SAVEWAIT") && getenv("GE_SAVEWAIT")[0] == '0');
        if (sWait && hh_game_save_busy) {
            if (!sWaiting) fprintf(stderr, "[plat] vi %u: save states wait, the game is writing its save\n", hh_vicount);
            sWaiting = 1;
        } else {
            sWaiting = 0;
            hh_os_pause_begin();
            savestate_check();
            hh_os_pause_end();
        }
    }
    { /* tests: GE_SLOWMS=<ms> adds this much host time to every frame (a slow device, paced) */
        static int sSlow SAVESTATE_EXCLUDE = -1;
        if (sSlow < 0) sSlow = getenv("GE_SLOWMS") ? atoi(getenv("GE_SLOWMS")) : 0;
        if (sSlow > 0) usleep((useconds_t)sSlow * 1000u);
    }
    { /* tests: GE_QSTALL_MS=<ms> stalls after each test request, as a slow SD card's save does */
        static int sStall SAVESTATE_EXCLUDE = -1;
        static unsigned sDone SAVESTATE_EXCLUDE;
        if (sStall < 0) sStall = getenv("GE_QSTALL_MS") ? atoi(getenv("GE_QSTALL_MS")) : 0;
        if (sStall > 0 && strlen(sQsList) + strlen(sQlList) != sDone) {
            sDone = (unsigned)(strlen(sQsList) + strlen(sQlList));
            printf("[plat] test stall %d ms\n", sStall);
            usleep((useconds_t)sStall * 1000u);
        }
    }
}
