/**
 * Miyoo Mini window backend: SDL 1.2, 16bpp with a 32bpp fallback, triple buffering.
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format. A dynamic-resolution ladder (RES_LADDER)
 *   renders smaller under load and NN-upscales in the same pass.
 * - Pacing: 30 fps via SDL_GetTicks, up to FRAMESKIP_MAX skipped renders in
 *   a row when behind (logic always runs).
 * - Miyoo Mini (MIYOO; HANDHELD stays defined): the N64's own 320x240 frame (no squeeze, no HUD layer, 3D at
 *   the frame's 4:3), gamma'd in 32 bits and doubled onto the 640x480 panel, turned 180 degrees
 *   (miyoo_panel.c). MENU is Escape and opens miyoo_menu.c. The OS's SIGTERM at power-off
 *   (or any kill) takes EXIT GAME's path -- auto save, then quit -- so the next start offers RESUME GAME.
 *   The second core draws half the rows of every big 3D
 *   triangle (miyoo_worker.c, gfx_soft.c), the same pixels.
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

#define SCREEN_W 240
#define SCREEN_H 240
/* SF64 paces its own frame length: gVIsPerFrame x 1/60 s (2 = 30 Hz in play,
 * 3 in some scenes). sf64_main.c reports it every frame. */
static uint32_t sFrameMs SAVESTATE_EXCLUDE = 33;
#define FRAME_TIME_MS sFrameMs
void port_set_frame_vis(int vis) {
    if (vis < 1) vis = 1;
    if (vis > 4) vis = 4;
    sFrameMs = (uint32_t) (vis * 1000 + 30) / 60;
}
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
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;
#if defined(MIYOO)
/* SF64_FILL (default 1; or fill=0 in sf64_display.cfg): the picture fills the 4:3 panel -- the scissor's TV-safe
 * edges widened to the frame's (gfx_pc.c gfx_dp_set_scissor). 0: the N64's 8-px black border, as the console sends it. */
int gfx_miyoo_fill(void) {
    static int sFillOn SAVESTATE_EXCLUDE = -1;
    if (sFillOn < 0) {
        const char *e = getenv("SF64_FILL");
        sFillOn = 1;
        if (e && e[0]) sFillOn = e[0] != '0';
        else {
            const char *dir = getenv("SF64_DATA");
            char path[512], line[64];
            snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f))
                    if (strncmp(line, "fill=", 5) == 0) sFillOn = line[5] != '0';
                fclose(f);
            }
        }
        fprintf(stderr, "fill: %s\n", sFillOn ? "the 4:3 panel filled (SF64_FILL=0: the N64's border)" : "the N64's border kept");
    }
    return sFillOn;
}
/* Miyoo: the frame as rendered (320x240, 32 bits, the VI gamma applied), which the present thread doubles
 * and turns onto the panel (miyoo_panel_present32): no RGB565 stage (GoldenEye's present). */
#define MIYOO_W 320
#define MIYOO_H 240
static uint32_t sStage32[MIYOO_W * MIYOO_H] SAVESTATE_EXCLUDE;
static volatile int sPinPresent SAVESTATE_EXCLUDE; /* the worker is on: the present thread moves to core 1 */
#endif

static bool sDoRender SAVESTATE_EXCLUDE = true;
static int sSkipsInARow SAVESTATE_EXCLUDE;
static uint32_t sLastFrameMs SAVESTATE_EXCLUDE; /* pacing: wall clock, never restore */
static volatile sig_atomic_t sTermPending SAVESTATE_EXCLUDE; /* SIGTERM: auto save, then quit */

/* Display modes (handheld menu "DISPLAY"):
 *  CONVERT (default): the N64's 320x240 frame, area-filtered to 240 wide at
 *      present (every 4 columns -> 3, weights 3:1 2:2 1:3; paper-mario round
 *      3). Rendering 2D at 240 directly dropped one texel column in four, so
 *      text strokes broke up. 3D is drawn pre-stretched (aspect 1:1) into the
 *      frame, so after the squeeze it keeps true aspect as before, with the
 *      same internal pixel count (240 * ladder %, square).
 *  STRETCH: render internal 320x240 (N64 aspect), squash to 240 on present.
 *  CROP: render 320x240, present the centered 240 columns 1:1 -- authentic
 *      proportions, HUD edges clipped (super-mario-64's approach). */
int gHandheldDisplayMode SAVESTATE_EXCLUDE = HH_DISPLAY_CONVERT; /* a setting, not game state */

/* Dynamic resolution ladder as a percentage of the base render size
 * (index 0 = native). Only the 3D (perspective) pass renders at this size;
 * 2D (HUD, text, menus) always renders at the base size and the backend
 * composites the two (gfx_soft.c). SF64_DYNRES=0 disables the ladder. */
static const int sResPct[] = { 100, 95, 90, 85, 80, 75, 70, 65, 60 };
#define RES_STEPS ((int)(sizeof(sResPct) / sizeof(sResPct[0])))
static int sResIdx SAVESTATE_EXCLUDE;     /* current ladder position */
static int sResEnabled SAVESTATE_EXCLUDE; /* SF64_DYNRES=0 disables the ladder */

extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

static void apply_res(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= RES_STEPS) idx = RES_STEPS - 1;
    sResIdx = idx;
    extern float gfx_persp_aspect;
    const int base_w = 320;
    const int base_h = 240;
#if defined(MIYOO)
    const int conv = 0; /* the Miyoo shows the N64's 320x240 frame as it is (doubled): nothing to squeeze */
