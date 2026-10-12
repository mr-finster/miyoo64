/**
 * Window backend: SDL 1.2 on the Miyoo Mini's 640x480 panel.
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format.
 * - Pacing is port_main.c's: the logic ticks at 60 Hz and pictures are
 *   skipped when behind; this window never sleeps or skips.
 * - The game draws the N64's own 320x240 (the 4:3 layout:
 *   no square-screen fill, HUD or menu changes, as the desktop proxy draws), and the present thread
 *   doubles and turns each 32-bit frame onto the 640x480 panel (miyoo/miyoo_panel.c). MENU is
 *   Escape (miyoo/miyoo_menu.c); SIGTERM / SIGHUP (the OS's power-off, any kill) take EXIT GAME's
 *   auto save and quit. The second core (miyoo/miyoo_worker.c) draws half
 *   of each 3D batch's rows (gfx_soft.c).
 */
#if defined(HANDHELD_DEVICE)

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
#include <ucontext.h>
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
#if defined(MIYOO)
#include "miyoo_panel.h"
#include "miyoo_worker.h"
#include "miyoo_clock.h"
#endif

/* Mirror of gfx_pc.h's GfxDimensions (that header drags in Gfx/ultra types).
 * gfx_pc reads it only at init; the res ladder pokes it directly. */
struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
extern struct GfxDimensions gfx_current_dimensions;

#if defined(MIYOO)
#define SCREEN_W 320 /* the N64's 4:3, doubled onto the panel */
#endif
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
static int present_thread(void *arg);
static uint16_t sStage[SCREEN_W * SCREEN_H] SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* The frame as drawn, 32 bits, which the present thread doubles and turns onto the panel
 * (miyoo_panel_present32): no RGB565 stage (GoldenEye's and DKR's Miyoo present). */
static uint32_t sStage32[SCREEN_W * SCREEN_H] SAVESTATE_EXCLUDE;
static volatile int sPinPresent SAVESTATE_EXCLUDE; /* the worker is on: the present thread moves to core 1 */
static volatile sig_atomic_t sTerm SAVESTATE_EXCLUDE; /* SIGTERM / SIGHUP: the OS is ending the game */
#endif
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;
/* Round 12: the picture converted straight into the screen surface (the present thread only
 * flips) instead of into sStage for the thread to copy. Every port's log shows ~2 ms of
 * "pres" a picture, even with an empty 3D scene. Default 2: direct, the source prefetched
 * ahead (round-12 device bench: 1.5 ms, direct 1.6, through sStage 2.0-2.1). FZX_PRES=0:
 * through sStage; 1: direct without the prefetch. */
static int sPresMode SAVESTATE_EXCLUDE = -1;
static int sScrDirect SAVESTATE_EXCLUDE = -1; /* FZX_SCRDIRECT=1: the composite into the screen (round 13c; off since its
                                                * device bench: +0.7 ms, the display surface is slow to write) */
static int sPresCheck SAVESTATE_EXCLUDE = -1; /* FZX_PRESCHECK=1: compare the screen with a full conversion (test) */
void gfx_wm_set_screen_direct(int on) { sScrDirect = on ? 1 : 0; } /* bench.c */
static volatile int sPresentDirect SAVESTATE_EXCLUDE; /* the pending picture is already in the surface */
void gfx_wm_set_present_mode(int mode) { sPresMode = mode; } /* bench.c (gfx_sdl.c: a no-op) */

static bool sDoRender SAVESTATE_EXCLUDE = true;
static int sSkipsInARow SAVESTATE_EXCLUDE;
static uint32_t sLastFrameMs SAVESTATE_EXCLUDE; /* pacing: wall clock, never restore */

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
#if defined(MIYOO)
    const int base_w = 320; /* the N64's own width at every display mode */
#endif
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

/* A FIXED internal 3D size, the ladder off. 3D RES left the menu (the user, 2026-09-24); the
 * game ran at 100 = native (the unified backend) until round 13, and at 80 since (the user's
 * pick for 60 fps: the 3D world drawn at 192x192 and scaled up bilinearly under the crisp
 * native 2D/HUD). FZX_INTRES and bench.c set other sizes for A/B. */
void gfx_sdl12_set_res_pct(int pct) {
    int best = 0;
    for (int i = 0; i < RES_STEPS; i++) if (abs(sResPct[i] - pct) < abs(sResPct[best] - pct)) best = i;
    if (sResEnvForced && sResEnabled) return; /* MK64_DYNRES=1 pins the ladder for testing */
    sResEnabled = 0;
    apply_res(best);
}
int port_res3d_pct(void) { return sResPct[sResIdx]; }
void port_res3d_set(int pct) { gfx_sdl12_set_res_pct(pct); }

