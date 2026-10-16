/* pd_romscreen.c: the ROM screens, in the layout of the other Miyoo ports' (the GoldenEye port's
 * hh_romscreen.c, after the OoT, SSB64 and MK64 ports'): the title, the ROM's file name, up to three
 * lines, a progress bar while the ROM is checked, and the errors, which wait for a button.
 *
 * Only when there is work (pd_card.c): the first start (the card search, the check and the copy) or a
 * new rom.z64 in the game's folder (its one check). A normal start never shows it. sysFatalError's
 * message (pd_glue.c miyooFatalScreen) uses the same screen, word-wrapped: before 1.0.1 it went to
 * the log only and the game just quit back to the menu.
 *
 * Drawn on the game's own SDL 1.2 screen (gfx_sdl12.c, open since videoInit): a 640x480 picture,
 * centred on a larger panel (Mini v4 / Flip), turned 180 degrees as the game's (PD_ROTATE), in the
 * menu's 8x8 font (kit/miyoo_font.h). The picture is composed in an mmap'd buffer that is unmapped
 * again, and nothing here calls malloc: a start that shows the screen leaves the heap and the
 * mappings where a normal start has them (save states hold pointers into the ROM mapping and the
 * game's heap, and must load after either kind of start).
 *
 * Test knob: PD_SPLASH_DUMP=<dir> writes each drawn screen as a PPM there and never waits for a
 * button; neither does SDL's dummy video driver (qemu). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include "kit/miyoo_font.h"
#include "pd_romscreen.h"

#define MW 640
#define MH 480

extern int gfx_miyoo_rotate(void);             /* gfx_sdl12.c */
extern SDL_Surface *gfx_miyoo_direct_begin(void); /* gfx_sdl12.c: the screen, the present thread idle */
extern void gfx_miyoo_direct_end(void);
extern int gfx_wm_shutdown(void) __attribute__((weak));
extern void miyoo_clock_restore(void);

static uint32_t *sDraw; /* MW x MH, 0xAARRGGBB; mmap'd while a screen is up */
static int sOpen, sShot, sLastPct = -2;
static char sLastStage[96], sKeepSub[64], sKeepL1[96], sKeepL2[96], sKeepFoot[64];

static int ui_begin(void)
{
    void *p;
    if (sDraw != NULL)
        return 1;
    p = mmap(NULL, MW * MH * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    sDraw = p;
    return 1;
}

static void ui_end(void)
{
    if (sDraw != NULL)
        munmap(sDraw, MW * MH * 4);
    sDraw = NULL;
}

static void ui_fill(int x, int y, int w, int h, uint32_t c)
{
    int i, j;
    for (j = y; j < y + h && j < MH; j++)
        for (i = x; i < x + w && i < MW; i++)
            sDraw[j * MW + i] = c;
}

/* a line centred at the top y, each font pixel k x k; a long line (a ROM's file name) keeps its start
 * and ends in "..." within 608 px */
static void ui_text(int y, const char *s, int k, int gray)
{
    const uint32_t px = 0xFF000000u | (uint32_t)gray << 16 | (uint32_t)gray << 8 | (uint32_t)gray;
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
        const uint8_t *g;
        int gx, gy, dx, dy;
        if (ch < 0x20 || ch > 0x7E)
            continue;
        g = kMiyooFont[ch - 0x20];
        for (gy = 0; gy < 8; gy++)
            for (gx = 0; gx < 8; gx++)
                if (g[gy] & (1u << gx))
                    for (dy = 0; dy < k; dy++)
                        for (dx = 0; dx < k; dx++)
                            sDraw[(y + gy * k + dy) * MW + x + gx * k + dx] = px;
    }
}

/* PD_SPLASH_DUMP: the picture as a PPM, written with open/write (no stdio buffer) */
static void ui_dump(int pct)
{
    const char *dd = getenv("PD_SPLASH_DUMP");
    char path[512], hdr[32];
    uint8_t row[MW * 3];
    int fd, x, y, n;
    if (dd == NULL || dd[0] == 0)
        return;
    if (pct >= 0)
        snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, sShot++, pct);
    else
        snprintf(path, sizeof(path), "%s/splash_%02d_fail.ppm", dd, sShot++);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", MW, MH);
    if (write(fd, hdr, n) != n) {
        close(fd);
        return;
    }
    for (y = 0; y < MH; y++) {
        for (x = 0; x < MW; x++) {
            const uint32_t p = sDraw[y * MW + x];
            row[x * 3] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)p;
        }
        if (write(fd, row, sizeof(row)) != (ssize_t)sizeof(row))
            break;
    }
    close(fd);
}

