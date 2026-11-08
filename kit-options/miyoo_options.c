/* miyoo_options.c: MENU > OPTIONS, the game's settings as a list (miyoo_options.h; docs/OPTIONS.md).
 *
 * The rows are the game's table (miyoo_options[], its hh_options.c). A row's value at the page's first opening
 * is what the game runs with: its variable (the game's start set it from debug.cfg) or the row's default. Left /
 * right (or A) change it: an applies-now row sets its variable and calls its apply(); a needs-restart row is
 * only marked (a * beside the value, "RESTART TO APPLY" below). Leaving the page writes the keys changed on
 * this visit to debug.cfg: those lines rewritten, a missing key appended, everything else kept, through
 * debug.cfg.new renamed over the file. Nothing here touches saves or save states, and the page's state lies
 * outside save states (SAVESTATE_EXCLUDE).
 *
 * The file part (miyoo_options_load / _rewrite / _write_file) builds without SDL for the kit's tests
 * (MIYOO_OPTIONS_TEST, tests/options_test.c). */
#if defined(MIYOO) || defined(MIYOO_OPTIONS_TEST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "miyoo_options.h"
#if !defined(MIYOO_OPTIONS_TEST)
#include "hh_game.h"
#include "savestate.h"
#endif
#ifndef HH_ENV
#define HH_ENV(n) "TEST_" n
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif

#define MAX_ROWS 64

/* -------- debug.cfg -------- */

/* the variable a row's setting is read from: its env, else the game's prefix + KEY */
static void row_env(const struct miyoo_option *o, char *buf, size_t n) {
    if (o->env) { snprintf(buf, n, "%s", o->env); return; }
    size_t i = (size_t)snprintf(buf, n, "%s", HH_ENV(""));
    for (const char *k = o->key; *k && i + 1 < n; k++) buf[i++] = (*k >= 'a' && *k <= 'z') ? (char)(*k - 'a' + 'A') : *k;
    buf[i < n ? i : n - 1] = 0;
}

/* debug.cfg's path, 0 if there is no folder */
extern const char *port_save_dir(void) __attribute__((weak));
extern const char *miyoo_data_dir(void) __attribute__((weak));
static int cfg_path(char *out, size_t n) {
    const char *d = miyoo_options_dir ? miyoo_options_dir() : port_save_dir ? port_save_dir() : miyoo_data_dir ? miyoo_data_dir() : NULL;
    if (!d || !d[0]) return 0;
    return snprintf(out, n, "%s/debug.cfg", d) < (int)n;
}

/* the whole file, NUL-terminated (an empty string when there is none); NULL: out of memory or unreadable */
static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    size_t len = 0, cap = 4096;
    char *s = malloc(cap);
    if (!s) { if (f) fclose(f); return NULL; }
    if (f) {
        size_t got;
        while ((got = fread(s + len, 1, cap - len - 1, f)) > 0) {
            len += got;
            if (cap - len - 1 == 0) {
                char *t = realloc(s, cap * 2);
                if (!t) { free(s); fclose(f); return NULL; }
                s = t; cap *= 2;
            }
        }
        const int err = ferror(f);
        fclose(f);
        if (err) { free(s); return NULL; }
    }
    s[len] = 0;
    return s;
}

/* a line's key as the games read it: everything before the first '=' (no '=': none) */
static int line_has_key(const char *line, size_t len, const char *key) {
    const size_t k = strlen(key);
    return k > 0 && len > k && memcmp(line, key, k) == 0 && line[k] == '=';
}

struct buf { char *s; size_t len, cap; int oom; };
static void put(struct buf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->len + n + 1) c *= 2;
        char *t = realloc(b->s, c);
        if (!t) { b->oom = 1; return; }
        b->s = t; b->cap = c;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
static void puts_(struct buf *b, const char *s) { put(b, s, strlen(s)); }

