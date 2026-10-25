/* miyoo_remap.c: MENU > CONTROLS, the button remap shared by every Miyoo port (miyoo_remap.h).
 *
 * The map: sMap[p] is the button the Miyoo button p acts as (identity by default), over the ten
 * remappable buttons. miyoo_remap_key() applies it to a key as the port takes it, so the port's
 * own layout, modes and chords all follow. Kept in <data dir>/controls.cfg, one line per moved
 * button, "<pressed>=<acts as>" (names A B X Y L1 L2 R1 R2 START SELECT); a file that is not a
 * permutation of the ten is ignored. Back to the defaults: the file is removed.
 *
 * The screen lists the port's rows (miyoo_remap_rows: each N64 input and the Miyoo button that
 * gives it), drawn as button icons made here from rectangles and circles (no images), in the
 * menu's 8x8 font. A on a row, then the new button: done, or, when that button already gives
 * another row, a swap to confirm. MENU or a direction cancels the wait (MENU is never a choice,
 * so the menu can always be reached). RESET TO DEFAULT puts the port's layout back. */
#if defined(MIYOO)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL/SDL.h>
#include "miyoo_remap.h"
#include "miyoo_font.h"

/* The Miyoo's SDL 1.2 key syms (the kits' miyoo_panel.h MIYOO_KEY_*), by MB_*. */
static const int kSym[MB_REMAPPABLE + 1] = { 0, 32, 306, 304, 308, 101, 9, 116, 8, 13, 305 };
static const char *const kName[MB_COUNT] = { "-", "A", "B", "X", "Y", "L1", "L2", "R1", "R2", "START", "SELECT", "D-PAD" };
#define SYM_MENU 27
#define SYM_UP 273
#define SYM_DOWN 274
#define SYM_RIGHT 275
#define SYM_LEFT 276

static unsigned char sMap[MB_REMAPPABLE + 1]; /* pressed -> acts as */
static int sLoaded;

static int mb_of_sym(int sym) {
    for (int b = 1; b <= MB_REMAPPABLE; b++)
        if (kSym[b] == sym) return b;
    return 0;
}

static void map_reset(void) {
    for (int b = 0; b <= MB_REMAPPABLE; b++) sMap[b] = (unsigned char)b;
}

static int map_is_default(void) {
    for (int b = 1; b <= MB_REMAPPABLE; b++)
        if (sMap[b] != b) return 0;
    return 1;
}

/* the pressed button that acts as `as` */
static int map_inv(int as) {
    for (int b = 1; b <= MB_REMAPPABLE; b++)
        if (sMap[b] == as) return b;
    return as;
}

/* The game's data folder: the port's save folder (port_save_dir), else the kit's (miyoo_data_dir). */
extern const char *port_save_dir(void) __attribute__((weak));
extern const char *miyoo_data_dir(void) __attribute__((weak));

static int cfg_path(char *out, size_t n) {
    const char *d = port_save_dir ? port_save_dir() : miyoo_data_dir ? miyoo_data_dir() : NULL;
    if (!d || !d[0]) return 0;
    return snprintf(out, n, "%s/controls.cfg", d) < (int)n;
}

static int name_to_mb(const char *s) {
    for (int b = 1; b <= MB_REMAPPABLE; b++)
        if (strcmp(s, kName[b]) == 0) return b;
    return 0;
}

static void map_load(void) {
    char path[512], line[96];
    unsigned char m[MB_REMAPPABLE + 1];
    sLoaded = 1;
    map_reset();
    if (!cfg_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    for (int b = 0; b <= MB_REMAPPABLE; b++) m[b] = (unsigned char)b;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='), *e;
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        for (e = eq + 1; *e && *e != '\r' && *e != '\n' && *e != ' '; e++) { }
        *e = 0;
        const int p = name_to_mb(line), as = name_to_mb(eq + 1);
        if (p && as) m[p] = (unsigned char)as;
    }
    fclose(f);
    int seen[MB_REMAPPABLE + 1] = { 0 };
    for (int b = 1; b <= MB_REMAPPABLE; b++) {
        if (seen[m[b]]) { fprintf(stderr, "controls: %s is not a whole layout, the defaults are used\n", path); return; }
        seen[m[b]] = 1;
    }
    memcpy(sMap, m, sizeof(sMap));
    if (!map_is_default()) fprintf(stderr, "controls: %s\n", path);
}

