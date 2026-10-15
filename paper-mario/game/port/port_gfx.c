/* The frame for the native port (M2): the game's display lists go to the M1 renderer (bench/kit, without
 * PM64_REPLAY), and a finished frame is shown. Host side (system headers and the kit's). On the desktop proxy a
 * frame is written as a PPM when PM64_SHOTS=<dir> is set, as a square (the area-filtered 4->3 squeeze the
 * bench uses); the device presents through SDL 1.2 (port_sdl.c). */
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

#define PANEL 240

extern struct GfxRenderingAPI gfx_soft_api;
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
extern uint16_t gFrameBuf0[], gFrameBuf1[], gFrameBuf2[];

float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

/* the window manager: the port presents by itself, after each game frame */
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

/* one 240-px panel row from one 320-px row: every 4 columns -> 3 (3:1, 2:2, 1:3), as the bench's fit_row */
static void fit_row(uint32_t *dst, const uint32_t *src, int sw) {
    if (sw != 320) { memcpy(dst, src, PANEL * 4); return; }
    for (int i = 0; i < 80; i++, src += 4, dst += 3) {
        const uint32_t a = src[0], b = src[1], c = src[2], d = src[3];
#define CH(p, sh) (((p) >> (sh)) & 255u)
#define MIX(p, q, wp, wq) ((((CH(p, 16) * wp + CH(q, 16) * wq + 2) >> 2) << 16) | (((CH(p, 8) * wp + CH(q, 8) * wq + 2) >> 2) << 8) | ((CH(p, 0) * wp + CH(q, 0) * wq + 2) >> 2) | 0xFF000000u)
        dst[0] = MIX(a, b, 3, 1); dst[1] = MIX(b, c, 2, 2); dst[2] = MIX(c, d, 1, 3);
#undef MIX
#undef CH
    }
}

/* The square picture is a rectangle of the 320x240 frame resampled to the square:
 *   the whole frame (0, 0, 320, 240) -- the squeeze, fit_row, as always;
 *   during the intro logos the middle (40, 0, 240, 240) 1:1, widening to the whole frame with the last logo's fade;
 *   in the field the scene inside its black frame (12, 20, 296, 200), stretched (port_main.c). A change between the
 *   whole frame and the stretch (a battle, the curtains, a map without the frame) moves the rectangle over 8 frames,
 *   so the picture zooms rather than jumps. PM64_STRETCH=0 keeps the black frame, PM64_LOGOCROP=0 the logos squeezed.
 * Other than the squeeze and the copy, a panel pixel is a bilinear sample (7-bit weights, rounded) of the source at
 * its centre: across first, each source row once (a two-row cache of planar rows), then down. The device does the same
 * integer math in NEON (port_panel_565); this plain C makes the desktop's and the tests' frames, pixel for pixel the
 * same (PM64_PANELCHECK=1 compares the two on the device build). The stretch's first version averaged areas with up to
 * three taps across in plain C: 2.0 M ARM instructions a frame against the squeeze's 0.3 M (qemu, dgb_09). */