/* SDL_TRIPLEBUF flips by panning /dev/fb0 across three pages. A process that
 * dies panned to page 2 or 3 leaves the display there, and the next app
 * draws into page 1 while the panel shows another page: a "split"
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
/* A crash leaves one line in the log: the signal, the registers that matter
 * and up to 12 stack words that point into the code (likely return addresses).
 * The binary is not position-independent, so `addr2line -f -e fzerox` on the
 * unstripped build names them. Only write() here: this runs in signal context,
 * on its own stack so that a stack overflow still reports. */
extern int port_log_fd(void);
extern void _init(void); /* code starts here: below are the ELF headers and symbol tables */
extern char etext[];
static char sCrashStack[16384] SAVESTATE_EXCLUDE;

static char *crash_hex(char *p, unsigned long v) {
    for (int s = 28; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(v >> s) & 15];
    return p;
}
static char *crash_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}
static void crash_write(const char *buf, size_t n) {
    const int fd = port_log_fd();
    if (fd >= 0) (void) !write(fd, buf, n);
    (void) !write(2, buf, n);
}

static void fatal_handler(int sig, siginfo_t *si, void *ucv) {
    const ucontext_t *uc = ucv;
    const unsigned long sp = uc->uc_mcontext.arm_sp;
    char buf[256], *p = buf;
    p = crash_str(p, "FATAL: signal ");
    *p++ = (char)('0' + sig / 10); *p++ = (char)('0' + sig % 10);
    p = crash_str(p, " pc "); p = crash_hex(p, uc->uc_mcontext.arm_pc);
    p = crash_str(p, " lr "); p = crash_hex(p, uc->uc_mcontext.arm_lr);
    p = crash_str(p, " sp "); p = crash_hex(p, sp);
    p = crash_str(p, " addr "); p = crash_hex(p, (unsigned long) si->si_addr);
    *p++ = '\n';
    crash_write(buf, (size_t)(p - buf)); /* first, in case the stack scan faults */
    p = crash_str(buf, "FATAL: stack");
    int found = 0;
    for (unsigned long a = sp & ~3ul; a < (sp & ~3ul) + 2048 && found < 12; a += 4) {
        const unsigned long w = *(const unsigned long *) a;
        if (w >= (unsigned long) _init && w < (unsigned long) etext) {
            *p++ = ' '; p = crash_hex(p, w); found++;
        }
    }
    *p++ = '\n';
    crash_write(buf, (size_t)(p - buf));
    fb_unpan();
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock the OS had (miyoo_clock.c: open/mmap/write only) */
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

static void fatal_handlers_install(void) {
    stack_t ss;
    struct sigaction sa;
    ss.ss_sp = sCrashStack; ss.ss_size = sizeof(sCrashStack); ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fatal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);  sigaction(SIGABRT, &sa, NULL);
}

/* The Miyoo Mini: Onion and the stock OS end a game with SIGTERM (then a kill up to 30 s later),
 * Allium with SIGTERM then SIGKILL after 5 s, MinUI and spruce with nothing or a plain kill. A
 * SIGTERM or SIGHUP takes EXIT GAME's path at the next retrace (the auto save, then quit:
 * handle_events). */
int handheld_power_pending(void) { return sTerm; } /* the menu and the resume prompt step aside */

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name; (void)start_in_fullscreen;
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
#if defined(MIYOO)
    sScreen = miyoo_panel_open(); /* 32 bpp, the panel's own size (miyoo_panel.c) */