char *miyoo_options_rewrite(const char *text, const char *const *keys, const char *const *values, int n) {
    struct buf b = { 0 };
    char found[MAX_ROWS] = { 0 };
    if (n > MAX_ROWS) n = MAX_ROWS;
    put(&b, "", 0);
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        int hit = -1;
        for (int i = 0; i < n && hit < 0; i++)
            if (line_has_key(p, len, keys[i])) hit = i;
        if (hit < 0) {
            put(&b, p, len);
        } else { /* key=value, the line's own ending kept (LF, CR LF or none at the end of the file) */
            const char *eol = len >= 2 && p[len - 1] == '\n' && p[len - 2] == '\r' ? "\r\n" : nl ? "\n" : "";
            puts_(&b, keys[hit]); puts_(&b, "="); puts_(&b, values[hit]); puts_(&b, eol);
            found[hit] = 1;
        }
        p += len;
    }
    for (int i = 0; i < n; i++) {
        if (found[i]) continue;
        if (b.len > 0 && b.s[b.len - 1] != '\n') puts_(&b, "\n");
        puts_(&b, keys[i]); puts_(&b, "="); puts_(&b, values[i]); puts_(&b, "\n");
    }
    if (b.oom) { free(b.s); return NULL; }
    return b.s;
}

int miyoo_options_write_file(const char *path, const char *const *keys, const char *const *values, int n) {
    char tmp[600];
    if (n <= 0) return 0;
    char *old = read_all(path);
    if (!old) return -1;
    char *nu = miyoo_options_rewrite(old, keys, values, n);
    if (!nu) { free(old); return -1; }
    int rc = 0;
    if (strcmp(old, nu) != 0) {
        rc = -1;
        if (snprintf(tmp, sizeof(tmp), "%s.new", path) < (int)sizeof(tmp)) {
            FILE *f = fopen(tmp, "wb");
            if (f) {
                const size_t len = strlen(nu);
                int ok = fwrite(nu, 1, len, f) == len && fflush(f) == 0;
                ok = fsync(fileno(f)) == 0 && ok;
                ok = fclose(f) == 0 && ok;
                if (ok && rename(tmp, path) == 0) rc = 0;
                else remove(tmp);
            }
        }
    }
    free(old);
    free(nu);
    return rc;
}

int miyoo_options_load(void) {
    char path[560], env[64];
    if (!cfg_path(path, sizeof(path))) return 0;
    char *s = read_all(path);
    if (!s) return 0;
    int set = 0;
    for (char *p = s; *p;) {
        char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        for (int i = 0; i < miyoo_options_count; i++) {
            const struct miyoo_option *o = &miyoo_options[i];
            if (!o->key || !line_has_key(p, len, o->key)) continue;
            char val[64];
            const char *v = p + strlen(o->key) + 1;
            size_t vl = strcspn(v, "\r\n");
            if (vl >= sizeof(val)) vl = sizeof(val) - 1;
            memcpy(val, v, vl);
            val[vl] = 0;
            row_env(o, env, sizeof(env));
            setenv(env, val, 1);
            set++;
        }
        p += len;
    }
    free(s);
    return set;
}

#if !defined(MIYOO_OPTIONS_TEST)
/* -------- the page -------- */
#include <SDL/SDL.h>

extern void miyoo_text(SDL_Surface *s, int x, int y, int k, const char *t, uint32_t c); /* miyoo_menu.c */

static const uint32_t C_TEXT = 0xFFECECECu, C_DIM = 0xFF808080u, C_SEL = 0xFFFFC040u, C_PANEL = 0xFF101418u,
                      C_EDGE = 0xFF505860u, C_BAR = 0xFF283038u;

/* each row's choice: an index into its values, or -1 for a value the list doesn't have (sRaw, as found) */
static signed char sCur[MAX_ROWS] SAVESTATE_EXCLUDE;
static signed char sBoot[MAX_ROWS] SAVESTATE_EXCLUDE; /* what the game started with (a restart row runs with it) */
static signed char sOpen[MAX_ROWS] SAVESTATE_EXCLUDE; /* at this visit's start: what leaving compares against */
static char sRaw[MAX_ROWS][16] SAVESTATE_EXCLUDE;
static int sRows SAVESTATE_EXCLUDE, sReady SAVESTATE_EXCLUDE;

static int is_row(int i) { return i >= 0 && i < sRows && miyoo_options[i].key != NULL; }

