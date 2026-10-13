/**
 * Miyoo Mini window backend: SDL 1.2, the game's output size (320x240, or the square size with
 * screen= in the settings file) scaled to the panel by miyoo_video.c, 16bpp, triple buffering.
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format. A dynamic-resolution ladder (RES_LADDER)
 *   renders smaller under load and NN-upscales in the same pass.
 * - Pacing: 30 fps via SDL_GetTicks, up to FRAMESKIP_MAX skipped renders in
 *   a row when behind (logic always runs).
 * - MENU (ESCAPE) opens the modal overlay in handheld_menu.c.
 * - Power: the OS's SIGTERM at power-off (or any other kill) takes the EXIT GAME path -- auto save,
 *   then quit -- so the next start offers RESUME GAME.
 */
#if defined(HANDHELD)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
extern int sched_yield(void); /* not <sched.h>: the game has a sched.h of its own on the include path */
#include <SDL/SDL.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <poll.h>
#include <time.h>

#include "gfx_window_manager_api.h"
#include "handheld_menu.h"
#include "handheld_screen.h"
#include "miyoo_video.h"
#if defined(MIYOO)
#include "miyoo_worker.h"
#include "miyoo_watchdog.h"
#include "miyoo_clock.h"
#endif
#include "savestate.h"
#include "../port.h"
extern u32 port_time_us(void);

/* Mirror of gfx_pc.h's GfxDimensions (that header drags in Gfx/ultra types).
 * gfx_pc reads it only at init; the res ladder pokes it directly. */
struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
extern struct GfxDimensions gfx_current_dimensions;

/* The output size: 320x240, or the square size with screen= in the settings file
 * (handheld_screen.c). Set once in gfx_sdl12_init, before anything draws. */
static int sScrW SAVESTATE_EXCLUDE = 240, sScrH SAVESTATE_EXCLUDE = 240;
#define SCREEN_W sScrW
#define SCREEN_H sScrH
#define SCREEN_MAX_W 320
#define SCREEN_MAX_H 240
#define FRAMERATE 30
#define FRAME_TIME_MS (1000 / FRAMERATE)
/* Max consecutive skipped renders. 1 (not 2): when the device can't hold
 * 30 fps, render-skip-render-skip is a steady 15 fps; allowing 2 skips
 * makes 3-frame stutter bursts that read as "choppy". */
#define FRAMESKIP_MAX 1

#ifndef SDL_TRIPLEBUF
#define SDL_TRIPLEBUF SDL_DOUBLEBUF
#endif

extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
extern void port_request_exit(void);
extern void controller_handheld_key(int sym, int down);