static void map_save(void) {
    char path[512], tmp[520];
    if (!cfg_path(path, sizeof(path))) return;
    if (map_is_default()) { remove(path); return; }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# MENU > CONTROLS: <button pressed>=<the button it acts as>\n");
    for (int b = 1; b <= MB_REMAPPABLE; b++)
        if (sMap[b] != b) fprintf(f, "%s=%s\n", kName[b], kName[sMap[b]]);
    if (fflush(f) == 0 && fclose(f) == 0) rename(tmp, path);
    else remove(tmp);
}

int miyoo_remap_key(int sym) {
    if (!sLoaded) map_load();
    const int p = mb_of_sym(sym);
    return p ? kSym[sMap[p]] : sym;
}

/* -------- drawing (32-bit surfaces, 0x00RRGGBB) -------- */

static SDL_Surface *sS;

static inline void put(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= sS->w || y >= sS->h) return;
    ((uint32_t *)((uint8_t *)sS->pixels + y * sS->pitch))[x] = c & 0x00FFFFFFu;
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(i, j, c);
}

static void circle(int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r + r) put(cx + i, cy + j, c);
}

/* a rectangle with corners of radius r */
static void rrect(int x, int y, int w, int h, int r, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int dx = i < r ? r - i : i >= w - r ? i - (w - r - 1) : 0;
            const int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
            if (dx * dx + dy * dy <= r * r) put(x + i, y + j, c);
        }
}

/* a solid triangle pointing dir (0 up, 1 down, 2 left, 3 right), its box s x s centred on cx, cy */
static void tri(int cx, int cy, int s, int dir, uint32_t c) {
    for (int a = 0; a < s; a++) {          /* along the point */
        const int half = a / 2;            /* the width grows toward the base */
        for (int b = -half; b <= half; b++) {
            const int o = a - s / 2;
            switch (dir) {
            case 0: put(cx + b, cy + o, c); break;
            case 1: put(cx + b, cy - o, c); break;
            case 2: put(cx + o, cy + b, c); break;
            default: put(cx - o, cy + b, c); break;
            }
        }
    }
}

static void text(int x, int y, int k, const char *t, uint32_t c) {
    for (; *t; t++, x += 8 * k) {
        const unsigned ch = (unsigned char)*t;
        if (ch < 0x20 || ch > 0x7E) continue;
        const uint8_t *g = kMiyooFont[ch - 0x20];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (1u << gx)) rect(x + gx * k, y + gy * k, k, k, c);
    }
}
static int text_w(const char *t, int k) { return (int)strlen(t) * 8 * k; }
static void text_c(int cx, int cy, int k, const char *t, uint32_t c) { text(cx - text_w(t, k) / 2, cy - 4 * k, k, t, c); }

static const uint32_t C_TEXT = 0xECECEC, C_DIM = 0x808080, C_SEL = 0xFFC040, C_PANEL = 0x101418, C_EDGE = 0x505860,
                      C_BAR = 0x283038, C_WHITE = 0xFFFFFF, C_INK = 0x202020;

/* the Miyoo's buttons: A B X Y in the Super Famicom colours of the Mini's face buttons, the
 * shoulders and START / SELECT dark grey, the d-pad a cross. Returns the icon's width. */
static int icon_miyoo(int x, int cy, int b) {
    static const uint32_t face[4] = { 0xD83A3A, 0xE8C030, 0x3A6AD8, 0x2E9E50 }; /* A red, B yellow, X blue, Y green */
    const uint32_t grey = 0x4C535E, edge = 0x22262C;
    switch (b) {
    case MB_A: case MB_B: case MB_X: case MB_Y:
        circle(x + 13, cy, 13, edge);
        circle(x + 13, cy, 12, face[b - MB_A]);
        text_c(x + 14, cy + 1, 2, kName[b], b == MB_B ? C_INK : C_WHITE);
        return 27;
    case MB_L1: case MB_L2: case MB_R1: case MB_R2:
        rrect(x, cy - 12, 44, 24, 8, edge);
        rrect(x + 1, cy - 11, 42, 22, 7, grey);
        text_c(x + 22, cy + 1, 2, kName[b], C_WHITE);
        return 44;
    case MB_START: case MB_SELECT:
        rrect(x, cy - 10, 60, 20, 10, edge);
        rrect(x + 1, cy - 9, 58, 18, 9, grey);
        text_c(x + 30, cy + 1, 1, kName[b], C_WHITE);
        return 60;
    case MB_DPAD:
        rect(x + 9, cy - 13, 9, 27, edge);
        rect(x, cy - 4, 27, 9, edge);
        rect(x + 10, cy - 12, 7, 25, grey);
        rect(x + 1, cy - 3, 25, 7, grey);
        circle(x + 13, cy, 2, 0x6A727E);
        return 27;
    default:
        text(x, cy - 8, 2, "-", C_DIM);
        return 16;
    }
}

