/* Miyoo Mini video (see miyoo_video.h): the game, the menu and the splash all draw into one
 * 320x240 RGB565 surface; miyoo_flip scales it onto the panel's real video surface and flips.
 *
 * The panel size comes from /dev/fb0 (640x480 on the Mini and Mini Plus, 752x560 on the Mini
 * v4), MK64_MIYOO_MODE=<w>x<h> overrides it, and a mode SDL refuses falls back to 640x480.
 *
 * The panel surface is 32 bpp: the Mini's SDL accepts a 16 bpp mode but shows it as streaks (the
 * first device run, 2026-09-30; the GoldenEye port's 32 bpp screen works). It doesn't turn the
 * picture for the panel either (GoldenEye came up upside down), so the flip turns it 180 degrees;
 * rotate180=0 in mk64_display.cfg draws it unturned. */
#if defined(MIYOO)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "miyoo_video.h"
#include "handheld_screen.h"
#include "savestate.h"

static SDL_Surface *sReal SAVESTATE_EXCLUDE;   /* the panel */
static SDL_Surface *sShadow SAVESTATE_EXCLUDE; /* what everything draws into */
static int sScale SAVESTATE_EXCLUDE, sOx SAVESTATE_EXCLUDE, sOy SAVESTATE_EXCLUDE, sRot SAVESTATE_EXCLUDE;

static void panel_size(int *w, int *h) {
    *w = 640; *h = 480;
    const char *e = getenv("MK64_MIYOO_MODE");
    int ew = 0, eh = 0;
    if (e && sscanf(e, "%dx%d", &ew, &eh) == 2 && ew >= 320 && eh >= 240) { *w = ew; *h = eh; return; }
    const int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) return;
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.xres >= 320 && v.yres >= 240 && v.xres <= 2048 && v.yres <= 2048) {
        *w = (int) v.xres; *h = (int) v.yres;
    }
    close(fd);
}

static void clear_real(void) {
    /* every page of a double/triple-buffered surface: the borders are never drawn again */
    for (int i = 0; i < 3; i++) {
        SDL_FillRect(sReal, NULL, SDL_MapRGB(sReal->format, 0, 0, 0) | sReal->format->Amask); /* opaque black */
        SDL_Flip(sReal);
    }
}

static void open_real(void) {
    int pw, ph;
    panel_size(&pw, &ph);
    sReal = SDL_SetVideoMode(pw, ph, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sReal && (pw != 640 || ph != 480)) sReal = SDL_SetVideoMode(640, 480, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sReal) sReal = SDL_SetVideoMode(640, 480, 16, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!sReal) { fprintf(stderr, "miyoo: SDL_SetVideoMode: %s\n", SDL_GetError()); return; }
    sRot = handheld_cfg_rotate180();
    fprintf(stderr, "miyoo: panel %dx%d (asked %dx%d), %d bpp (R%d G%d B%d), pitch %d%s\n", sReal->w, sReal->h, pw, ph,
            sReal->format->BitsPerPixel, sReal->format->Rshift, sReal->format->Gshift, sReal->format->Bshift,
            sReal->pitch, sRot ? ", turned 180" : "");
    clear_real();
}

SDL_Surface *miyoo_set_video_mode(int w, int h, int bpp, Uint32 flags) {
    (void) bpp; (void) flags;
    if (!sReal) open_real();
    if (!sReal) return NULL;
    if (sShadow && (sShadow->w != w || sShadow->h != h)) { SDL_FreeSurface(sShadow); sShadow = NULL; }
    if (!sShadow) {
        sShadow = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 16, 0xF800, 0x07E0, 0x001F, 0);
        if (!sShadow) { fprintf(stderr, "miyoo: shadow surface: %s\n", SDL_GetError()); return NULL; }
        SDL_FillRect(sShadow, NULL, 0);
        sScale = sReal->w / w < sReal->h / h ? sReal->w / w : sReal->h / h;
        if (sScale < 1) sScale = 1;
        sOx = (sReal->w - w * sScale) / 2; if (sOx < 0) sOx = 0;
        sOy = (sReal->h - h * sScale) / 2; if (sOy < 0) sOy = 0;
        fprintf(stderr, "miyoo: %dx%d drawn x%d at %d,%d\n", w, h, sScale, sOx, sOy);
        clear_real();
    }
    return sShadow;
}

