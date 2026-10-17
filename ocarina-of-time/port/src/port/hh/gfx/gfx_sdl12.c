/**
 * Window backend: SDL 1.2 on the Miyoo Mini's 640x480 32 bpp panel. The game renders its own
 * 320x240 and the present thread doubles it (turned 180 degrees as the panel is mounted).
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format. A dynamic-resolution ladder (RES_LADDER)
 *   renders smaller under load and NN-upscales to 240 in the same pass.
 * - Pacing: OoT's scheduler paces each frame (hh_sched.c, 16.67 ms x the
 *   game's update rate); start_frame here never sleeps or skips (MK64's
 *   30 fps pacer below is kept for reference, unused).
 * - MENU is Escape, miyoo_menu.c is the overlay, and SIGTERM (the OS shutting
 *   the app down) exit-saves.
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
#include <ucontext.h>
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

/* The output size: 320x240 on the Miyoo Mini (the square size elsewhere, or 320x240 with
 * screen=320x240 in debug.cfg; hh_screen.c). Set once in gfx_sdl12_init, before anything draws. */
extern int HhScreen_W(void), HhScreen_H(void);
extern void HhScreen_Confirm(int w, int h);
static int sScrW SAVESTATE_EXCLUDE = 240, sScrH SAVESTATE_EXCLUDE = 240;
#define SCREEN_W sScrW
#define SCREEN_H sScrH
#define SCREEN_MAX_W 320
#define SCREEN_MAX_H 240
#if defined(MIYOO)
/* Miyoo Mini: the game's screen is 320x240 (hh_screen.c: the N64's own 4:3, so the projections
 * and the HUD stay as laid out) on a 640x480 panel, which the present thread doubles onto on the
 * second core (GoldenEye's Miyoo present, goldeneye-007 48fb705). */
#define PANEL_W 640
#define PANEL_H 480
#define BASE_W 320
#define BASE_H 240
#endif
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
static uint32_t sStage32[BASE_W * BASE_H] SAVESTATE_EXCLUDE; /* the frame at the render size */
static int sStageW SAVESTATE_EXCLUDE, sStageH SAVESTATE_EXCLUDE;
#endif
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;
/* The present (FZX bbeb879 / GE's port of it; FZX's device bench: staging 2.0-2.1 ms a picture, direct
 * 1.6, direct + prefetch 1.5): default 2 converts a native square picture straight into the 16-bit
 * screen surface, the source prefetched 512 bytes ahead, instead of into sStage for the present
 * thread to copy. OOT_PRES=0: through sStage as before; 1: direct, no prefetch. The same pixels. */
static int sPresMode SAVESTATE_EXCLUDE = -1;
static volatile int sPresentDirect SAVESTATE_EXCLUDE;
/* Show each frame on its 50 ms tick (hh_sched.c's deadline for it), not when its render ends. The
 * pacer spaces frame STARTS 50 ms apart; flipping at render completion put frames on screen
 * 50 + (this render - the last one) ms apart, 35-65 ms when render times swing between 20 and
 * 40 ms. The user's v25 session: 46 of 148 over-60-ms "hitches" were frames drawn in time and
 * shown late; the short gaps between them weren't counted at all. A late frame still shows at
 * once. debug.cfg presenttick=0: flip at render completion (A/B). */
static volatile uint32_t sPresentAt SAVESTATE_EXCLUDE;
static volatile int sPresentTimed SAVESTATE_EXCLUDE;
static int sPresentTick SAVESTATE_EXCLUDE = -1;
/* On-screen spacing for the PERF line (gfx_flip_stats): the longest gap between flips and the
 * gaps over 60 ms, since the last read. */
