/* The frame: the game's display lists go to the renderer (hh/gfx: Paper Mario's Miyoo renderer, gfx_pc + gfx_soft),
 * a finished frame is presented (port_sdl.c) and, for tests, written as PPM. Host side.
 * After Paper Mario's port_gfx.c. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "gfx_pc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "port.h"
#include "savestate.h"
#include "../game/hh_port.h"

extern struct GfxRenderingAPI gfx_soft_api;
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
extern uint32_t hh_retraces;

float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

static void wm_init(const char *n, bool fs) { (void) n; (void) fs; }
static void wm_set_kb(bool (*a)(int), bool (*b)(int), void (*c)(void)) { (void) a; (void) b; (void) c; }
static void wm_set_fs_cb(void (*cb)(bool)) { (void) cb; }
static void wm_set_fs(bool e) { (void) e; }
static void wm_main_loop(void (*f)(void)) { (void) f; }
static void wm_dims(uint32_t *w, uint32_t *h) { *w = 320; *h = 240; }
static void wm_events(void) {}
static bool wm_start(void) { return true; }
static void wm_swap(void) {}
static double wm_time(void) { return port_time_us() / 1e6; }
static struct GfxWindowManagerAPI sWapi = { wm_init, wm_set_kb, wm_set_fs_cb, wm_set_fs, wm_main_loop, wm_dims,
                                            wm_events, wm_start, wm_swap, wm_swap, wm_time };

static const char *sDumpDir;
static uint32_t sDumpEvery = 1;

static void save_ppm(void) {
    char path[600];
    int w, h;
    static int sFrom = -1; /* HM64_DUMP_FROM=<frame>: none before it */
    if (sFrom < 0) { const char *e = getenv("HM64_DUMP_FROM"); sFrom = e ? atoi(e) : 0; }
    /* HM64_DUMP_AT=<retrace>,<retrace>,... (ascending; the release review): only the frames drawn at those
     * retraces, named by retrace (frame_r<retrace>.ppm) */
    static const char *sAt = (const char *) 1;
    if (sAt == (const char *) 1) sAt = getenv("HM64_DUMP_AT");
    if (sAt) {
        /* HM64_CLOCK=rand: the first frame at or past each listed clock, named by the listed value */
        extern uint32_t hh_test_clock(void);
        const uint32_t now = hh_test_clock();
        char *end;
        unsigned long want = strtoul(sAt, &end, 10);
        if (end == sAt || want > now) return;
        snprintf(path, sizeof path, "%s/frame_r%06lu.ppm", sDumpDir, want);
        do { /* past it: the next listed one (several at once only when the clock jumped over them) */
            if (*end != ',') { sAt = ""; break; }
            sAt = end + 1;
            want = strtoul(sAt, &end, 10);
        } while (end != sAt && want <= now);
    } else {
        if (port_frame % sDumpEvery != 0 || (int) port_frame < sFrom) return;
        snprintf(path, sizeof path, "%s/frame_%05u.ppm", sDumpDir, (unsigned) port_frame);
    }
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    FILE *f = fopen(path, "wb");
    if (!f) { port_log("gfx: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = malloc((size_t) w * 3);
    for (int y = 0; row && y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = fb[y * w + x];
            row[x * 3] = (uint8_t) (p >> 16); row[x * 3 + 1] = (uint8_t) (p >> 8); row[x * 3 + 2] = (uint8_t) p;
        }
        fwrite(row, 1, (size_t) w * 3, f);
    }
    free(row);
    fclose(f);
}

void hh_gfx_init(void) {
    static int done;
    if (done) return;
    done = 1;
    sDumpDir = getenv("HM64_DUMP");
    const char *every = getenv("HM64_DUMP_EVERY");
    if (every && atoi(every) > 0) sDumpEvery = (uint32_t) atoi(every);
    gfx_init(&sWapi, &gfx_soft_api, "Harvest Moon 64", false);
    port_log("gfx: renderer ready%s%s\n", sDumpDir ? ", frames to " : "", sDumpDir ? sDumpDir : "");
}

/* HM64_TRACE_AT=<frame>[,<tasks>]: the renderer's command trace for that frame's tasks */
void hh_gfx_task(void *gfx_list, int swap) {
    static uint32_t sTraceAt = 0, sTraceN = 3;
    static int sTraceInit;
    if (!sTraceInit) {
        sTraceInit = 1;
        const char *t = getenv("HM64_TRACE_AT");
        if (t && t[0]) {
            sTraceAt = (uint32_t) strtoul(t, NULL, 10);
            const char *c = strchr(t, ',');
            if (c) sTraceN = (uint32_t) strtoul(c + 1, NULL, 10);
        }
    }
    if (sTraceAt && port_frame == sTraceAt) {
        extern int gfx_trace_frames;
        gfx_trace_frames = (int) sTraceN;
        sTraceAt = 0;
    }
    const uint32_t t0 = port_time_us();
    gfx_start_frame();
    gfx_run((Gfx *) gfx_list);
    gfx_end_frame();
    if (swap) {
#ifdef PM64_SDL12
        port_video_present();
#endif
        if (sDumpDir) save_ppm();
        port_frame++;
    }
    port_perf_gfx_us += port_time_us() - t0;
}

void *port_seg_to_ptr(uintptr_t addr) {
    static int sWarned;
    if (addr < 0x10000000u && addr != 0 && sWarned < 20) {
        sWarned++;
        port_log("gfx: segmented address 0x%08X\n", (unsigned) addr);
    }
    return (void *) addr;
}

/* the game's three framebuffers (staticGfx.c FrameBuf: N64 RAM at 0x8038F800) */
extern uint16_t *FrameBuf[3];
bool port_is_framebuffer(const void *address) {
    const uint8_t *a = address;
    for (int i = 0; i < 3; i++)
        if (FrameBuf[i] && a >= (const uint8_t *) FrameBuf[i] && a < (const uint8_t *) FrameBuf[i] + 320 * 240 * 2) return true;
    return false;
}

uint32_t gPortProfFrame[16];
int port_letterbox_standard(void) { return 0; } /* the whole 320x240 frame on the panel */