#endif
    const int int_w = ((conv ? 240 : base_w) * sResPct[idx] / 100) & ~1;
    const int int_h = (base_h * sResPct[idx] / 100) & ~1;
    /* gfx_pc's dimensions are the NATIVE size; the backend swaps them to the
     * internal size for the duration of each 3D batch. */
    gfx_current_dimensions.width = (uint32_t)base_w;
    gfx_current_dimensions.height = (uint32_t)base_h;
    gfx_current_dimensions.aspect_ratio = (float)base_w / (float)base_h;
    gfx_persp_aspect = conv ? 1.0f : 0.0f; /* the panel's shape, before the squeeze */
    { /* HUD unsquish (round 21, gfx_soft.c): only where the frame is squeezed. hud=0 in the display cfg or
       * SF64_HUDUNSQUISH=0: the whole HUD squeezed with the frame, as before. */
        extern int gfx_soft_hud_unsquish;
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("SF64_HUDUNSQUISH");
            on = !(e && e[0] == '0');
            if (!e) {
                const char *dir = getenv("SF64_DATA");
                char path[512], line[64];
                snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
                FILE *f = fopen(path, "r");
                if (f) {
                    while (fgets(line, sizeof(line), f))
                        if (strncmp(line, "hud=", 4) == 0 && (line[4] == '0' || line[4] == '1')) on = line[4] - '0';
                    fclose(f);
                }
            }
        }
        gfx_soft_hud_unsquish = conv && on;
    }
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
    if (sResEnvForced) return; /* SF64_DYNRES pins it for testing */
    sResEnabled = on ? 1 : 0;
    if (!sResEnabled) apply_res(0); /* back to native and stay there */
}
int gfx_sdl12_get_res_dynamic(void) { return sResEnabled; }

/* Menu "3D RES" (device round 14): a FIXED internal 3D size the user steps
 * through by hand -- the ladder stays off. 100 = native (unified backend, the
 * cheapest rung); anything lower renders the 3D world smaller and nearest-
 * upscales it under the crisp native 2D/HUD, at a ~2 ms composite cost, so 95
 * is not worth it -- the first useful step is 90. Persisted as intres= in
 * sf64_display.cfg; the shipped default is 85 (chosen on hardware, round 15). */
void gfx_sdl12_set_res_pct(int pct) {
    int best = 0;
    for (int i = 0; i < RES_STEPS; i++) if (abs(sResPct[i] - pct) < abs(sResPct[best] - pct)) best = i;
    if (sResEnvForced && sResEnabled) return; /* SF64_DYNRES=1 pins the ladder for testing */
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
 * The binary is non-PIE and keeps its symbols, so `addr2line -f -e sf64 <pc> <lr>` names them.
 * Before, this handler only unpanned and re-raised, so a crash on the device left nothing: the log
 * just stopped (diddy-kong-racing cd3649a). Only write() and arithmetic here (signal context), then the
 * panel unpanned and the default action, as before. */
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

int handheld_power_pending(void) { return sTermPending; } /* the menu and the resume prompt step aside */

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    (void)game_name; (void)start_in_fullscreen;
    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
#if defined(MIYOO)
    sScreen = miyoo_panel_open(); /* main.c opened it first (video before audio): the same surface */
#endif
    if (!sScreen) {
        fprintf(stderr, "gfx_sdl12: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        exit(1);
    }
    { /* Resolution: FULL 240 by default (device round 7: native res is now
       * cheap -- the unified backend drops the composite at 240 -- so lock it
       * and never change resolution. The DYNAMIC ladder is a menu option for
       * a track that can't hold 30 at native. SF64_DYNRES overrides for tests:
       * =1 forces dynamic, =0 forces full; unset uses the saved setting
       * (loaded by handheld_menu_init -> gfx_sdl12_set_res_dynamic). */
        const char *e = getenv("SF64_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    apply_res(0);
    atexit(fb_unpan);
    fatal_handlers_install();
#if defined(MIYOO)
    /* Onion's and the stock OS's power-off, MinUI's, Allium's and any plain kill send SIGTERM (or SIGHUP):
     * the flag handheld_power_pending() reports, and the loop takes EXIT GAME's path (auto save, quit). */
    miyoo_term_install(&sTermPending);
#endif
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) fprintf(stderr, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen);
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("SF64_MENU_TEST");
        if (td && td[0]) { handheld_menu_test_dump(td); exit(0); }
    }
#if defined(MIYOO)
    /* The second core (gfx_soft.c splits each big 3D triangle's rows with it): started last, so the present,
     * audio and backstop threads made before keep both cores; the game thread is pinned to core 0, the worker
     * to core 1, and the present thread joins the worker there (sPinPresent). SF64_THREADS=1 (or threads=1 in
     * sf64_display.cfg) keeps one core; with one core online it stays off by itself. */
    {
        const char *e = getenv("SF64_THREADS");
        int cfg1 = 0;
        if (!(e && e[0])) {
            const char *dir = getenv("SF64_DATA");
            char path[512], line[64];
            snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f))
                    if (strncmp(line, "threads=", 8) == 0) cfg1 = line[8] == '1';
                fclose(f);
            }
        }
        if (cfg1) fprintf(stderr, "worker: off (threads=1 in sf64_display.cfg)\n");
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

