/* hh_romscreen.c: the first-start screens (hh_card.c), in the layout of the other ports' (the OoT port
 * hh_romscreen.c, the SSB64 port romdata.c, after the MK64 port's): the title, the ROM's file name, up to
 * three lines, a progress bar while the ROM is checked, and the errors, which wait for a button.
 *
 * Only when there is work: the first start (the card search, the check, the copy) or a new rom.z64 with
 * the ROM inside (its one check). A normal start never opens it, unless filling the game's data
 * tables (hh_romtables.c) runs past HH_LAZY_MS. On the device it is drawn with SDL 1.2 on the
 * Miyoo Mini's 640x480 screen (TARGET_MIYOO below). It covers the whole job: the
 * card search, the check and copy (0..80%), then the
 * tables' fill (80..99%), then "Done" at 100% only when everything is finished (hh_romscreen_finish
 * from hh_boot.c). It then ends on a black frame and leaves SDL initialised, so gfx_sdl12.c's
 * SDL_Init is a no-op and its mode switch starts from black, as the other ports' ui_end(0) does
 * (the Star Fox 64 port, the DKR port, the SSB64 port); BK_ROMSCREEN=0 is the old order (the screen closed with
 * SDL_Quit when the card work ends, the fill unseen). Elsewhere (the desktop proxy, a screen that
 * can't be opened) the lines go to the log only.
 *
 * Test knob: BK_SPLASH_DUMP=<dir> writes each drawn screen as a PPM there (SDL_ttf draws into the
 * surface under SDL_VIDEODRIVER=dummy too) and never waits for a button; BK_EXIT_AT and a bench run
 * (BK_BENCH_CHILD) never wait either. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hh_romscreen.h"

#define HH_LAZY_MS 1500 /* a later start's fill shows the screen only past this (BK_LAZY_MS=<ms>: a test knob) */

#if defined(TARGET_MIYOO)
/* The Miyoo Mini: the same screens drawn at 640x480, 32 bpp, in the menu's 8x8 font (miyoo_font.h; the
 * Mini has no handheld OS fonts), every coordinate the handheld's doubled, and turned 180 degrees as the
 * game is (gfx_miyoo_rotate: BK_ROTATE). The screen is opened as the game opens it (gfx_miyoo_open_panel,
 * the mode gfx_sdl12.c then keeps): 640x480 on a Mini; on a Mini v4 / Flip the panel's own 752x560, the
 * picture at its full height as the game's (BK_FLIPFILL). */
#include <SDL/SDL.h>
#include "miyoo_font.h"
#define MW 640
#define MH 480
extern int gfx_miyoo_rotate(void); /* gfx_sdl12.c */
extern SDL_Surface *gfx_miyoo_open_panel(void); /* gfx_sdl12.c */
static SDL_Surface *sScr, *sDraw;
static int sTried;

static void ui_begin(void)
{
    if (sScr != NULL || sTried)
        return;
    sTried = 1;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("[card] screen: SDL_Init: %s (the log only)\n", SDL_GetError());
        return;
    }
    sScr = gfx_miyoo_open_panel();
    sDraw = SDL_CreateRGBSurface(SDL_SWSURFACE, MW, MH, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (sScr == NULL || sDraw == NULL) {
        printf("[card] screen: SDL_SetVideoMode: %s (the log only)\n", SDL_GetError());
        if (sDraw != NULL)
            SDL_FreeSurface(sDraw);
        sScr = sDraw = NULL;
        SDL_Quit();
        return;
    }
    SDL_ShowCursor(SDL_DISABLE);
}

/* a line centred at the top y, each font pixel k x k; a long line (a ROM's file name) keeps its start
 * and ends in "..." within 608 px */