/* Shadow -> panel: each 565 pixel to the panel's 32-bit colour, k x k, turned 180 degrees when
 * sRot (the panel's row 0 is the picture's bottom row, and each row runs right to left). */
static void panel_dump(void);
static uint32_t *sLut565 SAVESTATE_EXCLUDE; /* 565 -> panel pixel (256 KB), built at the first flip */
static void scale_to_real32(void) {
    const SDL_PixelFormat *f = sReal->format;
    if (!sLut565) {
        sLut565 = malloc(65536 * sizeof(uint32_t));
        if (!sLut565) return;
        for (int c = 0; c < 65536; c++) {
            const Uint8 r = (Uint8) (((c >> 11) & 31) * 255 / 31), g = (Uint8) (((c >> 5) & 63) * 255 / 63), b = (Uint8) ((c & 31) * 255 / 31);
            sLut565[c] = SDL_MapRGB(f, r, g, b) | (f->Amask ? f->Amask : 0);
        }
    }
    const int sw = sShadow->w, sh = sShadow->h, k = sScale;
    const int dw = sReal->w, dh = sReal->h;
    const int vis_w = sw * k <= dw - sOx ? sw * k : dw - sOx, vis_h = sh * k <= dh - sOy ? sh * k : dh - sOy;
    uint8_t *const dbase = (uint8_t *) sReal->pixels;
    const int dp = sReal->pitch;
    for (int y = 0; y < vis_h; y++) {
        const int dy = sRot ? dh - 1 - (sOy + y) : sOy + y;
        uint32_t *d = (uint32_t *) (dbase + dy * dp);
        if (y % k) { /* the same source row as the line before: copy it */
            const int py = sRot ? dy + 1 : dy - 1;
            memcpy(d, dbase + py * dp, (size_t) dw * 4);
            continue;
        }
        const uint16_t *s = (const uint16_t *) ((const uint8_t *) sShadow->pixels + (y / k) * sShadow->pitch);
        if (sRot) { uint32_t *o = d + dw - 1 - sOx; for (int x = 0; x < vis_w; x++) o[-x] = sLut565[s[x / k]]; }
        else      { uint32_t *o = d + sOx;          for (int x = 0; x < vis_w; x++) o[x]  = sLut565[s[x / k]]; }
    }
}

/* The game's frame at 32 bits, doubled (and turned) onto the panel: GoldenEye's present
 * (the GoldenEye port 48fb705), here with the panel's offset for bigger panels. One source row
 * becomes two panel rows, each pixel twice, alpha forced opaque. Turned: the row lands at the
 * panel's mirrored row and runs right to left, so four source pixels a b c d become the eight
 * panel pixels d d c c b b a a ending at the mirrored column. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
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
/* d0/d1 point one past the turned row's last pixel (its rightmost on the panel). */
static void row_x2_rot(uint32_t *d0, uint32_t *d1, const uint32_t *src, int n, uint32_t a) {
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32x4_t av = vdupq_n_u32(a);
    for (; x + 4 <= n; x += 4) {
        const uint32x4_t p = vorrq_u32(vld1q_u32(src + x), av);
        const uint32x4_t r64 = vrev64q_u32(p);                               /* b a d c */
        const uint32x4_t r = vcombine_u32(vget_high_u32(r64), vget_low_u32(r64)); /* d c b a */
        const uint32x4x2_t z = vzipq_u32(r, r);                              /* d d c c | b b a a */
        uint32_t *o0 = d0 - x * 2 - 8, *o1 = d1 - x * 2 - 8;
        vst1q_u32(o0, z.val[0]); vst1q_u32(o0 + 4, z.val[1]);
        vst1q_u32(o1, z.val[0]); vst1q_u32(o1 + 4, z.val[1]);
    }
#endif
    for (; x < n; x++) { const uint32_t p = src[x] | a; d0[-x * 2 - 1] = d0[-x * 2 - 2] = d1[-x * 2 - 1] = d1[-x * 2 - 2] = p; }
}

