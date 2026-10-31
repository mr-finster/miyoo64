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
s32 hh_game_save_busy SAVESTATE_EXCLUDE; /* set while the game writes its EEPROM save (hh_hw.c) */
unsigned int hh_vicount;         /* the last VI tick: BK_EXIT_AT's and BK_PADSCRIPT's clock */
static unsigned int g_exit_at SAVESTATE_EXCLUDE;   /* BK_EXIT_AT=<frames> */
static unsigned int g_gfx_tasks SAVESTATE_EXCLUDE, g_audio_tasks SAVESTATE_EXCLUDE;

const char *hh_last_error(void) { return strerror(errno); }

#if defined(TARGET_MIYOO)
/* BK_DATA (launch.sh sets it: miyoo/game.json "data"), else "<the executable's folder>/data" */
const char *hh_miyoo_home(void)
{
    static char home[512] SAVESTATE_EXCLUDE;
    if (home[0] == 0) {
        char exe[480];
        const char *e = getenv("BK_DATA");
        if (e != NULL && e[0] == '/') {
            snprintf(home, sizeof(home), "%s", e);
            return home;
        }
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        char *slash;
        exe[n > 0 ? n : 0] = 0;
        slash = strrchr(exe, '/');
        if (n <= 0 || slash == NULL)
            snprintf(home, sizeof(home), "/mnt/SDCARD/App/BanjoKazooie/data");
        else {
            *slash = 0;
            snprintf(home, sizeof(home), "%s/data", exe);
        }
    }
    return home;
}
#endif

/* This process's save folder (BK_SAVE_DIR, else the card's), never game state: a load from a state
 * written with another BK_SAVE_DIR (desktop tests) handed this process that folder, and every later
 * save went there (the Star Fox 64 port 626adc7's sSaveDir; the 2026-10-01 consistency pass). */
const char *hh_plat_save_dir(void)
{
    static char dir[512] SAVESTATE_EXCLUDE;
    if (dir[0] == 0) {
        const char *e = getenv("BK_SAVE_DIR");
        snprintf(dir, sizeof(dir), "%s", e ? e : HH_DEFAULT_SAVE_DIR);
        mkdir(dir, 0755);
    }
    return dir;
}

void hh_gfx_init(void);
void hh_gfx_run_task(OSTask *task);

void hh_plat_init(void)
{
    const char *e = getenv("BK_EXIT_AT");
    g_exit_at = e ? (unsigned)strtoul(e, NULL, 0) : 0;
    hh_gfx_init();
}

void hh_plat_rsp_task(OSTask *task)
{
    if (task->t.type == M_GFXTASK) {
        g_gfx_tasks++;
        hh_gfx_run_task(task);
    } else if (task->t.type == M_AUDTASK) {
        /* the n_audio microcode's list (n_aspMain), run on the CPU (hh/audio/hh_aspmain.c) */
        extern void hh_aspmain_run(const uint32_t *cmd, int count);
        uintptr_t p = (uintptr_t)task->t.data_ptr;
        if (p != 0 && p < 0x20000000u)
            p |= 0x80000000u;
        g_audio_tasks++;
#if defined(TARGET_MIYOO)
        { /* its time, for the PERF line's aud (the Mini's frame outside the render) */
            extern uint32_t port_time_us(void);
            const uint32_t t0 = port_time_us();
            if (p != 0)
                hh_aspmain_run((const uint32_t *)p, (int)(task->t.data_size / 8));
            PORT_PROF_ADD(PROF_AUDIO, port_time_us() - t0);
        }
#else
        if (p != 0)
            hh_aspmain_run((const uint32_t *)p, (int)(task->t.data_size / 8));
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
    if (vicount % 60 == 0 && (getenv("BK_LOGVERBOSE") != NULL || getenv("BK_BENCH_CHILD") != NULL)) /* per-second chatter: Bench, or BK_LOGVERBOSE=1 (debug.cfg logverbose=1) */
    {
        extern u64 hh_ai_samples;
        printf("[plat] vi %u: frames %u, gfx tasks %u, audio tasks %u, ai samples %llu%s\n", vicount, hh_frames,
               g_gfx_tasks, g_audio_tasks, (unsigned long long)hh_ai_samples, hh_vi_black ? " (black)" : "");
    }
    if (g_exit_at && vicount >= g_exit_at) {
        extern void hh_bench_report(void);
        extern int gfx_wm_shutdown(void) __attribute__((weak)); /* gfx_sdl12.c (the device) */
        hh_bench_report();
        if (getenv("BK_BENCH_CHILD") != NULL && gfx_wm_shutdown != NULL) {
            fflush(stdout);
            gfx_wm_shutdown(); /* the next bench run gets a clean screen */
        }
        printf("[plat] BK_EXIT_AT %u reached\n", g_exit_at);
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
    extern s32 hh_bk_map(void);
    int where = 0, thread = -1;
    const u32 turns = hh_os_where(&where, &thread);
    if (n > 0)
        snprintf(buf, n, "stage %s, game thread %d, vi %u, frames %u (the last at vi %u), gfx tasks %u, audio tasks %u, map %d",
                 kWhere[where >= HH_WHERE_BOOT && where <= HH_WHERE_SLEEP ? where : HH_WHERE_BOOT], thread, hh_vicount,
                 hh_frames, sLastFrameVi, g_gfx_tasks, g_audio_tasks, (int)hh_bk_map());
    return turns;
}

void hh_plat_audio_out(const void *pcm, u32 len, u32 rate)
{
    extern void hh_audio_push(const void *pcm, u32 bytes, u32 rate);
    hh_audio_push(pcm, len, rate);
}

#if defined(TARGET_MIYOO)
int hh_minstep(void) { return 1; }
#endif

/* hh_os_run, once a frame with the game threads parked: the kit's save/load/the power-off resume requests. */
#if defined(TARGET_MIYOO)
#include <pthread.h>
#include <SDL/SDL.h>
/* tests: BK_QMENU_AT=<vi> presses keys into the kit menu the way the player does: MENU, down to SAVE,
 * A (slot 1, empty) -- BK_QMENU_KEYS overrides the sequence ("q d a": q MENU, u d l r the d-pad, a b x y the buttons, s START; ~0.4 s apart,
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
        SDLKey k;
        switch (*c) { /* the letters: the Miyoo's buttons */
        case 'q': k = SDLK_ESCAPE; break;   /* MENU */
        case 'u': k = SDLK_UP; break;
        case 'd': k = SDLK_DOWN; break;
        case 'l': k = SDLK_LEFT; break;
        case 'r': k = SDLK_RIGHT; break;
        case 'a': k = SDLK_SPACE; break;    /* A */
        case 'b': k = SDLK_LCTRL; break;    /* B */
        case 's': k = SDLK_RETURN; break;   /* START */
        case 'x': k = SDLK_LSHIFT; break;   /* X */
        case 'y': k = SDLK_LALT; break;     /* Y */
        default: k = (SDLKey)*c; break;
        }
        qmenu_key(k, 1);
        usleep(50000);
        qmenu_key(k, 0);
        usleep(*c == 'd' || *c == 'u' ? 1500000 : 400000);
    }
    return NULL;
}
#endif