static SDL_Surface *sScreen SAVESTATE_EXCLUDE; /* live SDL video surface */
static void present_acquire(void);
static void present_release(void);
static int present_thread(void *arg);
#if defined(MIYOO)
static volatile int sPinPresent SAVESTATE_EXCLUDE; /* the worker is on: the present thread moves to core 1 */
static int sWdPresent SAVESTATE_EXCLUDE;           /* the present thread has registered with the watchdog */
#endif
static uint16_t sStage[SCREEN_MAX_W * SCREEN_MAX_H] SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* Miyoo: the frame as rendered, 32 bits, which the present thread doubles and turns straight onto
 * the panel (miyoo_draw32): no RGB565 stage, so no banding and less work (GoldenEye's present).
 * sStage32W 0: this frame went the 565 way (a panel miyoo_draw32 can't take). */
static uint32_t sStage32[SCREEN_MAX_W * SCREEN_MAX_H] SAVESTATE_EXCLUDE;
static int sStage32W SAVESTATE_EXCLUDE, sStage32H SAVESTATE_EXCLUDE;
#endif
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;

static bool sDoRender SAVESTATE_EXCLUDE = true;
static int sSkipsInARow SAVESTATE_EXCLUDE;
static uint32_t sLastFrameMs SAVESTATE_EXCLUDE; /* pacing: wall clock, never restore */
static volatile sig_atomic_t sResumeSave SAVESTATE_EXCLUDE;

/* Display modes (handheld menu "DISPLAY"):
 *  CONVERT (default): render at the output size -- 3D FOV adapts via aspect
 *      ratio, 2D/HUD maps onto the output width, nothing clipped.
 *  STRETCH: render internal 320x240 (N64 aspect), squash to 240 on present.
 *  CROP: render 320x240, present the centered 240 columns 1:1 -- authentic
 *      proportions, HUD edges clipped (the SM64 port's approach). */
int gHandheldDisplayMode SAVESTATE_EXCLUDE = HH_DISPLAY_CONVERT; /* a setting, not game state */

/* Dynamic resolution ladder as a percentage of the base render size
 * (index 0 = native). Only the 3D (perspective) pass renders at this size;
 * 2D (HUD, text, menus) always renders at the base size and the backend
 * composites the two (gfx_soft.c). MK64_DYNRES=0 disables the ladder. */
static const int sResPct[] = { 100, 95, 90, 85, 80, 75, 70, 65, 60 };
#define RES_STEPS ((int)(sizeof(sResPct) / sizeof(sResPct[0])))
static int sResIdx SAVESTATE_EXCLUDE;     /* current ladder position */
static int sResEnabled SAVESTATE_EXCLUDE; /* MK64_DYNRES=0 disables the ladder */

extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

static void apply_res(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= RES_STEPS) idx = RES_STEPS - 1;
    sResIdx = idx;
    /* 320x240 screen: the N64's own size, presented 1:1 whatever the display mode says */
    const int base_w = (SCREEN_W == 320 || gHandheldDisplayMode != HH_DISPLAY_CONVERT) ? 320 : 240;
    const int base_h = 240;
    const int int_w = (base_w * sResPct[idx] / 100) & ~1;
    const int int_h = (base_h * sResPct[idx] / 100) & ~1;
    /* gfx_pc's dimensions are the NATIVE size; the backend swaps them to the
     * internal size for the duration of each 3D batch. */
    gfx_current_dimensions.width = (uint32_t)base_w;
    gfx_current_dimensions.height = (uint32_t)base_h;
    gfx_current_dimensions.aspect_ratio = (float)base_w / (float)base_h;
    gfx_soft_set_render_res(base_w, base_h, int_w, int_h);
}

void gfx_sdl12_set_display_mode(int mode) {
    if (mode < 0 || mode > HH_DISPLAY_CROP) mode = HH_DISPLAY_CONVERT;
    gHandheldDisplayMode = mode;
    apply_res(sResIdx); /* re-derive the render size for the new base */
}

/* Menu "RESOLUTION": FULL (locked native, default) vs DYNAMIC (the ladder). */
static int sResEnvForced SAVESTATE_EXCLUDE;
void gfx_sdl12_set_res_dynamic(int on) {
    if (sResEnvForced) return; /* MK64_DYNRES pins it for testing */
    sResEnabled = on ? 1 : 0;
    if (!sResEnabled) apply_res(0); /* back to native and stay there */
}
int gfx_sdl12_get_res_dynamic(void) { return sResEnabled; }

/* Menu "3D RES" (device round 14): a FIXED internal 3D size the user steps
 * through by hand -- the ladder stays off. 100 = native (unified backend, the
 * cheapest rung); anything lower renders the 3D world smaller and nearest-
 * upscales it under the crisp native 2D/HUD, at a ~2 ms composite cost, so 95
 * is not worth it -- the first useful step is 90. Persisted as intres= in
 * mk64_display.cfg; the shipped default is 85 (chosen on hardware, round 15). */
void gfx_sdl12_set_res_pct(int pct) {
    int best = 0;
    for (int i = 0; i < RES_STEPS; i++) if (abs(sResPct[i] - pct) < abs(sResPct[best] - pct)) best = i;
    if (sResEnvForced && sResEnabled) return; /* MK64_DYNRES=1 pins the ladder for testing */
    sResEnabled = 0;
    apply_res(best);
}
int gfx_sdl12_get_res_pct(void) { return sResPct[sResIdx]; }
int gfx_sdl12_step_res_pct(int dir) { /* dir: -1 = smaller (next rung), +1 = larger */
    int idx = sResIdx + (dir < 0 ? 1 : -1);
    if (idx < 0) idx = 0;
    if (idx >= RES_STEPS) idx = RES_STEPS - 1;
    gfx_sdl12_set_res_pct(sResPct[idx]);
    return sResPct[sResIdx];
}

/* SDL_TRIPLEBUF flips by panning /dev/fb0 across three pages. A process that
 * dies panned to page 2 or 3 leaves the display there, and the next app
 * (the launcher) draws into page 1 while the panel shows another page: a "split"
 * screen that a screenshot (which reads page 1) shows as normal. Pan back to
 * page 1 on the way out -- a normal exit and any crash. open/ioctl/close only,
 * so it is safe inside a signal handler. */
static void fb_unpan(void) {
    const int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return;
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.yoffset != 0) {
        v.yoffset = 0;
        ioctl(fd, FBIOPAN_DISPLAY, &v);
    }
    close(fd);
}
/* A fatal signal leaves one line in console.log (stderr is the log on the device): the signal, the
 * iteration, and on ARM where it happened -- pc, lr (usually the caller) and the faulting address.
 * The binary is non-PIE and keeps its symbols (build_miyoo.sh strips only .comment), so
 * `addr2line -f -e mk64 <pc> <lr>` names them. Before, nothing logged these signals: a crash on the
 * device left the log just stopping. Only write() and arithmetic here (signal context), then the
 * panel unpanned and the default action, as before (the DKR port cd3649a). */