static void ui_text(int y, const char *s, int k, int gray)
{
    const Uint32 px = 0xFF000000u | (Uint32)gray << 16 | (Uint32)gray << 8 | (Uint32)gray;
    char fit[128];
    size_t n;
    int x, i;
    if (sDraw == NULL || s == NULL || s[0] == 0)
        return;
    snprintf(fit, sizeof(fit), "%s", s);
    n = strlen(fit);
    if ((int)n * 8 * k > MW - 32) {
        n = (size_t)((MW - 32) / (8 * k));
        memcpy(fit + n - 3, "...", 4);
    }
    x = (MW - (int)n * 8 * k) / 2;
    for (i = 0; fit[i]; i++, x += 8 * k) {
        const unsigned ch = (unsigned char)fit[i];
        const Uint8 *g;
        int gx, gy, dx, dy;
        if (ch < 0x20 || ch > 0x7E)
            continue;
        g = kMiyooFont[ch - 0x20];
        for (gy = 0; gy < 8; gy++)
            for (gx = 0; gx < 8; gx++)
                if (g[gy] & (1u << gx))
                    for (dy = 0; dy < k; dy++) {
                        Uint32 *row = (Uint32 *)((Uint8 *)sDraw->pixels + (y + gy * k + dy) * sDraw->pitch);
                        for (dx = 0; dx < k; dx++)
                            row[x + gx * k + dx] = px;
                    }
    }
}

static void ui_dump(int pct)
{
    static int sShot;
    const char *dd = getenv("BK_SPLASH_DUMP");
    char path[512];
    FILE *f;
    int x, y;
    if (dd == NULL || dd[0] == 0)
        return;
    if (pct >= 0)
        snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, sShot++, pct);
    else
        snprintf(path, sizeof(path), "%s/splash_%02d_fail.ppm", dd, sShot++);
    f = fopen(path, "wb");
    if (f == NULL)
        return;
    fprintf(f, "P6\n%d %d\n255\n", MW, MH);
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++) {
            const Uint32 p = ((Uint32 *)((Uint8 *)sDraw->pixels + y * sDraw->pitch))[x];
            const Uint8 px[3] = { (Uint8)(p >> 16), (Uint8)(p >> 8), (Uint8)p };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* sDraw onto the screen, turned as the game's picture is */
static void ui_show(void)
{
    int x, y;
    if (gfx_miyoo_rotate() && sScr->format->BitsPerPixel == 32 && sScr->w == MW && sScr->h == MH) {
        if (SDL_MUSTLOCK(sScr))
            SDL_LockSurface(sScr);
        for (y = 0; y < MH; y++) {
            const Uint32 *src = (const Uint32 *)((Uint8 *)sDraw->pixels + y * sDraw->pitch);
            Uint32 *dst = (Uint32 *)((Uint8 *)sScr->pixels + (MH - 1 - y) * sScr->pitch);
            for (x = 0; x < MW; x++)
                dst[MW - 1 - x] = src[x] | 0xFF000000u;
        }
        if (SDL_MUSTLOCK(sScr))
            SDL_UnlockSurface(sScr);
    } else if (sScr->format->BitsPerPixel == 32 && (sScr->w != MW || sScr->h != MH)) {
        /* a larger panel (Mini v4 / Flip): at its full height, centred, nearest, as gfx_sdl12.c's
         * blit_stage_to_screen (a width within 2% of the panel's takes all of it: 752x560 whole) */
        const int W = sScr->w, H = sScr->h, rot = gfx_miyoo_rotate();
        int dw = H * MW / MH, ox;
        if (dw > W || (W - dw) * 50 <= W)
            dw = W;
        ox = (W - dw) / 2;
        if (SDL_MUSTLOCK(sScr))
            SDL_LockSurface(sScr);
        for (y = 0; y < H; y++) {
            const Uint32 *src = (const Uint32 *)((Uint8 *)sDraw->pixels + (y * MH / H) * sDraw->pitch);
            Uint32 *dst = (Uint32 *)((Uint8 *)sScr->pixels + (rot ? H - 1 - y : y) * sScr->pitch);
            for (x = 0; x < W; x++) {
                Uint32 p = 0xFF000000u;
                if (x >= ox && x < ox + dw) {
                    int sx = ((x - ox) * MW + MW / 2) / dw;
                    if (sx >= MW)
                        sx = MW - 1;
                    p = src[sx] | 0xFF000000u;
                }
                dst[rot ? W - 1 - x : x] = p;
            }
        }
        if (SDL_MUSTLOCK(sScr))
            SDL_UnlockSurface(sScr);
    } else {
        SDL_BlitSurface(sDraw, NULL, sScr, NULL);
    }
    SDL_Flip(sScr);
}

/* title, the ROM's name, up to three lines, a progress bar (pct < 0: none), a footer */
static void ui_draw(const char *sub, const char *l1, const char *l2, const char *l3, int pct, const char *foot)
{
    if (sScr == NULL)
        return;
    SDL_FillRect(sDraw, NULL, 0xFF000000u);
    ui_text(44, "BANJO-KAZOOIE", 4, 255);
    ui_text(100, sub, 2, 150);
    ui_text(172, l1, 2, 230);
    ui_text(208, l2, 2, 230);
    ui_text(244, l3, 2, 230);
    if (pct >= 0) {
        SDL_Rect frame, fill;
        char p[16];
        frame.x = 120; frame.y = 316; frame.w = 400; frame.h = 24;
        fill.x = 124; fill.y = 320; fill.w = (Uint16)(392 * pct / 100); fill.h = 16;
        SDL_FillRect(sDraw, &frame, 0xFF464646u);
        SDL_FillRect(sDraw, &fill, 0xFFFFFFFFu);
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(352, p, 2, 200);
    }
    ui_text(424, foot, 2, 150);
    ui_dump(pct);
    ui_show();
}

static void ui_black(void)
{
    if (sScr == NULL)
        return;
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0));
    SDL_Flip(sScr);
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0)); /* a double buffer's other page too */
    SDL_Flip(sScr);
}