static uint16_t sHL[PANEL] SAVESTATE_EXCLUDE; /* the left source column */
static uint8_t sHW[PANEL] SAVESTATE_EXCLUDE;  /* the right one's weight, 0..127 of 128 */
static uint8_t sHIdx[PANEL] SAVESTATE_EXCLUDE; /* sHL less its group of 8's first (NEON's table lookups) */
static uint16_t sVR[PANEL] SAVESTATE_EXCLUDE; /* the upper source row */
static uint8_t sVW[PANEL] SAVESTATE_EXCLUDE;  /* the lower one's weight, 0..127 of 128 */
static float sRect[4] SAVESTATE_EXCLUDE = { -1, -1, -1, -1 };
static void rect_tables(const float r[4]) {
    const float kx = r[2] / PANEL, ky = r[3] / PANEL;
    for (int x = 0; x < PANEL; x++) {
        float c = r[0] + (x + 0.5f) * kx - 0.5f;
        if (c < 0) c = 0;
        if (c > 318) c = 318;
        int l = (int) c, w = (int) ((c - l) * 128.0f + 0.5f);
        if (w >= 128) { l++; w = 0; }
        sHL[x] = (uint16_t) l; sHW[x] = (uint8_t) w;
    }
    for (int x = 0; x < PANEL; x++) sHIdx[x] = (uint8_t) (sHL[x] - sHL[x & ~7]);
    for (int y = 0; y < PANEL; y++) {
        float c = r[1] + (y + 0.5f) * ky - 0.5f;
        if (c < 0) c = 0;
        if (c > 238) c = 238;
        int t = (int) c, w = (int) ((c - t) * 128.0f + 0.5f);
        if (w >= 128) { t++; w = 0; }
        sVR[y] = (uint16_t) t; sVW[y] = (uint8_t) w;
    }
    memcpy(sRect, r, sizeof sRect);
}
static inline uint32_t lerp7(uint32_t a, uint32_t b, uint32_t w) { return (a * (128 - w) + b * w + 64) >> 7; }
/* a source row across, into planar r, g, b */
static void rect_hrow(uint8_t *pr, uint8_t *pg, uint8_t *pb, const uint32_t *src) {
    for (int x = 0; x < PANEL; x++) {
        const uint32_t a = src[sHL[x]], b = src[sHL[x] + 1], w = sHW[x];
        pr[x] = (uint8_t) lerp7((a >> 16) & 255, (b >> 16) & 255, w);
        pg[x] = (uint8_t) lerp7((a >> 8) & 255, (b >> 8) & 255, w);
        pb[x] = (uint8_t) lerp7(a & 255, b & 255, w);
    }
}

static int logos_view(void) { /* 0 = 1:1, 255 = the whole frame (port_main.c) */
    static int on = -1;
    if (on < 0) { const char *e = getenv("PM64_LOGOCROP"); on = !(e && e[0] == '0'); }
    return on ? port_logos_view() : 255;
}

