/* The handheld output size: see handheld_screen.h. */
#if defined(HANDHELD)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handheld_screen.h"
#include "savestate.h"

#define HH_SCREEN_CFG      "dkr_display.cfg"  /* the settings file, in the data dir */
#define HH_SCREEN_DATA_ENV "DKR_DATA"

static int sW SAVESTATE_EXCLUDE, sH SAVESTATE_EXCLUDE; /* 0 = not read yet; a setting, never game state */
static int sNeon SAVESTATE_EXCLUDE = -1;              /* neon=<n>: -1 absent */
static int sHud SAVESTATE_EXCLUDE = -1;               /* hud=<n>: -1 absent */
static int sFillRule SAVESTATE_EXCLUDE = -1;          /* fillrule=<n>: -1 absent (DKR_FILLRULE) */
static int sLogVerbose SAVESTATE_EXCLUDE = -1;        /* logverbose=1: today's full logging (DKR_LOGVERBOSE) */
static int sVdsoClock SAVESTATE_EXCLUDE = -1;         /* vdsoclock=<n>: -1 absent (DKR_VDSOCLOCK) */
static int sFps SAVESTATE_EXCLUDE = -1;               /* fps=30/60: -1 absent (DKR_FPS; video.c's hold, main.c port_fps60) */
#if defined(MIYOO)
static int sRot180 SAVESTATE_EXCLUDE = 1;             /* rotate180=0/1 (Miyoo: miyoo_video.c; on by default) */
static int sThreads SAVESTATE_EXCLUDE = -1;           /* threads=1/2 (Miyoo: miyoo_worker.c): -1 absent */
static int sCpuClock SAVESTATE_EXCLUDE = -1;          /* cpuclock=<MHz> (Miyoo: miyoo_clock.c): -1 absent, 0 leave it */
#endif

static void screen_read(void) {
    if (sW) return;
    sW = 320; sH = 240; /* the Miyoo's 640x480 panel is 320x240 doubled; the square size works too (boxed) */
    const char *dir = getenv(HH_SCREEN_DATA_ENV);
    if (!dir) dir = getenv("HOME");
    if (!dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, HH_SCREEN_CFG);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "neon=", 5) == 0) {
            if (p[5] >= '0' && p[5] <= '2') sNeon = p[5] - '0';
            continue;
        }
        if (strncmp(p, "hud=", 4) == 0) {
            if (p[4] == '0' || p[4] == '1') sHud = p[4] - '0';
            continue;
        }
        if (strncmp(p, "fillrule=", 9) == 0) {
            if (p[9] == '0' || p[9] == '1') sFillRule = p[9] - '0';
            continue;
        }
        if (strncmp(p, "vdsoclock=", 10) == 0) {
            if (p[10] == '0' || p[10] == '1') sVdsoClock = p[10] - '0';
            continue;
        }
        if (strncmp(p, "fps=", 4) == 0) {
            if (atoi(p + 4) == 30 || atoi(p + 4) == 60) sFps = atoi(p + 4);
            continue;
        }
        if (strncmp(p, "logverbose=", 11) == 0) {
            if (p[11] == '0' || p[11] == '1') sLogVerbose = p[11] - '0';
            continue;
        }
#if defined(MIYOO)
        if (strncmp(p, "rotate180=", 10) == 0) {
            if (p[10] == '0' || p[10] == '1') sRot180 = p[10] - '0';
            continue;
        }
        if (strncmp(p, "threads=", 8) == 0) {
            if (p[8] == '1' || p[8] == '2') sThreads = p[8] - '0';
            continue;
        }
        if (strncmp(p, "cpuclock=", 9) == 0) {
            if (p[9] >= '0' && p[9] <= '9') sCpuClock = atoi(p + 9);
            continue;
        }
#endif
        if (strncmp(p, "screen=", 7) != 0) continue;
        int w = 0, h = 0;
        if (sscanf(p + 7, "%dx%d", &w, &h) == 2 && w == 320 && h == 240) { sW = 320; sH = 240; }
        else if (w == 240 && h == 240) { sW = 240; sH = 240; }
        else fprintf(stderr, "screen: unknown %.*s, using 320x240\n", (int) strcspn(p, "\r\n"), p);
    }
    fclose(f);
    if (sNeon >= 0) fprintf(stderr, "neon: %d (neon=%d in %s: raster spans, envelope mixer, present)\n", sNeon, sNeon, path);
    if (sHud >= 0) fprintf(stderr, "hud: %d (hud=%d in %s: %s)\n", sHud, sHud, path, sHud ? "unsquished" : "squeezed, as before v38");
    if (sFillRule >= 0) fprintf(stderr, "fillrule: %d (fillrule=%d in %s: %s)\n", sFillRule, sFillRule, path,
                                sFillRule ? "owned edges for blended draws without AA" : "the inclusive row solve, as before");
}

int handheld_cfg_neon(void) { screen_read(); return sNeon; }
int handheld_cfg_hud(void) { screen_read(); return sHud; }
int handheld_cfg_fillrule(void) { screen_read(); return sFillRule; }
int handheld_cfg_logverbose(void) { screen_read(); return sLogVerbose; }
int handheld_cfg_vdsoclock(void) { screen_read(); return sVdsoClock; }
/* fps=30|60 in the cfg (-1: no line, 30: the hold); DKR_FPS wins (main.c port_fps60) */
int handheld_cfg_fps(void) { screen_read(); return sFps; }
#if defined(MIYOO)
/* DKR_ROTATE=0/1 wins: the MinUI launcher sets 0, its SDL turns the panel itself. */
int handheld_cfg_rotate180(void) {
    const char *e = getenv("DKR_ROTATE");
    if (e && (e[0] == '0' || e[0] == '1')) return e[0] - '0';
    screen_read(); return sRot180;
}
/* threads=1|2 in the cfg (-1: no line); DKR_THREADS, read by miyoo_worker.c, wins (gfx_sdl12.c) */
int handheld_cfg_threads(void) { screen_read(); return sThreads; }
/* cpuclock=<MHz> in the cfg (-1: no line, the model's default; 0: the clock left alone); DKR_CPUCLOCK wins (main.c) */
int handheld_cfg_cpuclock(void) { screen_read(); return sCpuClock; }
#endif

int handheld_screen_w(void) { screen_read(); return sW; }
int handheld_screen_h(void) { screen_read(); return sH; }
int handheld_screen_wide(void) { screen_read(); return sW == 320; }

void handheld_screen_confirm(int w, int h) {
    screen_read();
    if (w == sW && h == sH) return;
    fprintf(stderr, "screen: asked for %dx%d, the display came up at %dx%d; using the square size\n", sW, sH, w, h);
    sW = 240; sH = 240;
}
#endif /* HANDHELD */
