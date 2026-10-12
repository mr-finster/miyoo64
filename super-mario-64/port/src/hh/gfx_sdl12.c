/**
 * Window backend: SDL 1.2.
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format.
 * - Pacing: 30 fps via SDL_GetTicks, up to FRAMESKIP_MAX skipped renders in
 *   a row when behind (logic always runs).
 * - Miyoo Mini (MIYOO): the game's own 320x240 (the N64's screen), doubled and
 *   turned onto the 640x480 panel by the present thread (kit miyoo_panel.c),
 *   MENU is Escape (kit miyoo_menu.c). The OS's SIGTERM at power-off (or any
 *   other kill) takes EXIT GAME's path -- auto save, then quit -- so the next
 *   start offers RESUME.
 *   The second core draws half of each big 3D triangle (gfx_soft.c).
 */
#if defined(HANDHELD)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>

#include "gfx_window_manager_api.h"
#include "handheld_menu.h"
#include "savestate.h"
#include "../port.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#include "miyoo_worker.h"
#include "miyoo_clock.h"
#endif
extern u32 port_time_us(void);

/* Mirror of gfx_pc.h's GfxDimensions (that header drags in Gfx/ultra types).
 * gfx_pc reads it only at init; the res ladder pokes it directly. */
struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
extern struct GfxDimensions gfx_current_dimensions;

#define SCREEN_W 320 /* the N64's own screen, doubled onto the 640x480 panel */
#define SCREEN_H 240
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
#if defined(MIYOO)
/* sm64_display.cfg in the data dir (Miyoo only): one "key=value" a line (threads=1|2, cpuclock=<MHz>); the
 * value of `key` ("threads="), or def when the file or the line is missing. */
int gfx_miyoo_cfg(const char *key, int def) {
    extern const char *port_save_path(const char *name);
    FILE *f = fopen(port_save_path("sm64_display.cfg"), "r");
    char line[128];
    int v = def;
    if (!f) return def;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] >= '0' && line[strlen(key)] <= '9') v = atoi(line + strlen(key));
    fclose(f);
    return v;
}
#endif
static int present_thread(void *arg);
static uint16_t sStage[SCREEN_W * SCREEN_H] SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* The frame as rendered, 32 bits, which the present thread doubles and turns onto the panel
 * (miyoo_panel_present32): no RGB565 stage. sStage32W 0: no frame yet. */
static uint32_t sStage32[SCREEN_W * SCREEN_H] SAVESTATE_EXCLUDE;
static int sStage32W SAVESTATE_EXCLUDE, sStage32H SAVESTATE_EXCLUDE;
static volatile int sPinPresent SAVESTATE_EXCLUDE; /* the worker is on: the present thread moves to core 1 */
/* SM64_PRESJOB (2026-10-12, default 1; with the worker on): the frame's copy-out and its doubling onto the panel run as
 * a job on the second core, which is idle while the next frame's game logic runs, instead of the copy on the game
 * thread (PERF pres: ~1.4 ms on the Mini Plus in GoldenEye) and the doubling on the present thread -- which shared the
 * second core with the worker, spinning there between a frame's triangles. The present thread then only flips. The
 * renderer touches its frame again only after gfx_miyoo_present_fence (gfx_soft_start_frame, and the menu's
 * present_acquire), which waits for the job: the copy is then taken, and the worker is free for the next frame's
 * triangles. The panel gets the same pixels (SM64_PANEL_CHECK); the frames don't change. 0: as before. */
int gMiyooPresJob SAVESTATE_EXCLUDE = -1;
/* gMiyooMt (the bench's "mt-off" windows set 0): the second core's work at all -- the row split, the crack scan, the
 * present and audio jobs -- 1 by default; 0 draws and runs everything on this core, as SM64_THREADS=1 does at start */