extern u32 port_frame_count(void);
static char *fatal_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *fatal_hex(char *p, unsigned long v) {
    for (int s = 28; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(v >> s) & 15];
    return p;
}
static char *fatal_dec(char *p, unsigned long v) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 12);
    while (n) *p++ = t[--n];
    return p;
}
static void fatal_handler(int sig, siginfo_t *si, void *ucv) {
    char buf[160], *p = buf;
    p = fatal_str(p, "FATAL: signal "); p = fatal_dec(p, (unsigned long)sig);
    p = fatal_str(p, " at iteration "); p = fatal_dec(p, port_frame_count());
#if defined(__arm__)
    {
        const ucontext_t *uc = (const ucontext_t *)ucv;
        p = fatal_str(p, ", pc "); p = fatal_hex(p, uc->uc_mcontext.arm_pc);
        p = fatal_str(p, " lr "); p = fatal_hex(p, uc->uc_mcontext.arm_lr);
    }
#else
    (void)ucv;
#endif
    if (si && si->si_code > 0) { /* a fault the kernel raised (not abort() / raise()): the address */
        p = fatal_str(p, " addr "); p = fatal_hex(p, (unsigned long)(uintptr_t)si->si_addr);
    }
    *p++ = '\n';
    (void)!write(2, buf, (size_t)(p - buf));
    fb_unpan();
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock the OS had (miyoo_clock.c: open/mmap/write only) */
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}
static void fatal_handlers_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fatal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);  sigaction(SIGABRT, &sa, NULL);
}

/* The OS's power-off (and any plain kill) sends SIGTERM: the game saves the auto save at the next
 * iteration boundary and quits (savestate.c). When the signal came, so savestate.c can log how long
 * the save took after it: monotonic ms since init + 1 (0 = no signal yet); clock_gettime is
 * async-signal-safe. */
static time_t sPowerBaseSec SAVESTATE_EXCLUDE;
static volatile sig_atomic_t sPowerFirstMs SAVESTATE_EXCLUDE;
static int power_ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)((ts.tv_sec - sPowerBaseSec) * 1000 + ts.tv_nsec / 1000000);
}
int handheld_power_signal_ms(void) { return sPowerFirstMs ? power_ms_now() - (sPowerFirstMs - 1) : -1; }
int handheld_power_pending(void) { return sResumeSave; }

static void resume_save_handler(int sig) {
    (void)sig;
    const int saved_errno = errno;
    if (!sPowerFirstMs) sPowerFirstMs = power_ms_now() + 1;
    sResumeSave = 1; /* the save is handled at the next swap, outside the signal context */
    errno = saved_errno;
}

/* 16bpp, else the 32bpp fallback */
static SDL_Surface *set_video_mode(int w, int h) {
    SDL_Surface *s = HH_SetVideoMode(w, h, 16, SDL_HWSURFACE | SDL_TRIPLEBUF);
    if (!s) s = HH_SetVideoMode(w, h, 32, SDL_HWSURFACE | SDL_TRIPLEBUF);
    return s;
}

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name; (void)start_in_fullscreen;
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    sScrW = handheld_screen_w(); sScrH = handheld_screen_h();
    sScreen = set_video_mode(SCREEN_W, SCREEN_H);
    if (SCREEN_W != 240 && (!sScreen || sScreen->w != SCREEN_W || sScreen->h != SCREEN_H)) {
        /* the display can't do 320x240: back to the square size */
        handheld_screen_confirm(sScreen ? sScreen->w : 0, sScreen ? sScreen->h : 0);
        sScrW = handheld_screen_w(); sScrH = handheld_screen_h();
        sScreen = set_video_mode(SCREEN_W, SCREEN_H);
    }
    if (!sScreen) {
        fprintf(stderr, "gfx_sdl12: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        exit(1);
    }
    { /* Resolution: FULL 240 by default (device round 7: native res is now
       * cheap -- the unified backend drops the composite at 240 -- so lock it
       * and never change resolution. The DYNAMIC ladder is a menu option for
       * a track that can't hold 30 at native. MK64_DYNRES overrides for tests:
       * =1 forces dynamic, =0 forces full; unset uses the saved setting
       * (loaded by handheld_menu_init -> gfx_sdl12_set_res_dynamic). */
        const char *e = getenv("MK64_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    apply_res(0);
    atexit(fb_unpan);
    fatal_handlers_install();
    /* The OS's power-off (and any plain kill) sends SIGTERM: auto save and quit. */
    signal(SIGTERM, resume_save_handler);
    signal(SIGHUP, resume_save_handler);
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) fprintf(stderr, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen);
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("MK64_MENU_TEST");
        if (td && td[0]) { handheld_menu_test_dump(td); exit(0); }
    }
#if defined(MIYOO)
    /* The second core (gfx_soft.c splits each big 3D triangle's rows with it): started last, so the
     * present and audio threads made above keep both cores; the game thread is pinned to core 0, the
     * worker to core 1, and the present thread joins the worker there (sPinPresent). MK64_THREADS=1/2
     * wins, then threads=1/2 in mk64_display.cfg; with one core online it stays off by itself. */
    {
        const char *e = getenv("MK64_THREADS");
        if (!(e && e[0]) && handheld_cfg_threads() == 1) fprintf(stderr, "worker: off (threads=1 in the cfg)\n");
        else if (miyoo_worker_start()) sPinPresent = 1;
    }
    miyoo_wd_start(); /* a stall report in console.log if no picture comes for 3 s (miyoo_watchdog.c) */
#endif
}

