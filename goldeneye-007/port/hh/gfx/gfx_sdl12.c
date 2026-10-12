/**
 * Miyoo Mini window backend: SDL 1.2, a 640x480 32 bpp screen; the game renders its own 320x240
 * and the present thread doubles it.
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format. A dynamic-resolution ladder (RES_LADDER)
 *   renders smaller under load and NN-upscales in the same pass.
 * - Pacing: 30 fps via SDL_GetTicks, up to FRAMESKIP_MAX skipped renders in
 *   a row when behind (logic always runs).
 * - MENU (Escape) opens the overlay (miyoo_menu.c).
 * - Power: SIGTERM (the OS shutting the app down) exit-saves, so the next start offers RESUME GAME.
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
#include <poll.h>
#include <time.h>

#include "gfx_window_manager_api.h"
#include "handheld_menu.h"
#include "savestate.h"
#include "port.h"

/* Mirror of gfx_pc.h's GfxDimensions (that header drags in Gfx/ultra types).
 * gfx_pc reads it only at init; the res ladder pokes it directly. */
struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
extern struct GfxDimensions gfx_current_dimensions;

/* Miyoo Mini: a 640x480 panel. The game renders its own 320x240 (4:3, so hh_screen.c keeps the
 * N64's projections and the HUD as laid out) and the present thread doubles it on the second
 * core (notes/miyoo.md). */
#define SCREEN_W 640
#define SCREEN_H 480
#define BASE_W 320
#define BASE_H 240
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
static uint32_t sStage32[BASE_W * BASE_H] SAVESTATE_EXCLUDE; /* the frame at the render size */
static int sStageW SAVESTATE_EXCLUDE, sStageH SAVESTATE_EXCLUDE;
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;
/* The present (round 19, FZX 1b73772..bbeb879 / its round-12 device bench: staging 2.0-2.1 ms,
 * direct 1.6, direct + prefetch 1.5): default 2 converts a native picture straight into
 * the 16-bit screen surface with the source prefetched ahead; GE_PRES=0: through sStage as
 * before, 1: direct without the prefetch. The same pixels either way. */
static int sPresMode SAVESTATE_EXCLUDE = -1;
static volatile int sPresentDirect SAVESTATE_EXCLUDE; /* the pending picture is already in the surface */

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
static const int sResPct[] = { 100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30 }; /* 55..30: the user's 30% look, 2026-09-27 */
#define RES_STEPS ((int)(sizeof(sResPct) / sizeof(sResPct[0])))
static int sResIdx SAVESTATE_EXCLUDE;     /* current ladder position */
static int sResEnabled SAVESTATE_EXCLUDE; /* MK64_DYNRES=0 disables the ladder */

extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

static void apply_res(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= RES_STEPS) idx = RES_STEPS - 1;
    sResIdx = idx;
    const int base_w = BASE_W, base_h = BASE_H; /* always the N64's 4:3 */
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
/* A fatal signal: give the panel back, then the port's report (hh_boot.c: where it died,
 * the threads) and the default action. */
extern void hh_crash_handler_chain(int sig, siginfo_t *si, void *uc);
static void fatal_handler(int sig, siginfo_t *si, void *uc) {
    fb_unpan();
    hh_crash_handler_chain(sig, si, uc);
}

/* Onion / the stock OS end an app with SIGTERM (the power button's shut down): the handler asks
 * the game loop for an exit save and a quit, as EXIT GAME does, so RESUME GAME picks up there next
 * time. */
