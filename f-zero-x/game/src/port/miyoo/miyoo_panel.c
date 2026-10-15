/* miyoo_panel: see miyoo_panel.h. Merged from the three Miyoo ports that ran on the device
 * (2026-09-30 .. 10-02): GoldenEye's present (goldeneye-007 48fb705: 32 bpp, 2x NEON on the
 * second core, turned), MK64's panel sizing and centring (mario-kart-64 miyoo_video.c, 41c3d82: the
 * MK64_D32_CHECK scalar comparison, 0 pixels off under qemu, turned and not) and OoT's paths. */
#if defined(MIYOO)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <SDL/SDL.h>
#include "hh_game.h"
#include "savestate.h"
#include "handheld_menu.h"
#include "miyoo_panel.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
extern void port_log(const char *fmt, ...); /* F-Zero X: the console log, not stderr (naming stderr copies it into the executable: build gate 2b) */
#endif

static SDL_Surface *sPanel SAVESTATE_EXCLUDE;
static int sRot SAVESTATE_EXCLUDE = -1;
static int sCheck SAVESTATE_EXCLUDE = -1, sChecked SAVESTATE_EXCLUDE;
static long sCheckBad SAVESTATE_EXCLUDE;

int miyoo_panel_rotated(void) {
    if (sRot < 0) { const char *e = getenv(HH_ENV("ROTATE")); sRot = !(e && e[0] == '0'); }
    return sRot;
}

static void panel_size(int *w, int *h) {
    const char *e = getenv(HH_ENV("MIYOO_MODE"));
    int ew = 0, eh = 0;
    *w = 640; *h = 480;
    if (e && sscanf(e, "%dx%d", &ew, &eh) == 2 && ew >= 320 && eh >= 240) { *w = ew; *h = eh; return; }
    const int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) return;
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.xres >= 320 && v.yres >= 240 && v.xres <= 2048 && v.yres <= 2048) {
        *w = (int)v.xres; *h = (int)v.yres;
    }
    close(fd);
}

static void clear_panel(void) {
    for (int i = 0; i < 3; i++) { /* every page of a double/triple-buffered surface: borders stay black */
        SDL_FillRect(sPanel, NULL, SDL_MapRGB(sPanel->format, 0, 0, 0) | sPanel->format->Amask);
        SDL_Flip(sPanel);
    }
}

SDL_Surface *miyoo_panel_open(void) {
    int w, h;
    if (sPanel) return sPanel;
    panel_size(&w, &h);
    sPanel = SDL_SetVideoMode(w, h, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sPanel && (w != 640 || h != 480)) sPanel = SDL_SetVideoMode(640, 480, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sPanel) sPanel = SDL_SetVideoMode(640, 480, 16, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sPanel) { port_log("miyoo: SDL_SetVideoMode: %s\n", SDL_GetError()); return NULL; }
    port_log("miyoo: panel %dx%d (asked %dx%d), %d bpp (R%d G%d B%d), pitch %d%s\n", sPanel->w, sPanel->h, w, h,
            sPanel->format->BitsPerPixel, sPanel->format->Rshift, sPanel->format->Gshift, sPanel->format->Bshift,
            sPanel->pitch, miyoo_panel_rotated() ? ", turned 180" : "");
    clear_panel();
    return sPanel;
}

SDL_Surface *miyoo_panel_surface(void) { return sPanel; }

/* One source row onto two panel rows, each pixel twice. Turned: d0/d1 point one past the turned
 * row's last pixel, and four source pixels a b c d become d d c c b b a a ending there. */
static void row_x2(uint32_t *d0, uint32_t *d1, const uint32_t *src, int n, uint32_t a) {
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32x4_t av = vdupq_n_u32(a);
    for (; x + 4 <= n; x += 4) {
        const uint32x4_t p = vorrq_u32(vld1q_u32(src + x), av);
        const uint32x4x2_t z = vzipq_u32(p, p);
        vst1q_u32(d0 + x * 2, z.val[0]); vst1q_u32(d0 + x * 2 + 4, z.val[1]);
        vst1q_u32(d1 + x * 2, z.val[0]); vst1q_u32(d1 + x * 2 + 4, z.val[1]);
    }
#endif
    for (; x < n; x++) { const uint32_t p = src[x] | a; d0[x * 2] = d0[x * 2 + 1] = d1[x * 2] = d1[x * 2 + 1] = p; }
}
static void row_x2_rot(uint32_t *d0, uint32_t *d1, const uint32_t *src, int n, uint32_t a) {
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32x4_t av = vdupq_n_u32(a);
    for (; x + 4 <= n; x += 4) {
        const uint32x4_t p = vorrq_u32(vld1q_u32(src + x), av);
        const uint32x4_t r64 = vrev64q_u32(p);
        const uint32x4_t r = vcombine_u32(vget_high_u32(r64), vget_low_u32(r64));
        const uint32x4x2_t z = vzipq_u32(r, r);
        uint32_t *o0 = d0 - x * 2 - 8, *o1 = d1 - x * 2 - 8;
        vst1q_u32(o0, z.val[0]); vst1q_u32(o0 + 4, z.val[1]);
        vst1q_u32(o1, z.val[0]); vst1q_u32(o1 + 4, z.val[1]);
    }
#endif
    for (; x < n; x++) { const uint32_t p = src[x] | a; d0[-x * 2 - 1] = d0[-x * 2 - 2] = d1[-x * 2 - 1] = d1[-x * 2 - 2] = p; }
}

