/**
 * Window backend: SDL 1.2.
 *
 * - The soft rasterizer renders ARGB8888 at gfx_current_dimensions; present
 *   converts to the screen format.
 * - Pacing: 30 fps via SDL_GetTicks, up to FRAMESKIP_MAX skipped renders in
 *   a row when behind (logic always runs).
 * - Miyoo Mini (MIYOO): the game renders the N64's own 320x240 and the present
 *   thread doubles it onto the 640x480 panel, turned 180 degrees (kit
 *   miyoo_panel.c). MENU is Escape and opens miyoo_menu.c; the OS's SIGTERM
 *   takes EXIT GAME's path (the auto save, then quit).
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
#include <poll.h>
#include <time.h>
#include <sys/wait.h>
#include <ucontext.h>
#if defined(MIYOO)
#include <math.h>
#endif

#include "gfx_window_manager_api.h"
#include "handheld_menu.h"
#include "savestate.h"
#include "port.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#include "miyoo_clock.h"
#endif

/* Mirror of gfx_pc.h's GfxDimensions (that header drags in Gfx/ultra types).
 * gfx_pc reads it only at init; the res ladder pokes it directly. */
struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
extern struct GfxDimensions gfx_current_dimensions;

#define SCREEN_W 240
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
/* The Miyoo Mini: the frame at the render size (320x240), copied out by the game thread; the present
 * thread doubles and turns it onto the panel (miyoo_panel_present32). */
#define STAGE32_W 320
#define STAGE32_H 240
static uint32_t sStage32[STAGE32_W * STAGE32_H] SAVESTATE_EXCLUDE;
static int sStage32W SAVESTATE_EXCLUDE, sStage32H SAVESTATE_EXCLUDE;
#endif

/* Screen capture for device diagnostics (round 12). When <save dir>/dump.cfg
 * exists, keep what present() hands the screen (RGB565) for `count`
 * drawn frames from drawn frame `from`, every `every`th, in RAM so the capture
 * doesn't change the timing. When full (or at a clean exit) write them as PPMs
 * to <save dir>/dump/screen_<n>_t<game tick>.ppm and rename dump.cfg to
 * dump.cfg.done: one capture per arming. dump.cfg lines: from=N, every=N,
 * count=N (defaults 0, 1, 150). 115 KB of RAM a frame. */
static uint16_t *sCap SAVESTATE_EXCLUDE;
static int *sCapTick SAVESTATE_EXCLUDE;
static int sCapFrom SAVESTATE_EXCLUDE, sCapEvery SAVESTATE_EXCLUDE = 1, sCapCount SAVESTATE_EXCLUDE,
           sCapN SAVESTATE_EXCLUDE, sCapOn SAVESTATE_EXCLUDE;
static long sCapDrawn SAVESTATE_EXCLUDE;
extern int port_get_frame_count(void);

static void cap_init(void) {
    char path[512], line[64];
    int count = 150;
#if defined(MIYOO)
    return; /* the Miyoo build has SSB64_DUMP (the render buffer) instead */
#endif
    snprintf(path, sizeof path, "%s/dump.cfg", port_save_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "from=", 5)) sCapFrom = atoi(line + 5);
        else if (!strncmp(line, "every=", 6)) sCapEvery = atoi(line + 6) > 0 ? atoi(line + 6) : 1;
        else if (!strncmp(line, "count=", 6)) count = atoi(line + 6);
    }
    fclose(f);
    while (count > 0 && !(sCap = malloc((size_t)count * sizeof sStage))) count /= 2;
    sCapTick = sCap ? malloc((size_t)count * sizeof *sCapTick) : NULL;
    if (!sCap || !sCapTick) { free(sCap); sCap = NULL; port_log_always("SSB64: dump.cfg: no memory for a capture\n"); return; }
    sCapCount = count;
    sCapOn = 1;
    port_log_always("SSB64: dump.cfg: capturing %d screens from drawn frame %d, every %d\n", count, sCapFrom, sCapEvery);
}

