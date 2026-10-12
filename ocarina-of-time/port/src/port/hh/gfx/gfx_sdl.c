/**
 * SDL2 window backend (desktop debug). Presents the software rasterizer's
 * framebuffer and feeds input/quit events. The handheld build uses the SDL 1.2
 * variant with the dynamic-resolution ladder (gfx_sdl12.c); this one keeps it simple:
 * a fixed WxH window showing the rasterizer output 1:1.
 *
 * Headless (SDL_VIDEODRIVER=dummy): renderer creation may fail; that is
 * non-fatal — frames are still rasterized and can be dumped to PPM via
 * gfx_soft_dump_ppm() (see MK64_DUMP handling below).
 */
#ifndef HANDHELD /* the handheld build uses the SDL 1.2 backend in gfx_sdl12.c */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "gfx_window_manager_api.h"
#include "savestate.h"

/* Framebuffer owned by the rasterizer (gfx_soft.c). */
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
extern void gfx_soft_dump_ppm(const char *path); /* dumps current FB; no-op if none */

/* Input + lifecycle hooks (controller_sdl.c / main.c). */
extern void controller_sdl_key(int sdl_scancode, bool down);
extern void port_request_exit(void);

static SDL_Window   *sWindow   SAVESTATE_EXCLUDE;
static SDL_Renderer *sRenderer SAVESTATE_EXCLUDE;
static SDL_Texture  *sTexture  SAVESTATE_EXCLUDE;
static int sW = 320, sH = 240;      /* internal render resolution (N64 4:3) */
static int sScale = 2;              /* desktop window zoom */
static int sTexW SAVESTATE_EXCLUDE, sTexH SAVESTATE_EXCLUDE; /* texture size = rasterizer FB size */
static const char *sDumpDir SAVESTATE_EXCLUDE; /* MK64_DUMP: dir to write frame PPMs into */
static long sDumpEvery SAVESTATE_EXCLUDE, sFrame SAVESTATE_EXCLUDE; /* dump counter must not rewind on load */
static long sDumpFrom SAVESTATE_EXCLUDE;   /* MK64_DUMP_FROM: first frame eligible for a dump */
static int sIntPct SAVESTATE_EXCLUDE = 100;    /* MK64_INTRES=<pct>: 3D internal size, for testing the 2D-native composite */
extern void gfx_soft_set_render_res(int nat_w, int nat_h, int int_w, int int_h);

static void gfx_sdl_init(const char *game_name, bool start_in_fullscreen) {
    (void)start_in_fullscreen;
    const char *env;
    if ((env = getenv("OOT_W"))) sW = atoi(env);
    if ((env = getenv("OOT_H"))) sH = atoi(env);
    if ((env = getenv("OOT_SCALE"))) sScale = atoi(env);
    sDumpDir = getenv("OOT_DUMP");
    sDumpEvery = (env = getenv("OOT_DUMP_EVERY")) ? atol(env) : 60;
    sDumpFrom = (env = getenv("OOT_DUMP_FROM")) ? atol(env) : 0;
    if ((env = getenv("OOT_INTRES"))) { sIntPct = atoi(env); if (sIntPct < 25 || sIntPct > 100) sIntPct = 100; }
    gfx_soft_set_render_res(sW, sH, (sW * sIntPct / 100) & ~1, (sH * sIntPct / 100) & ~1);
    if ((env = getenv("OOT_TRACE_AT")) && atol(env) == 0) { extern int gfx_trace_frames; gfx_trace_frames = 2; }
    { extern int gfx_soft_bilinear; const char *b = getenv("OOT_BILINEAR"); if (b && b[0]) gfx_soft_bilinear = (b[0] == '1'); } /* unset = keep the SMOOTH default (round 13: it used to force SHARP) */
    { extern int gfx_soft_magbilin; const char *b = getenv("OOT_MAGBILIN"); if (b && b[0]) gfx_soft_magbilin = (b[0] == '1'); }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    }
    sWindow = SDL_CreateWindow(game_name ? game_name : "MK64",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               sW * sScale, sH * sScale, SDL_WINDOW_SHOWN);
    if (sWindow) {
        sRenderer = SDL_CreateRenderer(sWindow, -1, SDL_RENDERER_SOFTWARE);
        if (!sRenderer) fprintf(stderr, "SDL_CreateRenderer failed: %s (headless?)\n", SDL_GetError());
    }
    SDL_ShowCursor(SDL_DISABLE);
}

static void gfx_sdl_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = (uint32_t)sW;
    *height = (uint32_t)sH;
}

static void gfx_sdl_handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                port_request_exit();
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) { port_request_exit(); break; }
                controller_sdl_key(e.key.keysym.scancode, true);
                break;
            case SDL_KEYUP:
                controller_sdl_key(e.key.keysym.scancode, false);
                break;
            default: break;
        }
    }
}

static bool gfx_sdl_start_frame(void) {
    return true; /* no frameskip on desktop */
}

static void ensure_texture(int w, int h) {
    if (sTexture && sTexW == w && sTexH == h) return;
    if (sTexture) { SDL_DestroyTexture(sTexture); sTexture = NULL; }
    if (sRenderer) {
        sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    sTexW = w; sTexH = h;
}

static void gfx_sdl_swap_buffers_begin(void) {
    int w = 0, h = 0;
    uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    if (!fb || w <= 0 || h <= 0) return;

    if (sRenderer) {
        ensure_texture(w, h);
        if (sTexture) {
            SDL_UpdateTexture(sTexture, NULL, fb, w * (int)sizeof(uint32_t));
            SDL_RenderClear(sRenderer);
            SDL_RenderCopy(sRenderer, sTexture, NULL, NULL);
            SDL_RenderPresent(sRenderer);
        }
    }
    if (sDumpDir && sDumpEvery > 0 && sFrame >= sDumpFrom && (sFrame % sDumpEvery) == 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%06ld.ppm", sDumpDir, sFrame);
        gfx_soft_dump_ppm(path);
    }
    { /* MK64_TRACE_AT=<frame>: arm gfx_pc's per-draw trace for 2 frames there */
        static long sTraceAt = -2;
        if (sTraceAt == -2) { const char *e = getenv("OOT_TRACE_AT"); sTraceAt = e ? atol(e) : -1; }
        if (sTraceAt >= 0 && sFrame == sTraceAt) { extern int gfx_trace_frames; gfx_trace_frames = 2; }
    }
    sFrame++;
}

static void gfx_sdl_swap_buffers_end(void) { }

static double gfx_sdl_get_time(void) { return SDL_GetTicks() / 1000.0; }

/* Unused by gfx_pc.c on this backend — safe stubs. */
static void gfx_sdl_set_keyboard_callbacks(bool (*d)(int), bool (*u)(int), void (*a)(void)) { (void)d;(void)u;(void)a; }
static void gfx_sdl_set_fullscreen_changed_callback(void (*cb)(bool)) { (void)cb; }
static void gfx_sdl_set_fullscreen(bool e) { (void)e; }
static void gfx_sdl_main_loop(void (*run)(void)) { while (1) run(); }

struct GfxWindowManagerAPI gfx_sdl = {
    gfx_sdl_init,
    gfx_sdl_set_keyboard_callbacks,
    gfx_sdl_set_fullscreen_changed_callback,
    gfx_sdl_set_fullscreen,
    gfx_sdl_main_loop,
    gfx_sdl_get_dimensions,
    gfx_sdl_handle_events,
    gfx_sdl_start_frame,
    gfx_sdl_swap_buffers_begin,
    gfx_sdl_swap_buffers_end,
    gfx_sdl_get_time,
};

#endif /* !HANDHELD */