static void gfx_sdl12_set_keyboard_callbacks(bool (*on_key_down)(int scancode),
                                             bool (*on_key_up)(int scancode),
                                             void (*on_all_keys_up)(void)) {
    (void)on_key_down; (void)on_key_up; (void)on_all_keys_up; /* unused (beckerd path) */
}

static void gfx_sdl12_set_fullscreen_changed_callback(void (*cb)(bool)) { (void)cb; }
static void gfx_sdl12_set_fullscreen(bool enable) { (void)enable; }
static void gfx_sdl12_main_loop(void (*run_one_game_iter)(void)) { run_one_game_iter(); }

static void gfx_sdl12_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = gfx_current_dimensions.width ? gfx_current_dimensions.width : SCREEN_W;
    *height = gfx_current_dimensions.height ? gfx_current_dimensions.height : SCREEN_H;
}

static void gfx_sdl12_handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == HH_KEY_MENU) { /* MENU button */
                    { const u32 t0 = port_time_us();
#if defined(MIYOO)
                      gMiyooWdGame = WD_PRESENT_WAIT; /* a present thread stuck in its flip is still reported */
#endif
                      present_acquire();
#if defined(MIYOO)
                      gMiyooWdGame = WD_MENU;
#endif
                      handheld_menu_run(sScreen);
                      present_release();
#if defined(MIYOO)
                      gMiyooWdGame = WD_RUN;
#endif
                      PORT_PROF_ADD(PROF_SLEEP, port_time_us() - t0); }
                    sLastFrameMs = SDL_GetTicks(); /* don't count menu time as lag */
                    break;
                }
                controller_handheld_key(event.key.keysym.sym, 1);
                break;
            case SDL_KEYUP:
                controller_handheld_key(event.key.keysym.sym, 0);
                break;
            case SDL_QUIT:
                port_request_exit();
                break;
        }
    }
    if (sResumeSave) {
        /* the OS's power-off (SIGTERM): the auto save at the next iteration boundary, then quit.
         * EEPROM writes already hit disk. */
        sResumeSave = 0;
        fprintf(stderr, "gfx_sdl12: SIGTERM: auto save, then quit\n");
        savestate_request_exit_save(); /* the Miyoo's power-off: EXIT GAME's save */
    }
}

/* Pacing lives HERE and not in swap_buffers_begin: gfx_run skips the whole
 * render (including the swap) on a dropped frame, so state that only advances
 * in the swap would wedge into skipping forever after the first skip.
 * start_frame runs every logic frame.
 *
 * LOCKED-30 STRATEGY. Reactive skip-counting never held a lock; we PREDICT
 * from the measured frame profile (port.h buckets): work = iter - pacing, and
 * the ladder keeps work under budget at the HIGHEST resolution that fits.
 *
 * Device round 7 log finding: work is nearly FLAT across 144..216 (~26 ms
 * each) because the 3D raster is per-triangle-setup bound, not fill bound --
 * it barely scales with resolution. The old area-ratio prediction assumed
 * cost ~ pixels and so refused to climb past ~192. Now each rung REMEMBERS
 * its measured cost and the ladder trusts that: it probes upward whenever the
 * next rung is known (or estimated) to fit, and drops immediately on real
 * over-budget frames. The top rung (native, unified backend, no composite) is
 * usually the cheapest, so this settles at or near native. */
#define BUDGET_MS    31.5f  /* work ceiling; ~1.8 ms margin under the 33.3 ms frame */
#define UP_MARGIN_MS 2.0f   /* the next rung's estimate must beat budget by this */
#define EMA_ALPHA    0.18f
#define DROP_CONFIRM 3      /* consecutive raw over-budget rendered frames -> drop now */
#define UP_CONFIRM   24     /* ~0.8 s settled at a rung before probing up */
#define SETTLE       6      /* frames at a rung before its cost is "known" */

static float sEmaWork SAVESTATE_EXCLUDE = 16.0f, sEmaR3d SAVESTATE_EXCLUDE = 4.0f;
static float sRungMs[RES_STEPS] SAVESTATE_EXCLUDE; /* measured work EMA per rung (0 = unknown) */
static int sOverCount SAVESTATE_EXCLUDE, sRungFrames SAVESTATE_EXCLUDE;

