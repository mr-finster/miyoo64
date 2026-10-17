/**
 * Miyoo Mini in-game menu (MENU button) -- the handheld_menu.h API on a 640x480 screen.
 *
 * the handheld OS's menu (handheld_menu.c) draws with the OS's own fonts and card images and drives
 * the OS's shell commands (volume, brightness, keymap, notif); the Mini has none of them, and
 * its OS already owns volume and brightness. This one needs nothing from the card: an 8x8
 * bitmap font (miyoo_font.h) scaled up, drawn over a dimmed snapshot of the last game frame.
 *
 * Items: RESUME, SAVE STATE, LOAD STATE, EXIT GAME.
 * Keys (the Mini's SDL): up/down choose, left/right change the slot, A (Space) activates
 * (twice where it asks "Are you sure?"), B (Left Ctrl) backs out of a question or resumes,
 * MENU (Escape) resumes.
 * Notifications (SAVED SLOT n, ...) show for two seconds in the top-left corner of the game
 * picture: gfx_sdl12.c draws them on each presented frame (miyoo_menu_overlay).
 */
#if defined(MIYOO)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL/SDL.h>
#include "hh_game.h"
#include "handheld_menu.h"
#include "savestate.h"
#include "miyoo_font.h"
#if defined(MIYOO)
#include "miyoo_sleepsave.h"
#endif

#define SCREEN_W 640
#define SCREEN_H 480
#define FPS_MENU 30
#define NOTICE_MS 2000

extern void gfx_sdl12_set_display_mode(int mode);
extern void gfx_sdl12_set_res_dynamic(int on);
extern void gfx_sdl12_set_res_pct(int pct);
extern int gfx_soft_bilinear, gfx_soft_magbilin;
extern void port_request_exit(void);
extern void port_audio_out_set_muted(int muted);
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
extern void controller_handheld_key(int sym, int down) __attribute__((weak));

enum { I_RESUME, I_SAVE, I_LOAD, I_EXIT, I_COUNT };
static const char *ITEM[I_COUNT] = { "RESUME", "SAVE STATE", "LOAD STATE", "EXIT GAME" };
#define SLOT_MIN 1
#define SLOT_MAX 9

static SDL_Surface *sScreenPtr SAVESTATE_EXCLUDE;
static SDL_Surface *sBackup SAVESTATE_EXCLUDE, *sDraw SAVESTATE_EXCLUDE; /* dimmed frozen frame, compose */
static int sSlot SAVESTATE_EXCLUDE = SLOT_MIN;
static int sAutoSel SAVESTATE_EXCLUDE; /* LOAD: the auto save (EXIT GAME / SIGTERM), one step left of slot 1 */
static int sInited SAVESTATE_EXCLUDE;
static char sNotice[40] SAVESTATE_EXCLUDE;
static volatile uint32_t sNoticeUntil SAVESTATE_EXCLUDE;
#define AUTO_AVAILABLE() savestate_slot_valid(QUICKSAVE_SLOT)

static const uint32_t C_TEXT = 0xFFECECECu, C_DIM = 0xFF808080u, C_SEL = 0xFFFFC040u, C_PANEL = 0xFF101418u,
                      C_EDGE = 0xFF505860u;