/* <GAME>_PANEL_CHECK=1: the first 30 frames compared pixel by pixel with a scalar reference of the
 * same scale and turn (a qemu check of the NEON rows; the result goes to the log). */
static void check_frame(const uint32_t *src, int w, int h, int k, int ox, int oy) {
    if (sCheck < 0) { const char *e = getenv(HH_ENV("PANEL_CHECK")); sCheck = e && e[0] == '1'; }
    if (!sCheck || sChecked >= 30) return;
    const int dw = sPanel->w, dh = sPanel->h;
    const uint32_t a = sPanel->format->Amask;
    for (int y = 0; y < h * k; y++)
        for (int x = 0; x < w * k; x++) {
            const int px = ox + x, py = oy + y;
            const int dx = sRot ? dw - 1 - px : px, dy = sRot ? dh - 1 - py : py;
            const uint32_t want = src[(y / k) * w + x / k] | a;
            const uint32_t got = ((const uint32_t *)((const uint8_t *)sPanel->pixels + dy * sPanel->pitch))[dx];
            if (got != want && sCheckBad++ < 5) port_log("miyoo: panel check (%d,%d) %08x, want %08x\n", x, y, got, want);
        }
    if (++sChecked == 30) port_log("miyoo: panel check: 30 frames, %ld pixels differ\n", sCheckBad);
}

/* Mini v4 / Mini Flip (750x560 panel, 752x560 framebuffer): a game frame at most half the panel is scaled to the
 * panel's full height, centred, nearest (752x560: 320x240 -> all 752x560, 0.8% wider than 4:3; a 240x240 frame -> 560x560), black at
 * the sides. A 640x480 Mini keeps the exact 2x below (h * 2 == the panel's height); the menus' 640x480 stays centred.
 * <GAME>_FLIPFILL=0: the 2x picture centred with a border, as before. */
static int sFill SAVESTATE_EXCLUDE = -1;
static int fill_on(void) {
    if (sFill < 0) { const char *e = getenv(HH_ENV("FLIPFILL")); sFill = !(e && e[0] == '0'); }
    return sFill;
}
static void present_fill(uint8_t *base, int dw, int dh, int dp, int rot, uint32_t a, const uint32_t *src, int w, int h) {
    static uint16_t xmap[2048] SAVESTATE_EXCLUDE, ymap[2048] SAVESTATE_EXCLUDE;
    static int mw SAVESTATE_EXCLUDE, mh SAVESTATE_EXCLUDE, mdw SAVESTATE_EXCLUDE, mdh SAVESTATE_EXCLUDE;
    int fw = dh * w / h;
    if (fw > dw || (dw - fw) * 50 <= dw) fw = dw; /* a gap of 2% or less (752x560: 3 columns a side): the full width, 0.8% wider, no black slivers */
    const int ox = (dw - fw) / 2;
    if (mw != w || mh != h || mdw != fw || mdh != dh) {
        for (int x = 0; x < fw; x++) { const int sx = (x * w + w / 2) / fw; xmap[x] = (uint16_t)(sx < w ? sx : w - 1); }
        for (int y = 0; y < dh; y++) ymap[y] = (uint16_t)(y * h / dh);
        mw = w; mh = h; mdw = fw; mdh = dh;
    }
    const uint32_t *prev = NULL;
    uint32_t *prevrow = NULL;
    for (int y = 0; y < dh; y++) {
        uint32_t *d = (uint32_t *)(base + (rot ? dh - 1 - y : y) * dp);
        const uint32_t *s = src + ymap[y] * w;
        if (s == prev) { memcpy(d, prevrow, (size_t)dw * 4); continue; } /* the same source row: the row above, as is */
        if (rot) {
            for (int x = 0; x < dw - ox - fw; x++) d[x] = a;
            uint32_t *e = d + dw - 1 - ox;
            for (int x = 0; x < fw; x++) e[-x] = s[xmap[x]] | a;
            for (int x = dw - ox; x < dw; x++) d[x] = a;
        } else {
            for (int x = 0; x < ox; x++) d[x] = a;
            for (int x = 0; x < fw; x++) d[ox + x] = s[xmap[x]] | a;
            for (int x = ox + fw; x < dw; x++) d[x] = a;
        }
        prev = s; prevrow = d;
    }
}