/* Predicted work at rung `to` given the current scene: only the 3D raster
 * bucket moves, and it scales far weaker than pixel area (setup-bound), so
 * blend the linear and area ratios rather than assuming area. */
static float predict_work(int from, int to) {
    const float lin = (float)sResPct[to] / (float)sResPct[from];
    const float scale = 0.4f * lin + 0.6f * lin * lin;
    return sEmaWork - sEmaR3d + sEmaR3d * scale;
}

static void ladder_step(void) {
    extern int gfx_soft_last_frame_had_3d;
    /* Only rendered frames with 3D carry information: a skipped iteration is
     * cheap by construction, and a menu frame costs the same at every rung. */
    if (!sDoRender || !gfx_soft_last_frame_had_3d) { sOverCount = 0; return; }
    const u32 work_us = gPortProfLast[PROF_ITER] > gPortProfLast[PROF_SLEEP] ? gPortProfLast[PROF_ITER] - gPortProfLast[PROF_SLEEP] : 0;
    const float work = work_us / 1000.0f; /* iteration WORK: pacing sleep/menu/save excluded */
    const float r3d  = gPortProfLast[PROF_R3D] / 1000.0f;
    sEmaWork = sEmaWork * (1.0f - EMA_ALPHA) + work * EMA_ALPHA;
    sEmaR3d  = sEmaR3d  * (1.0f - EMA_ALPHA) + r3d  * EMA_ALPHA;
    sOverCount = (work > BUDGET_MS) ? sOverCount + 1 : 0;
    if (++sRungFrames >= SETTLE) sRungMs[sResIdx] = sEmaWork; /* remember this rung's real cost */

    /* Behind budget (sustained, or a short burst of over-budget frames): drop
     * a rung now. Seed the new rung's EMA from its remembered cost if known. */
    if ((sEmaWork > BUDGET_MS || sOverCount >= DROP_CONFIRM) && sResIdx < RES_STEPS - 1) {
        const int lo = sResIdx + 1;
        const float ar = (float)(sResPct[lo] * sResPct[lo]) / (float)(sResPct[sResIdx] * sResPct[sResIdx]);
        apply_res(lo);
        sEmaR3d *= ar;
        sEmaWork = sRungMs[lo] > 0.0f ? sRungMs[lo] : predict_work(sResIdx - 1, lo);
        sOverCount = 0; sRungFrames = 0;
        return;
    }
    /* Room to spare: probe one rung up when it's known (or estimated) to fit. */
    if (sResIdx > 0 && sRungFrames >= UP_CONFIRM) {
        const int hi = sResIdx - 1;
        const float est = sRungMs[hi] > 0.0f ? sRungMs[hi] : predict_work(sResIdx, hi);
        if (est < BUDGET_MS - UP_MARGIN_MS) {
            apply_res(hi);
            const float ar = (float)(sResPct[hi] * sResPct[hi]) / (float)(sResPct[sResIdx + 1] * sResPct[sResIdx + 1]);
            sEmaR3d *= ar; sEmaWork = est;
            sOverCount = 0; sRungFrames = 0;
        }
    }
}

/* Device round 14: deadline pacer. The old "min 33 ms between frame starts"
 * pacer skipped the NEXT render whenever a frame ran over 33 ms, and then
 * slept a full 31 ms after the 2 ms logic-only iteration -- so a course that
 * needs 36 ms per frame showed one picture every ~70 ms (a 15 fps cadence,
 * the v18 DK Jungle log: rendered 75/150 every window), and a 70 ms spike was
 * displayed for ~103 ms. The N64 itself never skips: it slows down. So:
 *   - on time            -> sleep to the deadline, render, deadline += 33
 *   - a little late      -> render NOW (the game runs slightly slow, smooth),
 *                           and the deadline restarts from now (no catch-up)
 *   - badly late (spike) -> one logic-only iteration, then render immediately
 *                           (no sleep after a skip -- that was the dead time)
 * A 36 ms course therefore renders every frame at ~28 fps instead of 15.
 * Round 2 (F-Zero X 06750ab's rule): a one-off spike -- an OS call, a screenshot, textures
 * re-imported after a load -- plays slow like any late frame; only LATE_RUN badly late
 * frames in a row take the logic-only iteration. One 53-99 ms hitch used to cost a skipped
 * picture, a visible jump. MK64_PACER=0: as before (A/B). */
#define LATE_SLOWDOWN_MS 24 /* late by up to this: render anyway (slow-mo floor ~17 fps... 33+24 = 57 ms) */
#define LATE_RUN 4          /* badly late this many frames in a row before a logic-only iteration */
static uint32_t sDeadlineMs SAVESTATE_EXCLUDE;
static int sLateRun SAVESTATE_EXCLUDE;