/* the N64's inputs in the N64 controller's colours: A blue, B green, C yellow, START red, Z / L / R grey */
static void icon_n64(int cx, int cy, int n) {
    const uint32_t grey = 0x9098A0, dark = 0x404850;
    switch (n) {
    case MR_A: circle(cx, cy, 13, 0x1A3A9A); circle(cx, cy, 12, 0x2E5CE0); text_c(cx + 1, cy + 1, 2, "A", C_WHITE); break;
    case MR_B: circle(cx, cy, 13, 0x16602E); circle(cx, cy, 12, 0x2EA650); text_c(cx + 1, cy + 1, 2, "B", C_WHITE); break;
    case MR_START: circle(cx, cy, 12, 0x801818); circle(cx, cy, 11, 0xD82828); text_c(cx + 1, cy + 1, 2, "S", C_WHITE); break;
    case MR_CUP: case MR_CDOWN: case MR_CLEFT: case MR_CRIGHT:
        circle(cx, cy, 12, 0x9A7A10);
        circle(cx, cy, 11, 0xF0C828);
        tri(cx, cy, 11, n - MR_CUP, 0x3A2A00);
        break;
    case MR_Z: rrect(cx - 11, cy - 13, 22, 26, 5, dark); rrect(cx - 10, cy - 12, 20, 24, 4, grey); text_c(cx + 1, cy + 1, 2, "Z", C_INK); break;
    case MR_L: case MR_R:
        rrect(cx - 18, cy - 11, 36, 22, 8, dark);
        rrect(cx - 17, cy - 10, 34, 20, 7, grey);
        text_c(cx + 1, cy + 1, 2, n == MR_L ? "L" : "R", C_INK);
        break;
    case MR_STICK: circle(cx, cy, 13, dark); circle(cx, cy, 12, grey); circle(cx, cy, 6, 0x606870); circle(cx - 1, cy - 1, 3, 0xB0B8C0); break;
    case MR_DPAD:
        rect(cx - 5, cy - 13, 11, 27, dark); rect(cx - 13, cy - 5, 27, 11, dark);
        rect(cx - 4, cy - 12, 9, 25, grey); rect(cx - 12, cy - 4, 25, 9, grey);
        break;
    default: break;
    }
}

static const char *const kN64Name[MR_TEXT] = { "A", "B", "Z", "START", "L", "R", "C-UP", "C-DOWN", "C-LEFT", "C-RIGHT", "STICK", "D-PAD" };

static int row_fixed(const struct miyoo_remap_row *r) {
    return r->btn < MB_A || r->btn > MB_SELECT || r->mod != 0;
}

static const char *row_name(const struct miyoo_remap_row *r) {
    return r->n64 < MR_TEXT ? kN64Name[r->n64] : (r->what ? r->what : "?");
}

/* the pressed buttons that give the row now (a logical button b is given by map_inv(b)) */
static int shown(int logical) {
    return logical >= MB_A && logical <= MB_SELECT ? map_inv(logical) : logical;
}

/* the first row (other than skip) that `logical` gives, or -1 */
static int row_using(int logical, int skip) {
    for (int i = 0; i < miyoo_remap_nrows; i++) {
        const struct miyoo_remap_row *r = &miyoo_remap_rows[i];
        if (i != skip && (r->btn == logical || r->alt == logical || r->mod == logical)) return i;
    }
    return -1;
}

enum { M_BROWSE, M_LISTEN, M_SWAP, M_RESET };
#define VIS 11   /* rows on the screen at once */
#define ROW_H 30