/// -------- drawing (32-bit 0xAARRGGBB surfaces) --------
static void fill(SDL_Surface *s, int x, int y, int w, int h, uint32_t c) {
    SDL_Rect r = { (Sint16)x, (Sint16)y, (Uint16)w, (Uint16)h };
    SDL_FillRect(s, &r, SDL_MapRGB(s->format, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
}

/* Text with its top-left at x, y, each font pixel k x k screen pixels. Needs the surface locked
 * (sDraw and sBackup are software surfaces: never). */
static void text_at(SDL_Surface *s, int x, int y, int k, const char *t, uint32_t c) {
    const uint32_t px = SDL_MapRGB(s->format, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    const int bpp = s->format->BytesPerPixel;
    for (; *t; t++, x += 8 * k) {
        const unsigned ch = (unsigned char)*t;
        if (ch < 0x20 || ch > 0x7E) continue;
        const uint8_t *g = kMiyooFont[ch - 0x20];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++) {
                if (!(g[gy] & (1u << gx))) continue;
                for (int dy = 0; dy < k; dy++) {
                    const int yy = y + gy * k + dy;
                    if (yy < 0 || yy >= s->h) continue;
                    uint8_t *row = (uint8_t *)s->pixels + yy * s->pitch;
                    for (int dx = 0; dx < k; dx++) {
                        const int xx = x + gx * k + dx;
                        if (xx < 0 || xx >= s->w) continue;
                        if (bpp == 4) ((uint32_t *)row)[xx] = px;
                        else if (bpp == 2) ((uint16_t *)row)[xx] = (uint16_t)px;
                    }
                }
            }
    }
}
static int text_w(const char *t, int k) { return (int)strlen(t) * 8 * k; }
static void text_centered(SDL_Surface *s, int cy, int k, const char *t, uint32_t c) {
    text_at(s, (s->w - text_w(t, k)) / 2, cy - 4 * k, k, t, c);
}

/* The last game frame, dimmed to 40%, doubled to the screen. */
static void snapshot_game_frame(void) {
    int w = 0, h = 0;
    uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    if (!sBackup) return;
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)sBackup->pixels + y * sBackup->pitch);
        if (!fb || w <= 0 || h <= 0) { memset(dst, 0, SCREEN_W * 4); continue; }
        const uint32_t *src = fb + (y * h / SCREEN_H) * w;
        for (int x = 0; x < SCREEN_W; x++) {
            const uint32_t p = src[x * w / SCREEN_W];
            dst[x] = 0xFF000000u | (((p >> 16 & 0xFF) * 2 / 5) << 16) | (((p >> 8 & 0xFF) * 2 / 5) << 8) | ((p & 0xFF) * 2 / 5);
        }
    }
}

static void panel(SDL_Surface *s, int x, int y, int w, int h) {
    fill(s, x - 2, y - 2, w + 4, h + 4, C_EDGE);
    fill(s, x, y, w, h, C_PANEL);
}

static void slot_label(char *buf, size_t n, int item) {
    if (item == I_LOAD && sAutoSel && AUTO_AVAILABLE()) snprintf(buf, n, "<  AUTO SAVE  >");
    else snprintf(buf, n, "<   SLOT %d   >", sSlot);
}

static void compose_menu(int cur, int confirm) {
    SDL_BlitSurface(sBackup, NULL, sDraw, NULL);
    const int pw = 420, ph = 330, px = (SCREEN_W - pw) / 2, py = (SCREEN_H - ph) / 2;
    panel(sDraw, px, py, pw, ph);
    text_centered(sDraw, py + 34, 3, "GOLDENEYE 007", C_TEXT);
    fill(sDraw, px + 30, py + 62, pw - 60, 2, C_EDGE);
    for (int i = 0; i < I_COUNT; i++) {
        const int cy = py + 100 + i * 44;
        if (i == cur) fill(sDraw, px + 20, cy - 18, pw - 40, 36, 0xFF283038u);
        text_centered(sDraw, cy, 3, ITEM[i], i == cur ? C_SEL : C_TEXT);
    }
    char buf[48];
    const int info_y = py + ph - 44;
    if (cur == I_SAVE || cur == I_LOAD) {
        const int au = cur == I_LOAD && sAutoSel && AUTO_AVAILABLE();
        slot_label(buf, sizeof(buf), cur);
        text_centered(sDraw, info_y - 12, 2, buf, C_TEXT);
        text_centered(sDraw, info_y + 14, 2, confirm ? "ARE YOU SURE? (A)" : (au || savestate_slot_valid(sSlot)) ? "USED" : "FREE",
                      confirm ? C_SEL : C_DIM);
    } else if (cur == I_EXIT && confirm) {
        text_centered(sDraw, info_y, 2, "ARE YOU SURE? (A)", C_SEL);
    } else {
        text_centered(sDraw, info_y, 2, "A: SELECT   B/MENU: BACK", C_DIM);
    }
}