static void ui_wait_button(void)
{
    SDL_Event ev;
    if (sScr == NULL || getenv("BK_SPLASH_DUMP") != NULL || getenv("BK_EXIT_AT") != NULL ||
        getenv("BK_BENCH_CHILD") != NULL)
        return; /* headless runs never block */
    while (SDL_PollEvent(&ev)) {
    } /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev))
            return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT)
            return;
    }
}

static void ui_end(int quit)
{
    if (sDraw != NULL)
        SDL_FreeSurface(sDraw);
    sDraw = NULL;
    if (sScr != NULL) {
        if (quit)
            SDL_Quit();
        sScr = NULL;
    }
    sTried = 0;
}
#else
static void ui_begin(void) {}
static void ui_draw(const char *sub, const char *l1, const char *l2, const char *l3, int pct, const char *foot)
{
    (void)sub; (void)l1; (void)l2; (void)l3; (void)pct; (void)foot;
}
static void ui_wait_button(void) {}
static void ui_black(void) {}
static void ui_end(int quit) { (void)quit; }
#endif

static int sOpen, sLastPct = -2;
static char sLastStage[96], sLastSub[64], sKeepL1[96], sKeepL2[96], sKeepSub[64], sKeepFoot[64];
static long sFillT0;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* SDL is left up after the screen unless BK_ROMSCREEN=0 */
static int keep_sdl(void)
{
    const char *e = getenv("BK_ROMSCREEN");
    return e == NULL || e[0] != '0';
}

/* the screen's lines: sub (the ROM's name), l1 and l2 fixed for the job, the stage on the third line */
static void draw(const char *sub, const char *l1, const char *l2, const char *stage, int pct, const char *foot)
{
    if (pct == sLastPct && strcmp(stage ? stage : "", sLastStage) == 0 && strcmp(sub ? sub : "", sLastSub) == 0)
        return;
    sLastPct = pct;
    snprintf(sLastStage, sizeof(sLastStage), "%s", stage ? stage : "");
    snprintf(sLastSub, sizeof(sLastSub), "%s", sub ? sub : "");
    snprintf(sKeepSub, sizeof(sKeepSub), "%s", sub ? sub : "");
    snprintf(sKeepL1, sizeof(sKeepL1), "%s", l1 ? l1 : "");
    snprintf(sKeepL2, sizeof(sKeepL2), "%s", l2 ? l2 : "");
    snprintf(sKeepFoot, sizeof(sKeepFoot), "%s", foot ? foot : "");
    ui_begin();
    sOpen = 1;
    ui_draw(sub, l1, l2, stage, pct, foot);
}

