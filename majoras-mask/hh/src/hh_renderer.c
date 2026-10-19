/* OotPspRenderer_* (the game's renderer hooks) on the MK64 handheld renderer:
 * gfx_pc.c (F3DEX2 display-list interpreter, CPU transform/clip) + gfx_soft.c
 * (software rasterizer) + gfx_sdl.c (desktop SDL2) / gfx_sdl12.c (handheld). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ultra64.h"
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

extern struct GfxRenderingAPI gfx_soft_api;
/* gfx_sdl.c (desktop SDL2) and gfx_sdl12.c (handheld SDL 1.2) both export gfx_sdl. */
extern struct GfxWindowManagerAPI gfx_sdl;
#define HH_WAPI gfx_sdl

static bool sInit;

void OotPspRenderer_Init(void) {
    if (!sInit) {
        gfx_init(&HH_WAPI, &gfx_soft_api, "Ocarina of Time", false);
        sInit = true;
    }
}

#include "port.h"
#if defined(MIYOO)
#include "miyoo_worker.h"
#endif

/* PERF: one line per 60 rendered frames -- frame interval (what the player sees),
 * the render time (interpreter + raster + present) and MK64's per-bucket profile. */
static uint32_t sPerfFrames, sPerfRenderUs, sPerfMaxRenderUs, sPerfLastUs, sPerfIntervalUs, sPerfMaxIntervalUs;
/* Intervals summed this window: 59 in the first (no previous frame), 60 after.
 * Dividing by a fixed 59 read a steady 50.0 ms as 50.8 (20.0 fps as 19.7). */
static uint32_t sPerfIntervals;
static uint32_t sPerfBuckets[PROF_COUNT];
static uint32_t sPerfWin; /* frames per PERF line: 60 (Bench, alt, OOT_LOGVERBOSE) or 200 */

/* HITCH: a line for each frame interval over 60 ms (a 20 fps frame is 50) that
 * splits it into: wait (the swap and the pacer's sleep after the previous
 * frame), logic (the game update that built this frame, and whatever it waited
 * on) and this frame's render. Also what ran meanwhile: DMA (ROM data reads and
 * decompression, on the DMA thread) and JPEG (prerendered room backgrounds).
 * At most 200 a session. The PERF line gains logic avg/max at the end.
 * Normal play logs only intervals over 100 ms (2026-10-06: a session logged 45 lines of 60-80 ms); a Bench
 * child, a cmneon/bil64=alt A/B and OOT_LOGVERBOSE=1 (debug.cfg: logverbose=1) keep 60 ms. */
static uint32_t sHitchUs; /* the HITCH threshold, set with sPerfWin */
static uint32_t sPerfWaitUs, sPerfLogicUs, sPerfMaxLogicUs, sPerfTotalFrames, sHitchLines;
static uint32_t sHitchDmaUs, sHitchDmaBytes, sHitchDmaN, sHitchJpegUs; /* since the last frame; other threads add */

/* hh_sched.c, after the swap and the pacer's sleep. */
void HhPerf_PaceDone(void) {
    sPerfWaitUs = port_time_us() - sPerfLastUs;
}

/* z_std_dma.c's DMA thread, per request. */
void HhPerf_Dma(uint32_t us, uint32_t bytes) {
    __atomic_fetch_add(&sHitchDmaUs, us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sHitchDmaBytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sHitchDmaN, 1u, __ATOMIC_RELAXED);
}

/* hh_sched.c, per JPEG task. */
void HhPerf_Jpeg(uint32_t us) {
    __atomic_fetch_add(&sHitchJpegUs, us, __ATOMIC_RELAXED);
}

/* The handheld backend shuts its present thread and SDL down on every exit (X50); elsewhere, nothing. */
__attribute__((weak)) int gfx_wm_shutdown(void) {
    return 1;
}

/* The handheld backend (gfx_sdl12.c) measures the spacing of frames on screen; elsewhere, none. */
__attribute__((weak)) void gfx_flip_stats(uint32_t *max_us, uint32_t *late) {
    *max_us = 0;
    *late = 0;
}

/* The device bench's child runs (hh_bench.c): every frame interval from OOT_BENCH_FROM to the exit,
 * and one BENCH line into $OOT_BENCH_LOG at the exit (HhPerf_BenchReport, from main). */
#define BENCH_MAX 1200
static int sBench = -1;
static uint32_t sBenchFrom, sBenchN, sBenchIv[BENCH_MAX];
static uint64_t sBenchRender, sBenchR3d, sBenchKpxSum;