/* The pacer's wait. Any sleep on the Miyoo Mini costs a whole kernel tick, 9.2-9.6 ms however short the request
 * (measured on the Mini Plus), so a frame's 1-11 ms remainder slept was up to a frame late. Only the part of a wait
 * above 12 ms is slept; the rest is a sched_yield loop on the monotonic clock, to the same deadline. On a kernel whose
 * sleeps are exact the timing is the same. MK64_SPINWAIT=0: SDL_Delay, as before. Returns the microseconds waited. */
static uint32_t pace_wait_ms(uint32_t ms) {
    static int spin SAVESTATE_EXCLUDE = -1;
    struct timespec ts;
    uint64_t t0, now, until;
    if (spin < 0) { const char *e = getenv("MK64_SPINWAIT"); spin = !(e && e[0] == '0'); }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!spin) {
        SDL_Delay(ms);
    } else {
        until = t0 + (uint64_t)ms * 1000000ull;
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (now >= until) break;
            if (until - now > 12000000ull) {
                const uint64_t s = until - now - 12000000ull;
                struct timespec d = { (time_t)(s / 1000000000ull), (long)(s % 1000000000ull) };
                nanosleep(&d, NULL);
            } else {
                sched_yield();
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return (uint32_t)((now - t0) / 1000u);
}

static bool gfx_sdl12_start_frame(void) {
    if (sResEnabled) ladder_step();

    { /* MK64_NOPACE=1: render every iteration, never sleep or skip (profiling
       * under emulation, where every frame is "late": repeatable runs). */
        static int sNoPace SAVESTATE_EXCLUDE = -1;
        if (sNoPace < 0) { const char *e = getenv("MK64_NOPACE"); sNoPace = e && e[0] == '1'; }
        if (sNoPace) { sDoRender = true; sSkipsInARow = 0; return true; }
    }
    const uint32_t now = SDL_GetTicks();
    if (sDeadlineMs == 0) sDeadlineMs = now;
    const int late = (int)(now - sDeadlineMs); /* < 0: early */
    static int sRobust SAVESTATE_EXCLUDE = -1;
    if (sRobust < 0) { const char *e = getenv("MK64_PACER"); sRobust = !(e && e[0] == '0'); }
    if (late < LATE_SLOWDOWN_MS) sLateRun = 0;
    else if (sDoRender) sLateRun++;          /* a rendered frame that came in badly late */
    if (late <= 0) {
        if (late < 0) { const uint32_t waited = pace_wait_ms((uint32_t)(-late)); PORT_PROF_ADD(PROF_SLEEP, waited); }
        sDoRender = true; sSkipsInARow = 0;
        sDeadlineMs += FRAME_TIME_MS;
    } else if (late < LATE_SLOWDOWN_MS || sSkipsInARow >= FRAMESKIP_MAX || (sRobust && sLateRun < LATE_RUN)) {
        sDoRender = true; sSkipsInARow = 0;
        sDeadlineMs = now + FRAME_TIME_MS; /* slow-motion: no debt carried */
    } else {
        sDoRender = false; sSkipsInARow++;   /* spike: one cheap logic-only iteration... */
        sDeadlineMs = now;                   /* ...then render at once, no sleep */
        sLateRun = 0;
    }
    sLastFrameMs = now;
    return sDoRender;
}

/* ARGB8888 -> RGB565 for one contiguous row of n pixels. NEON on the device
 * (8 px/iter, no gather): the per-frame present conversion was ~2 ms scalar. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
static void argb_row_to_565(uint16_t *dst, const uint32_t *src, int n) {
    int x = 0;
    static int sScalar SAVESTATE_EXCLUDE = -1; /* neon=0 in the cfg: the scalar loop (device bench) */
    if (sScalar < 0) sScalar = handheld_cfg_neon() == 0;
    if (!sScalar) for (; x + 8 <= n; x += 8) {
        const uint8x8x4_t p = vld4_u8((const uint8_t *)(src + x)); /* .val[0]=B .1=G .2=R .3=A */
        const uint16x8_t r = vshll_n_u8(p.val[2], 8); /* R<<8 */
        const uint16x8_t g = vshll_n_u8(p.val[1], 8); /* G<<8 */
        const uint16x8_t b = vshll_n_u8(p.val[0], 8); /* B<<8 */
        const uint16x8_t out = vorrq_u16(vorrq_u16(
            vandq_u16(r, vdupq_n_u16(0xF800)),
            vshrq_n_u16(vandq_u16(g, vdupq_n_u16(0xFC00)), 5)),
            vshrq_n_u16(vandq_u16(b, vdupq_n_u16(0xF800)), 11));
        vst1q_u16(dst + x, out);
    }
    for (; x < n; x++) {
        const uint32_t s = src[x];
        dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F));
    }
}
#else
static void argb_row_to_565(uint16_t *dst, const uint32_t *src, int n) {
    for (int x = 0; x < n; x++) {
        const uint32_t s = src[x];
        dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F));
    }
}
#endif