/* The card work: pct 0..HH_CARD_PCT. */
void hh_romscreen_progress(const char *sub, const char *l1, const char *l2, const char *stage, int pct, const char *foot)
{
    if (getenv("BK_BENCH_CHILD") != NULL) /* a bench run: its parent did the first start */
        return;
    if (!keep_sdl()) /* the old order: no fill share, the card work fills the bar */
        pct = pct * 100 / HH_CARD_PCT;
    draw(sub, l1, l2, stage, pct, foot);
}

void hh_romscreen_fill_begin(void)
{
    sFillT0 = now_ms();
}

/* The tables' fill, p 0..100 of its own share. Open already (the first start): the same lines, the bar
 * from 80 to 99. Not open (a later start): nothing shows unless it has run past HH_LAZY_MS. */
void hh_romscreen_fill(const char *stage, int p)
{
    if (getenv("BK_BENCH_CHILD") != NULL)
        return;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    if (sOpen) {
        char sub[64], l1[96], l2[96], foot[64];
        snprintf(sub, sizeof(sub), "%s", sKeepSub);
        snprintf(l1, sizeof(l1), "%s", sKeepL1);
        snprintf(l2, sizeof(l2), "%s", sKeepL2);
        snprintf(foot, sizeof(foot), "%s", sKeepFoot);
        draw(sub, l1, l2, stage, HH_CARD_PCT + p * (99 - HH_CARD_PCT) / 100, foot);
    } else if (keep_sdl() && now_ms() - sFillT0 >= (getenv("BK_LAZY_MS") ? atol(getenv("BK_LAZY_MS")) : HH_LAZY_MS)) {
        draw("", "Building the game data", "from your ROM.", stage, p * 99 / 100, "Just a moment.");
    }
}

/* Everything is built: 100% and "Done", a beat to read it, then a black frame and the fonts down. SDL
 * stays up (BK_ROMSCREEN=0: the screen was closed already, nothing to do). */
void hh_romscreen_finish(void)
{
    char sub[64], l1[96], l2[96], foot[64];
    if (!sOpen)
        return;
    snprintf(sub, sizeof(sub), "%s", sKeepSub);
    snprintf(l1, sizeof(l1), "%s", sKeepL1);
    snprintf(l2, sizeof(l2), "%s", sKeepL2);
    snprintf(foot, sizeof(foot), "%s", sKeepFoot);
    draw(sub, l1, l2, "Done", 100, foot);
    if (getenv("BK_SPLASH_DUMP") == NULL && getenv("BK_EXIT_AT") == NULL)
        usleep(600 * 1000);
    ui_black();
    sOpen = 0;
    sLastPct = -2;
    ui_end(0);
}

void hh_romscreen_fail(const char *sub, const char *l1, const char *l2, const char *l3)
{
    printf("[card] stopped: %s%s%s%s%s\n", l1, l2 ? " " : "", l2 ? l2 : "", l3 ? " " : "", l3 ? l3 : "");
    fflush(stdout);
    if (getenv("BK_BENCH_CHILD") == NULL) {
        ui_begin();
        ui_draw(sub, l1, l2, l3, -1, "Press any button to quit.");
        ui_wait_button();
    }
    ui_end(1);
    fflush(NULL);
    exit(1);
}

/* the screen down for good (SDL_Quit) */
void hh_romscreen_close(void)
{
    if (!sOpen)
        return;
    sOpen = 0;
    sLastPct = -2;
    ui_end(1);
}

/* the card work is over: the screen stays up for the fill, except under BK_ROMSCREEN=0 (the old order) */
void hh_romscreen_card_done(void)
{
    if (!keep_sdl())
        hh_romscreen_close();
}