/* main.c's SF64_MENU_AT (test hook, statefix B): two MENU presses queued, so the next event poll opens the
 * menu and the menu's own poll closes it (RESUME): a headless run goes through its mute and unmute. */
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
    if (sTermPending) {
        sTermPending = 0;
#if defined(MIYOO)
        fprintf(stderr, "gfx_sdl12: SIGTERM: auto save, then quit\n");
        savestate_request_exit_save(); /* the Miyoo's power-off: EXIT GAME's save */
#endif
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
 * Round 11 (F-Zero X 06750ab's rule, as mario-kart-64 3888bd4): a one-off spike -- an OS call,
 * a screenshot, textures re-imported after a load -- plays slow like any late frame; only
 * LATE_RUN badly late frames in a row take the logic-only iteration. One 53-99 ms hitch used
 * to cost a skipped picture, a visible jump. SF64_PACER=0: as before (A/B). */
#define LATE_SLOWDOWN_MS 24 /* late by up to this: render anyway (slow-mo floor ~17 fps... 33+24 = 57 ms) */
#define LATE_RUN 4          /* badly late this many frames in a row before a logic-only iteration */
static uint32_t sDeadlineMs SAVESTATE_EXCLUDE;
static int sLateRun SAVESTATE_EXCLUDE;

static bool gfx_sdl12_start_frame(void) {
    { /* SF64_FAST=1 (tests, with SF64_VCLOCK=1): every frame rendered, no
       * pacing or skips -- deterministic ARM sweeps under qemu-arm */
        static int fast SAVESTATE_EXCLUDE = -1;
        if (fast < 0) { const char *e = getenv("SF64_FAST"); fast = e && e[0] == '1'; }
        if (fast) { sDoRender = true; return true; }
    }
    if (sResEnabled) ladder_step();

    const uint32_t now = SDL_GetTicks();
    if (sDeadlineMs == 0) sDeadlineMs = now;
    const int late = (int)(now - sDeadlineMs); /* < 0: early */
    static int sRobust SAVESTATE_EXCLUDE = -1;
    if (sRobust < 0) { const char *e = getenv("SF64_PACER"); sRobust = !(e && e[0] == '0'); }
    if (late < LATE_SLOWDOWN_MS) sLateRun = 0;
    else if (sDoRender) sLateRun++;          /* a rendered frame that came in badly late */
    if (late <= 0) {
        if (late < 0) { SDL_Delay((uint32_t)(-late)); PORT_PROF_ADD(PROF_SLEEP, (u32)(-late) * 1000u); }
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

/* 320 ARGB8888 pixels -> 240 RGB565: every 4 source columns -> 3, each output
 * the area-weighted mix of the two it covers (3:1, 2:2, 1:3). Red and blue
 * mix together in one word, green in another. */
static void squeeze_row_to_565(uint16_t *dst, const uint32_t *src) {
    int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* Round 12 (the present was ~6% of Corneria's ARM instructions): the same word arithmetic on four
     * 4-column groups at once -- vld4q_u32 puts each group's first, second, third and fourth pixel in
     * one vector, vst3_u16 interleaves the three outputs back -- so it is bit-identical by construction.
     * neon=0 in the display cfg (or SF64_CMNEON=0) keeps the scalar loop below for the device A/B. */
    extern int gfx_soft_neon_on(void);
    if (gfx_soft_neon_on()) {
        const uint32x4_t mrb = vdupq_n_u32(0xFF00FFu), mg = vdupq_n_u32(0xFF00u);
        const uint32x4_t k2 = vdupq_n_u32(0x020002u), k1 = vdupq_n_u32(0x010001u);
        const uint32x4_t kg2 = vdupq_n_u32(0x200u), kg1 = vdupq_n_u32(0x100u);
        for (; i + 4 <= 80; i += 4, src += 16, dst += 12) {
            const uint32x4x4_t p = vld4q_u32(src);
            const uint32x4_t arb = vandq_u32(p.val[0], mrb), brb = vandq_u32(p.val[1], mrb);
            const uint32x4_t crb = vandq_u32(p.val[2], mrb), drb = vandq_u32(p.val[3], mrb);
            const uint32x4_t ag = vandq_u32(p.val[0], mg), bg = vandq_u32(p.val[1], mg);
            const uint32x4_t cg = vandq_u32(p.val[2], mg), dg = vandq_u32(p.val[3], mg);
            const uint32x4_t rb0 = vandq_u32(vshrq_n_u32(vaddq_u32(vmlaq_n_u32(brb, arb, 3), k2), 2), mrb);
            const uint32x4_t g0 = vandq_u32(vshrq_n_u32(vaddq_u32(vmlaq_n_u32(bg, ag, 3), kg2), 2), mg);
            const uint32x4_t rb1 = vandq_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(brb, crb), k1), 1), mrb);
            const uint32x4_t g1 = vandq_u32(vshrq_n_u32(vaddq_u32(vaddq_u32(bg, cg), kg1), 1), mg);
            const uint32x4_t rb2 = vandq_u32(vshrq_n_u32(vaddq_u32(vmlaq_n_u32(crb, drb, 3), k2), 2), mrb);
            const uint32x4_t g2 = vandq_u32(vshrq_n_u32(vaddq_u32(vmlaq_n_u32(cg, dg, 3), kg2), 2), mg);
#define P565V(rb, g) vmovn_u32(vorrq_u32(vorrq_u32(vandq_u32(vshrq_n_u32(rb, 8), vdupq_n_u32(0xF800u)), \
                                                   vandq_u32(vshrq_n_u32(g, 5), vdupq_n_u32(0x07E0u))), \
                                         vandq_u32(vshrq_n_u32(rb, 3), vdupq_n_u32(0x001Fu))))
            uint16x4x3_t o;
            o.val[0] = P565V(rb0, g0); o.val[1] = P565V(rb1, g1); o.val[2] = P565V(rb2, g2);
#undef P565V
            vst3_u16(dst, o);
        }
    }