static volatile sig_atomic_t sTermRequested SAVESTATE_EXCLUDE;
static void term_handler(int sig) { (void)sig; sTermRequested = 1; }
int handheld_power_pending(void) { return 0; }

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name; (void)start_in_fullscreen;
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    SDL_Init(SDL_INIT_VIDEO);
    { /* debug.cfg bilinear=0 / magbilin=0 (-> OOT_*): the desktop backend's A/B switches */
        extern int gfx_soft_bilinear, gfx_soft_magbilin;
        const char *b = getenv("GE_BILINEAR");
        if (b && b[0]) gfx_soft_bilinear = (b[0] == '1');
        b = getenv("GE_MAGBILIN");
        if (b && b[0]) gfx_soft_magbilin = (b[0] == '1');
    }
    SDL_ShowCursor(SDL_DISABLE);
    /* the Mini's SDL (MI_GFX) is 32 bpp and turns the picture for the panel itself */
    sScreen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sScreen) {
        sScreen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_HWSURFACE | SDL_DOUBLEBUF);
    }
    if (sScreen) fprintf(stderr, "gfx_sdl12: %dx%d, %d bpp, pitch %d\n", sScreen->w, sScreen->h, sScreen->format->BitsPerPixel, sScreen->pitch);
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
        const char *e = getenv("GE_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    apply_res(0);
    { /* 3D internal resolution in percent (GE_INTRES, debug.cfg, a developer knob for A/B):
       * 100 unless set. The user's call for every port (2026-09-24): no 3D options, the 3D at
       * 100% with SMOOTH, and the M3 gate read there. */
        const char *e = getenv("GE_INTRES");
        gfx_sdl12_set_res_pct(e && atoi(e) > 0 ? atoi(e) : 100);
    }
    atexit(fb_unpan);
    {
        struct sigaction sa;
        const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = fatal_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK; /* the port's alternate stack (hh_boot.c) */
        sigemptyset(&sa.sa_mask);
        for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
    }
    signal(SIGTERM, term_handler);
    { extern int miyoo_worker_start(void); (void)miyoo_worker_start(); } /* the second core (gfx_soft.c SplitJob); GE_THREADS=1: off */
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) fprintf(stderr, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen);
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("GE_MENU_TEST");
        if (td && td[0]) { handheld_menu_test_dump(td); exit(0); }
    }
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
                if (event.key.keysym.sym == SDLK_ESCAPE) { /* MENU button */
                    { const u32 t0 = port_time_us();
                      present_acquire();
                      handheld_menu_run(sScreen);
                      present_release();
                      { extern void hh_os_rebase_clock(void); hh_os_rebase_clock(); } /* the menu's time is not game time */
                      { extern uint32_t hh_menu_us; hh_menu_us += port_time_us() - t0; } /* nor render time (hh_gfx.c) */
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
#if defined(MIYOO)
    if (sTermRequested) {
        sTermRequested = 0;
        fprintf(stderr, "gfx_sdl12: SIGTERM, exit save\n");
        savestate_request_exit_save();
    }
#endif
    if (sResumeSave) {
        /* the OS's power-off (SIGTERM): the auto save at the next iteration boundary, then quit.
         * EEPROM writes already hit disk. */
        sResumeSave = 0;
        savestate_request_resume_save();
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
 * A 36 ms course therefore renders every frame at ~28 fps instead of 15. */
#define LATE_SLOWDOWN_MS 24 /* late by up to this: render anyway (slow-mo floor ~17 fps... 33+24 = 57 ms) */
static uint32_t sDeadlineMs SAVESTATE_EXCLUDE;

/* OoT: frame pacing is the game scheduler's (hh_sched.c paces each gfx task to
 * 16.67 ms x R_UPDATE_RATE, like the N64 VI), so the window never sleeps or
 * skips here. (MK64's deadline pacer above stays for reference; unused.) */
static bool gfx_sdl12_start_frame(void) {
    (void)sDeadlineMs;
    sDoRender = true;
    sSkipsInARow = 0;
    sLastFrameMs = SDL_GetTicks();
    return true;
}

/* ARGB8888 -> RGB565 for one contiguous row of n pixels. NEON on the device
 * (8 px/iter, no gather): the per-frame present conversion was ~2 ms scalar. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
static void argb_row_to_565(uint16_t *dst, const uint32_t *src, int n) {
    int x = 0;
    if (sPresMode == 2) { /* the same loop with the source prefetched 512 bytes ahead */
        for (; x + 8 <= n; x += 8) {
            __builtin_prefetch(src + x + 128);
            const uint8x8x4_t p = vld4_u8((const uint8_t *)(src + x));
            const uint16x8_t r = vshll_n_u8(p.val[2], 8), g = vshll_n_u8(p.val[1], 8), b = vshll_n_u8(p.val[0], 8);
            vst1q_u16(dst + x, vorrq_u16(vorrq_u16(vandq_u16(r, vdupq_n_u16(0xF800)), vshrq_n_u16(vandq_u16(g, vdupq_n_u16(0xFC00)), 5)),
                                         vshrq_n_u16(vandq_u16(b, vdupq_n_u16(0xF800)), 11)));
        }
    }
    for (; x + 8 <= n; x += 8) {
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

/* The panel's 16 bits with an ordered dither (round 39, the user: "bad color banding" in the
 * skies). Dropping the low bits turned smooth gradients -- the half-resolution sky's bilinear doubling
 * most of all -- into visible steps; the N64 dithers its 16-bit frame buffer for the same reason. A 4x4
 * Bayer offset (0..7 on the 5-bit channels, 0..3 on green) is added, saturating, before the bits are
 * dropped: a colour the panel can show exactly stays exact, one between two levels comes out as the right
 * mix of both. y: the row's screen line (the pattern). GE_DITHER=0: plain truncation. */
static const uint8_t kBayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
static int sDither SAVESTATE_EXCLUDE = -1;
static inline int dither_on(void) {
    if (sDither < 0) { const char *e = getenv("GE_DITHER"); sDither = !(e && e[0] == '0'); }
    return sDither;
}
static inline uint16_t px565_dither(uint32_t s, int x, int y) {
    const int b = kBayer4[y & 3][x & 3];
    int r = (int)((s >> 16) & 0xFF) + (b >> 1), g = (int)((s >> 8) & 0xFF) + (b >> 2), bl = (int)(s & 0xFF) + (b >> 1);
    if (r > 255) r = 255; if (g > 255) g = 255; if (bl > 255) bl = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}
static void argb_row_to_565_d(uint16_t *dst, const uint32_t *src, int n, int y) {
    if (!dither_on()) { argb_row_to_565(dst, src, n); return; }
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint8_t *row = kBayer4[y & 3];
    const uint8_t d5a[8] = { row[0] >> 1, row[1] >> 1, row[2] >> 1, row[3] >> 1, row[0] >> 1, row[1] >> 1, row[2] >> 1, row[3] >> 1 };
    const uint8_t d6a[8] = { row[0] >> 2, row[1] >> 2, row[2] >> 2, row[3] >> 2, row[0] >> 2, row[1] >> 2, row[2] >> 2, row[3] >> 2 };
    const uint8x8_t d5 = vld1_u8(d5a), d6 = vld1_u8(d6a);
    /* GE_PRES=2 (the default): the source prefetched 512 bytes ahead, as argb_row_to_565 does (round 19: 0.1 ms a
     * frame on the device); the dither path (round 39, on by default) had lost it. A prefetch reads nothing into
     * the pixels. */
    const int pf = sPresMode == 2;
    for (; x + 8 <= n; x += 8) {
        if (pf) __builtin_prefetch(src + x + 128);
        const uint8x8x4_t p = vld4_u8((const uint8_t *)(src + x)); /* .val[0]=B .1=G .2=R .3=A */
        const uint16x8_t r = vshll_n_u8(vqadd_u8(p.val[2], d5), 8);
        const uint16x8_t g = vshll_n_u8(vqadd_u8(p.val[1], d6), 8);
        const uint16x8_t b = vshll_n_u8(vqadd_u8(p.val[0], d5), 8);
        vst1q_u16(dst + x, vorrq_u16(vorrq_u16(vandq_u16(r, vdupq_n_u16(0xF800)), vshrq_n_u16(vandq_u16(g, vdupq_n_u16(0xFC00)), 5)),
                                     vshrq_n_u16(vandq_u16(b, vdupq_n_u16(0xF800)), 11)));
    }
#endif
    for (; x < n; x++) dst[x] = px565_dither(src[x], x, y);
}

/* ARGB8888 (render res) -> RGB565 staging -> screen, on a PRESENT THREAD.
 * The game thread converts the frame into a staging buffer (one generic NN
 * mapper covers all modes: CONVERT/STRETCH use the full source width, CROP a
 * centred 240/320 window) and signals; the thread copies staging to the video
 * surface and calls SDL_Flip. Whatever SDL_Flip does on this SDL (device
 * round 5: near-empty menu frames measured 22 ms of "render", i.e. the flip
 * was waiting for the panel's vsync inside the frame) now costs the game
 * thread nothing -- on a single core a blocked thread is free. The menu takes
 * the screen directly, under the same lock, once the thread is idle. */

/* Turned 180 degrees on the way out (the user's Mini Plus on Onion, 2026-09-30: the picture was
 * upside down -- that SDL hands us the panel as mounted). GE_ROTATE=0: as SDL gives it.
 * miyoo_menu.c turns its screens the same way. */
static int sRotate SAVESTATE_EXCLUDE = -1;
int gfx_miyoo_rotate(void) {
    if (sRotate < 0) { const char *e = getenv("GE_ROTATE"); sRotate = !(e && e[0] == '0'); }
    return sRotate;
}
static SDL_Surface *sStageSurf SAVESTATE_EXCLUDE; /* sStage32 as a surface: the notification is drawn on it */
/* The frame without the copy (GE_PRESCOPY=0, the default where the exact 2x applies): the present thread doubles the
 * renderer's own native frame, which present() hands over instead of copying it out (~300 KB a frame, ~1.4 ms of
 * the game thread on the Mini Plus: PERF pres). The renderer writes that frame again only after the fence at its
 * next frame's start (gfx_miyoo_present_fence, gfx_soft_start_frame), which waits until the present thread has
 * taken the frame -- its doubling runs under the lock, so taken is done. The notice is then drawn on the screen,
 * turned, instead of on the copy (miyoo_menu_overlay_turned: the same pixels). */
static const uint32_t *sPresSrc SAVESTATE_EXCLUDE; /* the frame to double: the renderer's, or NULL = sStage32 */
static int sPresCopy SAVESTATE_EXCLUDE = -1;
extern void miyoo_menu_overlay_turned(SDL_Surface *screen, int rot);
void gfx_miyoo_present_fence(void) {
    if (!sPresentLock) return;
    SDL_mutexP(sPresentLock);
    while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
    if (sPresSrc) { sPresSrc = NULL; sStageW = sStageH = 0; } /* the renderer's frame is about to change */
    SDL_mutexV(sPresentLock);
}
/* GE_PANELDUMP=<dir> (tests): every GE_PANELDUMP_EVERY'th presented frame (default 50), the screen as the panel gets
 * it (640x480, turned), as PPM, written by the present thread after the doubling */
static void panel_dump(void) {
    static const char *dir SAVESTATE_EXCLUDE;
    static int every SAVESTATE_EXCLUDE = -1, n SAVESTATE_EXCLUDE;
    if (every < 0) { dir = getenv("GE_PANELDUMP"); const char *e = getenv("GE_PANELDUMP_EVERY"); every = e && atoi(e) > 0 ? atoi(e) : 50; }
    if (!dir || !dir[0] || sScreen->format->BitsPerPixel != 32) return;
    if (n++ % every) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/panel_%06d.ppm", dir, n - 1);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sScreen->w, sScreen->h);
    for (int y = 0; y < sScreen->h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)sScreen->pixels + y * sScreen->pitch);
        for (int x = 0; x < sScreen->w; x++) {
            const uint8_t rgb[3] = { (uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* One 32-bit row of the render size onto two screen rows, each pixel twice (the exact 2x of
 * 320x240 on 640x480); alpha forced opaque for MI_GFX. rot: the row mirrored (d0/d1 are the
 * turned picture's rows). */
static void row_x2_32(uint32_t *d0, uint32_t *d1, const uint32_t *src, int n, int rot) {
    int x = 0;
    if (rot) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const uint32x4_t a = vdupq_n_u32(0xFF000000u);
        for (; x + 4 <= n; x += 4) {
            uint32x4_t p = vorrq_u32(vld1q_u32(src + x), a);
            p = vrev64q_u32(p);
            p = vcombine_u32(vget_high_u32(p), vget_low_u32(p)); /* p3 p2 p1 p0 */
            const uint32x4x2_t z = vzipq_u32(p, p);
            uint32_t *o0 = d0 + (n - x - 4) * 2, *o1 = d1 + (n - x - 4) * 2;
            vst1q_u32(o0, z.val[0]); vst1q_u32(o0 + 4, z.val[1]);
            vst1q_u32(o1, z.val[0]); vst1q_u32(o1 + 4, z.val[1]);
        }
#endif
        for (; x < n; x++) {
            const uint32_t p = src[x] | 0xFF000000u;
            const int o = (n - 1 - x) * 2;
            d0[o] = d0[o + 1] = d1[o] = d1[o + 1] = p;
        }
        return;
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32x4_t a = vdupq_n_u32(0xFF000000u);
    for (; x + 4 <= n; x += 4) {
        const uint32x4_t p = vorrq_u32(vld1q_u32(src + x), a);
        const uint32x4x2_t z = vzipq_u32(p, p);
        vst1q_u32(d0 + x * 2, z.val[0]); vst1q_u32(d0 + x * 2 + 4, z.val[1]);
        vst1q_u32(d1 + x * 2, z.val[0]); vst1q_u32(d1 + x * 2 + 4, z.val[1]);
    }
#endif
    for (; x < n; x++) {
        const uint32_t p = src[x] | 0xFF000000u;
        d0[x * 2] = d0[x * 2 + 1] = d1[x * 2] = d1[x * 2 + 1] = p;
    }
}

/* Present thread: the staged frame onto the screen surface. 320x240 is doubled exactly; any
 * other size (never in play) is scaled nearest to fit. */
static void blit_stage_to_screen(void) {
    const int sw = sStageW, sh = sStageH, rot = gfx_miyoo_rotate();
    const uint32_t *const stage = sPresSrc ? sPresSrc : sStage32;
    if (sw <= 0 || sh <= 0) return;
    if (sw == BASE_W && sh == BASE_H && !sPresSrc) {
        if (!sStageSurf)
            sStageSurf = SDL_CreateRGBSurfaceFrom(sStage32, BASE_W, BASE_H, 32, BASE_W * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
        if (sStageSurf) miyoo_menu_overlay(sStageSurf); /* SAVED SLOT n, ...: on the picture, before the turn */
    }
    if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
    uint8_t *const pix = (uint8_t *)sScreen->pixels;
    const int pitch = sScreen->pitch;
    const SDL_PixelFormat *f = sScreen->format;
    if (f->BitsPerPixel == 32 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0 && sw * 2 == SCREEN_W && sh * 2 == SCREEN_H) {
        for (int y = 0; y < sh; y++) {
            const int oy = rot ? SCREEN_H - 2 - y * 2 : y * 2;
            row_x2_32((uint32_t *)(pix + oy * pitch), (uint32_t *)(pix + (oy + 1) * pitch), stage + y * sw, sw, rot);
        }
        if (sPresSrc) miyoo_menu_overlay_turned(sScreen, rot); /* the notice, on the screen (the frame is the renderer's) */
    } else if (f->BitsPerPixel == 32) {
        for (int y = 0; y < SCREEN_H; y++) {
            uint32_t *dst = (uint32_t *)(pix + (rot ? SCREEN_H - 1 - y : y) * pitch);
            const uint32_t *src = stage + (y * sh / SCREEN_H) * sw;
            for (int x = 0; x < SCREEN_W; x++) {
                const uint32_t p = src[(rot ? SCREEN_W - 1 - x : x) * sw / SCREEN_W];
                dst[x] = SDL_MapRGB(sScreen->format, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            }
        }
    } else { /* 16 bpp */
        for (int y = 0; y < SCREEN_H; y++) {
            uint16_t *dst = (uint16_t *)(pix + (rot ? SCREEN_H - 1 - y : y) * pitch);
            const uint32_t *src = stage + (y * sh / SCREEN_H) * sw;
            for (int x = 0; x < SCREEN_W; x++) dst[x] = px565_dither(src[(rot ? SCREEN_W - 1 - x : x) * sw / SCREEN_W], x, y);
        }
    }
    panel_dump();
    if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
}

/* The staged frame again, with the notification on it, at once: for a notice no later frame will
 * carry (miyoo_menu.c handheld_menu_notify_wait: SAVE FAILED right before the quit). */
void gfx_miyoo_present_now(void) {
    if (!sScreen || !sPresentLock) return;
    present_acquire();
    blit_stage_to_screen();
    SDL_Flip(sScreen);
    present_release();
}

static int present_thread(void *arg) {
    (void)arg;
    SDL_mutexP(sPresentLock);
    for (;;) {
        while (!sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        sPresentPending = 0;
        sPresentBusy = 1;
        if (!sPresentDirect) blit_stage_to_screen();
        sPresentDirect = 0;
        SDL_mutexV(sPresentLock);          /* the flip may block: never hold the lock */
        const uint32_t t0 = port_time_us();
        SDL_Flip(sScreen);
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
    { /* the game thread only copies the frame out (~0.3 ms); the scale and the flip run on the
       * present thread, on the Mini's second core */
        const uint32_t t0 = port_time_us();
        const int fw = rw;
        if (rw > BASE_W) rw = BASE_W; /* the native target is never bigger than the base */
        if (rh > BASE_H) rh = BASE_H;
        if (sPresCopy < 0) {
            const SDL_PixelFormat *f = sScreen->format;
            const char *e = getenv("GE_PRESCOPY");
            sPresCopy = (e && e[0] == '1') || !(f->BitsPerPixel == 32 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0);
        }
        SDL_mutexP(sPresentLock);
        while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        if (!sPresCopy && fw == BASE_W && rw == BASE_W && rh == BASE_H) {
            sPresSrc = fb; /* doubled from where the renderer drew it (see sPresSrc) */
        } else {
            sPresSrc = NULL;
            for (int y = 0; y < rh; y++) memcpy(sStage32 + y * rw, fb + y * fw, (size_t)rw * 4);
        }
        sStageW = rw; sStageH = rh;
        sPresentDirect = 0;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
    }
}

/* Take the screen for direct drawing (menu): wait until the thread is idle
 * and hold the lock so it stays idle until present_release(). */
static void present_acquire(void) {
    if (!sPresentLock) return;
    SDL_mutexP(sPresentLock);
    while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock);
}
static void present_release(void) { if (sPresentLock) SDL_mutexV(sPresentLock); }

/* A clean end for a process that is followed by another SDL process on the same screen (the
 * device bench's runs, hh_bench.c): the present thread idle and held, the panel back on page 1,
 * then SDL_Quit, which gives the console back its text mode and keyboard. Without it the next
 * run's SDL came up and hung at its first flip (the user: "went to black after one test"). *
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
#if defined(MIYOO)
    { /* GE_NOTICE_AT=<frame> (tests): a SAVED SLOT 1 notice from that presented frame on (the panel dumps' overlay) */
        static long at SAVESTATE_EXCLUDE = -2, n SAVESTATE_EXCLUDE;
        if (at == -2) { const char *e = getenv("GE_NOTICE_AT"); at = e && e[0] ? atol(e) : -1; }
        if (at >= 0 && n++ == at) handheld_menu_notify("SAVED SLOT 1");
    }
#endif
    present();

    { /* MK64_DUMP=<dir> [MK64_DUMP_EVERY=n]: PPM dumps of the render buffer
       * (same contract as the desktop backend; drives the qemu smoke test).
       * Counts rendered frames -- dropped frames never reach the swap. */
        extern void gfx_soft_dump_ppm(const char *path);
        static const char *sDumpDir SAVESTATE_EXCLUDE; static long sEvery SAVESTATE_EXCLUDE = -1; static long sFrame SAVESTATE_EXCLUDE;
        if (sEvery < 0) {
            sDumpDir = getenv("GE_DUMP");
            const char *e = getenv("GE_DUMP_EVERY");
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