/* ARGB8888 (render res) -> RGB565 staging -> screen, on a PRESENT THREAD.
 * The game thread converts the frame into a staging buffer (one generic NN
 * mapper covers all modes: CONVERT/STRETCH use the full source width, CROP a
 * centred 240/320 window) and signals; the thread copies staging to the video
 * surface and calls SDL_Flip. Whatever SDL_Flip does on this SDL (device
 * round 5: near-empty menu frames measured 22 ms of "render", i.e. the flip
 * was waiting for the panel's vsync inside the frame) now costs the game
 * thread nothing -- on a single core a blocked thread is free. The menu takes
 * the screen directly, under the same lock, once the thread is idle. */

static void blit_stage_to_screen(void) {
    if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
    if (sScreen->format->BitsPerPixel == 16) {
        for (int y = 0; y < SCREEN_H; y++)
            memcpy((uint8_t *)sScreen->pixels + y * sScreen->pitch, &sStage[y * SCREEN_W], SCREEN_W * 2);
    } else { /* 32bpp fallback */
        for (int y = 0; y < SCREEN_H; y++) {
            uint32_t *dst = (uint32_t *)((uint8_t *)sScreen->pixels + y * sScreen->pitch);
            const uint16_t *src = &sStage[y * SCREEN_W];
            for (int x = 0; x < SCREEN_W; x++) {
                const uint16_t p = src[x];
                dst[x] = ((uint32_t)(p & 0xF800) << 8) | ((uint32_t)(p & 0x07E0) << 5) | ((uint32_t)(p & 0x001F) << 3);
            }
        }
    }
    if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
}

static int present_thread(void *arg) {
    (void)arg;
    SDL_mutexP(sPresentLock);
    for (;;) {
#if defined(MIYOO)
        gMiyooWdPresent = WD_PRESENT_IDLE;
#endif
        while (!sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
#if defined(MIYOO)
        gMiyooWdPresent = WD_PRESENT_DRAW;
        if (!sWdPresent) { sWdPresent = 1; miyoo_wd_register("present"); }
        if (sPinPresent) { sPinPresent = 0; miyoo_worker_pin_self(1); } /* core 1, beside the worker: core 0 is the game's */
#endif
        sPresentPending = 0;
        sPresentBusy = 1;
#if defined(MIYOO)
        const int d32 = sStage32W > 0;
        if (d32) miyoo_draw32(sStage32, sStage32W, sStage32H);
        else
#endif
        blit_stage_to_screen();
        SDL_mutexV(sPresentLock);          /* the flip may block: never hold the lock */
        const uint32_t t0 = port_time_us();
#if defined(MIYOO)
        gMiyooWdPresent = WD_PRESENT_FLIP;
        if (d32) miyoo_flip_panel();
        else
#endif
        HH_Flip(sScreen);
        gPortProfFlipUs += port_time_us() - t0;
        SDL_mutexP(sPresentLock);
        sPresentBusy = 0;
        SDL_CondBroadcast(sPresentCond);
    }
    return 0;
}

/* Game thread: wait for the previous frame to leave the staging buffer, then
 * convert this one. Present is one frame behind the render at most. */
static void present(void) {
    int rw = 0, rh = 0;
    uint32_t *fb = gfx_soft_get_framebuffer(&rw, &rh);
    if (!fb || rw <= 0 || rh <= 0) return;

    int win_w = rw, x0 = 0;
    if (gHandheldDisplayMode == HH_DISPLAY_CROP && SCREEN_W == 240) {
        win_w = rw * 240 / 320;
        x0 = (rw - win_w) / 2;
    }
    const uint32_t t0 = port_time_us();
    SDL_mutexP(sPresentLock);
#if defined(MIYOO)
    gMiyooWdGame = WD_PRESENT_WAIT;
#endif
    while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock); /* staging still unread */
#if defined(MIYOO)
    gMiyooWdGame = WD_RUN;
#endif
    const int direct = (win_w == SCREEN_W && rh == SCREEN_H); /* 1:1 (native convert / crop@native) */
#if defined(MIYOO)
    sStage32W = 0;
    if (direct && miyoo_can_draw32(SCREEN_W, SCREEN_H)) {
        for (int y = 0; y < SCREEN_H; y++) memcpy(&sStage32[y * SCREEN_W], fb + y * rw + x0, SCREEN_W * 4);
        sStage32W = SCREEN_W; sStage32H = SCREEN_H;
    } else
#endif
    for (int y = 0; y < SCREEN_H; y++) {
        const uint32_t *src = fb + (direct ? y : y * rh / SCREEN_H) * rw + x0;
        uint16_t *dst = &sStage[y * SCREEN_W];
        if (direct) {
            argb_row_to_565(dst, src, SCREEN_W); /* NEON on device */
        } else { /* exact NN (x * win_w / 240): no 16.16 drift at panel edges */
            for (int x = 0; x < SCREEN_W; x++) {
                const uint32_t s = src[x * win_w / SCREEN_W];
                dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F));
            }
        }
    }
    sPresentPending = 1;
    SDL_CondBroadcast(sPresentCond);
    SDL_mutexV(sPresentLock);
    PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
