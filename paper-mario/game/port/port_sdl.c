/* The native game on the Miyoo Mini (M2): the screen, the buttons and the frame pacing. Host side, device only
 * (PM64_SDL12, SDL 1.2); the desktop proxy writes PPMs instead (port_gfx.c).
 *
 * Miyoo Mini (MIYOO, with HANDHELD still defined; README_MIYOO.md, notes/miyoo.md): the renderer's 320x240 frame whole (the
 * N64's 4:3, no squeeze), doubled and turned onto the 640x480 panel by a present thread (kit miyoo_panel.c), so
 * the game thread never waits on a flip. Buttons: the d-pad is the stick (+-80, running speed); A, B and START are the
 * N64's; X is C-down (the partner's ability), Y is Z (spin dash), L1 is C-left (partners), R1 is C-right (items),
 * SELECT / R2 = C-up (the status bar) and L2 = N64 R; MENU (Escape) opens the Miyoo menu (miyoo_menu.c: RESUME, SAVE
 * STATE, LOAD STATE, EXIT GAME); the OS's SIGTERM (power off, any kill) takes EXIT GAME's path: the auto save, then
 * quit (port_hh.c). */
#ifdef PM64_SDL12
#include <SDL/SDL.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "port.h"
#include "savestate.h"
#include "handheld_menu.h"
#if defined(MIYOO)
#include <stdio.h>
#include <math.h>
#include "miyoo_panel.h"
#include "miyoo_worker.h"
#include "miyoo_clock.h"
#endif

void port_hh_init(SDL_Surface *screen);
int port_hh_take_power(void);

#ifndef SDL_TRIPLEBUF
#define SDL_TRIPLEBUF SDL_DOUBLEBUF
#endif

extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);

static SDL_Surface *sScreen;

/* SDL_TRIPLEBUF pans /dev/fb0 across three pages; a crash must put the console back on page 0 */
static void fb_unpan(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return;
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.yoffset != 0) { v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v); }
    close(fd);
}

/* ---------------------------------------------------------------- the Miyoo Mini's panel
 * The game thread copies each finished 320x240 frame into sStage32 and goes on; the present thread doubles and turns it
 * onto the panel (miyoo_panel_present32: NEON, the notice drawn first) and flips, which can wait for the panel. One
 * frame in flight: a frame that comes while the last is still unread waits for it (never dropped, never doubled). The
 * menu takes the panel with present_acquire (no frame pending, no flip running) and gives it back after. */
#define MW 320
#define MH 240
static uint32_t sStage32[MW * MH];
static SDL_mutex *sPresLock;
static SDL_cond *sPresCond;
static SDL_Thread *sPresThread;
static int sPresPending, sPresFlipping;
static uint32_t sStageFrame; /* the game frame in sStage32 */

/* ---- the fill (PM64_STRETCH, the handheld's stretch on the 4:3 panel; the user's device report of 2026-10-12: inset
 * black borders in the field and in battles)
 * The field and the battles draw the scene in the camera's 296x200 viewport at (12, 20) and clear the N64's overscan
 * margin around it to black (background_gfx.c; port_main.c port_letterbox_standard says when this frame did). The
 * the handheld's panel stretches that box over its screen (port_gfx.c port_panel_begin); the Mini showed the whole 320x240
 * frame doubled, border and all. Here the present stretches the same rectangle over the whole picture area (640x480):
 * the same 8-frame zoom when a scene with the box starts or ends, the whole frame where none is drawn (the logos, the
 * title, the chapter cards, the menus that draw no frame), and the paused scene keeps it (port_letterbox_keep). The box
 * fills the 4:3 panel exactly (x 2.16, y 2.4: the scene's 1.48:1 shown at 4:3, as the handheld shows it at 1:1).
 * Sharp bilinear: each source pixel stays a solid block of panel pixels and only the one panel pixel that straddles
 * two source pixels is blended (the weight (f - 0.5) * scale + 0.5, clamped), so text and the HUD stay crisp; at the
 * plain 2x it is exactly the old present (that frame takes miyoo_panel_present32 itself). The renderer's frame is not
 * touched: the stretch is the present's only. PM64_STRETCH=0 (debug.cfg too): the bordered picture, as before. */