int miyoo_can_draw32(int w, int h) {
    if (!sReal || !sShadow || sScale != 2 || w != sShadow->w || h != sShadow->h) return 0;
    const SDL_PixelFormat *f = sReal->format;
    return f->BitsPerPixel == 32 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0 &&
           sOx + w * 2 <= sReal->w && sOy + h * 2 <= sReal->h;
}

/* MK64_D32_CHECK=1: the first 30 frames compared, pixel by pixel, with a scalar reference of the
 * same doubling and turn (a qemu check of the NEON rows); the result goes to the log. */
static void draw32_check(const uint32_t *src, int w, int h) {
    static int sOn SAVESTATE_EXCLUDE = -1, sDone SAVESTATE_EXCLUDE;
    static long sBad SAVESTATE_EXCLUDE;
    if (sOn < 0) { const char *e = getenv("MK64_D32_CHECK"); sOn = e && e[0] == '1'; }
    if (!sOn || sDone >= 30) return;
    const int dw = sReal->w, dh = sReal->h;
    const uint32_t a = sReal->format->Amask;
    for (int y = 0; y < h * 2; y++)
        for (int x = 0; x < w * 2; x++) {
            const int px = sOx + x, py = sOy + y;
            const int dx = sRot ? dw - 1 - px : px, dy = sRot ? dh - 1 - py : py;
            const uint32_t want = src[(y / 2) * w + x / 2] | a;
            const uint32_t got = ((const uint32_t *) ((const uint8_t *) sReal->pixels + dy * sReal->pitch))[dx];
            if (got != want && sBad++ < 5) fprintf(stderr, "miyoo: d32 check: (%d,%d) %08x, want %08x\n", x, y, got, want);
        }
    if (++sDone == 30) fprintf(stderr, "miyoo: d32 check: 30 frames, %ld pixels differ\n", sBad);
}