#endif
    if (!sScreen) {
        dprintf(2, "gfx_sdl12: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        exit(1);
    }
    { /* Resolution: FULL 240 by default (device round 7: native res is now
       * cheap -- the unified backend drops the composite at 240 -- so lock it
       * and never change resolution. The DYNAMIC ladder is a menu option for
       * a track that can't hold 30 at native. MK64_DYNRES overrides for tests:
       * =1 forces dynamic, =0 forces full; unset uses the saved setting
       * (loaded by handheld_menu_init -> gfx_sdl12_set_res_dynamic). */
        const char *e = getenv("FZX_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    apply_res(0);
    atexit(fb_unpan);
    fatal_handlers_install();
#if defined(MIYOO)
    miyoo_term_install(&sTerm); /* the OS's power-off and any plain kill: EXIT GAME's save */
#endif
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) dprintf(2, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen);
    { /* test overrides (qemu profiles, A/B runs), as the desktop backend reads them;
       * after handheld_menu_init, which applies the saved menu settings */
        /* round 13 (the user, 2026-09-26: 60 fps, "80% + 3-tap, then grind"): the 3D at 80%,
         * scaled up smoothly under the full-size 2D (gfx_soft.c composite_bilinear) */
        /* 2026-10-03: back to 100% (the user, after a device test of branch fzx-res100: "good at 100%,
         * optimise more later"); FZX_INTRES=80 for round 13's size. */
        const char *e = getenv("FZX_INTRES");
        gfx_sdl12_set_res_pct(e && e[0] ? atoi(e) : 100);
        extern int gfx_soft_bilinear;
        e = getenv("FZX_BILINEAR");
        if (e && e[0]) gfx_soft_bilinear = (e[0] == '1');
        extern int gfx_soft_magbilin;
        e = getenv("FZX_MAGBILIN");
        if (e && e[0]) gfx_soft_magbilin = (e[0] == '1');
    }
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("FZX_MENU_TEST");
        if (td && td[0]) { handheld_menu_test_dump(td); exit(0); }
    }
#if defined(MIYOO)
    /* The second core (gfx_soft.c draws half of each 3D batch's rows on it): started last, so the
     * present and audio threads made before keep both cores; the game thread is pinned to core 0, the
     * worker to core 1, and the present thread joins the worker there (sPinPresent). FZX_THREADS=1:
     * one core; with one core online it stays off by itself. */
    if (miyoo_worker_start()) sPinPresent = 1;
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

/* savestate.c's FZX_MENU_AT (test hook, statefix B): two MENU presses queued, so the next event poll opens
 * the handheld menu and the menu's own poll closes it (RESUME). */
void port_test_menu_press(void) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
#if defined(MIYOO)
    ev.key.keysym.sym = MIYOO_KEY_MENU;
#endif
    SDL_PushEvent(&ev);
    SDL_PushEvent(&ev);
}

static void gfx_sdl12_handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
#if defined(MIYOO)
                if (event.key.keysym.sym == MIYOO_KEY_MENU) { /* MENU button (Escape) */
#endif
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
#if defined(MIYOO)
    if (sTerm == 1) {
        sTerm = 2; /* once; handheld_power_pending stays true */
        port_log("gfx_sdl12: SIGTERM: auto save, then quit\n");
        savestate_request_exit_save(); /* EXIT GAME's path: the auto save, then quit */
    }
#endif
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

/* F-Zero X: frame pacing is the host loop's (port_main.c: 60 Hz ticks, pictures
 * skipped when behind), so the window never sleeps or skips here. (MK64's
 * ladder and deadline pacer above stay for reference; unused.) */
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
#if defined(MIYOO)
        if (sPinPresent) { sPinPresent = 0; miyoo_worker_pin_self(1); } /* core 1, beside the worker: core 0 is the game's */
        miyoo_panel_present32(sStage32, SCREEN_W, SCREEN_H); /* doubled, turned, the notice drawn in */
#endif
        sPresentDirect = 0;
        SDL_mutexV(sPresentLock);          /* the flip may block: never hold the lock */
        const uint32_t t0 = port_time_us();
#if defined(MIYOO)
        miyoo_panel_flip();
#endif
        gPortProfFlipUs += port_time_us() - t0;
        SDL_mutexP(sPresentLock);
        sPresentBusy = 0;
        SDL_CondBroadcast(sPresentCond);
    }
    return 0;
}

/* The composite straight into the screen (round 13b, gfx_soft.c composite_bilinear): the
 * surface the direct present would write, after the same wait for the last flip, locked until
 * this frame's present. NULL where the direct present doesn't apply (staging, CROP, 32 bpp,
 * a picture not 1:1 with the screen). */