#endif
    for (; i < 80; i++, src += 4, dst += 3) {
        const uint32_t a = src[0], b = src[1], c = src[2], d = src[3];
        const uint32_t arb = a & 0xFF00FFu, brb = b & 0xFF00FFu, crb = c & 0xFF00FFu, drb = d & 0xFF00FFu;
        const uint32_t ag = a & 0xFF00u, bg = b & 0xFF00u, cg = c & 0xFF00u, dg = d & 0xFF00u;
        const uint32_t rb0 = ((arb * 3 + brb + 0x020002u) >> 2) & 0xFF00FFu, g0 = ((ag * 3 + bg + 0x200u) >> 2) & 0xFF00u;
        const uint32_t rb1 = ((brb + crb + 0x010001u) >> 1) & 0xFF00FFu,     g1 = ((bg + cg + 0x100u) >> 1) & 0xFF00u;
        const uint32_t rb2 = ((crb + drb * 3 + 0x020002u) >> 2) & 0xFF00FFu, g2 = ((cg + dg * 3 + 0x200u) >> 2) & 0xFF00u;
#define P565(rb, g) (uint16_t)((((rb) >> 8) & 0xF800) | (((g) >> 5) & 0x07E0) | (((rb) >> 3) & 0x001F))
        dst[0] = P565(rb0, g0); dst[1] = P565(rb1, g1); dst[2] = P565(rb2, g2);
#undef P565
    }
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
        miyoo_panel_present32(sStage32, MIYOO_W, MIYOO_H); /* the notice, doubled, turned */
#endif
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

/* Border fill (the owner, 2026-09-29: "always stretch", every game): the N64 draws inside an 8-pixel overscan
 * margin (SCREEN_MARGIN) a TV hid, black on the panel; so the 304x224 inside is scaled to the whole square output
 * (bilinear: rows blended in NEON, then two taps a column). The 1:1 HUD layer (gfx_soft.c) goes on after, at
 * its own place. Filled with the nearest pixel by default (the owner picked sharp over smooth, 2026-09-29; Smash
 * v24's way): the same code, taps at whole pixels with zero weights. border=3 in sf64_display.cfg or
 * SF64_FILL=smooth: bilinear; border=1 or SF64_BORDER=1: the frame squeezed with its margin, as before. */
/* 2026-10-03, the final exact speed check: two shortcuts in the present's conversion, the same 16-bit pixels by
 * construction (port/tools/handheld/present_test.sh drives this file's code, old against new, on x86 and under
 * qemu-arm). The VI gamma: the 128 KB RGB565 table is built channel by channel (ultra_shim.c port_vi_gamma565), so
 * three small tables taken from it (32 red, 64 green, 32 blue entries) give the same pixel; they are checked against
 * all 65,536 entries once, and a table that doesn't split keeps the lookup. NEON then runs 8 pixels a step with vtbl,
 * where the old loop did one dependent load a pixel from a table four times the L1 (57,600 a frame). The sharp border
 * fill (the default): its weights are all 256:0, so the lerp is the left tap; the gather skips the multiplies.
 * SF64_PRESFAST=0 or a line presfast=0 in sf64_display.cfg: the old loops (A/B); presfast=alt flips it every 150
 * presents and logs "PRESFAST n: convert x.xx ms" for each window, so one session prices it on the device. */
