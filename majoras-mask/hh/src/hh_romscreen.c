/* First-start screens, in the MK64 (mario-kart-64 assets_handheld.c) layout: the
 * progress of building the game data from the player's ROM (hh_convert.c), its
 * errors, and the missing-data screen. On the device they are drawn on the
 * screen and an error waits for a button; elsewhere they are printed.
 * An error or missing data ends the process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void HhRomScreen_NoData(void);

#if defined(MIYOO)
/* Miyoo Mini: the same layout at 320x240 in the menus' bitmap font (kit miyoo_menu.c: 2x for the
 * title, 1x for the rest), no font file; each screen goes onto the panel as the game's picture does
 * (gfx_sdl12.c): doubled on a Mini's 640x480, at full height on a Mini v4 / Flip's 752x560 (OOT_FLIPFILL),
 * and turned. A "font" here is just its scale. */
#include <stdint.h>
#include <SDL/SDL.h>
#include "handheld_menu.h"
typedef struct { int k; } TTF_Font;
static TTF_Font sBig = { 2 }, sSmall = { 1 };
static SDL_Surface* sScr;   /* 320x240 RGB565: what the screens draw on */
static SDL_Surface* sPanel; /* the panel */
static TTF_Font *sFontBig, *sFontSmall;
static char sSub[64];
extern int gfx_miyoo_rotate(void);
extern SDL_Surface* gfx_miyoo_open_panel(void); /* gfx_sdl12.c: the mode the game then keeps */

static void ui_begin(void) {
    if (sScr) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "hh: SDL_Init: %s\n", SDL_GetError()); return; }
    sPanel = gfx_miyoo_open_panel(); /* 640x480 on a Mini, the panel's own size on a Mini v4 / Flip */
    if (!sPanel) { fprintf(stderr, "hh: SDL_SetVideoMode: %s\n", SDL_GetError()); return; }
    sScr = SDL_CreateRGBSurface(SDL_SWSURFACE, 320, 240, 16, 0xF800, 0x07E0, 0x001F, 0);
    SDL_ShowCursor(SDL_DISABLE);
    sFontBig = &sBig; sFontSmall = &sSmall;
}
static void ui_text(int y, const char* s, TTF_Font* f, int gray) {
    char fit[128];
    if (!sScr || !f || !s || !s[0]) return;
    /* a long line (a ROM's file name) keeps its start and ends in "..." within 312 px */
    snprintf(fit, sizeof(fit), "%s", s);
    const int maxc = 312 / (8 * f->k);
    if ((int)strlen(fit) > maxc && maxc > 3) memcpy(fit + maxc - 3, "...", 4);
    const unsigned int c = 0xFF000000u | ((unsigned)gray << 16) | ((unsigned)gray << 8) | (unsigned)gray;
    miyoo_text(sScr, (sScr->w - miyoo_text_w(fit, f->k)) / 2, y + (f->k == 1 ? 2 : 1), f->k, fit, c);
}
/* sScr onto the panel, turned, as the game's picture (gfx_sdl12.c blit_stage_to_screen): at the panel's full
 * height, centred, nearest; a width within 2% of the panel's takes all of it. 640x480: the exact 2x. */