int gMiyooMt SAVESTATE_EXCLUDE = 1;
static volatile int sPresJobOut SAVESTATE_EXCLUDE;    /* a present job handed to the worker, not yet waited for */
volatile int gMiyooJobOut SAVESTATE_EXCLUDE;          /* any job handed to the worker not yet waited for (present, audio) */
volatile int gMiyooJobBusy SAVESTATE_EXCLUDE;         /* that job is still running (it clears this, release, as it ends) */
static const uint32_t *sPresJobFb SAVESTATE_EXCLUDE;
static int sPresJobRw SAVESTATE_EXCLUDE, sPresJobX0 SAVESTATE_EXCLUDE, sPresJobW SAVESTATE_EXCLUDE, sPresJobH SAVESTATE_EXCLUDE;
#endif
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;
/* The picture converted straight into the screen surface, the present thread only flipping
 * (f-zero-x dc2c6d6), instead of into sStage for the thread to copy: one pass over the
 * picture instead of two, where every port logs ~2 ms of "pres". The price: the game thread
 * waits for the previous flip to end (the flip moves sScreen->pixels to the next buffer), which
 * the staging copy never did. gfx_sdl12_pres_direct: 1 = direct (the default since M5 round 2:
 * F-Zero X's device bench, 2.0 -> 1.5 ms with the prefetch), 0 = staging; SM64_PRES=0|1, and the
 * bench (SM64_BENCH / sm64_bench.cfg, hh_main.c) switches it per PERF window. */
int gfx_sdl12_pres_direct SAVESTATE_EXCLUDE = -1;
static volatile int sPresentDirect SAVESTATE_EXCLUDE; /* the pending picture is already in the surface */

static bool sDoRender SAVESTATE_EXCLUDE = true;
static int sSkipsInARow SAVESTATE_EXCLUDE;
static uint32_t sLastFrameMs SAVESTATE_EXCLUDE; /* pacing: wall clock, never restore */
static volatile sig_atomic_t sTermRequest SAVESTATE_EXCLUDE;

/* Display modes (handheld menu "DISPLAY"):
 *  CONVERT (default): render at the output size -- 3D FOV adapts via aspect
 *      ratio, 2D/HUD maps onto the output width, nothing clipped.
 *  STRETCH: render internal 320x240 (N64 aspect), squash to 240 on present.
 *  CROP: render 320x240, present the centered 240 columns 1:1 -- authentic
 *      proportions, HUD edges clipped (super-mario-64's approach). */
int gHandheldDisplayMode SAVESTATE_EXCLUDE = HH_DISPLAY_CONVERT; /* a setting, not game state */

/* Dynamic resolution ladder as a percentage of the base render size
 * (index 0 = native). Only the 3D (perspective) pass renders at this size;
 * 2D (HUD, text, menus) always renders at the base size and the backend
 * composites the two (gfx_soft.c). SM64_DYNRES=0 disables the ladder. */
static const int sResPct[] = { 100, 95, 90, 85, 80, 75, 70, 65, 60 };
#define RES_STEPS ((int)(sizeof(sResPct) / sizeof(sResPct[0])))
static int sResIdx SAVESTATE_EXCLUDE;     /* current ladder position */
static int sResEnabled SAVESTATE_EXCLUDE; /* SM64_DYNRES=0 disables the ladder */

extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

static void apply_res(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= RES_STEPS) idx = RES_STEPS - 1;
    sResIdx = idx;
    const int base_w = (gHandheldDisplayMode == HH_DISPLAY_CONVERT) ? SCREEN_W : 320;
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
    if (sResEnvForced) return; /* SM64_DYNRES pins it for testing */
    sResEnabled = on ? 1 : 0;
    if (!sResEnabled) apply_res(0); /* back to native and stay there */
}
int gfx_sdl12_get_res_dynamic(void) { return sResEnabled; }

/* Menu "3D RES" (device round 14): a FIXED internal 3D size the user steps
 * through by hand -- the ladder stays off. 100 = native (unified backend, the
 * cheapest rung); anything lower renders the 3D world smaller and nearest-
 * upscales it under the crisp native 2D/HUD, at a ~2 ms composite cost, so 95
 * is not worth it -- the first useful step is 90. Persisted as intres= in
 * sm64_display.cfg; the shipped default is 85 (chosen on hardware, round 15). */
