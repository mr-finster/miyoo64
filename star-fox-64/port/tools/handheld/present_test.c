/* present_test.c: drives the device present's conversion (gfx_sdl12.c, extracted by present_test.sh) on random
 * frames in every mode -- CONVERT with the sharp fill, the smooth fill and the plain squeeze, CROP, a square
 * render, the dynamic-resolution sizes; VI gamma on/off; NEON on/off; HUD rows -- and prints one hash per case,
 * so two builds of the file (old, new; x86, ARM) can be diffed. Also checks every RGB565 value through the gamma.
 * Built by present_test.sh only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
typedef uint32_t u32;
#define SAVESTATE_EXCLUDE
#define SCREEN_W 240
#define SCREEN_H 240
#define HH_DISPLAY_CONVERT 0
#define HH_DISPLAY_CROP 2
typedef int SDL_mutex; typedef int SDL_cond;
#define SDL_mutexP(x) ((void)(x))
#define SDL_mutexV(x) ((void)(x))
#define SDL_CondWait(c, m) ((void)(c), (void)(m))
#define SDL_CondBroadcast(c) ((void)(c))
enum { PROF_PRES = 0 };
static u32 gPortProfFrame[1];
#define PORT_PROF_ADD(slot, us) (gPortProfFrame[(slot)] += (u32)(us))
static u32 port_time_us(void) { return 0; }
static int gHandheldDisplayMode;
static uint16_t sStage[SCREEN_W * SCREEN_H];
static SDL_mutex *sPresentLock; static SDL_cond *sPresentCond;
static volatile int sPresentPending;
static uint32_t *tFB; static int tW, tH, tNeon = 1, tHud, tHy0, tHy1;
uint32_t *gfx_soft_get_framebuffer(int *w, int *h) { *w = tW; *h = tH; return tFB; }
int gfx_soft_neon_on(void) { return tNeon; }
int gfx_soft_hud_rows(int *y0, int *y1) { *y0 = tHy0; *y1 = tHy1; return tHud; }
void gfx_soft_hud_overlay_row(int y, uint16_t *dst) { /* a stand-in layer: some opaque pixels on the row */
    for (int x = (y * 7) % 13; x < SCREEN_W; x += 11) dst[x] = (uint16_t)(x * 2654435761u >> 7) ^ (uint16_t)y;
}
int gPortViGamma;
#include "pt_gamma.inc"   /* ultra_shim.c: sGamma8 .. port_vi_gamma565 */
#include "pt_rows.inc"    /* gfx_sdl12.c: argb_row_to_565, squeeze_row_to_565 */
#include "pt_present.inc" /* gfx_sdl12.c: the border fill .. present() */

static uint64_t sRng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { sRng ^= sRng << 13; sRng ^= sRng >> 7; sRng ^= sRng << 17; return (uint32_t)(sRng >> 11); }
static uint32_t hash(const void *p, size_t n) {
    uint32_t h = 2166136261u; const uint8_t *b = p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}
static void fill_fb(int kind) {
    for (int i = 0; i < tW * tH; i++) {
        uint32_t v = rnd();
        if (kind == 1) { int x = i % tW, y = i / tW; v = (uint32_t)(x * 3 + y) * 0x010203u ^ (uint32_t)(y << 16); } /* gradients */
        if (kind == 2) v = (rnd() & 1) ? 0xFFFFFFFFu : 0xFF000000u; /* extremes */
        tFB[i] = v;
    }
}
int main(void) {
    static const int sizes[][2] = { { 320, 240 }, { 240, 240 }, { 288, 216 }, { 256, 192 }, { 224, 168 }, { 160, 120 } };
    static const char *fills[] = { "sharp", "smooth", "1" };
    tFB = malloc(320 * 240 * 4);
    if (getenv("PT_BENCH")) { /* a timing/instruction-count loop: the default device frame (320x240, sharp fill, gamma) */
        const int n = atoi(getenv("PT_BENCH"));
        tW = 320; tH = 240; gPortViGamma = 1; tNeon = 1; tHud = 1; tHy0 = 200; tHy1 = 232;
        fill_fb(1);
        for (int i = 0; i < n; i++) { sPresentPending = 0; present(); }
        printf("bench %d presents: %08x\n", n, hash(sStage, sizeof(sStage)));
        return 0;
    }
    int cases = 0;
    for (int fi = 0; fi < 3; fi++)
        for (int si = 0; si < 6; si++)
            for (int mode = 0; mode <= 2; mode += 2)
                for (int gm = 0; gm < 2; gm++)
                    for (int neon = 0; neon < 2; neon++)
                        for (int kind = 0; kind < 3; kind++) {
                            if (fi == 2) { unsetenv("SF64_FILL"); setenv("SF64_BORDER", "1", 1); }
                            else { unsetenv("SF64_BORDER"); setenv("SF64_FILL", fills[fi], 1); }
                            sFill = -1;
                            tW = sizes[si][0]; tH = sizes[si][1]; gHandheldDisplayMode = mode; gPortViGamma = gm; tNeon = neon;
                            tHud = kind != 1; tHy0 = 20 + kind * 30; tHy1 = 200 - kind * 10;
                            fill_fb(kind);
                            memset(sStage, 0, sizeof(sStage));
                            sPresentPending = 0;
                            present();
                            printf("case %d fill=%s %dx%d mode %d gamma %d neon %d kind %d: %08x\n", cases++, fills[fi], tW, tH,
                                   mode, gm, neon, kind, hash(sStage, sizeof(sStage)));
                        }
    /* every RGB565 value through the gamma, via the square direct path (565 -> ARGB8888 that converts back to it) */
    unsetenv("SF64_FILL"); unsetenv("SF64_BORDER"); sFill = -1;
    tW = tH = 240; gHandheldDisplayMode = 0; gPortViGamma = 1; tNeon = 1; tHud = 0;
    const unsigned short *g = port_vi_gamma565();
    long bad = 0;
    for (int base = 0; base < 65536; base += 57600) {
        for (int i = 0; i < 57600; i++) {
            const uint32_t p = (uint32_t)((base + i) & 0xFFFF);
            tFB[i] = 0xFF000000u | ((p >> 11) << 19) | (((p >> 5) & 63) << 10) | ((p & 31) << 3);
        }
        sPresentPending = 0;
        present();
        for (int i = 0; i < 57600; i++) if (sStage[i] != g[(base + i) & 0xFFFF]) bad++;
    }
    printf("gamma all 65536 values: %ld differ from the table\n", bad);
    return bad != 0;
}