static int sPresFast SAVESTATE_EXCLUDE = -1, sPresFastAlt SAVESTATE_EXCLUDE, sPresFastNow SAVESTATE_EXCLUDE; /* Now: this present's */
static int present_fast(void) {
    if (sPresFast < 0) {
        const char *e = getenv("SF64_PRESFAST");
        char v[8] = "";
        if (e) snprintf(v, sizeof(v), "%s", e);
        else {
            const char *dir = getenv("SF64_DATA");
            char path[512], line[64];
            snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
            FILE *f = fopen(path, "r");
            if (f) {
                while (fgets(line, sizeof(line), f))
                    if (strncmp(line, "presfast=", 9) == 0) snprintf(v, sizeof(v), "%.3s", line + 9);
                fclose(f);
            }
        }
        sPresFastAlt = strncmp(v, "alt", 3) == 0;
        sPresFast = v[0] != '0';
        if (v[0]) fprintf(stderr, "presfast: %s\n", sPresFastAlt ? "alternating every 150 presents" : sPresFast ? "on" : "off");
    }
    return sPresFast;
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static uint8_t sGamR[32] SAVESTATE_EXCLUDE, sGamG[64] SAVESTATE_EXCLUDE, sGamB[32] SAVESTATE_EXCLUDE;
static const unsigned short *sGamFrom SAVESTATE_EXCLUDE; static int sGamSplit SAVESTATE_EXCLUDE;
static int gamma_split(const unsigned short *t) { /* 1: t[p] is per channel, sGamR/G/B hold it */
    if (t == sGamFrom) return sGamSplit;
    sGamFrom = t; sGamSplit = 0;
    for (int i = 0; i < 32; i++) { sGamR[i] = (uint8_t)(t[i << 11] >> 11); sGamB[i] = (uint8_t)(t[i] & 31); }
    for (int i = 0; i < 64; i++) sGamG[i] = (uint8_t)((t[i << 5] >> 5) & 63);
    for (unsigned p = 0; p < 65536; p++)
        if (t[p] != (unsigned short)(sGamR[p >> 11] << 11 | sGamG[(p >> 5) & 63] << 5 | sGamB[p & 31])) {
            fprintf(stderr, "presfast: the gamma table doesn't split by channel at %04x; the lookup kept\n", p);
            return 0;
        }
    return sGamSplit = 1;
}
static void gamma_row_neon(uint16_t *d) { /* SCREEN_W pixels, in place */
    uint8x8x4_t tr, tgl, tgh, tbl;
    for (int k = 0; k < 4; k++) {
        tr.val[k] = vld1_u8(sGamR + 8 * k); tbl.val[k] = vld1_u8(sGamB + 8 * k);
        tgl.val[k] = vld1_u8(sGamG + 8 * k); tgh.val[k] = vld1_u8(sGamG + 32 + 8 * k);
    }
    const uint8x8_t m63 = vdup_n_u8(63), m31 = vdup_n_u8(31), k32 = vdup_n_u8(32);
    for (int x = 0; x < SCREEN_W; x += 8) {
        const uint16x8_t p = vld1q_u16(d + x);
        const uint8x8_t r = vtbl4_u8(tr, vmovn_u16(vshrq_n_u16(p, 11)));
        const uint8x8_t gi = vand_u8(vshrn_n_u16(p, 5), m63);
        const uint8x8_t g = vtbx4_u8(vtbl4_u8(tgl, gi), tgh, vsub_u8(gi, k32)); /* 0-31 from the first half, 32-63 the second */
        const uint8x8_t b = vtbl4_u8(tbl, vand_u8(vmovn_u16(p), m31));
        vst1q_u16(d + x, vorrq_u16(vorrq_u16(vshlq_n_u16(vmovl_u8(r), 11), vshlq_n_u16(vmovl_u8(g), 5)), vmovl_u8(b)));
    }
}
#endif

#define FILL_M 8
static int sFill SAVESTATE_EXCLUDE = -1;
static uint16_t sFillX0[SCREEN_W] SAVESTATE_EXCLUDE; static uint8_t sFillFx[SCREEN_W] SAVESTATE_EXCLUDE;
static uint16_t sFillY0[SCREEN_H] SAVESTATE_EXCLUDE; static uint8_t sFillFy[SCREEN_H] SAVESTATE_EXCLUDE;
static uint8_t sFillIdxA[16] SAVESTATE_EXCLUDE, sFillIdxB[16] SAVESTATE_EXCLUDE; /* NEON: one group's taps */
static uint16_t sFillW0[16] SAVESTATE_EXCLUDE, sFillW1[16] SAVESTATE_EXCLUDE;
static void fill_init(void) {
    const char *e = getenv("SF64_BORDER");
    int border = e ? (e[0] >= '0' && e[0] <= '3' ? e[0] - '0' : 0) : 0;
    { const char *fs = getenv("SF64_FILL"); if (fs) { border = strcmp(fs, "smooth") == 0 ? 3 : 2; e = fs; } }
    if (!e) {
        const char *dir = getenv("SF64_DATA");
        char path[512], line[64];
        snprintf(path, sizeof(path), "%s/sf64_display.cfg", dir ? dir : ".");
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f))
                if (strncmp(line, "border=", 7) == 0 && line[7] >= '0' && line[7] <= '3') border = line[7] - '0';
            fclose(f);
        }
    }
    sFill = border == 1 ? 0 : border == 3 ? 1 : 2; /* 2 sharp (nearest: the owner's pick, the default), 1 smooth */
    for (int i = 0; i < SCREEN_W; i++) { /* output centre -> source, in 1/256 px */
        int p = FILL_M * 256 + ((2 * i + 1) * (320 - 2 * FILL_M) * 256) / (2 * SCREEN_W) - 128;
        if (sFill == 2) p = (FILL_M + ((2 * i + 1) * (320 - 2 * FILL_M)) / (2 * SCREEN_W)) * 256; /* sharp: the nearest column */
        p = p < FILL_M * 256 ? FILL_M * 256 : p > (320 - FILL_M - 1) * 256 ? (320 - FILL_M - 1) * 256 : p; /* no margin tap */
        sFillX0[i] = (uint16_t)(p >> 8); sFillFx[i] = (uint8_t)(p & 0xFF);
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (int k = 0; k < 16; k++) { /* one group of 15 (lane 15 unused): taps relative to 19 x the group */
        const int kk = k < 15 ? k : 14;
        const int x0 = sFillX0[kk] - FILL_M, f = sFillFx[kk];
        sFillIdxA[k] = (uint8_t)x0; sFillIdxB[k] = (uint8_t)(x0 + 1); sFillW0[k] = (uint16_t)(256 - f); sFillW1[k] = (uint16_t)f;
    }
#endif
    for (int i = 0; i < SCREEN_H; i++) {
        int p = FILL_M * 256 + ((2 * i + 1) * (240 - 2 * FILL_M) * 256) / (2 * SCREEN_H) - 128;
        if (sFill == 2) p = (FILL_M + ((2 * i + 1) * (240 - 2 * FILL_M)) / (2 * SCREEN_H)) * 256; /* sharp: the nearest row */
        p = p < FILL_M * 256 ? FILL_M * 256 : p > (240 - FILL_M - 1) * 256 ? (240 - FILL_M - 1) * 256 : p;
        sFillY0[i] = (uint16_t)(p >> 8); sFillFy[i] = (uint8_t)(p & 0xFF);
    }
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* The same arithmetic in NEON (identical output): the rows lerped into byte planes (vld4 splits B G R A), then
 * 304 -> 240 is 19 -> 15 with the same taps and weights every 15 outputs -- one vtbl4 over 32 bytes of a plane
 * gives a group's left taps, another its right taps, and the lerp (a (256 - f) + b f) >> 8 runs in u16 lanes. */
static void fill_row_neon(uint16_t *dst, const uint32_t *fb, int rw, int y) {
    static uint8_t pl[3][336] __attribute__((aligned(16))) SAVESTATE_EXCLUDE; /* B G R of the 304 columns, + slack */
    static uint16_t out[256] SAVESTATE_EXCLUDE;
    const uint32_t *r0 = fb + sFillY0[y] * rw + FILL_M, *r1 = r0 + rw;
    const uint32_t fy = sFillFy[y];
    const uint8x8_t w0 = vdup_n_u8((uint8_t)(fy ? 256 - fy : 255)), w1 = vdup_n_u8((uint8_t)fy);
    for (int i = 0; i < 320 - 2 * FILL_M; i += 8) {
        const uint8x8x4_t a = vld4_u8((const uint8_t *)(r0 + i));
        if (fy) {
            const uint8x8x4_t b = vld4_u8((const uint8_t *)(r1 + i));
            for (int c = 0; c < 3; c++) vst1_u8(pl[c] + i, vshrn_n_u16(vmlal_u8(vmull_u8(a.val[c], w0), b.val[c], w1), 8));
        } else {
            for (int c = 0; c < 3; c++) vst1_u8(pl[c] + i, a.val[c]);
        }
    }
    const uint8x8_t iAl = vld1_u8(sFillIdxA), iAh = vld1_u8(sFillIdxA + 8), iBl = vld1_u8(sFillIdxB), iBh = vld1_u8(sFillIdxB + 8);
    if (sFill == 2 && sPresFastNow) { /* sharp: every weight is 256:0, so (a 256 + b 0) >> 8 = a -- the left tap itself */
        for (int g = 0; g < SCREEN_W / 15; g++) {
            uint8x16_t ch[3];
            for (int c = 0; c < 3; c++) {
                const uint8_t *t = pl[c] + 19 * g;
                uint8x8x4_t tb; tb.val[0] = vld1_u8(t); tb.val[1] = vld1_u8(t + 8); tb.val[2] = vld1_u8(t + 16); tb.val[3] = vld1_u8(t + 24);
                ch[c] = vcombine_u8(vtbl4_u8(tb, iAl), vtbl4_u8(tb, iAh));
            }
            const uint8x16_t r = vandq_u8(ch[2], vdupq_n_u8(0xF8)), gg = vandq_u8(ch[1], vdupq_n_u8(0xFC)), bb = vshrq_n_u8(ch[0], 3);
            const uint16x8_t plo = vorrq_u16(vorrq_u16(vshll_n_u8(vget_low_u8(r), 8), vshll_n_u8(vget_low_u8(gg), 3)), vmovl_u8(vget_low_u8(bb)));
            const uint16x8_t phi = vorrq_u16(vorrq_u16(vshll_n_u8(vget_high_u8(r), 8), vshll_n_u8(vget_high_u8(gg), 3)), vmovl_u8(vget_high_u8(bb)));
            vst1q_u16(out + 15 * g, plo); vst1q_u16(out + 15 * g + 8, phi);
        }
        memcpy(dst, out, SCREEN_W * sizeof(uint16_t));
        return;
    }
    const uint16x8_t W0l = vld1q_u16(sFillW0), W0h = vld1q_u16(sFillW0 + 8), W1l = vld1q_u16(sFillW1), W1h = vld1q_u16(sFillW1 + 8);
    for (int g = 0; g < SCREEN_W / 15; g++) {
        uint8x16_t ch[3];
        for (int c = 0; c < 3; c++) {
            const uint8_t *t = pl[c] + 19 * g;
            uint8x8x4_t tb; tb.val[0] = vld1_u8(t); tb.val[1] = vld1_u8(t + 8); tb.val[2] = vld1_u8(t + 16); tb.val[3] = vld1_u8(t + 24);
            const uint16x8_t lo = vmlaq_u16(vmulq_u16(vmovl_u8(vtbl4_u8(tb, iAl)), W0l), vmovl_u8(vtbl4_u8(tb, iBl)), W1l);
            const uint16x8_t hi = vmlaq_u16(vmulq_u16(vmovl_u8(vtbl4_u8(tb, iAh)), W0h), vmovl_u8(vtbl4_u8(tb, iBh)), W1h);
            ch[c] = vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8));
        }
        /* RGB565: r & 0xF8 << 8 | g & 0xFC << 3 | b >> 3 */
        const uint8x16_t r = vandq_u8(ch[2], vdupq_n_u8(0xF8)), gg = vandq_u8(ch[1], vdupq_n_u8(0xFC)), bb = vshrq_n_u8(ch[0], 3);
        const uint16x8_t plo = vorrq_u16(vorrq_u16(vshll_n_u8(vget_low_u8(r), 8), vshll_n_u8(vget_low_u8(gg), 3)), vmovl_u8(vget_low_u8(bb)));
        const uint16x8_t phi = vorrq_u16(vorrq_u16(vshll_n_u8(vget_high_u8(r), 8), vshll_n_u8(vget_high_u8(gg), 3)), vmovl_u8(vget_high_u8(bb)));
        vst1q_u16(out + 15 * g, plo); vst1q_u16(out + 15 * g + 8, phi);
    }
    memcpy(dst, out, SCREEN_W * sizeof(uint16_t));
}
#endif
/* Round 25 (the crosshair line art on the HUD layer): a point of the 320x240 frame on the panel, as present
 * shows it (the fill's crop and scale, or the plain 4 -> 3 squeeze). */