static int find_value(const struct miyoo_option *o, const char *v) {
    for (int j = 0; j < o->nvalues; j++)
        if (strcmp(o->values[j].value, v) == 0) return j;
    char *e1, *e2; /* 1.0 and 1, 0500 and 500 */
    const double a = strtod(v, &e1);
    for (int j = 0; j < o->nvalues; j++) {
        const double b = strtod(o->values[j].value, &e2);
        if (e1 != v && !*e1 && e2 != o->values[j].value && !*e2 && a == b) return j;
    }
    return -1;
}

static void state_init(void) {
    char env[64];
    if (sReady) return;
    sReady = 1;
    sRows = miyoo_options_count < MAX_ROWS ? miyoo_options_count : MAX_ROWS;
    for (int i = 0; i < sRows; i++) {
        const struct miyoo_option *o = &miyoo_options[i];
        sCur[i] = sBoot[i] = 0;
        if (!o->key) continue;
        row_env(o, env, sizeof(env));
        const char *v = getenv(env);
        if (!v || !v[0]) v = o->def ? o->def : "";
        int j = find_value(o, v);
        if (j < 0 && o->def) {
            snprintf(sRaw[i], sizeof(sRaw[i]), "%s", v);
            if (!sRaw[i][0]) j = find_value(o, o->def);
        }
        sCur[i] = sBoot[i] = (signed char)j;
    }
}

static const char *cur_name(int i) {
    return sCur[i] >= 0 ? miyoo_options[i].values[sCur[i]].name : sRaw[i];
}
static const char *cur_value(int i) {
    return sCur[i] >= 0 ? miyoo_options[i].values[sCur[i]].value : sRaw[i];
}

static void change(int i, int d) {
    const struct miyoo_option *o = &miyoo_options[i];
    char env[64];
    if (!is_row(i) || o->nvalues <= 0) return;
    sCur[i] = (signed char)(sCur[i] < 0 ? (d > 0 ? 0 : o->nvalues - 1) : (sCur[i] + d + o->nvalues) % o->nvalues);
    if (o->when == MIYOO_OPT_NOW) {
        row_env(o, env, sizeof(env));
        setenv(env, cur_value(i), 1);
        if (o->apply) o->apply(cur_value(i));
    }
}

/* the keys changed on this visit, to debug.cfg */
static void save_changes(void) {
    const char *keys[MAX_ROWS], *vals[MAX_ROWS];
    char path[560];
    int n = 0;
    for (int i = 0; i < sRows; i++)
        if (is_row(i) && sCur[i] != sOpen[i] && sCur[i] >= 0) { keys[n] = miyoo_options[i].key; vals[n] = cur_value(i); n++; }
    if (n == 0) return;
    if (!cfg_path(path, sizeof(path))) return;
    const int rc = miyoo_options_write_file(path, keys, vals, n);
    fprintf(stderr, "options: %d key%s to %s%s\n", n, n == 1 ? "" : "s", path, rc == 0 ? "" : " FAILED");
    for (int i = 0; i < n; i++) fprintf(stderr, "options: %s=%s\n", keys[i], vals[i]);
}