static void compose(SDL_Surface *draw, SDL_Surface *backdrop, int cur, int top, int mode, int pend, int blink) {
    SDL_BlitSurface(backdrop, NULL, draw, NULL);
    sS = draw;
    if (SDL_MUSTLOCK(draw)) SDL_LockSurface(draw);
    const int n = miyoo_remap_nrows + 1; /* + RESET TO DEFAULT */
    const int pw = 600, ph = 448, px = (draw->w - pw) / 2, py = (draw->h - ph) / 2;
    rect(px - 2, py - 2, pw + 4, ph + 4, C_EDGE);
    rect(px, py, pw, ph, C_PANEL);
    text_c(px + pw / 2, py + 26, 3, "CONTROLS", C_TEXT);
    rect(px + 30, py + 48, pw - 60, 2, C_EDGE);
    text(px + 30, py + 54, 1, "N64", C_DIM);
    text(px + pw - 30 - text_w("MIYOO", 1), py + 54, 1, "MIYOO", C_DIM);
    for (int v = 0; v < VIS && top + v < n; v++) {
        const int i = top + v, cy = py + 80 + v * ROW_H;
        const int sel = i == cur;
        if (sel) rect(px + 14, cy - 14, pw - 28, 28, C_BAR);
        if (i == miyoo_remap_nrows) {
            text_c(px + pw / 2, cy + 1, 2, "RESET TO DEFAULT", sel ? C_SEL : C_TEXT);
            continue;
        }
        const struct miyoo_remap_row *r = &miyoo_remap_rows[i];
        const int fixed = row_fixed(r);
        if (r->n64 < MR_TEXT) icon_n64(px + 44, cy, r->n64);
        text(px + 72, cy - 7, 2, row_name(r), sel ? C_SEL : fixed ? C_DIM : C_TEXT);
        if (r->what && r->n64 < MR_TEXT) text(px + 72 + 8 * 2 * 8, cy - 3, 1, r->what, C_DIM);
        int x = px + 396;
        if (sel && mode == M_LISTEN) {
            if (blink) text(x, cy - 7, 2, "PRESS...", C_SEL);
            continue;
        }
        if (r->mod) {
            x += icon_miyoo(x, cy, shown(r->mod)) + 6;
            text(x, cy - 7, 2, "+", C_TEXT);
            x += 22;
        }
        x += icon_miyoo(x, cy, shown(r->btn));
        if (r->alt) {
            text(x + 6, cy - 7, 2, "/", C_DIM);
            icon_miyoo(x + 28, cy, shown(r->alt));
        }
    }
    if (top > 0) tri(px + pw - 26, py + 66, 9, 0, C_DIM);
    if (top + VIS < n) tri(px + pw - 26, py + 80 + VIS * ROW_H - 16, 9, 1, C_DIM);
    char buf[64];
    const int fy = py + ph - 24;
    if (mode == M_LISTEN) {
        text_c(px + pw / 2, fy, 2, "PRESS THE NEW BUTTON  MENU: CANCEL", C_SEL);
    } else if (mode == M_SWAP) {
        snprintf(buf, sizeof(buf), "SWAP WITH %.10s? A: YES  B: NO", row_name(&miyoo_remap_rows[pend >> 8]));
        text_c(px + pw / 2, fy, 2, buf, C_SEL);
    } else if (mode == M_RESET) {
        text_c(px + pw / 2, fy, 2, "RESET ALL? A: YES  B: NO", C_SEL);
    } else if (cur == miyoo_remap_nrows) {
        text_c(px + pw / 2, fy, 2, "A: RESET   B: BACK", C_DIM);
    } else if (row_fixed(&miyoo_remap_rows[cur])) {
        text_c(px + pw / 2, fy, 2, "FIXED   B: BACK", C_DIM);
    } else {
        text_c(px + pw / 2, fy, 2, "A: CHANGE   B: BACK", C_DIM);
    }
    if (SDL_MUSTLOCK(draw)) SDL_UnlockSurface(draw);
}

/* the row gets the pressed button p: p and the button that gave the row trade places */
static void assign(int row, int p) {
    const int as = miyoo_remap_rows[row].btn, q = map_inv(as), other = sMap[p];
    sMap[p] = (unsigned char)as;
    sMap[q] = (unsigned char)other;
    map_save();
}

static void release_all(void (*key)(int, int)) {
    if (!key) return;
    for (int b = MB_A; b <= MB_SELECT; b++) key(kSym[b], 0);
}