void gfx_sdl12_native_to_panel(float x, float y, float *px, float *py) {
    if (sFill < 0) fill_init();
    if (sFill) {
        *px = (x - FILL_M) * (float)SCREEN_W / (float)(320 - 2 * FILL_M);
        *py = (y - FILL_M) * (float)SCREEN_H / (float)(240 - 2 * FILL_M);
    } else {
        *px = x * (float)SCREEN_W / 320.0f;
        *py = y;
    }
}
static void fill_row_to_565(uint16_t *dst, const uint32_t *fb, int rw, int y) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    extern int gfx_soft_neon_on(void);
    if (gfx_soft_neon_on()) { fill_row_neon(dst, fb, rw, y); return; }
#endif
    static uint32_t row[320] SAVESTATE_EXCLUDE;
    const uint32_t *r0 = fb + sFillY0[y] * rw, *r1 = r0 + rw;
    const uint32_t fy = sFillFy[y];
    const uint32_t *src = r0;
    if (fy) { /* rows lerped: (a (256 - f) + b f) >> 8 a byte */
        int i = FILL_M - 1;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const uint8x8_t w0 = vdup_n_u8((uint8_t)(256 - fy > 255 ? 255 : 256 - fy)), w1 = vdup_n_u8((uint8_t)fy);
        if (fy <= 255 && 256 - fy <= 255) /* both weights fit a byte (fy 1..255) */
            for (; i + 2 <= 320 - FILL_M + 1; i += 2) {
                const uint8x8_t a = vreinterpret_u8_u32(vld1_u32(r0 + i)), b = vreinterpret_u8_u32(vld1_u32(r1 + i));
                vst1_u32(row + i, vreinterpret_u32_u8(vshrn_n_u16(vmlal_u8(vmull_u8(a, w0), b, w1), 8)));
            }
#endif
        for (; i < 320 - FILL_M + 1; i++) {
            const uint32_t a = r0[i], b = r1[i];
            const uint32_t rb = (((a & 0xFF00FFu) * (256 - fy) + (b & 0xFF00FFu) * fy) >> 8) & 0xFF00FFu;
            const uint32_t g = ((((a >> 8) & 0xFF00FFu) * (256 - fy) + ((b >> 8) & 0xFF00FFu) * fy) >> 8) & 0xFF00FFu;
            row[i] = rb | (g << 8);
        }
        src = row;
    }
    for (int x = 0; x < SCREEN_W; x++) { /* two taps a column */
        const uint32_t a = src[sFillX0[x]], b = src[sFillX0[x] + 1], f = sFillFx[x];
        const uint32_t rb = (((a & 0xFF00FFu) * (256 - f) + (b & 0xFF00FFu) * f) >> 8) & 0xFF00FFu;
        const uint32_t g = (((a & 0xFF00u) * (256 - f) + (b & 0xFF00u) * f) >> 8) & 0xFF00u;
        dst[x] = (uint16_t)(((rb >> 8) & 0xF800) | ((g >> 5) & 0x07E0) | ((rb >> 3) & 0x001F));
    }
}

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
    SDL_mutexP(sPresentLock);
    while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock); /* staging still unread */