/* Also called from main.c at a clean exit (weak there, so the desktop links). */
void port_capture_flush(void) {
    char dir[512], path[600], done[600];
    static uint8_t row[SCREEN_W * 3];
    if (!sCapOn || sCapN == 0) return;
    sCapOn = 0;
    const uint32_t t0 = port_time_us();
    snprintf(dir, sizeof dir, "%s/dump", port_save_dir());
    port_fs_mkdir(dir);
    for (int i = 0; i < sCapN; i++) {
        snprintf(path, sizeof path, "%s/screen_%03d_t%05d.ppm", dir, i, sCapTick[i]);
        FILE *f = fopen(path, "wb");
        if (!f) break;
        fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
        const uint16_t *s = &sCap[(size_t)i * SCREEN_W * SCREEN_H];
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                const uint16_t p = s[y * SCREEN_W + x];
                row[x * 3] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
                row[x * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
                row[x * 3 + 2] = (uint8_t)((p & 31) * 255 / 31);
            }
            fwrite(row, 1, sizeof row, f);
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/dump.cfg", port_save_dir());
    snprintf(done, sizeof done, "%s/dump.cfg.done", port_save_dir());
    rename(path, done);
    sync();
    port_log_always("SSB64: dump.cfg: wrote %d screens to %s in %u ms\n", sCapN, dir, (unsigned)((port_time_us() - t0) / 1000));
    free(sCap); free(sCapTick); sCap = NULL; sCapTick = NULL;
}
static SDL_mutex *sPresentLock SAVESTATE_EXCLUDE;
static SDL_cond *sPresentCond SAVESTATE_EXCLUDE;
static SDL_Thread *sPresentThread SAVESTATE_EXCLUDE;
static volatile int sPresentPending SAVESTATE_EXCLUDE, sPresentBusy SAVESTATE_EXCLUDE;

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
 * composites the two (gfx_soft.c). SSB64_DYNRES=0 disables the ladder. */
static const int sResPct[] = { 100, 95, 90, 85, 80, 75, 70, 65, 60 };
#define RES_STEPS ((int)(sizeof(sResPct) / sizeof(sResPct[0])))
static int sResIdx SAVESTATE_EXCLUDE;     /* current ladder position */
static int sResEnabled SAVESTATE_EXCLUDE; /* SSB64_DYNRES=0 disables the ladder */

extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

static void apply_res(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= RES_STEPS) idx = RES_STEPS - 1;
    sResIdx = idx;
    const int base_w = (gHandheldDisplayMode == HH_DISPLAY_CONVERT) ? 240 : 320;
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
    if (sResEnvForced) return; /* SSB64_DYNRES pins it for testing */
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
    if (sResEnvForced && sResEnabled) return; /* SSB64_DYNRES=1 pins the ladder for testing */
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
/* A fatal signal leaves one line in console.log (2026-10-01, OPEN_ISSUES S6; diddy-kong-racing cd3649a): the
 * signal, the tick, and on ARM where it happened -- pc, lr (usually the caller) and, for a fault the
 * kernel raised, the address. Before, the handler only unpanned, so a crash on the device left nothing:
 * the log just stopped. `addr2line -f -e port/build/miyoo/ssb64 <pc> <lr>` on the unstripped build of
 * the same commit names them (the binary is non-PIE). Only write() and arithmetic here (signal
 * context): the line goes to stderr and to console.log's descriptor (hh_port.c), then the panel is
 * unpanned and the default action taken, as before. A crash that overflows a stack still dies
 * silently (no alternate signal stack). */
extern int port_log_get_fd(void);
extern int port_get_frame_count(void);
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
    const int fd = port_log_get_fd();
    p = fatal_str(p, "FATAL: signal "); p = fatal_dec(p, (unsigned long)sig);
    p = fatal_str(p, " at tick "); p = fatal_dec(p, (unsigned long)port_get_frame_count());
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
    if (fd >= 0) (void)!write(fd, buf, (size_t)(p - buf));
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

/* Every OS ends an app with SIGTERM (Onion and the stock OS
 * at their power-off, Allium, a plain kill; spruce kills outright): miyoo_term_install's handler sets the
 * flag, handle_events asks the game loop for EXIT GAME's auto save and quit, and the menu and the resume
 * prompt step aside for it (handheld_power_pending: the prompt then quits with the auto save kept). */
static volatile sig_atomic_t sTermRequested SAVESTATE_EXCLUDE;
int handheld_power_pending(void) { return sTermRequested; }

static void gfx_sdl12_init(const char *game_name, bool start_in_fullscreen) {
    cap_init(); /* diagnostics: <save dir>/dump.cfg */
    (void)game_name; (void)start_in_fullscreen;
    /* No SDL_INIT_TIMER: nothing uses SDL timers (SDL_GetTicks/SDL_Delay work
     * without it), and SDL 1.2's threaded timer wakes every 1 ms for good. */
    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    sScreen = miyoo_panel_open(); /* 32 bpp, the panel's own size (the first-start screen may have opened it) */
    if (!sScreen) {
        fprintf(stderr, "gfx_sdl12: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        exit(1);
    }
    { /* Resolution: FULL 240 by default (device round 7: native res is now
       * cheap -- the unified backend drops the composite at 240 -- so lock it
       * and never change resolution. The DYNAMIC ladder is a menu option for
       * a track that can't hold 30 at native. SSB64_DYNRES overrides for tests:
       * =1 forces dynamic, =0 forces full; unset uses the saved setting
       * (loaded by handheld_menu_init -> gfx_sdl12_set_res_dynamic). */
        const char *e = getenv("SSB64_DYNRES");
        if (e) { sResEnabled = (e[0] != '0'); sResEnvForced = 1; }
        else   sResEnabled = 0; /* saved setting applied at menu init */
    }
    /* 3D always at 100%, SMOOTH (the user's call for every port, 2026-09-24:
     * no 3D FILTER / 3D RES options in any menu or hotkey). Lower rungs and
     * nearest stay developer switches for A/B only (SSB64_INTRES below). */
    gfx_sdl12_set_res_pct(100);
    { /* SSB64_MAGBILIN=1: the magnified-only bilinear gate, for A/Bs (off since round 15) */
        extern int gfx_soft_magbilin;
        const char *e = getenv("SSB64_MAGBILIN");
        if (e && e[0]) gfx_soft_magbilin = (e[0] == '1');
    }
    { /* SSB64_INTRES=<pct> (bench --intres): a fixed 3D resolution, as the
       * MK64 "3D RES" menu card sets it. */
        const char *e = getenv("SSB64_INTRES");
        if (e) gfx_sdl12_set_res_pct(atoi(e));
    }
    atexit(fb_unpan);
    fatal_handlers_install();
    miyoo_term_install(&sTermRequested); /* over main.c's SIGTERM handler: the exit save, then quit */
    sPresentLock = SDL_CreateMutex();
    sPresentCond = SDL_CreateCond();
    sPresentThread = SDL_CreateThread(present_thread, NULL);
    if (!sPresentThread) fprintf(stderr, "gfx_sdl12: present thread failed: %s\n", SDL_GetError());
    handheld_menu_init(sScreen); /* sets 3D at 100%, SMOOTH (the kit's k1.1/k1.2 rule) */
#if defined(MIYOO)
    /* The Mini's picture is the N64's own 4:3, 320x240 (doubled onto the panel): the renderer's base is
     * 320 wide (apply_res' non-square base), so the square screen's 0.75 layout steps aside by itself
     * (gfx_pc.c: every such rule applies only to an output narrower than 4:3). */
    gfx_sdl12_set_display_mode(HH_DISPLAY_STRETCH);
#endif
    { /* SSB64_INTRES=<pct> (bench --intres) again after it: the kit's init resets the 3D size */
        const char *e = getenv("SSB64_INTRES");
        if (e) gfx_sdl12_set_res_pct(atoi(e));
    }
    { /* Headless menu-layout check: render every zone to PPM and exit. */
        const char *td = getenv("SSB64_MENU_TEST");
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

/* main.c's SSB64_MENU_AT (test hook, statefix B): two MENU presses queued, so the next event poll opens the
 * handheld menu and the menu's own poll closes it (RESUME). */
void port_test_menu_press(void) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = SDLK_ESCAPE;
    SDL_PushEvent(&ev);
    SDL_PushEvent(&ev);
}

static void gfx_sdl12_handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) { /* MENU button (the Mini's SDL) */
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
    if (sTermRequested == 1) {
        sTermRequested = 2; /* once; handheld_power_pending stays true, so a menu or prompt steps aside */
        port_log_always("SSB64: SIGTERM: the auto save, then quit\n");
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

/* the panel: the staged frame doubled, centred, turned and alpha-opaque (kit miyoo_panel.c, NEON); the
 * current notice (SAVED SLOT 1, ...) is drawn onto the staged copy first */
static void blit_stage_to_screen(void) { miyoo_panel_present32(sStage32, sStage32W, sStage32H); }

static int present_thread(void *arg) {
    (void)arg;
    SDL_mutexP(sPresentLock);
    for (;;) {
        while (!sPresentPending) SDL_CondWait(sPresentCond, sPresentLock);
        sPresentPending = 0;
        sPresentBusy = 1;
        blit_stage_to_screen();
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
#if defined(MIYOO)
    {   /* SSB64_LOADPIC (as diddy-kong-racing's and ocarina-of-time's Miyoo builds): a start that resumes the auto save loads it at
         * the kit's HH_BOOT_LOAD_FRAME (the third tick), so the boot's first pictures would show for a moment before
         * the resumed game's; until that load they stay off the panel (the resume prompt's black stays up).
         * SSB64_LOADPIC=0: shown, as before. */
        static int sOn SAVESTATE_EXCLUDE = -1, sHidden SAVESTATE_EXCLUDE;
        if (sOn < 0) { const char *e = getenv("SSB64_LOADPIC"); sOn = !(e && e[0] == '0'); }
        if (sOn && savestate_boot_load_pending()) { sHidden++; return; }
        if (sHidden > 0) { port_log_always("SSB64: loadpic: %d boot pictures before the resume not shown\n", sHidden); sHidden = -1; }
    }
    { /* the game thread only copies the frame out; the doubling and the flip run on the present thread */
        const uint32_t t0 = port_time_us();
        const int cw = rw > STAGE32_W ? STAGE32_W : rw, ch = rh > STAGE32_H ? STAGE32_H : rh;
        SDL_mutexP(sPresentLock);
        while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock); /* staging still unread */
        extern bool gfx_soft_stretch_on(void);
        extern void gfx_soft_stretch_row(uint32_t *dst, int y);
        if (cw == rw && ch == rh && gfx_soft_stretch_on()) /* the N64 frame over the whole picture (fill mode 4, as the handheld) */
            for (int y = 0; y < ch; y++) gfx_soft_stretch_row(sStage32 + y * cw, y);
        else
            for (int y = 0; y < ch; y++) memcpy(sStage32 + y * cw, fb + y * rw, (size_t)cw * 4);
        sStage32W = cw; sStage32H = ch;
        sPresentPending = 1;
        SDL_CondBroadcast(sPresentCond);
        SDL_mutexV(sPresentLock);
        PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
        return;
    }
#endif

    int win_w = rw, x0 = 0;
    if (gHandheldDisplayMode == HH_DISPLAY_CROP) {
        win_w = rw * 240 / 320;
        x0 = (rw - win_w) / 2;
    }
    const uint32_t t0 = port_time_us();
    SDL_mutexP(sPresentLock);
    while (sPresentPending) SDL_CondWait(sPresentCond, sPresentLock); /* staging still unread */
    const int direct = (win_w == SCREEN_W && rh == SCREEN_H); /* 1:1 (native convert / crop@native) */
    extern bool gfx_soft_stretch_on(void);
    extern void gfx_soft_stretch_row(uint32_t *dst, int y);
    const bool stretch = direct && x0 == 0 && gfx_soft_stretch_on(); /* the TV-safe frame over the screen */
    for (int y = 0; y < SCREEN_H; y++) {
        const uint32_t *src = fb + (direct ? y : y * rh / SCREEN_H) * rw + x0;
        uint16_t *dst = &sStage[y * SCREEN_W];
        if (stretch) {
            uint32_t row[SCREEN_W];
            gfx_soft_stretch_row(row, y);
            argb_row_to_565(dst, row, SCREEN_W);
        } else if (direct) {
            argb_row_to_565(dst, src, SCREEN_W); /* NEON on device */
        } else { /* exact NN (x * win_w / 240): no 16.16 drift at panel edges */
            for (int x = 0; x < SCREEN_W; x++) {
                const uint32_t s = src[x * win_w / SCREEN_W];
                dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F));
            }
        }
    }
    if (sCapOn) { /* diagnostics: exactly what goes to the screen */
        if (sCapDrawn >= sCapFrom && (sCapDrawn - sCapFrom) % sCapEvery == 0 && sCapN < sCapCount) {
            memcpy(&sCap[(size_t)sCapN * SCREEN_W * SCREEN_H], sStage, sizeof sStage);
            sCapTick[sCapN++] = port_get_frame_count();
        }
        sCapDrawn++;
    }
    sPresentPending = 1;
    SDL_CondBroadcast(sPresentCond);
    SDL_mutexV(sPresentLock);
    PORT_PROF_ADD(PROF_PRES, port_time_us() - t0);
    if (sCapOn && sCapN == sCapCount) port_capture_flush();
}

#if defined(MIYOO)
/* The Miyoo menu's backdrop (kit miyoo_menu.c, weak; paper-mario 46f03c0): the part of the frame the present shows over
 * the whole panel. With the fill stretch (SSB64_FILLBORDER 4 or 1, gfx_soft_stretch_on) that is the N64 frame inside its
 * border, the same bounds gfx_soft.c's stretch maps (R23's pixel centres: 10..310 x 10..230 at 320x240), so the dimmed
 * picture behind the menu fills the panel as the game does; else the whole frame (0). */
int miyoo_menu_view_rect(float r[4]) {
    extern bool gfx_soft_stretch_on(void);
    int w = 0, h = 0;
    if (!gfx_soft_get_framebuffer(&w, &h) || w <= 0 || h <= 0 || !gfx_soft_stretch_on()) return 0;
    const float kx = (float)w / 320.0f, ky = (float)h / 240.0f;
    const float ax = ceilf(10.0f * kx - 0.5f), bx = ceilf(310.0f * kx - 0.5f), ay = ceilf(10.0f * ky - 0.5f), by = ceilf(230.0f * ky - 0.5f);
    r[0] = ax; r[1] = ay; r[2] = bx - ax; r[3] = by - ay;
    return 1;
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

/* port_exit (round 15b): let the present thread finish its flip, then hold it idle for good, so
 * the atexit fb_unpan has the last word on the display's pan offset (a flip after it left the
 * panel on a back buffer the launcher never draws to). Bounded (~0.5 s): a flip that never
 * returns doesn't hold up the exit, and port_exit's watchdog covers the rest. The audio is
 * paused so the device plays silence until the process ends.
 * X50 (2026-10-01, from ocarina-of-time's EXIT GAME freeze): then SDL is shut down, on every exit.
 * Without SDL_Quit, SDL 1.2's fbcon leaves the console, keyboard and video modes it set up,
 * and the next SDL program (the launcher after MENU) can hang at its first flip, as
 * GoldenEye's second bench run did. Skipped while a flip is still running (it would draw
 * into the unmapped screen); the watchdog covers an SDL_Quit that never returns. */
int port_platform_quiesce(void) {
    SDL_PauseAudio(1);
#if defined(MIYOO)
    miyoo_clock_restore(); /* the clock and governor the OS had (atexit does it too, for every other way out) */
#endif
    if (sPresentLock) {
        SDL_mutexP(sPresentLock);
        for (int i = 0; (sPresentPending || sPresentBusy) && i < 50; i++) {
            SDL_CondWaitTimeout(sPresentCond, sPresentLock, 10);
        }
        /* the lock stays held: the thread can't take another frame */
        if (sPresentBusy) return 0;
    }
    fb_unpan();
    SDL_Quit();
    return 1;
}

static void gfx_sdl12_swap_buffers_begin(void) {
    present();

    { /* MK64_DUMP=<dir> [MK64_DUMP_EVERY=n]: PPM dumps of the render buffer
       * (same contract as the desktop backend; drives the qemu smoke test).
       * Counts rendered frames -- dropped frames never reach the swap. */
        extern void gfx_soft_dump_ppm(const char *path);
        static const char *sDumpDir SAVESTATE_EXCLUDE; static long sEvery SAVESTATE_EXCLUDE = -1; static long sFrame SAVESTATE_EXCLUDE;
        if (sEvery < 0) {
            sDumpDir = getenv("SSB64_DUMP");
            const char *e = getenv("SSB64_DUMP_EVERY");
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