void gfx_sdl12_set_res_pct(int pct) {
    int best = 0;
    for (int i = 0; i < RES_STEPS; i++) if (abs(sResPct[i] - pct) < abs(sResPct[best] - pct)) best = i;
    if (sResEnvForced && sResEnabled) return; /* SM64_DYNRES=1 pins the ladder for testing */
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
static void fatal_handler(int sig) {
    fb_unpan();
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock the OS had (miyoo_clock.c: open/mmap/write only) */
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

/* The Miyoo Mini's power-off is the OS's SIGTERM (miyoo_term_install). The kit's savestate.c and the
 * menu ask this. */
int handheld_power_pending(void) { return sTermRequest; }

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name; (void)start_in_fullscreen;
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    sScreen = miyoo_panel_open(); /* 640x480 32 bpp, turned (already open after a first start's screen) */
    if (!sScreen) {
        fprintf(stderr, "gfx_sdl12: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        exit(1);
    }
    { /* Resolution: FULL 240 by default (device round 7: native res is now
       * cheap -- the unified backend drops the composite at 240 -- so lock it
       * and never change resolution. The DYNAMIC ladder is a menu option for
       * a track that can't hold 30 at native. SM64_DYNRES overrides for tests:
       * =1 forces dynamic, =0 forces full; unset uses the saved setting
       * (loaded by handheld_menu_init -> gfx_sdl12_set_res_dynamic). */
        const char *e = getenv("SM64_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    apply_res(0);
    atexit(fb_unpan);
    signal(SIGSEGV, fatal_handler); signal(SIGBUS, fatal_handler); signal(SIGFPE, fatal_handler);
    signal(SIGILL, fatal_handler);  signal(SIGABRT, fatal_handler);
    /* Onion's and the stock OS's power-off, MinUI's, any plain kill: SIGTERM (or SIGHUP): auto save and quit. */
    miyoo_term_install(&sTermRequest);
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) fprintf(stderr, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen);
    { /* SM64's default over the kit's (M5 round 3, the user's pick, 2026-09-26). It touches only
       * the 3D; the HUD, text and menus are drawn after it on the native target at full size.
       *  - Distant textures sharp: bilinear only where a texture is magnified (the gate P17 had
       *    turned off). qemu: BoB -3.8%, JRB -1.0% instructions; flicker (a pixel that changes
       *    and comes straight back, 120 consecutive frames) BoB 278 -> 302 px a frame, JRB 110 ->
       *    112, as MK64 found (a6e4ca7). SM64_MAGBILIN=0: every textured pixel bilinear.
       *  The resolution ladder stays off (the kit's FULL): the user, after trying it on the device
       *  (9b6f60cc): "sacrificing 3D res is bad" -- the 3D is always drawn at 100%. */
        extern int gfx_soft_magbilin;
        const char *b = getenv("SM64_MAGBILIN");
        gfx_soft_magbilin = !(b && b[0] == '0');
    }
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("SM64_MENU_TEST");
        if (td && td[0]) { handheld_menu_test_dump(td); exit(0); }
    }
#if defined(MIYOO)
    /* The second core (gfx_soft.c splits each big 3D triangle's rows with it): started last, so the present and
     * audio threads made above keep both cores; the game thread is pinned to core 0, the worker to core 1, and the
     * present thread joins the worker there (sPinPresent). SM64_THREADS=1/2 wins, then threads=1/2 in
     * sm64_display.cfg; with one core online it stays off by itself. */
    {
        const char *e = getenv("SM64_THREADS");
        if (!(e && e[0]) && gfx_miyoo_cfg("threads=", -1) == 1) fprintf(stderr, "worker: off (threads=1 in sm64_display.cfg)\n");
        else if (miyoo_worker_start()) sPinPresent = 1;
    }
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
                if (event.key.keysym.sym == MIYOO_KEY_MENU) { /* MENU button (Escape) */
                    { const u32 t0 = port_time_us();
                      present_acquire();
                      handheld_menu_run(sScreen);
                      present_release();
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
    if (sTermRequest) {
        /* the OS's SIGTERM: the auto save at the next iteration boundary, then quit. EEPROM writes already hit disk. */
        sTermRequest = 0;
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
 * The lead's round 2 (MK64 3888bd4, F-Zero X 06750ab's rule): a one-off spike -- an OS call, a
 * screenshot, a load's first frames -- plays slow like any late frame; only LATE_RUN badly late
 * frames in a row take the logic-only iteration, where one hitch used to cost a skipped picture.
 * SM64_PACER=0: as before (A/B). */
#define LATE_SLOWDOWN_MS 24 /* late by up to this: render anyway (slow-mo floor ~17 fps... 33+24 = 57 ms) */
#define RESET_PAY_MS 4      /* an audio reset's debt paid per on-time frame, at most (gfx_sdl12_start_frame) */
#define LATE_RUN 4          /* badly late this many rendered frames in a row before a logic-only iteration */
static uint32_t sDeadlineMs SAVESTATE_EXCLUDE;
static int sLateRun SAVESTATE_EXCLUDE;

static bool gfx_sdl12_start_frame(void) {
    if (sResEnabled) ladder_step();

    { /* SM64_NOPACE=1: render every iteration, never sleep or skip (profiling
       * under emulation, where every frame is "late": repeatable runs). */
        static int sNoPace SAVESTATE_EXCLUDE = -1;
        if (sNoPace < 0) { const char *e = getenv("SM64_NOPACE"); sNoPace = e && e[0] == '1'; }
        if (sNoPace) { sDoRender = true; sSkipsInARow = 0; return true; }
    }
    const uint32_t now = SDL_GetTicks();
    if (sDeadlineMs == 0) sDeadlineMs = now;
    const int late = (int)(now - sDeadlineMs); /* < 0: early */
    static int sRobust SAVESTATE_EXCLUDE = -1;
    if (sRobust < 0) { const char *e = getenv("SM64_PACER"); sRobust = !(e && e[0] == '0'); }
    if (late < LATE_SLOWDOWN_MS) sLateRun = 0;
    else if (sDoRender) sLateRun++;          /* a rendered frame that came in badly late */
    /* An audio reset's frames that went into the ring at once (hh_main.c SM64_RESETNOWAIT): the wait they didn't
     * take, paid here -- a late frame's lateness pays (the ring drained meanwhile), an on-time one sleeps up to
     * RESET_PAY_MS more and moves its deadline on by as much. */
    extern volatile u32 gPortResetDebtUs;
    if (late > 0 && gPortResetDebtUs) gPortResetDebtUs = (u32)late * 1000u >= gPortResetDebtUs ? 0 : gPortResetDebtUs - (u32)late * 1000u;
    if (late <= 0) {
        uint32_t pay = 0;
        if (gPortResetDebtUs) {
            pay = gPortResetDebtUs / 1000u < RESET_PAY_MS ? gPortResetDebtUs / 1000u : RESET_PAY_MS;
            gPortResetDebtUs = pay < RESET_PAY_MS ? 0 : gPortResetDebtUs - pay * 1000u; /* under 1 ms left: dropped */
        }
        const uint32_t sleep = (uint32_t)(-late) + pay;
        if (sleep) { SDL_Delay(sleep); PORT_PROF_ADD(PROF_SLEEP, sleep * 1000u); }
        sDoRender = true; sSkipsInARow = 0;
        sDeadlineMs += FRAME_TIME_MS + pay;
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
    for (; x + 8 <= n; x += 8) {
        __builtin_prefetch(src + x + 128); /* the source 512 bytes ahead (FZX bbeb879: 1.6 -> 1.5 ms on the device) */
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
        while (!sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        sPresentPending = 0;
        sPresentBusy = 1;
        if (sPinPresent) { sPinPresent = 0; miyoo_worker_pin_self(1); } /* core 1, beside the worker: core 0 is the game's */
        (void) blit_stage_to_screen; /* (the handheld's 16 bpp stage) */
        if (!sPresentDirect) miyoo_panel_present32(sStage32, sStage32W, sStage32H); /* doubled, turned, the notice drawn in */
        sPresentDirect = 0; /* (SM64_PRESJOB: the worker drew it already) */
        SDL_mutexV(sPresentLock);          /* the flip may block: never hold the lock */
        const uint32_t t0 = port_time_us();
        miyoo_panel_flip();
        gPortProfFlipUs += port_time_us() - t0;
        SDL_mutexP(sPresentLock);
        sPresentBusy = 0;
        SDL_CondBroadcast(sPresentCond);
    }
    return 0;
}

#if defined(MIYOO)
/* SM64_PRESJOB's job, on the worker: the frame into the stage, doubled and turned onto the panel (the notice drawn on
 * the stage, as before), then the present thread is told to flip. */
static void pres_job(void *unused) {
    (void)unused;
    const int w = sPresJobW, h = sPresJobH;
    for (int y = 0; y < h; y++) memcpy(&sStage32[y * w], sPresJobFb + y * sPresJobRw + sPresJobX0, (size_t)w * 4);
    sStage32W = w; sStage32H = h;
    miyoo_panel_present32(sStage32, w, h);
    { extern void gfx_soft_preclear_z(void); gfx_soft_preclear_z(); } /* the next frame's depth clear, here too */
    SDL_mutexP(sPresentLock);
    sPresentDirect = 1; /* drawn: the present thread only flips */
    sPresentPending = 1;
    SDL_CondBroadcast(sPresentCond);
    SDL_mutexV(sPresentLock);
    __atomic_store_n(&gMiyooJobBusy, 0, __ATOMIC_RELEASE);
}
/* Every job handed to the worker without waiting for it (SM64_PRESJOB's, SM64_AUDIOJOB's) is done: before the next
 * job (the worker takes one at a time) */
void gfx_miyoo_job_fence(void) {
    if (!gMiyooJobOut) return;
    miyoo_worker_wait();
    gMiyooJobOut = 0; sPresJobOut = 0;
}
/* The same without waiting: 1 when no such job is left (the worker can take the next), 0 while one still runs. The
 * renderer then draws a triangle on this core alone rather than wait (the same pixels either way). A job that has
 * ended needs no wait: the worker reads the next job's function only after it sees the next hand-over. */
int gfx_miyoo_job_try(void) {
    if (!gMiyooJobOut) return 1;
    if (__atomic_load_n(&gMiyooJobBusy, __ATOMIC_ACQUIRE)) return 0;
    gMiyooJobOut = 0; sPresJobOut = 0;
    return 1;
}
/* The present job is done with the renderer's frame and the panel */
void gfx_miyoo_present_fence(void) {
    if (sPresJobOut) gfx_miyoo_job_fence();
}
#endif
/* Game thread: wait for the previous frame to leave the staging buffer, then
 * convert this one. Present is one frame behind the render at most. */
static void present(void) {
    int rw = 0, rh = 0;
    uint32_t *fb = gfx_soft_get_framebuffer(&rw, &rh);
    if (!fb || rw <= 0 || rh <= 0) return;

    int win_w = rw, x0 = 0;
    if (gHandheldDisplayMode == HH_DISPLAY_CROP) {
        win_w = rw * 240 / 320;
        x0 = (rw - win_w) / 2;
    }
    const uint32_t t0 = port_time_us();
#if defined(MIYOO)
    { /* the frame as it is (320x240, or the boxed 240 of another mode's window) into the 32-bit stage */
        const int w = win_w <= SCREEN_W ? win_w : SCREEN_W, h = rh <= SCREEN_H ? rh : SCREEN_H;
        if (gMiyooPresJob < 0) { const char *e = getenv("SM64_PRESJOB"); gMiyooPresJob = !(e && e[0] == '0'); }
        if (gMiyooPresJob > 0 && gMiyooMt && miyoo_worker_ready()) { /* SM64_PRESJOB: copy and doubling on the second core */
            gfx_miyoo_present_fence(); /* (normally done already, at this frame's start) */
            SDL_mutexP(sPresentLock);
            while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock); /* the last flip is done: the panel is free */
            SDL_mutexV(sPresentLock);
            sPresJobFb = fb; sPresJobRw = rw; sPresJobX0 = x0; sPresJobW = w; sPresJobH = h;
            gfx_miyoo_job_fence(); /* (the worker takes one job at a time) */
            sPresJobOut = 1; gMiyooJobOut = 1; gMiyooJobBusy = 1;
            miyoo_worker_run(pres_job, NULL);
            PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
            return;
        }
        SDL_mutexP(sPresentLock);
        while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock); /* staging still unread */
        for (int y = 0; y < h; y++) memcpy(&sStage32[y * w], fb + y * rw + x0, (size_t)w * 4);
        sStage32W = w; sStage32H = h;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
#endif
    if (gfx_sdl12_pres_direct < 0) { const char *e = getenv("SM64_PRES"); gfx_sdl12_pres_direct = !(e && e[0] == '0'); }
    const int direct = (win_w == SCREEN_W && rh == SCREEN_H); /* 1:1 (native convert / crop@native) */
    const int surf = gfx_sdl12_pres_direct && direct && sScreen->format->BitsPerPixel == 16;
    SDL_mutexP(sPresentLock);
    /* staging still unread; for the surface, also a flip in progress */
    while (sPresentPending || (surf && sPresentBusy)) SDL_CondWait(sPresentCond, sPresentLock);
    if (surf) {
        if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
        if (sScreen->pitch == SCREEN_W * 2 && rw == SCREEN_W && x0 == 0) {
            argb_row_to_565((uint16_t *)sScreen->pixels, fb, SCREEN_W * SCREEN_H); /* rows contiguous: one run */
        } else {
            for (int y = 0; y < SCREEN_H; y++)
                argb_row_to_565((uint16_t *)((uint8_t *)sScreen->pixels + y * sScreen->pitch), fb + y * rw + x0, SCREEN_W);
        }
        if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
        sPresentDirect = 1;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
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
}

/* Take the screen for direct drawing (menu): wait until the thread is idle
 * and hold the lock so it stays idle until present_release(). */
static void present_acquire(void) {
    if (!sPresentLock) return;
#if defined(MIYOO)
    gfx_miyoo_present_fence(); /* SM64_PRESJOB: the worker is done with the panel */
#endif
    SDL_mutexP(sPresentLock);
    while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock);
}
static void present_release(void) { if (sPresentLock) SDL_mutexV(sPresentLock); }

static void gfx_sdl12_swap_buffers_begin(void) {
    present();

    { /* SM64_DUMP=<dir> [SM64_DUMP_EVERY=n]: PPM dumps of the render buffer
       * (same contract as the desktop backend; drives the qemu smoke test).
       * Counts rendered frames -- dropped frames never reach the swap. */
        extern void gfx_soft_dump_ppm(const char *path);
        static const char *sDumpDir SAVESTATE_EXCLUDE; static long sEvery SAVESTATE_EXCLUDE = -1; static long sFrame SAVESTATE_EXCLUDE;
        if (sEvery < 0) {
            sDumpDir = getenv("SM64_DUMP");
            const char *e = getenv("SM64_DUMP_EVERY");
            sEvery = e ? atol(e) : 60;
            if (sEvery <= 0) sEvery = 60;
        }
        if (sDumpDir && (sFrame % sEvery) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06ld.ppm", sDumpDir, sFrame);
            gfx_soft_dump_ppm(path);
        }
        sFrame++;
    }
}

/* X50 (the other ports' exit, 2026-10-13: SM64 on MinUI sat at a black screen after EXIT GAME, the other games came
 * back): SDL down before the launcher comes back. SDL 1.2's fbcon otherwise leaves the console, keyboard and video
 * modes set up, and the launcher after us can hang at its first flip. Any job still on the worker and the last flip
 * finish first (~0.5 s at most for the flip; SDL left up rather than pulled from under it). The watchdog first:
 * SIGALRM's default action ends the process, so a flip or an SDL_Quit that never returns can't keep the launcher
 * from coming back. Never released: the process ends right after. */
int gfx_wm_shutdown(void) {
    static int sDone = -1;
    sigset_t s;

    if (sDone >= 0) return sDone;
    signal(SIGALRM, SIG_DFL);
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    alarm(5);
#if defined(MIYOO)
    gfx_miyoo_job_fence(); /* SM64_PRESJOB / SM64_AUDIOJOB: the worker is done with the panel and the audio */
#endif
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