extern int gfx_miyoo_rotate(void); /* gfx_sdl12.c: the panel wants the picture turned 180 degrees */

static void show(SDL_Surface *screen) {
    if (!screen) return;
    if (gfx_miyoo_rotate() && screen->format->BitsPerPixel == 32 && screen->w == SCREEN_W && screen->h == SCREEN_H) {
        if (SDL_MUSTLOCK(screen)) SDL_LockSurface(screen);
        for (int y = 0; y < SCREEN_H; y++) {
            const uint32_t *src = (const uint32_t *)((uint8_t *)sDraw->pixels + y * sDraw->pitch);
            uint32_t *dst = (uint32_t *)((uint8_t *)screen->pixels + (SCREEN_H - 1 - y) * screen->pitch);
            for (int x = 0; x < SCREEN_W; x++) dst[SCREEN_W - 1 - x] = src[x] | 0xFF000000u;
        }
        if (SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
    } else if (screen->format->BitsPerPixel == 32 && (screen->w > SCREEN_W || screen->h > SCREEN_H)) {
        /* a larger panel (Mini v4 / Flip, gfx_sdl12.c <GAME>_FLIPFILL): the 640x480 menu centred on black, turned */
        const int W = screen->w, H = screen->h, ox = (W - SCREEN_W) / 2, oy = (H - SCREEN_H) / 2, rot = gfx_miyoo_rotate();
        if (SDL_MUSTLOCK(screen)) SDL_LockSurface(screen);
        for (int y = 0; y < H; y++) memset((uint8_t *)screen->pixels + y * screen->pitch, 0, (size_t)W * 4);
        for (int y = 0; y < SCREEN_H; y++) {
            const uint32_t *src = (const uint32_t *)((uint8_t *)sDraw->pixels + y * sDraw->pitch);
            const int dy = rot ? H - 1 - (oy + y) : oy + y;
            uint32_t *dst = (uint32_t *)((uint8_t *)screen->pixels + dy * screen->pitch);
            for (int x = 0; x < SCREEN_W; x++) dst[rot ? W - 1 - (ox + x) : ox + x] = src[x] | 0xFF000000u;
        }
        if (SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
    } else {
        SDL_BlitSurface(sDraw, NULL, screen, NULL);
    }
    SDL_Flip(screen);
}

/* A key let go while the menu is up must be let go in the game too (the kit's B6). */
static void pass_key_up(const SDL_Event *ev) {
    if (controller_handheld_key) controller_handheld_key(ev->key.keysym.sym, 0);
}

/// -------- public API (handheld_menu.h) --------
void handheld_menu_notify(const char *msg) {
    snprintf(sNotice, sizeof(sNotice), "%s", msg);
    sNoticeUntil = SDL_GetTicks() + NOTICE_MS;
    if (sNoticeUntil == 0) sNoticeUntil = 1;
}

/* The same, shown at once and held a moment (the game quits right after: SAVE FAILED on the exit
 * save, savestate.c), where the handheld's waits for the OS banner. */
extern void gfx_miyoo_present_now(void); /* gfx_sdl12.c */
void handheld_menu_notify_wait(const char *msg) {
    handheld_menu_notify(msg);
    gfx_miyoo_present_now();
    SDL_Delay(1500);
}

/* gfx_sdl12.c's present thread, on the frame before it is doubled and turned: the current
 * notification, if any. */
void miyoo_menu_overlay(SDL_Surface *screen) {
    const uint32_t until = sNoticeUntil;
    if (!until || !screen) return;
    if ((int32_t)(SDL_GetTicks() - until) >= 0) { sNoticeUntil = 0; return; }
    char msg[sizeof(sNotice)];
    memcpy(msg, sNotice, sizeof(msg));
    msg[sizeof(msg) - 1] = 0;
    /* sized for the 640x480 screen: at 320x240 (the game's picture, doubled later) all halved */
    const int k = screen->w >= 640 ? 2 : 1, m = 4 * k, w = text_w(msg, k) + 8 * k, h = 14 * k, bpp = screen->format->BytesPerPixel;
    for (int y = m; y < m + h && y < screen->h; y++) /* a black box, written directly: the surface may be locked */
        memset((uint8_t *)screen->pixels + y * screen->pitch + m * bpp, 0, (size_t)((w < screen->w - m ? w : screen->w - m) * bpp));
    text_at(screen, m + 4 * k, m + 3 * k, k, msg, C_TEXT);
}

/* The same notice drawn straight on the 640x480 screen, turned as the picture is (rot), for a frame doubled without
 * a copy (gfx_sdl12.c sPresSrc): the pixels miyoo_menu_overlay on the 320x240 frame gives once doubled and turned --
 * every coordinate doubled, k 2, the box opaque black and the text opaque, as row_x2_32 makes them. */
void miyoo_menu_overlay_turned(SDL_Surface *screen, int rot) {
    const uint32_t until = sNoticeUntil;
    if (!until || !screen || screen->format->BitsPerPixel != 32) return;
    if ((int32_t)(SDL_GetTicks() - until) >= 0) { sNoticeUntil = 0; return; }
    char msg[sizeof(sNotice)];
    memcpy(msg, sNotice, sizeof(msg));
    msg[sizeof(msg) - 1] = 0;
    const int W = screen->w, H = screen->h;
    /* the 320x240 frame's box and text, every coordinate doubled */
    const int m1 = 4, w1 = text_w(msg, 1) + 8, h1 = 14, bw1 = w1 < 320 - m1 ? w1 : 320 - m1;
    const uint32_t black = 0xFF000000u;
    const uint32_t tpx = SDL_MapRGB(screen->format, (C_TEXT >> 16) & 0xFF, (C_TEXT >> 8) & 0xFF, C_TEXT & 0xFF) | 0xFF000000u;
#define TURN_PUT(X, Y, C) do { const int x_ = rot ? W - 1 - (X) : (X), y_ = rot ? H - 1 - (Y) : (Y); \
                               ((uint32_t *)((uint8_t *)screen->pixels + y_ * screen->pitch))[x_] = (C); } while (0)
    for (int y = 2 * m1; y < 2 * (m1 + h1) && y < H; y++)
        for (int x = 2 * m1; x < 2 * (m1 + bw1); x++) TURN_PUT(x, y, black);
    int x0 = 2 * (m1 + 4);
    const int y0 = 2 * (m1 + 3);
    for (const char *t = msg; *t; t++, x0 += 16) {
        const unsigned ch = (unsigned char)*t;
        if (ch < 0x20 || ch > 0x7E) continue;
        const uint8_t *g = kMiyooFont[ch - 0x20];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++) {
                if (!(g[gy] & (1u << gx))) continue;
                for (int d = 0; d < 4; d++) {
                    const int xx = x0 + gx * 2 + (d & 1), yy = y0 + gy * 2 + (d >> 1);
                    if (xx < 2 * 320 && yy < H) TURN_PUT(xx, yy, tpx);
                }
            }
    }
#undef TURN_PUT
}