static void ui_flip(void) {
    if (!sPanel || !sScr) return;
    const int rot = gfx_miyoo_rotate(), pw = sPanel->w, ph = sPanel->h, sw = sScr->w, sh = sScr->h;
    int dw = ph * sw / sh;
    if (dw > pw || (pw - dw) * 50 <= pw) dw = pw;
    const int ox = (pw - dw) / 2;
    const Uint32 black = SDL_MapRGB(sPanel->format, 0, 0, 0) | sPanel->format->Amask;
    if (SDL_MUSTLOCK(sPanel)) SDL_LockSurface(sPanel);
    for (int y = 0; y < ph; y++) {
        const uint16_t* src = (const uint16_t*) ((const uint8_t*) sScr->pixels + (y * sh / ph) * sScr->pitch);
        uint8_t* row = (uint8_t*) sPanel->pixels + (rot ? ph - 1 - y : y) * sPanel->pitch;
        for (int x = 0; x < pw; x++) {
            Uint32 m = black;
            if (x >= ox && x < ox + dw) {
                int sx = ((x - ox) * sw + sw / 2) / dw;
                if (sx >= sw) sx = sw - 1;
                const uint16_t p = src[sx];
                m = SDL_MapRGB(sPanel->format, (Uint8) ((p >> 8) & 0xF8), (Uint8) ((p >> 3) & 0xFC), (Uint8) (p << 3)) | sPanel->format->Amask;
            }
            const int dx = rot ? pw - 1 - x : x;
            if (sPanel->format->BitsPerPixel == 32) ((Uint32*) row)[dx] = m;
            else ((Uint16*) row)[dx] = (Uint16) m;
        }
    }
    if (SDL_MUSTLOCK(sPanel)) SDL_UnlockSurface(sPanel);
    SDL_Flip(sPanel);
}
#define SDL_Flip(s) ui_flip()
#endif
#if defined(HANDHELD) && !defined(TARGET_PC)
/* title, up to three body lines, a progress bar (pct < 0: none), a footer */
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0));
    ui_text(22, "OCARINA OF TIME", sFontBig, 255);
    ui_text(50, sSub, sFontSmall, 150);
    ui_text(86, l1, sFontSmall, 230);
    ui_text(104, l2, sFontSmall, 230);
    ui_text(122, l3, sFontSmall, 230);
    if (pct >= 0) {
        const int bx = (sScr->w - 180) / 2; /* 30 on the 240 screen */
        SDL_Rect frame = { (Sint16) bx, 158, 180, 12 }, fill = { (Sint16) (bx + 2), 160, (Uint16) (176 * pct / 100), 8 };
        char p[8];
        SDL_FillRect(sScr, &frame, SDL_MapRGB(sScr->format, 70, 70, 70));
        SDL_FillRect(sScr, &fill, SDL_MapRGB(sScr->format, 255, 255, 255));
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(176, p, sFontSmall, 200);
    }
    ui_text(212, foot, sFontSmall, 150);
    /* OOT_SPLASH_DUMP=<dir>: write the rendered surface to a PPM (debug only,
     * env-gated). Lets the screen be captured off-device (SDL_ttf renders to
     * the surface even under the dummy video driver). */
    {
        const char* dd = getenv("OOT_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256]; FILE* f; int x, y;
            static int seq;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", sScr->w, sScr->h);
                if (SDL_MUSTLOCK(sScr)) SDL_LockSurface(sScr);
                for (y = 0; y < sScr->h; y++) for (x = 0; x < sScr->w; x++) {
                    Uint16* p = (Uint16*) ((Uint8*) sScr->pixels + y * sScr->pitch + x * 2);
                    Uint8 r, g, b, px[3];
                    SDL_GetRGB(*p, sScr->format, &r, &g, &b);
                    px[0] = r; px[1] = g; px[2] = b; fwrite(px, 1, 3, f);
                }
                if (SDL_MUSTLOCK(sScr)) SDL_UnlockSurface(sScr);
                fclose(f);
            }
        }
    }
    SDL_Flip(sScr);
}
static void ui_wait_button(void) {
    SDL_Event ev;
    /* headless runs (bounded OOT_EXIT_AT run, splash capture): never block */
    if (!sScr || getenv("OOT_EXIT_AT") || getenv("OOT_SPLASH_DUMP")) return;
    while (SDL_PollEvent(&ev)) { } /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0));
    SDL_Flip(sScr);
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0)); /* a double buffer's other page too */
    SDL_Flip(sScr);
}
static void ui_end(int quit) {
    sFontBig = sFontSmall = NULL;
#if defined(MIYOO)
    if (quit && sScr) { SDL_FreeSurface(sScr); SDL_Quit(); sScr = NULL; sPanel = NULL; }
#endif
}
#else
static void ui_begin(void) { }
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    printf("hh: %s | %s | %s | %d%% | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "", pct, foot ? foot : "");
}
static void ui_wait_button(void) { }
static void ui_black(void) { }
static void ui_end(int quit) { (void) quit; }
#endif

