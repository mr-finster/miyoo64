/**
 * Platform layer: the grafted MK64/OoT handheld renderer (gfx/gfx_pc.c display
 * list interpreter + gfx/gfx_soft.c rasterizer + gfx/gfx_sdl.c desktop window /
 * gfx/gfx_sdl12.c handheld), keyboard -> N64 pad, and the hooks the renderer
 * expects from its host (port.h).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#include "port_platform.h"
#include "gfx/gfx_pc.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/port.h"
#include "gfx/savestate.h"

extern struct GfxRenderingAPI gfx_soft_api;
extern struct GfxWindowManagerAPI gfx_sdl;
extern void gfx_soft_dump_ppm(const char* path);

/* ------------------------------------------------------------ renderer host --- */
uint32_t gPortProfFrame[PROF_COUNT];
uint32_t gPortProfLast[PROF_COUNT];
volatile uint32_t gPortProfFlipUs;
static volatile int sQuit;

void port_request_exit(void) { sQuit = 1; }
void port_gfx_overlay(void) { }
const char* port_save_dir(void) { return port_data_path(""); }
const char* port_save_path(const char* name) { return port_data_path(name); }
int port_fs_mkdir(const char* path) { return mkdir(path, 0777); }

void port_log_always(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    port_log("%s", buf);
}

/* The renderer's fallback for a segment the display list did not set: the CPU's
 * segment table (Segment_SetAddress), whose bases are native pointers here. */
extern uintptr_t gSegments[16];
void* port_seg_to_ptr(uintptr_t addr) {
    u32 seg = (addr >> 24) & 0xF;
    if (addr >= PORT_REAL_PTR_MIN || addr == 0) {
        return (void*) addr;
    }
    if (gSegments[seg] == 0 && seg != 0) {
        return NULL;
    }
    return (void*) (gSegments[seg] + (addr & 0x00FFFFFF));
}

/* Input: sdl/input.c. */

/* ------------------------------------------------------------------- audio --- */
/* The mixer runs the audio task synchronously; its output reaches the device
 * through osAiSetNextBuffer -> port_audio_push (sdl/audio_sdl.c). */
void port_platform_audio_task(u64* acmd, u32 bytes) {
    port_audio_run_alist(acmd, bytes);
}

/* ------------------------------------------------------------------ frames --- */
void port_platform_init(int headless) {
    if (headless) {
        setenv("SDL_VIDEODRIVER", "dummy", 0);
    }
    /* The desktop proxy draws a square output by default. */
    setenv("FZX_W", "240", 0);
    setenv("FZX_H", "240", 0);
    gfx_init(&gfx_sdl, &gfx_soft_api, "F-Zero X", false);
    port_audio_init(headless);
}

void port_platform_quit(void) {
    {
        extern u32 port_audio_cushion_peak_ms(void);
        port_log("audio: %u underruns, cushion up to %u ms, %u restarts after a gap\n", port_audio_underruns(),
                 port_audio_cushion_peak_ms(), port_audio_gaps());
    }
    {
        extern void gfx_soft_comp_counts(uint32_t * whole, uint32_t * over);
        uint32_t whole = 0, over = 0;
        gfx_soft_comp_counts(&whole, &over);
        port_log("composite: %u over the whole frame, %u over 2D drawn first\n", (unsigned) whole, (unsigned) over);
    }
    port_audio_quit();
    {   /* SDL down on every exit (EXIT GAME, the bench's end), not left to the process exit
         * (gfx_sdl12.c, X50). */
        extern int gfx_wm_shutdown(void);
        port_log(gfx_wm_shutdown() ? "exit: SDL shut down\n" : "exit: SDL left up (a flip still running)\n");
    }
}

int port_platform_poll(void) {
    return !sQuit;
}

/* The last display list the pacer skipped, while no later one was drawn (round 13d). The game
 * reads back "what the screen shows" for a transition's backdrop (port_gfx_readback): on the
 * N64 that is its last frame, but when the pacer skipped that frame the port's picture is an
 * older one. Leaving vehicle select cuts to black, then the speed screen's random intro captures
 * the screen: with the black frame skipped it captured vehicle select, and the phased strips
 * stretched it across the screen (the user: "the vehicle select screen gets wide for a split
 * second"). The readback now draws the skipped list first, unpresented. The game builds its
 * frames in two pools (D_8024DCE0[2]), so the last frame's list is intact while it builds this
 * one. */