void hh_frame_hook(void)
{
    { extern void hh_heap_check(void); hh_heap_check(); }
#if defined(TARGET_MIYOO)
    {
        static int sQm SAVESTATE_EXCLUDE = -1;
        if (sQm < 0) sQm = getenv("BK_QMENU_AT") ? atoi(getenv("BK_QMENU_AT")) : 0;
        if (sQm > 0 && hh_vicount >= (unsigned)sQm) {
            pthread_t t;
            const char *k = getenv("BK_QMENU_KEYS");
            printf("[plat] test menu keys at vi %u\n", hh_vicount);
            sQm = 0;
            pthread_create(&t, NULL, qmenu_thread, (void *)(k ? k : "qda"));
            pthread_detach(t);
        }
    }
#endif
    extern void savestate_check(void);
    extern void savestate_request_save(int slot), savestate_request_load(int slot);
    /* tests: BK_QSAVE_AT=<vi>[:<slot>][,<vi>[:<slot>]...] saves (slot 1 by default) at the first frame at or
     * after each VI, BK_QLOAD_AT the same for loads -- e.g. a state written by one process and read by a
     * fresh one, or save / load / save again in one session */
    static char sQsList[128] SAVESTATE_EXCLUDE, sQlList[128] SAVESTATE_EXCLUDE;
    static int sQInit SAVESTATE_EXCLUDE;
    if (!sQInit) {
        const char *e = getenv("BK_QSAVE_AT"), *l = getenv("BK_QLOAD_AT");
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
         * a load or the power-off resume waits for the next frame after it, a few retraces. BK_SAVEWAIT=0: as before. */
        extern s32 hh_game_save_busy;
        static int sWait SAVESTATE_EXCLUDE = -1, sWaiting SAVESTATE_EXCLUDE;
        if (sWait < 0) sWait = !(getenv("BK_SAVEWAIT") && getenv("BK_SAVEWAIT")[0] == '0');
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
    { /* tests: BK_SLOWMS=<ms> adds this much host time to every frame (a slow device, paced) */
        static int sSlow SAVESTATE_EXCLUDE = -1;
        if (sSlow < 0) sSlow = getenv("BK_SLOWMS") ? atoi(getenv("BK_SLOWMS")) : 0;
        if (sSlow > 0) usleep((useconds_t)sSlow * 1000u);
    }
    { /* tests: BK_QSTALL_MS=<ms> stalls after each test request, as a slow SD card's save does */
        static int sStall SAVESTATE_EXCLUDE = -1;
        static unsigned sDone SAVESTATE_EXCLUDE;
        if (sStall < 0) sStall = getenv("BK_QSTALL_MS") ? atoi(getenv("BK_QSTALL_MS")) : 0;
        if (sStall > 0 && strlen(sQsList) + strlen(sQlList) != sDone) {
            sDone = (unsigned)(strlen(sQsList) + strlen(sQlList));
            printf("[plat] test stall %d ms\n", sStall);
            usleep((useconds_t)sStall * 1000u);
        }
    }
}