void miyoo_draw32(uint32_t *src, int w, int h) {
    static SDL_Surface *sWrap SAVESTATE_EXCLUDE; /* the frame as a surface, for the notice */
    if (!sWrap || sWrap->pixels != src || sWrap->w != w || sWrap->h != h) {
        if (sWrap) SDL_FreeSurface(sWrap);
        sWrap = SDL_CreateRGBSurfaceFrom(src, w, h, 32, w * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    }
    if (sWrap) miyoo_menu_overlay(sWrap);
    const int dw = sReal->w, dh = sReal->h, dp = sReal->pitch;
    const uint32_t a = sReal->format->Amask;
    uint8_t *const base = (uint8_t *) sReal->pixels;
    if (SDL_MUSTLOCK(sReal)) SDL_LockSurface(sReal);
    for (int y = 0; y < h; y++) {
        const uint32_t *s = src + y * w;
        const int py = sOy + y * 2;
        if (sRot) {
            uint32_t *d0 = (uint32_t *) (base + (dh - 1 - py) * dp) + (dw - sOx);
            uint32_t *d1 = (uint32_t *) (base + (dh - 2 - py) * dp) + (dw - sOx);
            row_x2_rot(d0, d1, s, w, a);
        } else {
            row_x2((uint32_t *) (base + py * dp) + sOx, (uint32_t *) (base + (py + 1) * dp) + sOx, s, w, a);
        }
    }
    draw32_check(src, w, h);
    if (SDL_MUSTLOCK(sReal)) SDL_UnlockSurface(sReal);
    panel_dump();
}

void miyoo_flip_panel(void) { if (sReal) SDL_Flip(sReal); }

/* The same for a 16 bpp panel (the fallback mode). */
static void scale_to_real16(void) {
    const int sw = sShadow->w, sh = sShadow->h, k = sScale;
    const int dw = sReal->w, dh = sReal->h;
    const int vis_w = sw * k <= dw - sOx ? sw * k : dw - sOx, vis_h = sh * k <= dh - sOy ? sh * k : dh - sOy;
    uint8_t *const dbase = (uint8_t *) sReal->pixels;
    const int dp = sReal->pitch;
    for (int y = 0; y < vis_h; y++) {
        const int dy = sRot ? dh - 1 - (sOy + y) : sOy + y;
        const uint16_t *s = (const uint16_t *) ((const uint8_t *) sShadow->pixels + (y / k) * sShadow->pitch);
        uint16_t *d = (uint16_t *) (dbase + dy * dp);
        if (sRot) for (int x = 0; x < vis_w; x++) d[dw - 1 - (sOx + x)] = s[x / k];
        else      for (int x = 0; x < vis_w; x++) d[sOx + x] = s[x / k];
    }
}

void miyoo_show_full(SDL_Surface *s) {
    if (!sReal || !s || s->format->BitsPerPixel != 32) return;
    const int ox = (sReal->w - s->w) / 2, oy = (sReal->h - s->h) / 2;
    const int dw = sReal->w, dh = sReal->h, bpp = sReal->format->BitsPerPixel;
    const int same = bpp == 32 && sReal->format->Rshift == 16 && sReal->format->Gshift == 8 && sReal->format->Bshift == 0;
    const Uint32 amask = sReal->format->Amask;
    if (SDL_MUSTLOCK(sReal)) SDL_LockSurface(sReal);
    for (int y = 0; y < s->h; y++) {
        const int py = oy + y;
        if (py < 0 || py >= dh) continue;
        const int dy = sRot ? dh - 1 - py : py;
        const uint32_t *src = (const uint32_t *) ((const uint8_t *) s->pixels + y * s->pitch);
        uint8_t *row = (uint8_t *) sReal->pixels + dy * sReal->pitch;
        for (int x = 0; x < s->w; x++) {
            const int px = ox + x;
            if (px < 0 || px >= dw) continue;
            const int dx = sRot ? dw - 1 - px : px;
            const uint32_t p = src[x];
            if (same) ((uint32_t *) row)[dx] = p | amask;
            else {
                const Uint32 m = SDL_MapRGB(sReal->format, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF) | amask;
                if (bpp == 32) ((uint32_t *) row)[dx] = m;
                else if (bpp == 16) ((uint16_t *) row)[dx] = (uint16_t) m;
            }
        }
    }
    if (SDL_MUSTLOCK(sReal)) SDL_UnlockSurface(sReal);
    panel_dump();
    SDL_Flip(sReal);
}

/* MK64_PANEL_DUMP=<dir>: every 150th flip, the panel surface as the display gets it (turn and
 * all) to <dir>/panel_NNNNNN.ppm -- checks the scale and rotation off the device. */
static void panel_dump(void) {
    static const char *sDir SAVESTATE_EXCLUDE; static int sInit SAVESTATE_EXCLUDE; static long sN SAVESTATE_EXCLUDE;
    if (!sInit) { sDir = getenv("MK64_PANEL_DUMP"); sInit = 1; }
    if (!sDir || (sN++ % 150) != 0 || sReal->format->BitsPerPixel != 32) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/panel_%06ld.ppm", sDir, sN - 1);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sReal->w, sReal->h);
    for (int y = 0; y < sReal->h; y++) {
        const uint32_t *row = (const uint32_t *) ((const uint8_t *) sReal->pixels + y * sReal->pitch);
        for (int x = 0; x < sReal->w; x++) {
            Uint8 rgb[3];
            SDL_GetRGB(row[x], sReal->format, &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

int miyoo_flip(SDL_Surface *s) {
    if (!s || s != sShadow || !sReal) return s ? SDL_Flip(s) : -1;
    miyoo_menu_overlay(sShadow); /* a notification (SAVED SLOT n) on the game's frame */
    const int bpp = sReal->format->BitsPerPixel;
    if (bpp == 32 || bpp == 16) {
        if (SDL_MUSTLOCK(sReal)) SDL_LockSurface(sReal);
        if (bpp == 32) scale_to_real32(); else scale_to_real16();
        if (SDL_MUSTLOCK(sReal)) SDL_UnlockSurface(sReal);
        panel_dump();
    } else { /* anything else: unscaled and centred, but a picture */
        SDL_Rect d = { (Sint16) ((sReal->w - sShadow->w) / 2), (Sint16) ((sReal->h - sShadow->h) / 2), 0, 0 };
        SDL_BlitSurface(sShadow, NULL, sReal, &d);
    }
    return SDL_Flip(sReal);
}
#endif /* MIYOO */