static uint32_t sLastFlipUs SAVESTATE_EXCLUDE, sFlipMaxUs SAVESTATE_EXCLUDE, sFlipLate SAVESTATE_EXCLUDE;
void gfx_flip_stats(uint32_t *max_us, uint32_t *late) {
    *max_us = __atomic_exchange_n(&sFlipMaxUs, 0u, __ATOMIC_RELAXED);
    *late = __atomic_exchange_n(&sFlipLate, 0u, __ATOMIC_RELAXED);
}

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
 * composites the two (gfx_soft.c). OOT_DYNRES=0 disables the ladder.
 * OoT: no 3D options (user, 2026-09-24): the 3D always renders at 100%;
 * the ladder and the lower rungs are developer A/B switches only
 * (OOT_DYNRES, OOT_INTRES), never a menu item or a hotkey. */
static const int sResPct[] = { 100, 95, 90, 85, 80, 75, 70, 65, 60 };
#define RES_STEPS ((int)(sizeof(sResPct) / sizeof(sResPct[0])))
static int sResIdx SAVESTATE_EXCLUDE;     /* current ladder position */
static int sResEnabled SAVESTATE_EXCLUDE; /* OOT_DYNRES=0 disables the ladder */

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

/* MK64's old menu "RESOLUTION": FULL (locked native, default) vs DYNAMIC (the ladder). */
static int sResEnvForced SAVESTATE_EXCLUDE;
void gfx_sdl12_set_res_dynamic(int on) {
    if (sResEnvForced) return; /* OOT_DYNRES pins it for testing */
    sResEnabled = on ? 1 : 0;
    if (!sResEnabled) apply_res(0); /* back to native and stay there */
}
int gfx_sdl12_get_res_dynamic(void) { return sResEnabled; }

/* MK64's old menu "3D RES" (device round 14; OoT has none, see above): a FIXED internal 3D size the user steps
 * through by hand -- the ladder stays off. 100 = native (unified backend, the
 * cheapest rung); anything lower renders the 3D world smaller and nearest-
 * upscales it under the crisp native 2D/HUD, at a ~2 ms composite cost, so 95
 * is not worth it -- the first useful step is 90. Persisted as intres= in
 * mk64_display.cfg; the shipped default is 85 (chosen on hardware, round 15). */
