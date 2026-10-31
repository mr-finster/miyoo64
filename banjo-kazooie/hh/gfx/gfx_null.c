/* gfx_null.c: the window manager of the headless x86 proxy (PLAT=pc): no window, no input; the renderer draws into
 * its own frame and hh_gfx.c dumps it (BK_DUMP). Same interface as gfx_sdl12.c. */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "gfx_window_manager_api.h"

static void n_init(const char *game_name, bool fs) { (void)game_name; (void)fs; }
static void n_set_keyboard_callbacks(bool (*d)(int), bool (*u)(int), void (*a)(void)) { (void)d; (void)u; (void)a; }
static void n_set_fullscreen_changed_callback(void (*cb)(bool)) { (void)cb; }
static void n_set_fullscreen(bool e) { (void)e; }
static void n_main_loop(void (*it)(void)) { it(); }
static void n_get_dimensions(uint32_t *w, uint32_t *h) { *w = 320; *h = 240; }
static void n_handle_events(void) {}
static bool n_start_frame(void) { return true; }
#include <stdio.h>
#include <stdlib.h>
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
extern unsigned int hh_vicount;
/* BK_DUMP=<dir>: every BK_DUMP_EVERY'th frame (default 60) from BK_DUMP_FROM, as <dir>/f<frame>_v<vi>.ppm;
 * BK_DUMP_AT=<frame>,<frame>,...: just those */
static void n_swap_begin(void)
{
    { extern void hh_gfx_vi_black_apply(void); hh_gfx_vi_black_apply(); }
    static long n, every = -1, from;
    static const char *dir, *at;
    char path[512];
    long f = n++;
    { static long t = -2; extern int gfx_trace_frames;
      if (t == -2) t = getenv("BK_TRACE_AT") ? atol(getenv("BK_TRACE_AT")) : -1;
      if (t >= 0 && f + 1 == t) gfx_trace_frames = 1;
      { static long tv = -2, fired; /* BK_TRACE_VI=<vi>: the first frame at or after that VI */
        if (tv == -2) tv = getenv("BK_TRACE_VI") ? atol(getenv("BK_TRACE_VI")) : -1;
        if (tv >= 0 && !fired && (long)hh_vicount >= tv) { gfx_trace_frames = 1; fired = 1; } } }
    if (every < 0) {
        dir = getenv("BK_DUMP");
        at = getenv("BK_DUMP_AT");
        every = getenv("BK_DUMP_EVERY") ? atol(getenv("BK_DUMP_EVERY")) : 60;
        from = getenv("BK_DUMP_FROM") ? atol(getenv("BK_DUMP_FROM")) : 0;
        if (every <= 0) every = 1;
    }
    { /* BK_DUMP_VI=<first>:<step>: the first frame at or after each VI target, as <dir>/vi<target>.ppm (to pair
       * with mupen64plus --testshots, which counts VIs) */
        static long vnext = -2, vstep;
        if (vnext == -2) {
            const char *e = getenv("BK_DUMP_VI");
            vnext = e ? atol(e) : -1;
            vstep = (e && strchr(e, ':')) ? atol(strchr(e, ':') + 1) : 60;
        }
        if (vnext >= 0 && getenv("BK_DUMP") && (long)hh_vicount >= vnext) {
            int w, h, i;
            uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
            FILE *o;
            snprintf(path, sizeof(path), "%s/vi%06ld.ppm", getenv("BK_DUMP"), vnext);
            while ((long)hh_vicount >= vnext) vnext += vstep;
            if (fb != NULL && (o = fopen(path, "wb")) != NULL) {
                fprintf(o, "P6\n%d %d\n255\n", w, h);
                for (i = 0; i < w * h; i++) {
                    unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] };
                    fwrite(px, 1, 3, o);
                }
                fclose(o);
            }
        }
    }
    if (dir == NULL || f < from)
        return;
    if (at != NULL) {
        char key[24];
        const char *p = at;
        int hit = 0;
        while (*p) { if (atol(p) == f) hit = 1; while (*p && *p != ',') p++; if (*p) p++; }
        (void)key;
        if (!hit) return;
    } else if ((f - from) % every != 0)
        return;
    {
        int w, h, i;
        uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
        FILE *o;
        snprintf(path, sizeof(path), "%s/f%06ld_v%06u.ppm", dir, f, hh_vicount);
        if (fb == NULL || (o = fopen(path, "wb")) == NULL) return;
        fprintf(o, "P6\n%d %d\n255\n", w, h);
        for (i = 0; i < w * h; i++) {
            unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] };
            fwrite(px, 1, 3, o);
        }
        fclose(o);
    }
}
static void n_swap_end(void) {}
static double n_get_time(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

struct GfxWindowManagerAPI gfx_sdl = { n_init, n_set_keyboard_callbacks, n_set_fullscreen_changed_callback, n_set_fullscreen,
                                       n_main_loop, n_get_dimensions, n_handle_events, n_start_frame, n_swap_begin,
                                       n_swap_end, n_get_time };