void miyoo_panel_area(int w, int h, int *ox, int *oy, int *pw, int *ph) {
    const int dw = sPanel ? sPanel->w : 640, dh = sPanel ? sPanel->h : 480;
    int k = w > 0 && h > 0 ? (dw / w < dh / h ? dw / w : dh / h) : 1;
    if (k < 1) k = 1;
    if (w > 0 && h > 0 && k >= 2 && h * k != dh && fill_on()) { /* present_fill's */
        *ph = dh; *pw = dh * w / h; if (*pw > dw || (dw - *pw) * 50 <= dw) *pw = dw;
    } else { *pw = w * k; *ph = h * k; }
    *ox = (dw - *pw) / 2; *oy = (dh - *ph) / 2;
}

static void present(uint32_t *src, int w, int h) {
    if (!sPanel || !src || w <= 0 || h <= 0) return;
    const int dw = sPanel->w, dh = sPanel->h, dp = sPanel->pitch, rot = miyoo_panel_rotated();
    int k = dw / w < dh / h ? dw / w : dh / h;
    if (k < 1) k = 1;
    const int ox = (dw - w * k) / 2, oy = (dh - h * k) / 2;
    const SDL_PixelFormat *f = sPanel->format;
    const uint32_t a = f->Amask;
    uint8_t *const base = (uint8_t *)sPanel->pixels;
    if (SDL_MUSTLOCK(sPanel)) SDL_LockSurface(sPanel);
    const int xrgb = f->BitsPerPixel == 32 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0;
    if (xrgb && k >= 2 && h * k != dh && fill_on()) {
        present_fill(base, dw, dh, dp, rot, a, src, w, h);
    } else if (xrgb && k == 2 && w <= dw / 2 && h <= dh / 2) {
        for (int y = 0; y < h; y++) {
            const uint32_t *s = src + y * w;
            const int py = oy + y * 2;
            if (rot)
                row_x2_rot((uint32_t *)(base + (dh - 1 - py) * dp) + (dw - ox), (uint32_t *)(base + (dh - 2 - py) * dp) + (dw - ox), s, w, a);
            else
                row_x2((uint32_t *)(base + py * dp) + ox, (uint32_t *)(base + (py + 1) * dp) + ox, s, w, a);
        }
        check_frame(src, w, h, k, ox, oy);
    } else if (xrgb && k == 1) {
        if (w < dw || h < dh) /* the menus' 640x480 on a bigger panel: the filled game frame around it cleared */
            for (int y = 0; y < dh; y++) {
                uint32_t *d = (uint32_t *)(base + y * dp);
                const int py = rot ? dh - 1 - y : y, l = rot ? dw - ox - w : ox; /* the menu's first column here */
                if (py < oy || py >= oy + h) { for (int x = 0; x < dw; x++) d[x] = a; continue; }
                for (int x = 0; x < l; x++) d[x] = a;
                for (int x = l + w; x < dw; x++) d[x] = a;
            }
        for (int y = 0; y < h; y++) { /* the menus' 640x480 on a 640x480 panel */
            const uint32_t *s = src + y * w;
            if (rot) {
                uint32_t *d = (uint32_t *)(base + (dh - 1 - (oy + y)) * dp) + (dw - 1 - ox);
                for (int x = 0; x < w; x++) d[-x] = s[x] | a;
            } else {
                uint32_t *d = (uint32_t *)(base + (oy + y) * dp) + ox;
                for (int x = 0; x < w; x++) d[x] = s[x] | a;
            }
        }
    } else { /* any other format or factor: per pixel through SDL_MapRGB (slow, never in play so far) */
        for (int y = 0; y < h * k; y++) {
            const int py = oy + y, dy = rot ? dh - 1 - py : py;
            if (py >= dh) break;
            uint8_t *row = base + dy * dp;
            const uint32_t *s = src + (y / k) * w;
            for (int x = 0; x < w * k && ox + x < dw; x++) {
                const uint32_t p = s[x / k];
                const int dx = rot ? dw - 1 - (ox + x) : ox + x;
                const Uint32 m = SDL_MapRGB(sPanel->format, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF) | a;
                if (f->BitsPerPixel == 32) ((uint32_t *)row)[dx] = m;
                else if (f->BitsPerPixel == 16) ((uint16_t *)row)[dx] = (uint16_t)m;
            }
        }
    }
    if (SDL_MUSTLOCK(sPanel)) SDL_UnlockSurface(sPanel);
}