void gfx_sdl12_set_res_pct(int pct) {
    int best = 0;
    for (int i = 0; i < RES_STEPS; i++) if (abs(sResPct[i] - pct) < abs(sResPct[best] - pct)) best = i;
    if (sResEnvForced && sResEnabled) return; /* OOT_DYNRES=1 pins the ladder for testing */
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
/* A fatal signal leaves one line in console.log (stderr is the log on the device): the signal, the
 * iteration, and on ARM where it happened -- pc, lr (usually the caller) and the faulting address.
 * The binary is non-PIE and keeps its symbols, so `addr2line -f -e oot <pc> <lr>` names them.
 * The device build installs nothing else for these signals (hh_main.c's backtrace is the desktop
 * build's), so a crash on the device left nothing: the log just stopped. Only write() and
 * arithmetic here (signal context), then the panel unpanned and the default action, as before.
 * The same handler as DKR cd3649a. */
extern uint32_t port_frame_count(void);
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

#if defined(MIYOO)
/* Onion / the stock OS end an app with SIGTERM
 * (the power button's shut down): the handler asks the game loop for an exit save and a quit,
 * as EXIT GAME does, so RESUME GAME picks up there next time. */
static volatile sig_atomic_t sTermRequested SAVESTATE_EXCLUDE;
static void term_handler(int sig) { (void)sig; sTermRequested = 1; }
int gfx_miyoo_term_pending(void) { return sTermRequested; } /* miyoo_menu.c: the menu and the resume prompt step aside */
#endif /* MIYOO */

/* 16bpp, else the 32bpp fallback */
static SDL_Surface *set_video_mode(int w, int h) {
#if defined(MIYOO)
    /* the Mini's SDL gives a 16 bpp mode but shows it as streaks; 32 bpp is the panel's own */
    (void)w; (void)h;
    SDL_Surface *s = NULL;
    { /* Mini v4 / Mini Flip: a 750x560 panel (752x560 framebuffer). OOT_FLIPFILL (default on): the panel opened at
       * /dev/fb0's visible size (OOT_MIYOO_MODE=<w>x<h> forces one, tests) and the picture scaled to its full height
       * (blit_stage_to_screen); a Mini's 640x480 keeps the exact 2x. OOT_FLIPFILL=0: 640x480 as before (the OS
       * shows it with a border). */
        int mw = 0, mh = 0;
        const char *e = getenv("OOT_FLIPFILL");
        const int fill = !(e && e[0] == '0');
        const char *m = getenv("OOT_MIYOO_MODE");
        if (m && sscanf(m, "%dx%d", &mw, &mh) != 2) mw = mh = 0;
        if (!mw) {
            const int fd = open("/dev/fb0", O_RDONLY);
            if (fd >= 0) {
                struct fb_var_screeninfo vi;
                if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0) { mw = (int)vi.xres; mh = (int)vi.yres; }
                close(fd);
            }
        }
        fprintf(stderr, "gfx_sdl12: panel %dx%d%s\n", mw, mh, m ? " (OOT_MIYOO_MODE)" : " (/dev/fb0)");
        if (fill && mw >= PANEL_W && mh >= PANEL_H && (mw != PANEL_W || mh != PANEL_H) && mw <= 1280 && mh <= 960) {
            s = SDL_SetVideoMode(mw, mh, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
            if (!s) fprintf(stderr, "gfx_sdl12: %dx%d refused (%s); 640x480\n", mw, mh, SDL_GetError());
        }
    }
    if (!s) s = SDL_SetVideoMode(PANEL_W, PANEL_H, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!s) s = SDL_SetVideoMode(PANEL_W, PANEL_H, 16, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (s) fprintf(stderr, "gfx_sdl12: %dx%d, %d bpp, pitch %d\n", s->w, s->h, s->format->BitsPerPixel, s->pitch);
    return s;
#endif
}
#if defined(MIYOO)
/* hh_romscreen.c: the first-start screens open the panel as the game does (a Mini v4 / Flip: its own 752x560, not
 * a 640x480 the OS shows boxed), so they fill it the same way and the game's mode switch keeps the size. */
SDL_Surface *gfx_miyoo_open_panel(void) { return set_video_mode(0, 0); }
#endif

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name; (void)start_in_fullscreen;
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    SDL_Init(SDL_INIT_VIDEO);
    { /* debug.cfg bilinear=0 / magbilin=0 (-> OOT_*): the desktop backend's A/B switches */
        extern int gfx_soft_bilinear, gfx_soft_magbilin;
        const char *b = getenv("OOT_BILINEAR");
        if (b && b[0]) gfx_soft_bilinear = (b[0] == '1');
        b = getenv("OOT_MAGBILIN");
        if (b && b[0]) gfx_soft_magbilin = (b[0] == '1');
    }
    SDL_ShowCursor(SDL_DISABLE);
    sScrW = HhScreen_W(); sScrH = HhScreen_H();
    sScreen = set_video_mode(SCREEN_W, SCREEN_H);
#if defined(MIYOO)
    if (0) {
#endif
        /* the display can't do 320x240: back to the handheld size */
        HhScreen_Confirm(sScreen ? sScreen->w : 0, sScreen ? sScreen->h : 0);
        sScrW = HhScreen_W(); sScrH = HhScreen_H();
        sScreen = set_video_mode(SCREEN_W, SCREEN_H);
    }
    if (!sScreen) {
        fprintf(stderr, "gfx_sdl12: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        exit(1);
    }
    { /* Resolution: FULL 240 by default (device round 7: native res is now
       * cheap -- the unified backend drops the composite at 240 -- so lock it
       * and never change resolution. The DYNAMIC ladder is a menu option for
       * a track that can't hold 30 at native. OOT_DYNRES overrides for tests:
       * =1 forces dynamic, =0 forces full; unset uses the saved setting
       * (loaded by handheld_menu_init -> gfx_sdl12_set_res_dynamic). */
        const char *e = getenv("OOT_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    apply_res(0);
    atexit(fb_unpan);
    fatal_handlers_install();
#if defined(MIYOO)
    signal(SIGTERM, term_handler); /* the OS shutting the game down: exit save, then quit */
#endif
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) fprintf(stderr, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen);
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("OOT_MENU_TEST");
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

/* hh_main.c's OOT_MENU_AT (test hook, statefix B): two MENU presses queued, so the next event poll opens the
 * menu and the menu's own poll closes it (RESUME). */
void port_test_menu_press(void) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = SDLK_q;
    SDL_PushEvent(&ev);
    SDL_PushEvent(&ev);
}

static void gfx_sdl12_handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
#if defined(MIYOO)
                if (event.key.keysym.sym == SDLK_ESCAPE) { /* MENU button */
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
    if (sTermRequested) {
        sTermRequested = 0;
        fprintf(stderr, "gfx_sdl12: SIGTERM, exit save\n");
        savestate_request_exit_save();
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

/* ARGB8888 (render res) -> RGB565 staging -> screen, on a PRESENT THREAD.
 * The game thread converts the frame into a staging buffer (one generic NN
 * mapper covers all modes: CONVERT/STRETCH use the full source width, CROP a
 * centred 240/320 window) and signals; the thread copies staging to the video
 * surface and calls SDL_Flip. Whatever SDL_Flip does on this SDL (device
 * round 5: near-empty menu frames measured 22 ms of "render", i.e. the flip
 * was waiting for the panel's vsync inside the frame) now costs the game
 * thread nothing -- on a single core a blocked thread is free. The menu takes
 * the screen directly, under the same lock, once the thread is idle. */

#if defined(MIYOO)
/* Turned 180 degrees on the way out (the user's Mini Plus on Onion, 2026-09-30: the picture was
 * upside down -- that SDL hands us the panel as mounted). OOT_ROTATE=0 (debug.cfg rotate=0): as SDL gives it.
 * miyoo_menu.c turns its screens the same way. */
static int sRotate SAVESTATE_EXCLUDE = -1;
int gfx_miyoo_rotate(void) {
    if (sRotate < 0) { const char *e = getenv("OOT_ROTATE"); sRotate = !(e && e[0] == '0'); }
    return sRotate;
}
static SDL_Surface *sStageSurf SAVESTATE_EXCLUDE; /* sStage32 as a surface: the notification is drawn on it */

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
    if (sw <= 0 || sh <= 0) return;
    if (sw == BASE_W && sh == BASE_H) {
        if (!sStageSurf)
            sStageSurf = SDL_CreateRGBSurfaceFrom(sStage32, BASE_W, BASE_H, 32, BASE_W * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
        if (sStageSurf) miyoo_menu_overlay(sStageSurf); /* SAVED SLOT n, ...: on the picture, before the turn */
    }
    if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
    uint8_t *const pix = (uint8_t *)sScreen->pixels;
    const int pitch = sScreen->pitch;
    const SDL_PixelFormat *f = sScreen->format;
    if (f->BitsPerPixel == 32 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0 && sw * 2 == PANEL_W && sh * 2 == PANEL_H &&
        sScreen->w == PANEL_W && sScreen->h == PANEL_H) {
        for (int y = 0; y < sh; y++) {
            const int oy = rot ? PANEL_H - 2 - y * 2 : y * 2;
            row_x2_32((uint32_t *)(pix + oy * pitch), (uint32_t *)(pix + (oy + 1) * pitch), sStage32 + y * sw, sw, rot);
        }
    } else if (f->BitsPerPixel == 32 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0 &&
               (sScreen->w != PANEL_W || sScreen->h != PANEL_H)) {
        /* A larger panel (Mini v4 / Flip, OOT_FLIPFILL): the 4:3 frame at the panel's full height, centred (752x560:
         * the whole 752x560, 0.8% wider than 4:3), nearest by precomputed maps (the 2.33x has no exact form; bilinear would blur the HUD).
         * The black sides are written every frame (cheap, and a menu may have drawn there). */
        static int mapW SAVESTATE_EXCLUDE, mapH SAVESTATE_EXCLUDE, mapSw SAVESTATE_EXCLUDE, mapSh SAVESTATE_EXCLUDE;
        static uint16_t xmap[1280] SAVESTATE_EXCLUDE, ymap[960] SAVESTATE_EXCLUDE;
        static int dw SAVESTATE_EXCLUDE, dx SAVESTATE_EXCLUDE;
        const int W = sScreen->w, H = sScreen->h;
        if (mapW != W || mapH != H || mapSw != sw || mapSh != sh) {
            dw = H * sw / sh; if (dw > W || (W - dw) * 50 <= W) dw = W; /* a gap of 2% or less: the full width */
            dx = (W - dw) / 2;
            for (int x = 0; x < dw; x++) xmap[x] = (uint16_t)((x * sw + sw / 2) / dw < sw ? (x * sw + sw / 2) / dw : sw - 1);
            for (int y = 0; y < H; y++) ymap[y] = (uint16_t)((y * sh) / H);
            mapW = W; mapH = H; mapSw = sw; mapSh = sh;
            fprintf(stderr, "gfx_sdl12: %dx%d frame to %dx%d at x %d on the %dx%d panel\n", sw, sh, dw, H, dx, W, H);
        }
        for (int y = 0; y < H; y++) {
            uint32_t *dst = (uint32_t *)(pix + (rot ? H - 1 - y : y) * pitch);
            const uint32_t *src = sStage32 + ymap[y] * sw;
            for (int x = 0; x < dx; x++) dst[x] = 0xFF000000u;
            for (int x = dx + dw; x < W; x++) dst[x] = 0xFF000000u;
            if (rot) { for (int x = 0; x < dw; x++) dst[W - 1 - dx - x] = src[xmap[x]] | 0xFF000000u; }
            else     { for (int x = 0; x < dw; x++) dst[dx + x] = src[xmap[x]] | 0xFF000000u; }
        }
    } else if (f->BitsPerPixel == 32) {
        for (int y = 0; y < PANEL_H; y++) {
            uint32_t *dst = (uint32_t *)(pix + (rot ? PANEL_H - 1 - y : y) * pitch);
            const uint32_t *src = sStage32 + (y * sh / PANEL_H) * sw;
            for (int x = 0; x < PANEL_W; x++) {
                const uint32_t p = src[(rot ? PANEL_W - 1 - x : x) * sw / PANEL_W];
                dst[x] = SDL_MapRGB(sScreen->format, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            }
        }
    } else { /* 16 bpp */
        for (int y = 0; y < PANEL_H; y++) {
            uint16_t *dst = (uint16_t *)(pix + (rot ? PANEL_H - 1 - y : y) * pitch);
            const uint32_t *src = sStage32 + (y * sh / PANEL_H) * sw;
            for (int x = 0; x < PANEL_W; x++) {
                const uint32_t p = src[(rot ? PANEL_W - 1 - x : x) * sw / PANEL_W];
                dst[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
            }
        }
    }
    if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
}
#endif /* MIYOO */

static int present_thread(void *arg) {
    (void)arg;
    SDL_mutexP(sPresentLock);
    for (;;) {
        while (!sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        sPresentBusy = 1;
        if (sPresentTimed) { /* wait for the frame's tick with the lock released; pending stays set,
                              * so the game thread can't overwrite the staging buffer meanwhile */
            const uint32_t at = sPresentAt;
            SDL_mutexV(sPresentLock);
            const int32_t w = (int32_t)(at - port_time_us());
            if (w > 0 && w < 100000) usleep((useconds_t)w);
            SDL_mutexP(sPresentLock);
        }
        sPresentPending = 0;
        if (!sPresentDirect) blit_stage_to_screen(); /* direct: the game thread wrote the surface */
        sPresentDirect = 0;
        SDL_mutexV(sPresentLock);          /* the flip may block: never hold the lock */
        const uint32_t t0 = port_time_us();
        SDL_Flip(sScreen);
        {
            const uint32_t t1 = port_time_us();
            gPortProfFlipUs += t1 - t0;
            if (sLastFlipUs != 0) {
                const uint32_t d = t1 - sLastFlipUs;
                if (d > __atomic_load_n(&sFlipMaxUs, __ATOMIC_RELAXED)) __atomic_store_n(&sFlipMaxUs, d, __ATOMIC_RELAXED);
                if (d > 60000) __atomic_fetch_add(&sFlipLate, 1u, __ATOMIC_RELAXED);
            }
            sLastFlipUs = t1;
        }
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
#if defined(MIYOO)
    { /* the game thread only copies the frame out (~0.3 ms); the scale and the flip run on the
       * present thread, on the Mini's second core */
        const uint32_t t0 = port_time_us();
        const int fw = rw;
        if (rw > BASE_W) rw = BASE_W; /* the native target is never bigger than the base */
        if (rh > BASE_H) rh = BASE_H;
        SDL_mutexP(sPresentLock);
        while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        for (int y = 0; y < rh; y++) memcpy(sStage32 + y * rw, fb + y * fw, (size_t)rw * 4);
        sStageW = rw; sStageH = rh;
        sPresentDirect = 0;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
    }
#endif /* MIYOO */
}

/* Take the screen for direct drawing (menu): wait until the thread is idle
 * and hold the lock so it stays idle until present_release(). */
static void present_acquire(void) {
    if (!sPresentLock) return;
    SDL_mutexP(sPresentLock);
    while (sPresentPending || sPresentBusy) SDL_CondWait(sPresentCond, sPresentLock);
}
static void present_release(void) { if (sPresentLock) SDL_mutexV(sPresentLock); }

static void gfx_sdl12_swap_buffers_begin(void) {
    present();

    { /* OOT_DUMP=<dir> [OOT_DUMP_EVERY=n]: PPM dumps of the render buffer
       * (same contract as the desktop backend; drives the qemu smoke test).
       * Counts rendered frames -- dropped frames never reach the swap. */
        extern void gfx_soft_dump_ppm(const char *path);
        static const char *sDumpDir SAVESTATE_EXCLUDE; static long sEvery SAVESTATE_EXCLUDE = -1; static long sFrame SAVESTATE_EXCLUDE;
        if (sEvery < 0) {
            sDumpDir = getenv("OOT_DUMP");
            const char *e = getenv("OOT_DUMP_EVERY");
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

/* Every exit ends here (hh_main.c, the bench's child runs too): the present thread idle and held, the console
 * unpanned, SDL shut down -- GoldenEye's second bench run hung at its first flip without it. *
 * X50 (the same in every port): returns 1 once SDL is down, 0 if a flip was still running after
 * ~0.5 s (SDL left up rather than pulled from under it). The watchdog first (SSB64's port_exit):
 * SIGALRM's default action ends the process, so a flip or an SDL_Quit that never returns can't
 * keep the launcher from coming back. Never released: the process ends right after. */
int gfx_wm_shutdown(void) {
    static int sDone = -1;
    sigset_t s;

    if (sDone >= 0) return sDone; /* the bench's way out, then the exit's */
#if defined(MIYOO)
    { /* the CPU clock the OS had (kit miyoo_clock; atexit does it too, for every other way out) */
        extern void miyoo_clock_restore(void);
        miyoo_clock_restore();
    }
#endif
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