void miyoo_remap_screen(SDL_Surface *draw, SDL_Surface *backdrop, void (*show)(void), void (*key)(int, int),
                        int (*should_close)(void)) {
    if (!draw || !backdrop || draw->format->BitsPerPixel != 32) return;
    if (!sLoaded) map_load();
    const int n = miyoo_remap_nrows + 1;
    int cur = 0, top = 0, mode = M_BROWSE, pend = 0, running = 1, dirty = 1, blink = 1;
    uint32_t blinkAt = SDL_GetTicks();
    release_all(key); /* nothing held into the old layout */
    while (running) {
        const uint32_t t0 = SDL_GetTicks();
        if (should_close()) break;
        SDL_Event ev;
        while (running && SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { SDL_PushEvent(&ev); running = 0; break; }
            if (ev.type == SDL_KEYUP) { if (key) key(ev.key.keysym.sym, 0); continue; }
            if (ev.type != SDL_KEYDOWN) continue;
            const int sym = ev.key.keysym.sym;
            dirty = 1;
            if (mode == M_LISTEN) {
                const int p = mb_of_sym(sym);
                mode = M_BROWSE;
                if (!p) break; /* MENU, a direction: cancel */
                const int as = miyoo_remap_rows[cur].btn;
                if (map_inv(as) == p) break;
                const int other = row_using(sMap[p], cur);
                if (other >= 0) { mode = M_SWAP; pend = other << 8 | p; }
                else assign(cur, p);
                break;
            }
            if (mode == M_SWAP || mode == M_RESET) {
                if (sym == kSym[MB_A]) {
                    if (mode == M_SWAP) assign(cur, pend & 0xFF);
                    else { map_reset(); map_save(); }
                }
                if (sym == kSym[MB_A] || sym == kSym[MB_B] || sym == SYM_MENU) mode = M_BROWSE;
                break;
            }
            switch (sym) {
            case SYM_MENU: case 306: /* MENU, B */
                running = 0;
                break;
            case SYM_UP:
                cur = (cur + n - 1) % n;
                break;
            case SYM_DOWN:
                cur = (cur + 1) % n;
                break;
            case 32: case 13: /* A, START */
                if (cur == miyoo_remap_nrows) mode = M_RESET;
                else if (!row_fixed(&miyoo_remap_rows[cur])) { mode = M_LISTEN; blink = 1; blinkAt = SDL_GetTicks(); }
                break;
            default:
                break;
            }
            if (cur < top) top = cur;
            if (cur >= top + VIS) top = cur - VIS + 1;
        }
        if (mode == M_LISTEN && SDL_GetTicks() - blinkAt >= 400) { blink ^= 1; blinkAt = SDL_GetTicks(); dirty = 1; }
        if (dirty && running) { compose(draw, backdrop, cur, top, mode, pend, blink); show(); dirty = 0; }
        const uint32_t dt = SDL_GetTicks() - t0;
        if (dt < 33) SDL_Delay(33 - dt);
    }
    release_all(key); /* nothing held into the new one */
}

/* <GAME>_MENU_TEST: the screen as it opens, a row waiting for its button, a swap question, the
 * last rows (RESET TO DEFAULT), with the defaults (the map is not touched). */
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

void miyoo_remap_test_dump(SDL_Surface *draw, SDL_Surface *backdrop, const char *dir) {
    char path[512];
    unsigned char keep[sizeof(sMap)];
    if (!draw || !backdrop || draw->format->BitsPerPixel != 32) return;
    if (!sLoaded) map_load();
    memcpy(keep, sMap, sizeof(keep));
    map_reset();
    const int n = miyoo_remap_nrows + 1, last = n - 1, ltop = n > VIS ? n - VIS : 0;
    int first = 0;
    while (first < miyoo_remap_nrows && row_fixed(&miyoo_remap_rows[first])) first++;
    compose(draw, backdrop, 0, 0, M_BROWSE, 0, 1);
    snprintf(path, sizeof(path), "%s/controls_0_open.ppm", dir); dump_ppm(draw, path);
    compose(draw, backdrop, first, 0, M_LISTEN, 0, 1);
    snprintf(path, sizeof(path), "%s/controls_1_listen.ppm", dir); dump_ppm(draw, path);
    {
        const int other = row_using(MB_B, first);
        compose(draw, backdrop, first, 0, M_SWAP, (other >= 0 ? other : 0) << 8 | MB_B, 1);
        snprintf(path, sizeof(path), "%s/controls_2_swap.ppm", dir); dump_ppm(draw, path);
    }
    compose(draw, backdrop, last, ltop, M_BROWSE, 0, 1);
    snprintf(path, sizeof(path), "%s/controls_3_reset.ppm", dir); dump_ppm(draw, path);
    /* a moved layout: the first changeable row on Y (swapped), as the screen then shows it */
    if (first < miyoo_remap_nrows) {
        const int as = miyoo_remap_rows[first].btn, q = map_inv(as), other = sMap[MB_Y];
        sMap[MB_Y] = (unsigned char)as; sMap[q] = (unsigned char)other;
        compose(draw, backdrop, first, 0, M_BROWSE, 0, 1);
        snprintf(path, sizeof(path), "%s/controls_4_moved.ppm", dir); dump_ppm(draw, path);
    }
    memcpy(sMap, keep, sizeof(sMap));
}

#endif /* MIYOO */