static void set_sub(const char* sub) {
#if defined(HANDHELD) && !defined(TARGET_PC)
    snprintf(sSub, sizeof(sSub), "%s", sub ? sub : "");
#else
    (void) sub;
#endif
}

static int sOpen, sLastPct = -2;
static char sLastStage[96], sLastSub[64], sKeepSub[64], sKeepL1[96], sKeepL2[96], sKeepFoot[64];

/* The progress screen: the same lines for the whole job, the stage on the third. Drawn only when the
 * pct, the stage or the sub line changed. */
void HhRomScreen_Progress(const char* sub, const char* l1, const char* l2, const char* stage, int pct, const char* foot) {
    if (pct == sLastPct && strcmp(stage ? stage : "", sLastStage) == 0 && strcmp(sub ? sub : "", sLastSub) == 0) return;
    sLastPct = pct;
    snprintf(sLastStage, sizeof(sLastStage), "%s", stage ? stage : "");
    snprintf(sLastSub, sizeof(sLastSub), "%s", sub ? sub : "");
    snprintf(sKeepSub, sizeof(sKeepSub), "%s", sub ? sub : "");
    snprintf(sKeepL1, sizeof(sKeepL1), "%s", l1 ? l1 : "");
    snprintf(sKeepL2, sizeof(sKeepL2), "%s", l2 ? l2 : "");
    snprintf(sKeepFoot, sizeof(sKeepFoot), "%s", foot ? foot : "");
    ui_begin();
    sOpen = 1;
    set_sub(sub);
    ui_draw(l1, l2, stage, pct, foot);
}

/* an error screen has no "Looking on the SD card" line */
static void clear_default_sub(void) {
#if defined(HANDHELD) && !defined(TARGET_PC)
    if (strcmp(sSub, "Looking on the SD card") == 0) sSub[0] = 0;
#endif
}

void HhRomScreen_Fail(const char* l1, const char* l2, const char* l3) {
    extern void port_log_always(const char* fmt, ...);
    port_log_always("hh: first start stopped: %s %s %s\n", l1 ? l1 : "", l2 ? l2 : "", l3 ? l3 : "");
    ui_begin();
    clear_default_sub();
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
    fflush(NULL);
    exit(1);
}

/* Everything is built: "Done" at 100%, a beat to read it, one black frame; the fonts go but SDL stays
 * up, so the game's mode switch starts from black (no SDL_Quit, no flash). */
void HhRomScreen_Close(void) {
    if (sOpen) {
        char sub[64], l1[96], l2[96], foot[64]; /* copies: Progress overwrites the kept lines */
        snprintf(sub, sizeof(sub), "%s", sKeepSub);
        snprintf(l1, sizeof(l1), "%s", sKeepL1);
        snprintf(l2, sizeof(l2), "%s", sKeepL2);
        snprintf(foot, sizeof(foot), "%s", sKeepFoot);
        HhRomScreen_Progress(sub, l1, l2, "Done", 100, foot);
        if (!getenv("OOT_SPLASH_DUMP") && !getenv("OOT_EXIT_AT") && !getenv("OOT_CONVERT_ONLY")) usleep(600 * 1000);
        ui_black();
        sOpen = 0;
    }
    ui_end(0);
}

/* The device bench's parent (hh_bench.c) after its first start: SDL goes all the way down before the
 * first child starts. Close above keeps SDL up for the game's own mode switch, but the bench parent never
 * draws again: with its display and console still held, the child's video start hung on a black screen
 * (the 2026-10-07 cross-port Bench, view 1, nothing after the child's first line). */
void HhRomScreen_Shutdown(void) {
    ui_end(1);
}

void HhRomScreen_NoData(void) {
    ui_begin();
    clear_default_sub();
    ui_draw("The game data didn't load.", "Start the game again", "to rebuild it from your ROM.", -1,
            "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
    fflush(NULL);
    exit(1);
}