#if defined(MIYOO)
    { /* the 320x240 frame (any other render size: nearest), through the N64 VI's gamma per channel when the
       * game has it on (ultra_shim.c port_vi_gamma8: the same curve the handheld's 565 table is made from) */
        const unsigned char *g8 = port_vi_gamma8();
        for (int y = 0; y < MIYOO_H; y++) {
            const uint32_t *src = fb + (rh == MIYOO_H ? y : y * rh / MIYOO_H) * rw;
            uint32_t *dst = &sStage32[y * MIYOO_W];
            if (rw == MIYOO_W && !g8) { memcpy(dst, src, MIYOO_W * 4); continue; }
            for (int x = 0; x < MIYOO_W; x++) {
                const uint32_t p = src[rw == MIYOO_W ? x : x * rw / MIYOO_W];
                dst[x] = g8 ? (uint32_t)g8[(p >> 16) & 0xFF] << 16 | (uint32_t)g8[(p >> 8) & 0xFF] << 8 | g8[p & 0xFF] : p;
            }
        }
        (void)x0; (void)win_w;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
#endif
    const int direct = (win_w == SCREEN_W && rh == SCREEN_H); /* 1:1 (crop, or a 240 render) */
    const unsigned short *gamma = port_vi_gamma565(); /* the N64 VI's gamma, when the game has it on */
    extern int gfx_soft_hud_rows(int *y0, int *y1);
    extern void gfx_soft_hud_overlay_row(int y, uint16_t *dst);
    int hy0 = 0, hy1 = 0;
    const int hud = win_w == 320 && gHandheldDisplayMode != HH_DISPLAY_CROP && gfx_soft_hud_rows(&hy0, &hy1);
    sPresFastNow = present_fast();
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const int gfast = gamma && sPresFastNow && gamma_split(gamma);
#endif
    for (int y = 0; y < SCREEN_H; y++) {
        const uint32_t *src = fb + (direct ? y : y * rh / SCREEN_H) * rw + x0;
        uint16_t *dst = &sStage[y * SCREEN_W];
        if (direct) {
            argb_row_to_565(dst, src, SCREEN_W); /* NEON on device */
        } else if (win_w == 320 && gHandheldDisplayMode != HH_DISPLAY_CROP) {
            if (sFill < 0) fill_init();
            if (sFill && rw == 320 && rh == 240) fill_row_to_565(dst, fb, rw, y); /* the margin cropped, scaled */
            else squeeze_row_to_565(dst, src); /* area filter: every column contributes */
        } else { /* exact NN (x * win_w / 240): no 16.16 drift at panel edges */
            for (int x = 0; x < SCREEN_W; x++) {
                const uint32_t s = src[x * win_w / SCREEN_W];
                dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F));
            }
        }
        if (hud && y >= hy0 && y < hy1) gfx_soft_hud_overlay_row(y, dst); /* the 1:1 HUD layer (round 21) */
        if (gamma) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            if (gfast) gamma_row_neon(dst); else