#if defined(MIYOO)
    miyoo_wd_frame();
#endif
}
#if defined(MIYOO)
/* the watchdog's present line (plain reads from its thread) */
int miyoo_wd_detail_present(char *buf, int n) {
    return snprintf(buf, n, "picture pending %d, present thread busy %d, 32-bit frame %dx%d", (int) sPresentPending, (int) sPresentBusy,
                    sStage32W, sStage32H);
}
#endif

/* Take the screen for direct drawing (menu): wait until the thread is idle
 * and hold the lock so it stays idle until present_release(). */
static void present_acquire(void) {
    if (!sPresentLock) return;
    SDL_mutexP(sPresentLock);
    while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock);
}
static void present_release(void) { if (sPresentLock) SDL_mutexV(sPresentLock); }

/* The way out (main.c, after the game loop): the present thread idle and held, the console
 * unpanned, SDL shut down. SDL 1.2's fbcon otherwise leaves the console, keyboard and video modes
 * set, and the next SDL program (the launcher after EXIT GAME) can hang at its first flip, as
 * GoldenEye's second bench run did (X50, from the OoT port's EXIT GAME freeze, 2026-10-01). *
 * X50 (the same in every port): returns 1 once SDL is down, 0 if a flip was still running after
 * ~0.5 s (SDL left up rather than pulled from under it). The watchdog first (SSB64's port_exit):
 * SIGALRM's default action ends the process, so a flip or an SDL_Quit that never returns can't
 * keep the launcher from coming back. Never released: the process ends right after. */
int gfx_wm_shutdown(void) {
    static int sDone = -1;
    sigset_t s;

    if (sDone >= 0) return sDone; /* the bench's way out, then the exit's */
    signal(SIGALRM, SIG_DFL);
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    alarm(5);
    if (sPresentLock) {
        SDL_mutexP(sPresentLock);
        for (int i = 0; (sPresentPending || sPresentBusy) && i < 50; i++)
            SDL_CondWaitTimeout(sPresentCond, sPresentLock, 10);
        if (sPresentBusy) return sDone = 0;
    }
    fb_unpan();
    SDL_Quit();
    return sDone = 1;
}

static void gfx_sdl12_swap_buffers_begin(void) {
    present();

    { /* MK64_DUMP=<dir> [MK64_DUMP_EVERY=n]: PPM dumps of the render buffer
       * (same contract as the desktop backend; drives the qemu smoke test).
       * Counts rendered frames -- dropped frames never reach the swap. */
        extern void gfx_soft_dump_ppm(const char *path);
        static const char *sDumpDir SAVESTATE_EXCLUDE; static long sEvery SAVESTATE_EXCLUDE = -1; static long sFrame SAVESTATE_EXCLUDE;
        if (sEvery < 0) {
            sDumpDir = getenv("MK64_DUMP");
            const char *e = getenv("MK64_DUMP_EVERY");
            sEvery = e ? atol(e) : 60;
            if (sEvery <= 0) sEvery = 60;
        }
        if (sDumpDir && (sFrame % sEvery) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06ld.ppm", sDumpDir, sFrame);
            gfx_soft_dump_ppm(path);
            /* MK64_SCREEN_DUMP=1: also what present() staged for the screen, at the screen's size */
            static int sScreenDump SAVESTATE_EXCLUDE = -1;
            if (sScreenDump < 0) { const char *e = getenv("MK64_SCREEN_DUMP"); sScreenDump = e && e[0] == '1'; }
            if (sScreenDump) {
                snprintf(path, sizeof(path), "%s/screen_%06ld.ppm", sDumpDir, sFrame);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
                    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                        const uint16_t c = sStage[i];
                        const uint8_t rgb[3] = { (uint8_t) ((c >> 8) & 0xF8), (uint8_t) ((c >> 3) & 0xFC), (uint8_t) (c << 3) };
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                }
            }
        }
        sFrame++;
    }
}

static void gfx_sdl12_swap_buffers_end(void) { }
static double gfx_sdl12_get_time(void) { return SDL_GetTicks() / 1000.0; }

struct GfxWindowManagerAPI gfx_sdl = {
    gfx_sdl12_init,
    gfx_sdl12_set_keyboard_callbacks,
    gfx_sdl12_set_fullscreen_changed_callback,
    gfx_sdl12_set_fullscreen,
    gfx_sdl12_main_loop,
    gfx_sdl12_get_dimensions,
    gfx_sdl12_handle_events,
    gfx_sdl12_start_frame,
    gfx_sdl12_swap_buffers_begin,
    gfx_sdl12_swap_buffers_end,
    gfx_sdl12_get_time,
};

#endif /* HANDHELD */