enum { PANEL_SQUEEZE, PANEL_COPY, PANEL_RECT, PANEL_OTHER };
static const uint32_t *sPanelFb SAVESTATE_EXCLUDE;
static int sPanelW SAVESTATE_EXCLUDE, sPanelH SAVESTATE_EXCLUDE, sPanelMode SAVESTATE_EXCLUDE;
static float sStretchT SAVESTATE_EXCLUDE = -1; /* 0 = the whole frame, 1 = the stretch */
static uint8_t sHRow[2][3][PANEL] __attribute__((aligned(16))) SAVESTATE_EXCLUDE; /* planar r, g, b */
static int sHRowSrc[2] SAVESTATE_EXCLUDE;
void port_panel_begin(const uint32_t *fb, int w, int h) {
    sPanelFb = fb; sPanelW = w; sPanelH = h;
    sHRowSrc[0] = sHRowSrc[1] = -1;
    if (w != 320 || h != 240) { sPanelMode = PANEL_OTHER; return; }
    const float target = port_letterbox_standard() ? 1.0f : 0.0f;
    if (sStretchT < 0) sStretchT = target;
    else if (sStretchT < target) sStretchT = sStretchT + 0.125f > target ? target : sStretchT + 0.125f;
    else if (sStretchT > target) sStretchT = sStretchT - 0.125f < target ? target : sStretchT - 0.125f;
    const int view = logos_view();
    float r[4];
    if (view < 255) { /* the logos: (40, 0, 240, 240) widening to the whole frame */
        const float t = view / 255.0f;
        r[0] = 40 * (1 - t); r[1] = 0; r[2] = 240 + 80 * t; r[3] = 240;
    } else { /* the whole frame, or the scene inside its black frame */
        const float t = sStretchT;
        r[0] = 12 * t; r[1] = 20 * t; r[2] = 320 - 24 * t; r[3] = 240 - 40 * t;
    }
    if (r[0] == 0 && r[1] == 0 && r[2] == 320 && r[3] == 240) { sPanelMode = PANEL_SQUEEZE; return; }
    if (r[0] == 40 && r[1] == 0 && r[2] == 240 && r[3] == 240) { sPanelMode = PANEL_COPY; return; }
    if (memcmp(r, sRect, sizeof r) != 0) rect_tables(r);
    sPanelMode = PANEL_RECT;
}
int port_panel_plain(void) { return sPanelMode == PANEL_SQUEEZE; }
int port_panel_rect(void) { return sPanelMode == PANEL_RECT; }
static uint8_t (*hrow(int r))[PANEL] { /* source row r across (planar), from the two-row cache */
    const int slot = r & 1;
    if (sHRowSrc[slot] != r) {
        rect_hrow(sHRow[slot][0], sHRow[slot][1], sHRow[slot][2], sPanelFb + (size_t) r * sPanelW);
        sHRowSrc[slot] = r;
    }
    return sHRow[slot];
}
void port_panel_row(uint32_t *dst, int y) {
    const uint32_t *src = sPanelFb + (size_t) (y < sPanelH ? y : sPanelH - 1) * sPanelW;
    switch (sPanelMode) {
        case PANEL_OTHER: memcpy(dst, src, PANEL * 4); return;
        case PANEL_SQUEEZE: fit_row(dst, src, sPanelW); return;
        case PANEL_COPY: memcpy(dst, src + 40, PANEL * 4); return;
    }
    uint8_t (*a)[PANEL] = hrow(sVR[y]), (*b)[PANEL] = hrow(sVR[y] + 1);
    const uint32_t w = sVW[y];
    for (int x = 0; x < PANEL; x++)
        dst[x] = 0xFF000000u | lerp7(a[0][x], b[0][x], w) << 16 | lerp7(a[1][x], b[1][x], w) << 8 | lerp7(a[2][x], b[2][x], w);
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
/* The device's rectangle (PANEL_RECT): the same integer math as port_panel_row, 8 panel pixels at a time, straight to
 * RGB565 (the row stride in pixels). Across, a group of 8 panel pixels reads at most 11 source pixels (the rectangle is
 * 240..320 wide), so one de-interleaving load of 16 and table lookups gather them. */
static void neon_hrow(uint8_t *pr, uint8_t *pg, uint8_t *pb, const uint32_t *src) {
    const uint8x8_t k128 = vdup_n_u8(128), one = vdup_n_u8(1);
    for (int x = 0; x < PANEL; x += 8) {
        const int base = sHL[x] > 304 ? 304 : sHL[x]; /* 16 pixels from base stay inside the row */
        const uint8x16x4_t p = vld4q_u8((const uint8_t *) (src + base));
        uint8x8_t il = vld1_u8(&sHIdx[x]);
        if (base != sHL[x]) il = vadd_u8(il, vdup_n_u8((uint8_t) (sHL[x] - base)));
        const uint8x8_t ir = vadd_u8(il, one), w = vld1_u8(&sHW[x]), nw = vsub_u8(k128, w);
        uint8_t *const out[3] = { pr, pg, pb };
        for (int c = 0; c < 3; c++) {
            const uint8x16_t q = p.val[2 - c]; /* memory order b, g, r, a */
            const uint8x8x2_t t = { { vget_low_u8(q), vget_high_u8(q) } };
            const uint16x8_t acc = vmlal_u8(vmull_u8(vtbl2_u8(t, il), nw), vtbl2_u8(t, ir), w);
            vst1_u8(out[c] + x, vrshrn_n_u16(acc, 7));
        }
    }
}
void port_panel_565(uint16_t *dst, int stride) {
    static uint8_t cache[2][3][PANEL] __attribute__((aligned(16)));
    int src_of[2] = { -1, -1 };
    const uint8x8_t k128 = vdup_n_u8(128);
    for (int y = 0; y < PANEL; y++, dst += stride) {
        uint8_t (*ab[2])[PANEL];
        for (int k = 0; k < 2; k++) {
            const int r = sVR[y] + k, slot = r & 1;
            if (src_of[slot] != r) { neon_hrow(cache[slot][0], cache[slot][1], cache[slot][2], sPanelFb + (size_t) r * sPanelW); src_of[slot] = r; }
            ab[k] = cache[slot];
        }
        const uint8x8_t w = vdup_n_u8(sVW[y]), nw = vsub_u8(k128, w);
        for (int x = 0; x < PANEL; x += 8) {
            const uint8x8_t r = vrshrn_n_u16(vmlal_u8(vmull_u8(vld1_u8(ab[0][0] + x), nw), vld1_u8(ab[1][0] + x), w), 7);
            const uint8x8_t g = vrshrn_n_u16(vmlal_u8(vmull_u8(vld1_u8(ab[0][1] + x), nw), vld1_u8(ab[1][1] + x), w), 7);
            const uint8x8_t b = vrshrn_n_u16(vmlal_u8(vmull_u8(vld1_u8(ab[0][2] + x), nw), vld1_u8(ab[1][2] + x), w), 7);
            const uint16x8_t px = vorrq_u16(vorrq_u16(vandq_u16(vshll_n_u8(r, 8), vdupq_n_u16(0xF800)),
                                                      vshrq_n_u16(vandq_u16(vshll_n_u8(g, 8), vdupq_n_u16(0xFC00)), 5)),
                                            vshrq_n_u16(vshll_n_u8(b, 8), 11));
            vst1q_u16(dst + x, px);
        }
    }
}
#endif

static const char *sShotDir;
static uint32_t sShot, sShotEvery = 1; /* PM64_SHOTS_EVERY=<n>: every nth frame only (the map sweep) */
static void save_ppm(void) {
    char path[600];
    int w, h;
    /* PM64_SHOTS_GAMEFRAME=1: named and picked (every nth) by the game frame (port_frame), not by the shown frame: the
     * frames a slow host leaves undrawn differ from run to run, so two runs' shots then pair by name (renderer A/Bs) */
    static int byGame = -1;
    if (byGame < 0) { const char *e = getenv("PM64_SHOTS_GAMEFRAME"); byGame = e && e[0] == '1'; }
    const uint32_t n = byGame ? port_frame : sShot;
    sShot++;
    if (n % sShotEvery != 0) return;
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    snprintf(path, sizeof path, "%s/frame_%05u.ppm", sShotDir, n);
    FILE *f = fopen(path, "wb");
    if (!f) { port_log("gfx: cannot write %s\n", path); return; }
    static int raw = -1; /* PM64_SHOTS_RAW=1: the renderer's own frame (320x240), before the panel's squeeze */
    if (raw < 0) { const char *e = getenv("PM64_SHOTS_RAW"); raw = e && e[0] == '1'; }
    const int ow = raw ? w : PANEL;
    port_panel_begin(fb, w, h);
    fprintf(f, "P6\n%d %d\n255\n", ow, h);
    uint32_t row[640];
    for (int y = 0; y < h; y++) {
        if (ow == w) memcpy(row, fb + y * w, (size_t) w * 4);
        else port_panel_row(row, y);
        for (int x = 0; x < ow; x++) { const uint8_t px[3] = { row[x] >> 16, row[x] >> 8, row[x] }; fwrite(px, 3, 1, f); }
    }
    fclose(f);
}

void port_gfx_init(void) {
    sShotDir = getenv("PM64_SHOTS");
    const char *every = getenv("PM64_SHOTS_EVERY");
    if (every && atoi(every) > 0) sShotEvery = (uint32_t) atoi(every);
    gfx_init(&sWapi, &gfx_soft_api, "Paper Mario", false);
    port_log("gfx: renderer ready%s%s\n", sShotDir ? ", frames to " : "", sShotDir ? sShotDir : "");
}

/* Paper Mario's depth queries (model.c is_point_visible / is_model_center_visible, decomp.patch): the N64 copies one
 * pixel of nuGfxZBuffer into DepthCopyBuffer[id] with a texture rectangle drawn into that buffer, and the next frame
 * decodes it (DepthFloatLookupTable) and compares it with the point's own depth: in front, the bulb glow (the star rod's
 * bloom, Watt, the lightning's flash, the lantern ghost) is drawn. This renderer keeps its depth in its own buffer, so
 * that memory never held a depth and every glow tested as hidden. Now the query is noted while the list is built
 * and answered when the frame is drawn, from the renderer's depth in the RDP's format: 18-bit z (0..32704 x 8 for
 * z/w * 0.5 + 0.5 = 0..1, the viewport's G_MAXZ range) as a 3-bit exponent and 11-bit mantissa, dz 0.
 * PM64_DEPTHQUERY=0: the old way (the texrect, which never wrote that memory). */
int gfx_soft_depth_at(int x, int y);
static struct { uint16_t *dst; int x, y; } sDq[16] SAVESTATE_EXCLUDE;
static int sDqN SAVESTATE_EXCLUDE;
int32_t port_depth_query(uint16_t *dst, int32_t x, int32_t y) {
    static int on SAVESTATE_EXCLUDE = -1;
    if (on < 0) { const char *e = getenv("PM64_DEPTHQUERY"); on = !(e && e[0] == '0'); }
    if (!on) return 0;
    if (sDqN < 16) { sDq[sDqN].dst = dst; sDq[sDqN].x = x; sDq[sDqN].y = y; sDqN++; }
    return 1;
}
static uint16_t n64_depth_encode(int z16) {
    static const uint32_t bias[8] = { 0, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800 };
    static const uint8_t shift[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
    const uint32_t z18 = (uint32_t) ((uint64_t) (uint32_t) z16 * (32704u * 8u) / 65535u);
    int e = 7;
    while (e > 0 && z18 < bias[e]) e--;
    uint32_t m = (z18 - bias[e]) >> shift[e];
    if (m > 0x7FF) m = 0x7FF;
    return (uint16_t) (e << 13 | m << 2);
}
static void depth_query_resolve(void) {
    for (int i = 0; i < sDqN; i++) {
        const int z = gfx_soft_depth_at(sDq[i].x, sDq[i].y);
        if (z >= 0) *sDq[i].dst = n64_depth_encode(z);
    }
    sDqN = 0;
}

void port_gfx_task(void *gfx_list, int swap) {
    /* PM64_TRACE_AT=<frame>[,<tasks>]: the renderer's command trace (PM64_TRILOG=1 adds every triangle) for that
     * frame's tasks, frames counted as the shots are (the file select's window flip, M2) */
    /* With PM64_SHOTS_GAMEFRAME=1 the frame is the game frame (port_frame), and the switch is read again after a state
     * load (these are kept out of states), so a trace can start from a PM64_STATE_LOAD */
    static uint32_t sFrame, sTraceAt SAVESTATE_EXCLUDE, sTraceN SAVESTATE_EXCLUDE = 1;
    static int sTraceInit SAVESTATE_EXCLUDE, sTraceGame SAVESTATE_EXCLUDE;
    if (!sTraceInit) {
        { const char *g = getenv("PM64_SHOTS_GAMEFRAME"); sTraceGame = g && g[0] == '1'; }
        const char *t = getenv("PM64_TRACE_AT");
        sTraceInit = 1;
        if (t && t[0]) {
            sTraceAt = (uint32_t) strtoul(t, NULL, 10);
            const char *c = strchr(t, ',');
            if (c) sTraceN = (uint32_t) strtoul(c + 1, NULL, 10);
        }
    }
    if (sTraceAt && (sTraceGame ? port_frame : sFrame) == sTraceAt) {
        extern int gfx_trace_frames;
        gfx_trace_frames = (int) sTraceN;
        sTraceAt = 0;
    }
    if (swap) sFrame++;
    const uint32_t perfT0 = port_time_us();
    gfx_start_frame();
    gfx_run((Gfx *) gfx_list);
    gfx_end_frame();
    if (!swap) { port_perf_gfx_us += port_time_us() - perfT0; return; }
#ifdef PM64_SDL12
    port_video_present();
#endif
    port_perf_gfx_us += port_time_us() - perfT0;
    depth_query_resolve();
    if (sShotDir) save_ppm();
}

/* The last frame as the game's RGBA16 framebuffer holds it (big-endian 5551, 320x240): the pause menu's backdrop
 * (background_gfx.c, BACKGROUND_RENDER_STATE_FILTER_PAUSED). */
void port_capture_frame_rgba16(uint16_t *dst) {
    int w, h;
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    if (!fb || w <= 0 || h <= 0) return;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            const uint32_t p = fb[(y * h / 240) * w + (x * w / 320)];
            const uint16_t c = (uint16_t) (((p >> 19) & 31) << 11 | ((p >> 11) & 31) << 6 | ((p >> 3) & 31) << 1 | 1);
            dst[y * 320 + x] = (uint16_t) (c >> 8 | c << 8);
        }
}

/* The frame into the game's own framebuffer, for the effects that texture the screen with the frame before
 * (screen_render_util.c appendGfx_draw_prev_frame_buffer: the battle-start swirl, the blur overlay, the Dizzy Dial,
 * the firework). The renderer draws into its own buffer and never into that memory, so the swirl drew the frame
 * before as nothing: the black screen before the prologue's Bowser battle (the user, 29 Sep). A copy at the draw's
 * building is too early -- each frame's background task is drawn before its main list is built, so it held the sky
 * alone -- so, as on the N64, a finished frame goes into its own framebuffer (nuGfxTaskStart's), while the effects
 * ask for it: a frame that asked copies itself when drawn, for the next frame's draw. An effect's first frame finds
 * no copy of the frame before (it is gone by then): that buffer is cleared. PM64_PREVCOPY=0: the old way (A/B). */
static uint16_t *sPrevCopied; /* the framebuffer the last finished frame went to */
static int sPrevWanted;       /* this frame asked */
static int sPrevX0 = 320, sPrevY0 = 240, sPrevX1 = -1, sPrevY1 = -1;
bool port_is_framebuffer(const void *address);
void port_capture_prev_rows(uint16_t *dst, int x0, int y0, int x1, int y1) {
    static int on = -1; /* PM64_PREVCOPY=0: no copy (the A/B) */
    if (on < 0) { const char *e = getenv("PM64_PREVCOPY"); on = !(e && e[0] == '0'); }
    if (!on || !port_is_framebuffer(dst)) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 319) x1 = 319;
    if (y1 > 239) y1 = 239;
    if (x0 > x1 || y0 > y1) return;
    if (dst != sPrevCopied)
        for (int y = y0; y <= y1; y++) memset(dst + y * 320 + x0, 0, (size_t) (x1 - x0 + 1) * 2);
    if (x0 < sPrevX0) sPrevX0 = x0;
    if (y0 < sPrevY0) sPrevY0 = y0;
    if (x1 > sPrevX1) sPrevX1 = x1;
    if (y1 > sPrevY1) sPrevY1 = y1;
    sPrevWanted = 1;
}
/* After a frame's main task (port_nusys.c nuGfxTaskStart): that frame into its framebuffer, if it asked */
void port_prev_frame_done(uint16_t *dst) {
    int w, h;
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    sPrevCopied = NULL;
    if (!sPrevWanted) return;
    sPrevWanted = 0;
    if (fb && w > 0 && h > 0 && port_is_framebuffer(dst)) {
        for (int y = sPrevY0; y <= sPrevY1; y++) {
            const uint32_t *row = fb + (size_t) (y * h / 240) * w;
            uint16_t *out = dst + y * 320;
            for (int x = sPrevX0; x <= sPrevX1; x++) {
                const uint32_t p = row[x * w / 320];
                const uint16_t c = (uint16_t) (((p >> 19) & 31) << 11 | ((p >> 11) & 31) << 6 | ((p >> 3) & 31) << 1 | 1);
                out[x] = (uint16_t) (c >> 8 | c << 8);
            }
        }
        sPrevCopied = dst;
    }
    sPrevX0 = 320, sPrevY0 = 240, sPrevX1 = -1, sPrevY1 = -1;
}

bool port_is_framebuffer(const void *address) {
    const uint8_t *a = address;
    const size_t n = 320 * 240 * 2;
    return (a >= (const uint8_t *) gFrameBuf0 && a < (const uint8_t *) gFrameBuf0 + n) ||
           (a >= (const uint8_t *) gFrameBuf1 && a < (const uint8_t *) gFrameBuf1 + n) ||
           (a >= (const uint8_t *) gFrameBuf2 && a < (const uint8_t *) gFrameBuf2 + n);
}

void *port_seg_to_ptr(uintptr_t addr) {
    static int sWarned;
    if (addr < 0x10000000u && sWarned < 20) {
        sWarned++;
        port_log("gfx: segmented address 0x%08X has no segment base (0x%08X)\n", (unsigned) addr, (unsigned) addr);
    }
    return (void *) addr;
}