static float sStageRect[4] = { 0, 0, MW, MH };  /* the stage's source rectangle (x, y, w, h) */
static float sFillT = -1;                         /* 0 = the whole frame, 1 = the box (the 8-frame zoom) */
static void fill_rect_for_frame(float r[4]) {
    const float target = port_letterbox_standard() ? 1.0f : 0.0f;
    if (sFillT < 0) sFillT = target;
    else if (sFillT < target) sFillT = sFillT + 0.125f > target ? target : sFillT + 0.125f;
    else if (sFillT > target) sFillT = sFillT - 0.125f < target ? target : sFillT - 0.125f;
    const float t = sFillT;
    r[0] = 12 * t; r[1] = 20 * t; r[2] = MW - 24 * t; r[3] = MH - 40 * t;
}
static uint16_t sFxL[2048], sFyT[2048]; /* per panel column / row: the left / upper source pixel */
static uint8_t sFxW[2048], sFyW[2048];  /* the right / lower one's weight, 0..128 */
static float sFxKey[3] = { -1, -1, -1 }, sFyKey[3] = { -1, -1, -1 };
static void sharp_axis(uint16_t *idx, uint8_t *wt, int n, float r0, float rw, int smax, float *key) {
    if (key[0] == r0 && key[1] == rw && key[2] == (float) n) return;
    const float s = (float) n / rw;
    for (int o = 0; o < n; o++) {
        const float t = r0 + (o + 0.5f) / s - 0.5f;
        int i = (int) floorf(t);
        float f = (t - (float) i - 0.5f) * s + 0.5f;
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        int w = (int) (f * 128.0f + 0.5f);
        if (i < 0) { i = 0; w = 0; }
        if (i >= smax - 1) { i = smax - 1; w = 0; }
        idx[o] = (uint16_t) i; wt[o] = (uint8_t) w;
    }
    key[0] = r0; key[1] = rw; key[2] = (float) n;
}
static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w) {
    if (w == 0) return a;
    if (w == 128) return b;
    const uint32_t nw = 128 - w;
    const uint32_t rb = (((a & 0xFF00FFu) * nw + (b & 0xFF00FFu) * w + 0x400040u) >> 7) & 0xFF00FFu;
    const uint32_t g = (((a & 0xFF00u) * nw + (b & 0xFF00u) * w + 0x4000u) >> 7) & 0xFF00u;
    return rb | g;
}
/* the stage's rectangle r over the panel's picture area (the kit's 2x area, centred), turned like the kit's present */
static void fill_present(uint32_t *src, const float r[4]) {
    static SDL_Surface *sWrap;
    if (!sWrap) sWrap = SDL_CreateRGBSurfaceFrom(src, MW, MH, 32, MW * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (sWrap) miyoo_menu_overlay(sWrap); /* the notice, as miyoo_panel_present32 draws it */
    SDL_Surface *const p = sScreen;
    const int dw = p->w, dh = p->h, rot = miyoo_panel_rotated();
    int ox, oy, tw, th; /* the kit's picture area: 2x on a Mini, the full-height fill on a Mini v4 / Flip */
    miyoo_panel_area(MW, MH, &ox, &oy, &tw, &th);
    if (tw > 2048 || th > 2048) return;
    sharp_axis(sFxL, sFxW, tw, r[0], r[2], MW, sFxKey);
    sharp_axis(sFyT, sFyW, th, r[1], r[3], MH, sFyKey);
    static uint32_t rowbuf[2][2048];
    int rowsrc[2] = { -1, -1 };
    const uint32_t a = p->format->Amask;
    if (SDL_MUSTLOCK(p)) SDL_LockSurface(p);
    if (ox > 0) /* the sides of a filled Mini v4 / Flip panel: black (the menu or an earlier mode may have drawn there) */
        for (int y = 0; y < dh; y++) {
            uint32_t *d = (uint32_t *) ((uint8_t *) p->pixels + (size_t) y * p->pitch);
            for (int x = 0; x < ox; x++) d[x] = d[dw - 1 - x] = a;
        }
    for (int y = 0; y < th; y++) {
        const uint32_t *h2[2];
        for (int q = 0; q < 2; q++) { /* source rows sFyT[y] and the one below, across (a two-row cache) */
            const int sr = sFyT[y] + (q && sFyT[y] < MH - 1 ? 1 : 0), slot = sr & 1;
            if (rowsrc[slot] != sr) {
                const uint32_t *srow = src + (size_t) sr * MW;
                for (int x = 0; x < tw; x++) rowbuf[slot][x] = lerp_px(srow[sFxL[x]], srow[sFxL[x] + (sFxL[x] < MW - 1)], sFxW[x]);
                rowsrc[slot] = sr;
            }
            h2[q] = rowbuf[slot];
        }
        const uint32_t w = sFyW[y];
        uint8_t *const drow = (uint8_t *) p->pixels + (size_t) (rot ? dh - 1 - (oy + y) : oy + y) * p->pitch;
        if (rot) {
            uint32_t *d = (uint32_t *) drow + (dw - 1 - ox);
            for (int x = 0; x < tw; x++) d[-x] = lerp_px(h2[0][x], h2[1][x], w) | a;
        } else {
            uint32_t *d = (uint32_t *) drow + ox;
            for (int x = 0; x < tw; x++) d[x] = lerp_px(h2[0][x], h2[1][x], w) | a;
        }
    }
    if (SDL_MUSTLOCK(p)) SDL_UnlockSurface(p);
}
static void stage_present(void) {
    const float *r = sStageRect;
    if (r[0] == 0 && r[1] == 0 && r[2] == MW && r[3] == MH) miyoo_panel_present32(sStage32, MW, MH); /* the plain 2x */
    else fill_present(sStage32, r);
}
/* the menu's dimmed backdrop (miyoo_menu.c, weak): the picture as the panel shows it */
int miyoo_menu_view_rect(float r[4]) { memcpy(r, sStageRect, sizeof sStageRect); return 1; }
/* PM64_PANEL_DUMP=<dir> [PM64_PANEL_DUMP_EVERY=n] (test): the panel as the player sees it (the turn undone), every nth
 * presented frame, as PPM (panel_<game frame>.ppm), after the present and before the flip. */
static void panel_dump(uint32_t frame) {
    static const char *dir;
    static int every = -1;
    if (every < 0) {
        dir = getenv("PM64_PANEL_DUMP");
        const char *e = getenv("PM64_PANEL_DUMP_EVERY");
        every = e && atoi(e) > 0 ? atoi(e) : 1;
    }
    if (!dir || !dir[0] || frame % (uint32_t) every || !sScreen || sScreen->format->BitsPerPixel != 32) return;
    char path[512];
    snprintf(path, sizeof path, "%s/panel_%05u.ppm", dir, (unsigned) frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int w = sScreen->w, h = sScreen->h, rot = miyoo_panel_rotated();
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
    uint8_t *row = malloc((size_t) w * 3);
    for (int y = 0; row && y < h; y++) {
        const uint32_t *src = (const uint32_t *) ((const uint8_t *) sScreen->pixels + (size_t) (rot ? h - 1 - y : y) * sScreen->pitch);
        for (int x = 0; x < w; x++) {
            const uint32_t p = src[rot ? w - 1 - x : x];
            row[x * 3] = (uint8_t) (p >> 16); row[x * 3 + 1] = (uint8_t) (p >> 8); row[x * 3 + 2] = (uint8_t) p;
        }
        fwrite(row, 1, (size_t) w * 3, f);
    }
    free(row);
    if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
    fclose(f);
}
static int present_thread(void *arg) {
    int pinned = 0;
    (void) arg;
    SDL_mutexP(sPresLock);
    for (;;) {
        while (!sPresPending) SDL_CondWait(sPresCond, sPresLock);
        if (!pinned && miyoo_worker_ready()) { pinned = 1; miyoo_worker_pin_self(1); } /* core 1, beside the worker */
        stage_present();
        panel_dump(sStageFrame);
        sPresPending = 0;
        sPresFlipping = 1;
        SDL_CondBroadcast(sPresCond);
        SDL_mutexV(sPresLock); /* the flip may wait for the panel: never under the lock */
        miyoo_panel_flip();
        SDL_mutexP(sPresLock);
        sPresFlipping = 0;
        SDL_CondBroadcast(sPresCond);
    }
    return 0;
}
static void present_acquire(void) {
    if (!sPresLock) return;
    SDL_mutexP(sPresLock);
    while (sPresPending || sPresFlipping) SDL_CondWait(sPresCond, sPresLock);
}
static void present_release(void) {
    if (sPresLock) SDL_mutexV(sPresLock);
}

void port_video_init(void) {
    SDL_Init(SDL_INIT_VIDEO); /* before any audio: the Mini's SDL does its MI system init here (the dsp driver needs it) */
    SDL_ShowCursor(SDL_DISABLE);
    sScreen = miyoo_panel_open();
    if (!sScreen) { port_log("video: no panel: %s\n", SDL_GetError()); exit(1); }
    port_log("video: panel %dx%d, %d bpp%s, the 320x240 frame doubled (filled on a bigger panel)\n", sScreen->w, sScreen->h, sScreen->format->BitsPerPixel,
             miyoo_panel_rotated() ? ", turned 180" : "");
    miyoo_clock_boost(); /* PM64_CPUCLOCK (debug.cfg too): 1500 MHz on the Mini, 1800 on the Mini Plus; put back at exit */
    sPresLock = SDL_CreateMutex();
    sPresCond = SDL_CreateCond();
    sPresThread = sPresLock && sPresCond ? SDL_CreateThread(present_thread, NULL) : NULL;
    if (!sPresThread) port_log("video: no present thread: the game thread flips\n");
    port_hh_init(sScreen);
}

void port_video_crash(void) { miyoo_clock_restore(); }

int port_video_shutdown(void) {
    sigset_t s;

    if (!sScreen) return 1;
    present_acquire(); /* no flip running; the thread then waits for a frame that never comes */
    sScreen = NULL;
    miyoo_clock_restore();
    signal(SIGALRM, SIG_DFL);
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    alarm(5);
    SDL_Quit();
    return 1;
}

void port_video_present(void) {
    int w, h;
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    if (!sScreen || !fb || w != MW || h != MH) return;
    { /* PM64_LOADPIC (the ports' round of 2026-10-10, as DKR and MK64 on the Mini): a start that resumes the
       * auto save loads it a few frames in (savestate.c HH_BOOT_LOAD_FRAME); those frames -- the game's boot -- are not
       * shown, and the panel keeps the RESUME GAME screen until the first frame after the load. 0: as before. */
        static int sOn = -1, sHidden;
        if (sOn < 0) { const char *e = getenv("PM64_LOADPIC"); sOn = !(e && e[0] == '0'); }
        if (sOn && savestate_boot_load_pending()) { sHidden++; return; }
        if (sHidden > 0) { port_log("loadpic: %d frames before the resume not shown\n", sHidden); sHidden = -1; }
    }
    if (!sPresThread) { /* no thread: here, then the flip */
        memcpy(sStage32, fb, sizeof sStage32);
        fill_rect_for_frame(sStageRect);
        stage_present();
        panel_dump(port_frame);
        port_loop_stage = PORT_STAGE_FLIP;
        miyoo_panel_flip();
        port_loop_stage = PORT_STAGE_GAME;
        return;
    }
    port_loop_stage = PORT_STAGE_FLIP; /* waiting for the present thread to take the last frame */
    SDL_mutexP(sPresLock);
    while (sPresPending) SDL_CondWait(sPresCond, sPresLock);
    memcpy(sStage32, fb, sizeof sStage32);
    sStageFrame = port_frame;
    fill_rect_for_frame(sStageRect); /* the game's state for this frame, read here on the game thread */
    sPresPending = 1;
    SDL_CondBroadcast(sPresCond);
    SDL_mutexV(sPresLock);
    port_loop_stage = PORT_STAGE_GAME;
}

/* ---------------------------------------------------------------- buttons */
enum { B_A = 0x8000, B_B = 0x4000, B_Z = 0x2000, B_START = 0x1000, B_L = 0x0020, B_R = 0x0010, B_CU = 0x0008,
       B_CD = 0x0004, B_CL = 0x0002, B_CR = 0x0001 };
static uint8_t sKey[128];

void port_script_poll(void);
void port_script_read(uint16_t *button, int8_t *stick_x, int8_t *stick_y);

/* The Mini's keys (miyoo_panel.h) as letters: SELECT and R2 = C-up ('c', the status bar), L2 = N64 R ('o', page down,
 * the pause menu's next tab). -1: not a game key. */
static int miyoo_key(int sym) {
    switch (sym) {
    case MIYOO_KEY_UP: return 'u';
    case MIYOO_KEY_DOWN: return 'd';
    case MIYOO_KEY_LEFT: return 'l';
    case MIYOO_KEY_RIGHT: return 'r';
    case MIYOO_KEY_A: return 'a';
    case MIYOO_KEY_B: return 'b';
    case MIYOO_KEY_X: return 'x';
    case MIYOO_KEY_Y: return 'y';
    case MIYOO_KEY_L1: return 'm';
    case MIYOO_KEY_R1: return 'n';
    case MIYOO_KEY_L2: return 'o';
    case MIYOO_KEY_R2: case MIYOO_KEY_SELECT: return 'c';
    case MIYOO_KEY_START: return 's';
    default: return -1;
    }
}

void port_input_poll(void) {
    port_script_poll(); /* PM64_KEYS, as on the desktop proxy (tests under qemu-arm) */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) port_quit = 1;
        if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) continue;
        const int sym = e.key.keysym.sym, down = e.type == SDL_KEYDOWN;
        if (sym == MIYOO_KEY_MENU && down) { /* MENU: modal until RESUME or EXIT GAME; a save it asks for runs next */
            present_acquire();
            handheld_menu_run(sScreen);
            present_release();
            port_state_stage();
            continue;
        }
        const int k = miyoo_key(sym);
        if (k >= 0) sKey[k] = (uint8_t) down;
    }
    if (port_hh_take_power()) { /* SIGTERM (the OS powering off, any kill): EXIT GAME's save, then quit */
        port_log("power: SIGTERM: the auto save, then quit\n");
        port_state_stage();
        savestate_request_exit_save();
    }
}