void miyoo_panel_present32(uint32_t *src, int w, int h) {
    static SDL_Surface *sWrap SAVESTATE_EXCLUDE; /* src as a surface, for the notice */
    if (!src || w <= 0 || h <= 0) return;
    if (!sWrap || sWrap->pixels != src || sWrap->w != w || sWrap->h != h) {
        if (sWrap) SDL_FreeSurface(sWrap);
        sWrap = SDL_CreateRGBSurfaceFrom(src, w, h, 32, w * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    }
    if (sWrap) miyoo_menu_overlay(sWrap);
    present(src, w, h);
}

/* <GAME>_PANEL_DUMP=<dir>: every 150th flip, the panel surface as the display gets it (turn, scale and all) to
 * <dir>/panel_NNNNNN.ppm (as DKR's and MK64's): checks the Mini's 2x and the Mini v4 / Flip fill off the device. */
static void panel_dump(void) {
    static const char *sDir SAVESTATE_EXCLUDE; static int sInit SAVESTATE_EXCLUDE; static long sN SAVESTATE_EXCLUDE;
    if (!sInit) { sDir = getenv(HH_ENV("PANEL_DUMP")); sInit = 1; }
    if (!sDir || (sN++ % 150) != 0 || sPanel->format->BitsPerPixel != 32) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/panel_%06ld.ppm", sDir, sN - 1);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sPanel->w, sPanel->h);
    for (int y = 0; y < sPanel->h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)sPanel->pixels + y * sPanel->pitch);
        for (int x = 0; x < sPanel->w; x++) {
            Uint8 rgb[3];
            SDL_GetRGB(row[x], sPanel->format, &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

void miyoo_panel_flip(void) { if (sPanel) { panel_dump(); SDL_Flip(sPanel); } }

void miyoo_panel_show_full(SDL_Surface *s) {
    if (!sPanel || !s || s->format->BitsPerPixel != 32) return;
    if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
    /* the menus draw 640x480: on a bigger panel present32 centres it (k = 1) */
    uint32_t *px = (uint32_t *)s->pixels;
    if (s->pitch == s->w * 4) present(px, s->w, s->h);
    else {
        static uint32_t *sCopy SAVESTATE_EXCLUDE;
        static int sCopyN SAVESTATE_EXCLUDE;
        if (sCopyN < s->w * s->h) { free(sCopy); sCopy = malloc((size_t)s->w * s->h * 4); sCopyN = sCopy ? s->w * s->h : 0; }
        if (sCopy) {
            for (int y = 0; y < s->h; y++) memcpy(sCopy + y * s->w, (uint8_t *)s->pixels + y * s->pitch, (size_t)s->w * 4);
            present(sCopy, s->w, s->h);
        }
    }
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    miyoo_panel_flip();
}

static char sExeDir[480] SAVESTATE_EXCLUDE;
const char *miyoo_exe_dir(void) {
    if (!sExeDir[0]) {
        const ssize_t n = readlink("/proc/self/exe", sExeDir, sizeof(sExeDir) - 1);
        char *slash;
        sExeDir[n > 0 ? n : 0] = 0;
        slash = strrchr(sExeDir, '/');
        if (n <= 0 || !slash) snprintf(sExeDir, sizeof(sExeDir), ".");
        else *slash = 0;
    }
    return sExeDir;
}

const char *miyoo_data_dir(void) {
    static char sDir[512] SAVESTATE_EXCLUDE;
    if (!sDir[0]) {
        const char *e = getenv(HH_ENV("DATA"));
        if (e && e[0]) snprintf(sDir, sizeof(sDir), "%s", e);
        else snprintf(sDir, sizeof(sDir), "%s/data", miyoo_exe_dir());
        mkdir(sDir, 0777);
    }
    return sDir;
}

const char *const miyoo_rom_roots[] = { "/mnt/SDCARD/Roms/N64", "/mnt/SDCARD", NULL };

static volatile sig_atomic_t *sTermFlag SAVESTATE_EXCLUDE;
static void term_handler(int sig) { (void)sig; if (sTermFlag) *sTermFlag = 1; }
void miyoo_term_install(volatile sig_atomic_t *flag) {
    sTermFlag = flag;
    signal(SIGTERM, term_handler);
    signal(SIGHUP, term_handler);
}

#endif /* MIYOO */