/* the picture onto the game's screen: centred on a larger panel, turned as the game's picture is */
static void ui_show(void)
{
    SDL_Surface *s = gfx_miyoo_direct_begin();
    int x, y;
    if (s == NULL || s->w < MW || s->h < MH) {
        gfx_miyoo_direct_end();
        return;
    }
    {
        const int W = s->w, H = s->h, ox = (W - MW) / 2, oy = (H - MH) / 2, rot = gfx_miyoo_rotate();
        const int bpp = s->format->BytesPerPixel;
        if (SDL_MUSTLOCK(s))
            SDL_LockSurface(s);
        for (y = 0; y < H; y++)
            memset((uint8_t *)s->pixels + y * s->pitch, 0, (size_t)W * bpp);
        for (y = 0; y < MH; y++) {
            const uint32_t *src = sDraw + y * MW;
            uint8_t *dst = (uint8_t *)s->pixels + (rot ? H - 1 - (oy + y) : oy + y) * s->pitch;
            for (x = 0; x < MW; x++) {
                const int dx = rot ? W - 1 - (ox + x) : ox + x;
                const uint32_t p = src[x];
                if (bpp == 4)
                    ((uint32_t *)dst)[dx] = SDL_MapRGB(s->format, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF) | 0xFF000000u;
                else if (bpp == 2)
                    ((uint16_t *)dst)[dx] = (uint16_t)SDL_MapRGB(s->format, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            }
        }
        if (SDL_MUSTLOCK(s))
            SDL_UnlockSurface(s);
        SDL_Flip(s);
    }
    gfx_miyoo_direct_end();
}

/* title, the ROM's name, up to three lines, a progress bar (pct < 0: none), a footer */
static void ui_draw(const char *sub, const char *l1, const char *l2, const char *l3, int pct, const char *foot)
{
    if (!ui_begin())
        return;
    ui_fill(0, 0, MW, MH, 0xFF000000u);
    ui_text(44, "PERFECT DARK", 4, 255);
    ui_text(100, sub, 2, 150);
    ui_text(172, l1, 2, 230);
    ui_text(208, l2, 2, 230);
    ui_text(244, l3, 2, 230);
    if (pct >= 0) {
        char p[16];
        ui_fill(120, 316, 400, 24, 0xFF464646u);
        ui_fill(124, 320, 392 * pct / 100, 16, 0xFFFFFFFFu);
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(352, p, 2, 200);
    }
    ui_text(424, foot, 2, 150);
    ui_dump(pct);
    ui_show();
}

static void ui_black(void)
{
    SDL_Surface *s = gfx_miyoo_direct_begin();
    int i;
    if (s != NULL)
        for (i = 0; i < 2; i++) { /* a double buffer's other page too */
            SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 0, 0));
            SDL_Flip(s);
        }
    gfx_miyoo_direct_end();
}

static int headless(void)
{
    char drv[32];
    if (getenv("PD_SPLASH_DUMP") != NULL)
        return 1;
    return SDL_VideoDriverName(drv, sizeof(drv)) == NULL || strcmp(drv, "dummy") == 0;
}

static void ui_wait_button(void)
{
    SDL_Event ev;
    if (headless())
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

void pd_romscreen_progress(const char *sub, const char *l1, const char *l2, const char *stage, int pct, const char *foot)
{
    if (pct == sLastPct && strcmp(stage ? stage : "", sLastStage) == 0 && strcmp(sub ? sub : "", sKeepSub) == 0)
        return;
    sLastPct = pct;
    snprintf(sLastStage, sizeof(sLastStage), "%s", stage ? stage : "");
    snprintf(sKeepSub, sizeof(sKeepSub), "%s", sub ? sub : "");
    snprintf(sKeepL1, sizeof(sKeepL1), "%s", l1 ? l1 : "");
    snprintf(sKeepL2, sizeof(sKeepL2), "%s", l2 ? l2 : "");
    snprintf(sKeepFoot, sizeof(sKeepFoot), "%s", foot ? foot : "");
    sOpen = 1;
    ui_draw(sub, l1, l2, stage, pct, foot);
}

void pd_romscreen_finish(void)
{
    if (!sOpen)
        return;
    ui_draw(sKeepSub, sKeepL1, sKeepL2, "Done", 100, sKeepFoot);
    if (!headless())
        usleep(600 * 1000);
    ui_black();
    sOpen = 0;
    sLastPct = -2;
    ui_end();
}

static void __attribute__((noreturn)) give_back_and_exit(void)
{
    ui_end();
    fflush(NULL);
    /* back to the OS's menu in a usable state: SDL 1.2's fbcon down, the clock as found */
    if (gfx_wm_shutdown)
        gfx_wm_shutdown();
    miyoo_clock_restore();
    exit(1);
}

void pd_romscreen_fail(const char *sub, const char *l1, const char *l2, const char *l3)
{
    printf("[card] stopped: %s%s%s%s%s\n", l1, l2 ? " " : "", l2 ? l2 : "", l3 ? " " : "", l3 ? l3 : "");
    fflush(stdout);
    ui_draw(sub, l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    give_back_and_exit();
}

/* sysFatalError's text: its lines, each wrapped at a word to the screen's 38 columns (2x font), the
 * first nine shown */
void pd_romscreen_fatal(const char *msg)
{
    enum { COLS = (MW - 32) / 16, ROWS = 9 };
    char lines[ROWS][COLS + 1];
    int n = 0, i;
    const char *p = msg;
    while (*p && n < ROWS) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (n < ROWS) {
            size_t take = len;
            if (take > COLS) {
                take = COLS;
                while (take > 0 && p[take] != ' ')
                    take--;
                if (take == 0)
                    take = COLS;
            }
            memcpy(lines[n], p, take);
            lines[n++][take] = 0;
            p += take;
            len -= take;
            while (len > 0 && *p == ' ') {
                p++;
                len--;
            }
            if (len == 0)
                break;
        }
        p += len;
        if (*p == '\n')
            p++;
    }
    if (!ui_begin())
        return;
    ui_fill(0, 0, MW, MH, 0xFF000000u);
    ui_text(44, "PERFECT DARK", 4, 255);
    ui_text(100, "The game had to stop.", 2, 150);
    for (i = 0; i < n; i++)
        ui_text(150 + i * 28, lines[i], 2, 230);
    ui_text(424, "Press any button to quit.", 2, 150);
    ui_dump(-1);
    ui_show();
    ui_wait_button();
    ui_end();
}