/* the menu hands back the key releases it swallowed, so no button stays held */
void controller_handheld_key(int sym, int down) {
    const int k = miyoo_key(sym);
    if (k >= 0) sKey[k] = (uint8_t) down;
}

void port_input_read(uint16_t *button, int8_t *stick_x, int8_t *stick_y) {
    uint16_t b = 0;
    if (sKey['a']) b |= B_A;
    if (sKey['b']) b |= B_B;
    if (sKey['s']) b |= B_START;
    if (sKey['x']) b |= B_CD;
    if (sKey['y']) b |= B_Z;
    if (sKey['m']) b |= B_CL;
    if (sKey['n']) b |= B_CR;
    if (sKey['o']) b |= B_R; /* L2 (menus page down, the pause menu's tabs) */
#if defined(MIYOO)
    if (sKey['c']) b |= B_CU; /* SELECT or R2 */
#endif
    uint16_t sb;
    int8_t sx, sy;
    port_script_read(&sb, &sx, &sy);
    *button = b | sb;
    *stick_x = (int8_t) (sKey['l'] ? -80 : sKey['r'] ? 80 : sx);
    *stick_y = (int8_t) (sKey['u'] ? 80 : sKey['d'] ? -80 : sy);
}
#endif /* PM64_SDL12 */