static int HhPerf_BenchCmp(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

void HhPerf_BenchReport(void) {
    const char* path = getenv("OOT_BENCH_LOG");
    FILE* f;
    uint64_t sum = 0;
    uint32_t i;

    if (sBench <= 0 || sBenchN == 0 || path == NULL || (f = fopen(path, "a")) == NULL) {
        return;
    }
    for (i = 0; i < sBenchN; i++) {
        sum += sBenchIv[i];
    }
    const float mean = (float)sum / sBenchN;
    qsort(sBenchIv, sBenchN, sizeof(sBenchIv[0]), HhPerf_BenchCmp);
    const uint32_t p99 = sBenchIv[sBenchN - 1 - sBenchN / 100]; /* the 1%-low frame */
    uint32_t over55 = 0;
    for (i = 0; i < sBenchN; i++) {
        over55 += sBenchIv[i] > 55000;
    }
    fprintf(f, "BENCH %s | frames %u | fps %.2f | 1%% low %.1f | worst %.1f ms | >55 ms %u | render %.1f r3d %.1f | kpx %.0f\n",
            getenv("OOT_BENCH_CHILD"), (unsigned)sBenchN, 1e6f / mean, 1e6f / (float)p99,
            sBenchIv[sBenchN - 1] / 1000.0f, (unsigned)over55, sBenchRender / (float)sBenchN / 1000.0f,
            sBenchR3d / (float)sBenchN / 1000.0f, sBenchKpxSum / (float)sBenchN / 1000.0f);
    fclose(f);
}

int gHhRoomNum = -1; /* hh_main.c: the room being played (-1 outside play), for the SCENE lines */
static void HhPerf_Frame(uint32_t renderUs) {
    uint32_t now = port_time_us();
    int i;

    const uint32_t dmaUs = __atomic_exchange_n(&sHitchDmaUs, 0u, __ATOMIC_RELAXED);
    const uint32_t dmaBytes = __atomic_exchange_n(&sHitchDmaBytes, 0u, __ATOMIC_RELAXED);
    const uint32_t dmaN = __atomic_exchange_n(&sHitchDmaN, 0u, __ATOMIC_RELAXED);
    const uint32_t jpegUs = __atomic_exchange_n(&sHitchJpegUs, 0u, __ATOMIC_RELAXED);

    sPerfTotalFrames++;
    {   /* console.log: the scene and room from this frame on (2026-10-02: a slow stretch in a gameplay log could not be
         * placed), numbered like the HITCH lines; only when they change */
        extern int gHhSceneId;
        static int sLogScene = -2, sLogRoom = -2;
        if (gHhSceneId != sLogScene || gHhRoomNum != sLogRoom) {
            sLogScene = gHhSceneId;
            sLogRoom = gHhRoomNum;
            port_log_always("SCENE f%u scene %d room %d\n", (unsigned) sPerfTotalFrames, sLogScene, sLogRoom);
        }
    }
    if (sPerfTotalFrames == 1) {
        extern const char* port_clock_source(void);
        port_log_always("CLOCK %s\n", port_clock_source()); /* hh_port.c: OOT_VDSOCLOCK */
    }
    if (sPerfLastUs != 0) {
        uint32_t iv = now - sPerfLastUs;
        const uint32_t used = sPerfWaitUs + renderUs;
        const uint32_t logic = iv > used ? iv - used : 0;

        sPerfIntervalUs += iv;
        sPerfIntervals++;
        if (sBench < 0) {
            sBench = getenv("OOT_BENCH_CHILD") != NULL;
            sBenchFrom = getenv("OOT_BENCH_FROM") ? (uint32_t)atoi(getenv("OOT_BENCH_FROM")) : 0;
        }
        if (sBench > 0 && sPerfTotalFrames > sBenchFrom && sBenchN < BENCH_MAX) {
            extern uint64_t gfx_soft_span_px;
            static uint64_t sPx0;
            const uint64_t px = gfx_soft_span_px;
            sBenchIv[sBenchN++] = iv;
            sBenchRender += renderUs;
            sBenchR3d += gPortProfFrame[PROF_R3D];
            sBenchKpxSum += px >= sPx0 ? px - sPx0 : px; /* this frame's span pixels (PERF zeroes the sum every 60) */
            sPx0 = px;
        }
        if (iv > sPerfMaxIntervalUs) {
            sPerfMaxIntervalUs = iv;
        }
        sPerfLogicUs += logic;
        if (logic > sPerfMaxLogicUs) {
            sPerfMaxLogicUs = logic;
        }
        if (iv > sHitchUs && sPerfTotalFrames > 30 && sHitchLines < 200) {
            sHitchLines++;
            port_log_always("HITCH f%u iv %.1f = wait %.1f + logic %.1f + render %.1f (r3d %.1f r2d %.1f tex %.1f) | "
                            "dma %u %uKB %.1f ms | jpeg %.1f ms\n",
                            (unsigned) sPerfTotalFrames, iv / 1000.0f, sPerfWaitUs / 1000.0f, logic / 1000.0f,
                            renderUs / 1000.0f, gPortProfFrame[PROF_R3D] / 1000.0f, gPortProfFrame[PROF_R2D] / 1000.0f,
                            gPortProfFrame[PROF_TEX] / 1000.0f, (unsigned) dmaN, (unsigned) (dmaBytes >> 10),
                            dmaUs / 1000.0f, jpegUs / 1000.0f);
        }
    }
    sPerfLastUs = now;
    sPerfRenderUs += renderUs;
    if (renderUs > sPerfMaxRenderUs) {
        sPerfMaxRenderUs = renderUs;
    }
    for (i = 0; i < PROF_COUNT; i++) {
        sPerfBuckets[i] += gPortProfFrame[i];
        gPortProfLast[i] = gPortProfFrame[i];
        gPortProfFrame[i] = 0;
    }
    /* Normal play: one PERF line per 200 frames (~10 s at 20 fps). A Bench child, a cmneon/bil64=alt device A/B
     * (it flips per PERF window) and OOT_LOGVERBOSE=1 (debug.cfg: logverbose=1) keep the 60-frame window. */
    if (sPerfWin == 0) {
        const char* a = getenv("OOT_CMNEON");
        const char* b = getenv("OOT_BIL64");
        const char* v = getenv("OOT_LOGVERBOSE");
        sPerfWin = (getenv("OOT_BENCH_CHILD") != NULL || (a != NULL && a[0] == 'a') || (b != NULL && b[0] == 'a') ||
                    (v != NULL && v[0] == '1'))
                       ? 60
                       : 200;
        sHitchUs = sPerfWin == 60 ? 60000 : 100000;
    }
    if (++sPerfFrames == sPerfWin) {
        float n = (float) sPerfWin;
        float iv = sPerfIntervals ? sPerfIntervalUs / (float) sPerfIntervals / 1000.0f : 0.0f;

        extern uint64_t gfx_soft_span_px, gfx_soft_tris;
        extern int gfx_soft_cmneon_alt_step(void);
        const int neon = gfx_soft_cmneon_alt_step(); /* cmneon=alt: the rows this window used, then flipped */
        extern int gfx_soft_bil64_alt_step(void);
        const int b64 = gfx_soft_bil64_alt_step(); /* bil64=alt: likewise the one-load bilinear taps */
#if defined(MIYOO)
        char wk[96] = "";
        { /* the second core (kit miyoo_worker): jobs (split triangles) and waits that slept, per frame; the share of the
           * 3D span pixels drawn split */
            extern uint64_t gfx_soft_split_px;
            unsigned jobs = 0, sleeps = 0;
            unsigned long long spins = 0;
            miyoo_worker_stats(&jobs, &sleeps, &spins);
            if (miyoo_worker_ready())
                snprintf(wk, sizeof(wk), " | wk %.1f sl %.2f split %.0f%%", jobs / (float) sPerfWin, sleeps / (float) sPerfWin,
                         gfx_soft_span_px ? 100.0f * (float) gfx_soft_split_px / (float) gfx_soft_span_px : 0.0f);
            gfx_soft_split_px = 0;
        }
#endif
        uint32_t shownMax = 0, shownLate = 0;
        gfx_flip_stats(&shownMax, &shownLate); /* on-screen spacing (handheld backend; 0 elsewhere) */

        /* tris / kpx: 3D triangles and span pixels per frame (setup vs fill cost). */
#if defined(MIYOO)
        port_log_always("PERF fps %.1f iv %.1f max %.1f | render %.1f max %.1f | r3d %.1f r2d %.1f tex %.1f clr %.1f pres %.1f "
                        "gfx %.1f | tris %u kpx %.1f | logic %.1f max %.1f | shown max %.1f late %u%s%s%s\n",
                        iv > 0.0f ? 1000.0f / iv : 0.0f, iv, sPerfMaxIntervalUs / 1000.0f, sPerfRenderUs / n / 1000.0f,
                        sPerfMaxRenderUs / 1000.0f, sPerfBuckets[PROF_R3D] / n / 1000.0f,
                        sPerfBuckets[PROF_R2D] / n / 1000.0f, sPerfBuckets[PROF_TEX] / n / 1000.0f,
                        sPerfBuckets[PROF_CLR] / n / 1000.0f, sPerfBuckets[PROF_PRES] / n / 1000.0f,
                        sPerfBuckets[PROF_GFX] / n / 1000.0f, (unsigned) (gfx_soft_tris / sPerfWin),
                        (float) gfx_soft_span_px / n / 1000.0f,
                        sPerfIntervals ? sPerfLogicUs / (float) sPerfIntervals / 1000.0f : 0.0f, sPerfMaxLogicUs / 1000.0f,
                        shownMax / 1000.0f, (unsigned) shownLate, neon < 0 ? "" : neon ? " | neon 1" : " | neon 0",
                        b64 < 0 ? "" : b64 ? " | b64 1" : " | b64 0", wk);
#else
        port_log_always("PERF fps %.1f iv %.1f max %.1f | render %.1f max %.1f | r3d %.1f r2d %.1f tex %.1f clr %.1f pres %.1f "
                        "gfx %.1f | tris %u kpx %.1f | logic %.1f max %.1f | shown max %.1f late %u%s%s\n",
                        iv > 0.0f ? 1000.0f / iv : 0.0f, iv, sPerfMaxIntervalUs / 1000.0f, sPerfRenderUs / n / 1000.0f,
                        sPerfMaxRenderUs / 1000.0f, sPerfBuckets[PROF_R3D] / n / 1000.0f,
                        sPerfBuckets[PROF_R2D] / n / 1000.0f, sPerfBuckets[PROF_TEX] / n / 1000.0f,
                        sPerfBuckets[PROF_CLR] / n / 1000.0f, sPerfBuckets[PROF_PRES] / n / 1000.0f,
                        sPerfBuckets[PROF_GFX] / n / 1000.0f, (unsigned) (gfx_soft_tris / sPerfWin),
                        (float) gfx_soft_span_px / n / 1000.0f,
                        sPerfIntervals ? sPerfLogicUs / (float) sPerfIntervals / 1000.0f : 0.0f, sPerfMaxLogicUs / 1000.0f,
                        shownMax / 1000.0f, (unsigned) shownLate, neon < 0 ? "" : neon ? " | neon 1" : " | neon 0",
                        b64 < 0 ? "" : b64 ? " | b64 1" : " | b64 0");
#endif
        gfx_soft_tris = gfx_soft_span_px = 0;
        sPerfFrames = sPerfRenderUs = sPerfMaxRenderUs = sPerfIntervalUs = sPerfMaxIntervalUs = sPerfIntervals = 0;
        sPerfLogicUs = sPerfMaxLogicUs = 0;
        for (i = 0; i < PROF_COUNT; i++) {
            sPerfBuckets[i] = 0;
        }
    }
}

void OotPspRenderer_RenderDisplayList(Gfx* dl) {
    extern const char* volatile gHhLoopStage; /* hh_main.c: the loop note */
    uint32_t t0 = port_time_us();

    gHhLoopStage = "drawing"; /* the handheld menu, the power request, the render and the present */
    gfx_start_frame();
    gfx_run(dl);
    gfx_end_frame();
    HhPerf_Frame(port_time_us() - t0);
    gHhLoopStage = "audio update"; /* the rest of Graph_Update (hh_sched.c's pacer sleep first) */
}

void OotPspRenderer_RenderTask(const OSTask* task) {
    if (task != NULL && task->t.data_ptr != NULL) {
        OotPspRenderer_RenderDisplayList((Gfx*)task->t.data_ptr);
    }
}

/* Depth queries (sun/lens flare in z_kankyo.c, light glow in z_lights.c).
 * TODO(M4): answer from the soft rasterizer's z-buffer. */
bool gfx_soft_depth_is_clear(int32_t x, int32_t y);
bool gfx_soft_depth_test(int32_t x, int32_t y, float projectedZ);

/* The sun's lens flare and light glows test the last frame's depth buffer. */
bool OotPspRenderer_IsDepthClear(int32_t x, int32_t y) {
    return gfx_soft_depth_is_clear(x, y);
}

bool OotPspRenderer_DepthTest(int32_t x, int32_t y, float projectedZ) {
    return gfx_soft_depth_test(x, y, projectedZ);
}

/* Prerendered JPEG rooms: nothing to do on room load. The 3D is squashed to
 * match the background image in the frames that draw it (gfx_pc.c,
 * gfx_jpeg_room_squash): a room draws its image only under the fixed camera. */
void OotPspRenderer_SetJpegBackgroundResolution(bool active, uint32_t width, uint32_t height) {
    (void)active;
    (void)width;
    (void)height;
}

void gfx_soft_capture_pause_background(void);
void gfx_soft_set_pause_background_active(bool on);

/* Called from the game update, before this frame renders: the framebuffer still
 * holds the last presented frame, which becomes the pause background. */
void OotPspRenderer_RequestPauseBackground(void) {
    gfx_soft_capture_pause_background();
}

void OotPspRenderer_SetPauseBackgroundActive(bool active) {
    gfx_soft_set_pause_background_active(active);
}

void OotPspRenderer_RenderFirstBootProgress(uint32_t progressPermille, const char* statusMessage, bool error) {
    printf("first boot: %u.%u%% %s%s\n", progressPermille / 10, progressPermille % 10, statusMessage ? statusMessage : "",
           error ? " (ERROR)" : "");
}