#endif
            for (int x = 0; x < SCREEN_W; x++) dst[x] = gamma[dst[x]];
        }
    }
    sPresentPending = 1;
    SDL_CondBroadcast(sPresentCond);
    SDL_mutexV(sPresentLock);
    const uint32_t dt = port_time_us() - t0;
    PORT_PROF_ADD(PROF_PRES, dt);
    if (sPresFastAlt) { /* presfast=alt: the convert time of each mode, 150 presents a window */
        static uint32_t sAltUs SAVESTATE_EXCLUDE, sAltN SAVESTATE_EXCLUDE;
        sAltUs += dt;
        if (++sAltN == 150) {
            fprintf(stderr, "PRESFAST %d: convert %.2f ms a frame over 150 presents\n", sPresFast, sAltUs / 150000.0);
            sAltUs = sAltN = 0; sPresFast = !sPresFast;
        }
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

/* The way out (main.c, after the game loop): the present thread idle and held, the console
 * unpanned, SDL shut down (the audio device closed with it). SDL 1.2's fbcon otherwise leaves the
 * console, keyboard and video modes set, and the next SDL program (the launcher after EXIT GAME)
 * can hang at its first flip, as GoldenEye's second bench run did (X50, from ocarina-of-time's EXIT
 * GAME freeze, 2026-10-01). *
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
    {   /* SF64_LOADPIC (DKR's, the handheld ports' round of 2026-10-10): a start that resumes the auto save loads it a
         * few frames in (savestate.c HH_BOOT_LOAD_FRAME), and those frames -- the game's boot -- flashed up before
         * the level. Not shown: the panel keeps the RESUME GAME screen until the first frame after the load.
         * SF64_LOADPIC=0: as before. */
        static int sOn SAVESTATE_EXCLUDE = -1, sHidden SAVESTATE_EXCLUDE;
        if (sOn < 0) { const char *e = getenv("SF64_LOADPIC"); sOn = !(e && e[0] == '0'); }
        if (sOn && savestate_boot_load_pending()) { sHidden++; return; }
        if (sHidden > 0) { fprintf(stderr, "loadpic: %d frames before the resume not shown\n", sHidden); sHidden = -1; }
    }
#endif
    present();

    { /* SF64_DUMP=<dir> [SF64_DUMP_EVERY=n] [SF64_DUMP_FROM=n]: PPM dumps of the render buffer
       * (same contract as the desktop backend; drives the qemu smoke test).
       * Counts rendered frames -- dropped frames never reach the swap. */
        extern void gfx_soft_dump_ppm(const char *path);
        static const char *sDumpDir SAVESTATE_EXCLUDE; static long sEvery SAVESTATE_EXCLUDE = -1; static long sFrame SAVESTATE_EXCLUDE;
        static long sFrom SAVESTATE_EXCLUDE;
        if (sEvery < 0) {
            sDumpDir = getenv("SF64_DUMP");
            const char *e = getenv("SF64_DUMP_EVERY");
            sEvery = e ? atol(e) : 60;
            if (sEvery <= 0) sEvery = 60;
            e = getenv("SF64_DUMP_FROM"); /* round 11: a moment's panels without thousands before it */
            sFrom = e ? atol(e) : 0;
        }
        if (sDumpDir && sFrame >= sFrom && (sFrame % sEvery) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06ld.ppm", sDumpDir, sFrame);
            gfx_soft_dump_ppm(path);
            /* and the panel image itself: after the screen fit and gamma */
            snprintf(path, sizeof(path), "%s/panel_%06ld.ppm", sDumpDir, sFrame);
            FILE *f = fopen(path, "wb");
#if defined(MIYOO)
            if (f) { /* the 320x240 the panel doubles (sStage32 is the present thread's until it is read: wait) */
                SDL_mutexP(sPresentLock);
                while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
                fprintf(f, "P6\n%d %d\n255\n", MIYOO_W, MIYOO_H);
                for (int i = 0; i < MIYOO_W * MIYOO_H; i++) {
                    const uint32_t p = sStage32[i];
                    const uint8_t c[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
                    fwrite(c, 1, 3, f);
                }
                SDL_mutexV(sPresentLock);
                fclose(f);
                f = NULL;
            }
#endif
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
                for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                    const uint16_t p = sStage[i];
                    const uint8_t c[3] = { (uint8_t)((p >> 8) & 0xF8), (uint8_t)((p >> 3) & 0xFC), (uint8_t)(p << 3) };
                    fwrite(c, 1, 3, f);
                }
                fclose(f);
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