void handheld_menu_init(struct SDL_Surface *screen) {
    sScreenPtr = (SDL_Surface *)screen;
    sBackup = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, SCREEN_H, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    sDraw = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, SCREEN_H, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    sInited = sBackup != NULL && sDraw != NULL;
    /* the settings handheld_menu.c's settings_load makes: the 3D at 100% (or <GAME>_INTRES) with SMOOTH */
    { const char *b = getenv(HH_ENV("MAGBILIN")); gfx_soft_magbilin = (b && b[0] == '1'); }
    gfx_sdl12_set_display_mode(HH_DISPLAY_CONVERT);
    gfx_sdl12_set_res_dynamic(0);
    {
        const char *r = getenv(HH_ENV("INTRES"));
#ifndef HH_DEFAULT_3D_PCT
#define HH_DEFAULT_3D_PCT 100
#endif
        gfx_sdl12_set_res_pct(r && atoi(r) > 0 ? atoi(r) : HH_DEFAULT_3D_PCT);
    }
    gfx_soft_bilinear = 1;
}

void handheld_menu_run(struct SDL_Surface *screen_) {
    SDL_Surface *screen = (SDL_Surface *)screen_;
    if (!sInited || !screen) return;
    port_audio_out_set_muted(1);
    snapshot_game_frame();
    int cur = I_RESUME, confirm = 0, running = 1, dirty = 1, exiting = 0;
    while (running) {
        const uint32_t t0 = SDL_GetTicks();
        if (handheld_power_pending()) break; /* SIGTERM: back to the game loop, which exit-saves */
#if defined(MIYOO)
        if (miyoo_sleepsave_menu_should_close()) break; /* POWER: the game loop writes the resume state, then reopens this */
#endif
        SDL_Event ev;
        while (running && SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { port_request_exit(); exiting = 1; running = 0; break; }
            if (ev.type == SDL_KEYUP) { pass_key_up(&ev); continue; }
            if (ev.type != SDL_KEYDOWN) continue;
            switch (ev.key.keysym.sym) {
            case SDLK_ESCAPE:
                running = 0;
                break;
            case SDLK_LCTRL: /* B */
                if (confirm) { confirm = 0; dirty = 1; }
                else running = 0;
                break;
            case SDLK_DOWN:
                cur = (cur + 1) % I_COUNT; confirm = 0; dirty = 1;
                break;
            case SDLK_UP:
                cur = (cur + I_COUNT - 1) % I_COUNT; confirm = 0; dirty = 1;
                break;
            case SDLK_LEFT:
                if (cur == I_SAVE || cur == I_LOAD) {
                    if (cur == I_LOAD && sAutoSel) { sAutoSel = 0; sSlot = SLOT_MAX; }
                    else if (cur == I_LOAD && sSlot == SLOT_MIN && AUTO_AVAILABLE()) sAutoSel = 1;
                    else sSlot = sSlot > SLOT_MIN ? sSlot - 1 : SLOT_MAX;
                    confirm = 0; dirty = 1;
                }
                break;
            case SDLK_RIGHT:
                if (cur == I_SAVE || cur == I_LOAD) {
                    if (cur == I_LOAD && sAutoSel) { sAutoSel = 0; sSlot = SLOT_MIN; }
                    else if (cur == I_LOAD && sSlot == SLOT_MAX && AUTO_AVAILABLE()) sAutoSel = 1;
                    else sSlot = sSlot < SLOT_MAX ? sSlot + 1 : SLOT_MIN;
                    confirm = 0; dirty = 1;
                }
                break;
            case SDLK_SPACE: /* A */
            case SDLK_RETURN: /* START */
                if (cur == I_RESUME) running = 0;
                else if (cur == I_SAVE) {
                    if (confirm || !savestate_slot_valid(sSlot)) { savestate_request_save(sSlot); running = 0; }
                    else { confirm = 1; dirty = 1; }
                } else if (cur == I_LOAD) {
                    const int au = sAutoSel && AUTO_AVAILABLE();
                    if (!au && !savestate_slot_valid(sSlot)) break; /* FREE: nothing to load */
                    if (confirm) { savestate_request_load(au ? QUICKSAVE_SLOT : sSlot); running = 0; }
                    else { confirm = 1; dirty = 1; }
                } else if (cur == I_EXIT) {
                    if (confirm) { savestate_request_exit_save(); exiting = 1; running = 0; }
                    else { confirm = 1; dirty = 1; }
                }
                break;
            default:
                break;
            }
        }
        if (dirty && running) { compose_menu(cur, confirm); show(screen); dirty = 0; }
        const uint32_t dt = SDL_GetTicks() - t0;
        if (dt < 1000 / FPS_MENU) SDL_Delay(1000 / FPS_MENU - dt);
    }
    /* EXIT GAME: muted through the exit save, as on the handheld */
    if (!exiting) port_audio_out_set_muted(0);
}