static int sScrAcquired SAVESTATE_EXCLUDE;
uint16_t *gfx_wm_screen_acquire(int *pitch_px) {
    int rw = 0, rh = 0;
    if (sPresMode < 0) { const char *e = getenv("FZX_PRES"); sPresMode = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 2; }
    if (sScrDirect < 0) { const char *e = getenv("FZX_SCRDIRECT"); sScrDirect = e && e[0] == '1'; }
    if (!sScrDirect || !sPresMode || !sScreen || sScreen->format->BitsPerPixel != 16 || gHandheldDisplayMode == HH_DISPLAY_CROP) return NULL;
    (void)gfx_soft_get_framebuffer(&rw, &rh);
    if (rw != SCREEN_W || rh != SCREEN_H) return NULL;
    if (!sScrAcquired) {
        const uint32_t t0 = port_time_us();
        SDL_mutexP(sPresentLock);
        while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PWAIT, port_time_us() - t0);
        if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
        sScrAcquired = 1;
    }
    *pitch_px = sScreen->pitch / 2;
    return (uint16_t *)sScreen->pixels;
}
void gfx_wm_row_to_565(uint16_t *dst, const uint32_t *src, int n) { argb_row_to_565(dst, src, n); }

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
    if (rw == SCREEN_W && rh == SCREEN_H) { /* the 32-bit frame for the present thread (always: 320x240, 3D at 100%) */
        SDL_mutexP(sPresentLock);
        while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock); /* the last frame still unread */
        PORT_PROF_ADD(PROF_PWAIT, port_time_us() - t0);
        memcpy(sStage32, fb, sizeof(sStage32));
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
    { /* any other size (never so far): scaled to 320x240, nearest */
        SDL_mutexP(sPresentLock);
        while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        for (int y = 0; y < SCREEN_H; y++)
            for (int x = 0; x < SCREEN_W; x++) sStage32[y * SCREEN_W + x] = fb[(y * rh / SCREEN_H) * rw + x * rw / SCREEN_W];
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
#endif
    if (sScrAcquired) {
        /* the composite wrote the screen: convert what 2D drew over it (or all, sScrAll) */
        extern int gfx_soft_screen_blocks(const uint16_t **blk);
        const uint16_t *blk = NULL;
        const int part = gfx_soft_screen_blocks(&blk);
        uint8_t *const pix = (uint8_t *)sScreen->pixels;
        for (int y = 0; y < SCREEN_H; y++) {
            uint16_t *d = (uint16_t *)(pix + y * sScreen->pitch);
            const uint32_t *r = fb + y * rw;
            if (!part) { argb_row_to_565(d, r, SCREEN_W); continue; }
            uint32_t m = blk[y];
            while (m) { /* each run of marked blocks */
                const int b0 = __builtin_ctz(m);
                int b1 = b0;
                while ((m >> (b1 + 1)) & 1u) b1++;
                m &= ~(((2u << b1) - 1u) & ~((1u << b0) - 1u));
                const int x0 = b0 * 16, x1 = (b1 + 1) * 16 > SCREEN_W ? SCREEN_W : (b1 + 1) * 16;
                argb_row_to_565(d + x0, r + x0, x1 - x0);
            }
        }
        if (sPresCheck < 0) { const char *e = getenv("FZX_PRESCHECK"); sPresCheck = e && e[0] == '1'; }
        if (sPresCheck) { /* test: the screen must equal a full conversion */
            static uint16_t chk[SCREEN_W];
            static unsigned frames, bad;
            int diff = 0;
            for (int y = 0; y < SCREEN_H; y++) {
                argb_row_to_565(chk, fb + y * rw, SCREEN_W);
                diff += memcmp(chk, pix + y * sScreen->pitch, SCREEN_W * 2) != 0;
            }
            frames++; bad += diff != 0;
            if (diff || (frames % 300) == 0) port_log("PRESCHECK %u frames, %u differ (this one: %d rows, partial %d)\n", frames, bad, diff, part);
        }
        if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
        sScrAcquired = 0;
        SDL_mutexP(sPresentLock);
        sPresentDirect = 1;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
    if (sPresMode < 0) { const char *e = getenv("FZX_PRES"); sPresMode = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 2; }
    const int direct = (win_w == SCREEN_W && rh == SCREEN_H); /* 1:1 (native convert / crop@native) */
    const int surf = sPresMode && direct && sScreen->format->BitsPerPixel == 16;
    SDL_mutexP(sPresentLock);
    /* the staging buffer still unread; for the surface, also a flip in progress (it moves
     * sScreen->pixels to the next buffer) */
    while (sPresentPending || (surf && sPresentBusy)) SDL_CondWait(sPresentCond, sPresentLock);
    PORT_PROF_ADD(PROF_PWAIT, port_time_us() - t0);
    if (surf) {
        if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
        if (sScreen->pitch == SCREEN_W * 2 && rw == SCREEN_W && x0 == 0) {
            argb_row_to_565((uint16_t *)sScreen->pixels, fb, SCREEN_W * SCREEN_H); /* one run: rows are contiguous */
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
    SDL_mutexP(sPresentLock);
    while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock);
}
static void present_release(void) { if (sPresentLock) SDL_mutexV(sPresentLock); }

/* The way out, every exit (port_platform_quit; cross-port X50, from ocarina-of-time): the present
 * thread idle and held, the console unpanned, SDL shut down. Without SDL_Quit, SDL 1.2's fbcon
 * leaves the console, keyboard and video modes as the game set them, and the next SDL program
 * (the launcher after EXIT GAME) can hang at its first flip, as GoldenEye's second bench run did. *
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
            sDumpDir = getenv("FZX_DUMP");
            const char *e = getenv("FZX_DUMP_EVERY");
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