static Gfx* sSkippedDl;
/* FZX_LOADPIC (2026-10-10, the device report "after a resume on the title, the N64 logo shows for a moment"): the
 * newest display list the port was given, drawn or skipped, kept in the state. A process that loads a state has
 * its own last picture in the renderer (a resumed state: the boot's N64 logo), and the screen kept it until
 * the game's next display list. After a load this list is drawn again (port_platform_load_picture): the picture the
 * saving process showed. Its pool is intact while the game builds the next frame in the other (see above). */
static Gfx* sLoadDl SAVESTATE_INCLUDE;
void port_platform_gfx_skipped(Gfx* dl) {
    sSkippedDl = dl;
    sLoadDl = dl;
}

int port_platform_loadpic_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("FZX_LOADPIC"); s = !(e && e[0] == '0'); }
    return s;
}

/* After a state load (savestate.c): draw and present the saving process's last display list again. */
void port_platform_load_picture(void) {
    Gfx* dl = sLoadDl;
    if (!port_platform_loadpic_on() || dl == NULL) return;
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
}

void port_platform_gfx_task(Gfx* dl) {
    u32 t0 = port_time_us();
    sSkippedDl = NULL;
    sLoadDl = dl;
    {
        /* FZX_TRACE_VI=<vi>: log every display-list command of the next task at that retrace */
        static long sTraceVi = -2;
        extern int gfx_trace_frames;
        if (sTraceVi == -2) {
            const char* e = getenv("FZX_TRACE_VI");
            sTraceVi = e ? atol(e) : -1;
        }
        if ((long) port_os_vi_count() == sTraceVi) {
            gfx_trace_frames = 1;
            sTraceVi = -1;
        }
    }
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
    gPortStats.gfxRendered++;
    (void) t0;
}

void port_platform_vi(u32 vi) {
    (void) vi;
    port_audio_vi();
}

void port_platform_show_framebuffer(const void* fb) {
    extern void gfx_present_rdram_framebuffer(const uint16_t* fb);
    gfx_present_rdram_framebuffer((const uint16_t*) fb);
}

/* The game reads what the VI shows back out of RDRAM (the transition backdrop,
 * ovl_i2/transition.c). The port draws into its own buffer, so first write the
 * last picture there: 320x240 RGBA5551, nearest from the native picture. */
void port_gfx_readback(void* frameBuffer) {
    extern uint32_t* gfx_soft_get_framebuffer(int* w, int* h);
    u16* dst = frameBuffer;
    int w, h, x, y;
    if (sSkippedDl != NULL) {
        extern int gfx_no_present;
        Gfx* dl = sSkippedDl;
        sSkippedDl = NULL;
        gfx_no_present = 1;
        gfx_start_frame();
        gfx_run(dl);
        gfx_end_frame();
        gfx_no_present = 0;
    }
    const uint32_t* src = gfx_soft_get_framebuffer(&w, &h);
    if (src == NULL || w <= 0 || h <= 0) {
        return;
    }
    /* The screen filled (gfx_pc.c gfx_frame_crop): the picture is the frame's box stretched over the
     * screen, so a layout pixel is read where the box put it, and outside the box it is the black
     * the N64 had there. */
    extern int gfx_frame_crop(float r[4]);
    float cr[4];
    const int crop = gfx_frame_crop(cr);
    for (y = 0; y < 240; y++) {
        const int fy = crop ? (int) floorf(((float) y + 0.5f - cr[1]) * (float) h / (cr[3] - cr[1])) : y * h / 240;
        const uint32_t* row = src + (fy < 0 ? 0 : fy >= h ? h - 1 : fy) * w;
        for (x = 0; x < 320; x++) {
            const int fx = crop ? (int) floorf(((float) x + 0.5f - cr[0]) * (float) w / (cr[2] - cr[0])) : x * w / 320;
            const uint32_t p = (fy < 0 || fy >= h || fx < 0 || fx >= w) ? 0u : row[fx];
            *dst++ = (u16) (((p >> 19) & 31) << 11 | ((p >> 11) & 31) << 6 | ((p >> 3) & 31) << 1 | 1);
        }
    }
}

void port_platform_dump(const char* path) {
    gfx_soft_dump_ppm(path);
}

/* Referenced by gfx_pc.c's RSP reset. */
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