/* At launch with an auto save on the card: RESUME GAME / NEW GAME (NEW asks first: it deletes
 * the auto save). 1 = resume, 0 = new game. */
int handheld_menu_resume_prompt(void) {
    SDL_Surface *screen = sScreenPtr;
    if (!sInited || !screen) return 1;
    static const char *OPT[2] = { "RESUME GAME", "NEW GAME" };
    int opt = 0, confirm = 0, dirty = 1, result = -2;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { } /* anything held from the launch */
    while (result == -2) {
        if (dirty) {
            SDL_FillRect(sDraw, NULL, SDL_MapRGB(sDraw->format, 0, 0, 0));
            text_centered(sDraw, 120, 4, "GOLDENEYE 007", C_TEXT);
            for (int i = 0; i < 2; i++) text_centered(sDraw, 240 + i * 60, 3, OPT[i], i == opt ? C_SEL : C_DIM);
            if (confirm) text_centered(sDraw, 400, 2, "ARE YOU SURE? (A)", C_SEL);
            show(screen);
            dirty = 0;
        }
        if (handheld_power_pending()) { result = -1; break; } /* the OS is ending the game (SIGTERM) */
        if (!SDL_PollEvent(&ev)) { SDL_Delay(15); continue; }
        if (ev.type == SDL_QUIT) { port_request_exit(); result = 1; break; }
        if (ev.type == SDL_KEYUP) { pass_key_up(&ev); continue; }
        if (ev.type != SDL_KEYDOWN) continue;
        switch (ev.key.keysym.sym) {
        case SDLK_UP: case SDLK_DOWN:
            opt ^= 1; confirm = 0; dirty = 1;
            break;
        case SDLK_SPACE: case SDLK_RETURN:
            if (opt == 0) result = 1;
            else if (confirm) result = 0;
            else { confirm = 1; dirty = 1; }
            break;
        case SDLK_LCTRL:
            if (confirm) { confirm = 0; dirty = 1; }
            break;
        default:
            break;
        }
    }
    SDL_FillRect(screen, NULL, 0);
    SDL_Flip(screen);
    return result;
}

/* Headless check (<GAME>_MENU_TEST=<dir>): every menu state and the prompt to PPM. */
static void dump_ppm(SDL_Surface *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int y = 0; y < s->h; y++) {
        const uint32_t *row = (const uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
        for (int x = 0; x < s->w; x++) {
            const uint8_t rgb[3] = { (uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

void handheld_menu_test_dump(const char *dir) {
    if (!sInited) return;
    char path[512];
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)sBackup->pixels + y * sBackup->pitch);
        for (int x = 0; x < SCREEN_W; x++) row[x] = 0xFF1A2640u;
    }
    for (int i = 0; i < I_COUNT; i++) {
        compose_menu(i, 0);
        snprintf(path, sizeof(path), "%s/menu_%02d.ppm", dir, i);
        dump_ppm(sDraw, path);
    }
    compose_menu(I_EXIT, 1);
    snprintf(path, sizeof(path), "%s/menu_%02d_confirm.ppm", dir, I_EXIT);
    dump_ppm(sDraw, path);
    handheld_menu_notify("SAVED SLOT 1");
    miyoo_menu_overlay(sDraw);
    snprintf(path, sizeof(path), "%s/notice.ppm", dir);
    dump_ppm(sDraw, path);
}

#endif /* MIYOO */