static void fill(SDL_Surface *s, int x, int y, int w, int h, uint32_t c) {
    SDL_Rect r = { (Sint16)x, (Sint16)y, (Uint16)w, (Uint16)h };
    SDL_FillRect(s, &r, SDL_MapRGB(s->format, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
}
static int tw(const char *t, int k) { return (int)strlen(t) * 8 * k; }
static void text_c(SDL_Surface *s, int cx, int cy, int k, const char *t, uint32_t c) { miyoo_text(s, cx - tw(t, k) / 2, cy - 4 * k, k, t, c); }

/* the help line, centred at cy: at 2x in up to two lines, else at 1x */
static void help_text(SDL_Surface *s, int cx, int cy, int width, const char *t) {
    char l1[96], l2[96];
    for (int k = 2; k >= 1; k--) {
        const int per = width / (8 * k);
        const int n = (int)strlen(t);
        if (n <= per) { snprintf(l1, sizeof(l1), "%s", t); text_c(s, cx, cy, k, l1, C_TEXT); return; }
        int cut = per; /* break at the last space that fits */
        while (cut > 0 && t[cut] != ' ') cut--;
        if (cut <= 0 || n - cut - 1 > per || per >= (int)sizeof(l1)) continue;
        snprintf(l1, sizeof(l1), "%.*s", cut, t);
        snprintf(l2, sizeof(l2), "%s", t + cut + 1);
        text_c(s, cx, cy - 6 * k, k, l1, C_TEXT);
        text_c(s, cx, cy + 6 * k, k, l2, C_TEXT);
        return;
    }
    snprintf(l1, sizeof(l1), "%.*s", width / 8, t);
    text_c(s, cx, cy, 1, l1, C_TEXT);
}

#define VIS 9
#define ROW_H 30
#define VAL_W 240

static void compose(SDL_Surface *draw, SDL_Surface *backdrop, int cur, int top) {
    SDL_BlitSurface(backdrop, NULL, draw, NULL);
    const int pw = 600, ph = 448, px = (draw->w - pw) / 2, py = (draw->h - ph) / 2;
    fill(draw, px - 2, py - 2, pw + 4, ph + 4, C_EDGE);
    fill(draw, px, py, pw, ph, C_PANEL);
    text_c(draw, px + pw / 2, py + 26, 3, "OPTIONS", C_TEXT);
    fill(draw, px + 30, py + 48, pw - 60, 2, C_EDGE);
    const int vx = px + pw - 40 - VAL_W / 2;
    int pending = 0;
    for (int i = 0; i < sRows; i++)
        if (is_row(i) && miyoo_options[i].when != MIYOO_OPT_NOW && sCur[i] != sBoot[i]) pending = 1;
    for (int v = 0; v < VIS && top + v < sRows; v++) {
        const int i = top + v, cy = py + 80 + v * ROW_H;
        const struct miyoo_option *o = &miyoo_options[i];
        if (!o->key) { /* a section */
            miyoo_text(draw, px + 30, cy - 8, 2, o->label, C_DIM);
            const int x0 = px + 30 + tw(o->label, 2) + 12;
            if (x0 < px + pw - 30) fill(draw, x0, cy - 1, px + pw - 30 - x0, 2, C_EDGE);
            continue;
        }
        const int sel = i == cur;
        if (sel) fill(draw, px + 14, cy - 14, pw - 28, 28, C_BAR);
        miyoo_text(draw, px + 30, cy - 7, 2, o->label, sel ? C_SEL : C_TEXT);
        char buf[48];
        snprintf(buf, sizeof(buf), sel ? "< %s >" : "%s", cur_name(i));
        text_c(draw, vx, cy + 1, 2, buf, sel ? C_SEL : C_TEXT);
        if (o->when != MIYOO_OPT_NOW && sCur[i] != sBoot[i]) miyoo_text(draw, px + pw - 34, cy - 7, 2, "*", C_SEL);
    }
    if (top > 0) text_c(draw, px + pw - 22, py + 62, 1, "^", C_DIM);
    if (top + VIS < sRows) text_c(draw, px + pw - 22, py + 80 + VIS * ROW_H - 18, 1, "v", C_DIM);
    fill(draw, px + 30, py + ph - 92, pw - 60, 2, C_EDGE);
    if (is_row(cur) && miyoo_options[cur].help) help_text(draw, px + pw / 2, py + ph - 62, pw - 40, miyoo_options[cur].help);
    if (is_row(cur) && miyoo_options[cur].when != MIYOO_OPT_NOW)
        text_c(draw, px + pw / 2, py + ph - 22, 2, pending ? "* RESTART TO APPLY   B: BACK" : "APPLIES AFTER A RESTART   B: BACK",
               sCur[cur] != sBoot[cur] ? C_SEL : C_DIM);
    else
        text_c(draw, px + pw / 2, py + ph - 22, 2, pending ? "* RESTART TO APPLY   B: BACK" : "LEFT/RIGHT: CHANGE   B: BACK", C_DIM);
}

/* the next selectable row from i going d (wrapping); i itself if there is none */
static int step(int i, int d) {
    for (int k = 1; k <= sRows; k++) {
        const int j = ((i + d * k) % sRows + sRows) % sRows;
        if (is_row(j)) return j;
    }
    return i;
}

/* the window's top for the selection: cur visible, its section's header too when it fits */
static int scroll(int cur, int top) {
    if (cur < top) top = cur;
    if (cur >= top + VIS) top = cur - VIS + 1;
    if (cur > 0 && !is_row(cur - 1) && top == cur) top = cur - 1;
    return top < 0 ? 0 : top;
}

static void release_dirs(void (*key)(int, int)) {
    static const int dirs[] = { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_SPACE, SDLK_LCTRL, SDLK_RETURN };
    if (!key) return;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) key(dirs[i], 0);
}

void miyoo_options_screen(SDL_Surface *draw, SDL_Surface *backdrop, void (*show)(void), void (*key)(int, int),
                          int (*should_close)(void)) {
    if (!draw || !backdrop || draw->format->BitsPerPixel != 32) return;
    state_init();
    if (sRows <= 0) return;
    memcpy(sOpen, sCur, sizeof(sOpen));
    int cur = step(sRows - 1, 1), top = 0, running = 1, dirty = 1;
    top = scroll(cur, top);
    release_dirs(key);
    while (running) {
        const uint32_t t0 = SDL_GetTicks();
        if (should_close && should_close()) break;
        SDL_Event ev;
        while (running && SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { SDL_PushEvent(&ev); running = 0; break; }
            if (ev.type == SDL_KEYUP) { if (key) key(ev.key.keysym.sym, 0); continue; }
            if (ev.type != SDL_KEYDOWN) continue;
            switch (ev.key.keysym.sym) {
            case SDLK_ESCAPE: case SDLK_LCTRL: /* MENU, B */
                running = 0;
                break;
            case SDLK_UP: cur = step(cur, -1); break;
            case SDLK_DOWN: cur = step(cur, 1); break;
            case SDLK_LEFT: change(cur, -1); break;
            case SDLK_RIGHT: case SDLK_SPACE: case SDLK_RETURN: change(cur, 1); break; /* A, START: the next value */
            default: continue;
            }
            top = scroll(cur, top);
            /* one picture a key (a scripted test pushes several at once and looks at each) */
            if (running) { compose(draw, backdrop, cur, top); show(); }
            dirty = 0;
        }
        if (dirty && running) { compose(draw, backdrop, cur, top); show(); dirty = 0; }
        const uint32_t dt = SDL_GetTicks() - t0;
        if (dt < 33) SDL_Delay(33 - dt);
    }
    save_changes();
    release_dirs(key);
}

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

/* <GAME>_MENU_TEST: as the page opens, a needs-restart row changed (its mark and note), the last rows */
void miyoo_options_test_dump(SDL_Surface *draw, SDL_Surface *backdrop, const char *dir) {
    char path[512];
    signed char keep[MAX_ROWS];
    if (!draw || !backdrop || draw->format->BitsPerPixel != 32) return;
    state_init();
    if (sRows <= 0) return;
    memcpy(keep, sCur, sizeof(keep));
    const int first = step(sRows - 1, 1);
    compose(draw, backdrop, first, scroll(first, 0));
    snprintf(path, sizeof(path), "%s/options_0_open.ppm", dir); dump_ppm(draw, path);
    int r = -1;
    for (int i = 0; i < sRows && r < 0; i++)
        if (is_row(i) && miyoo_options[i].when != MIYOO_OPT_NOW && miyoo_options[i].nvalues > 1) r = i;
    if (r >= 0) {
        sCur[r] = (signed char)((sBoot[r] < 0 ? 0 : sBoot[r] + 1) % miyoo_options[r].nvalues);
        compose(draw, backdrop, r, scroll(r, 0));
        snprintf(path, sizeof(path), "%s/options_1_restart.ppm", dir); dump_ppm(draw, path);
        sCur[r] = keep[r];
    }
    const int last = step(0, -1);
    compose(draw, backdrop, last, scroll(last, last >= VIS ? last - VIS + 1 : 0));
    snprintf(path, sizeof(path), "%s/options_2_last.ppm", dir); dump_ppm(draw, path);
    memcpy(sCur, keep, sizeof(sCur));
}
#endif /* !MIYOO_OPTIONS_TEST */

#endif /* MIYOO || MIYOO_OPTIONS_TEST */
