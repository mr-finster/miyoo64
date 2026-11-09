/* hwblit_test: checks and times the Miyoo Mini's 2D engine (MI_GFX) for the games' present (kit-hwblit/miyoo_hwblit.c,
 * the same file the games build), against the CPU's 2x. Run on the device (App/HwblitTest in Onion's Apps, or over
 * SSH: sh /mnt/SDCARD/App/HwblitTest/launch.sh); the results go to the screen's log file result.txt beside it.
 *
 *   1. the framebuffer and the libraries
 *   2. the engine's 180-degree turn, and each way of a nearest 2x checked pixel for pixel against the CPU's
 *   3. timings in memory: the CPU's 2x (the games' NEON rows) vs the engine's ways: the hand-over (what the game's
 *      thread spends: copy in, flushes, starting the blits) and until the engine is done, wall and CPU time
 *   4. the panel as the games open it (SDL, the device's own): where its buffer is, then frames drawn by the engine and
 *      read back, and the present timed both ways (a test picture shows for a few seconds)
 * No argument needed. "nosdl": skip 4. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <linux/fb.h>
#include <SDL/SDL.h>
#include "miyoo_hwblit.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#define W 320
#define H 240

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* the games' CPU 2x (miyoo_panel.c row_x2 / row_x2_rot, as they are) */
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
/* the frame 2x into a dw x dh buffer (stride dp bytes) at (ox, oy), turned or not: miyoo_panel.c's k == 2 present */
static void cpu_x2(uint8_t *base, int dw, int dh, int dp, int ox, int oy, const uint32_t *src, int w, int h, int rot, uint32_t a) {
    for (int y = 0; y < h; y++) {
        const uint32_t *s = src + y * w;
        const int py = oy + y * 2;
        if (rot)
            row_x2_rot((uint32_t *)(base + (dh - 1 - py) * dp) + (dw - ox), (uint32_t *)(base + (dh - 2 - py) * dp) + (dw - ox), s, w, a);
        else
            row_x2((uint32_t *)(base + py * dp) + ox, (uint32_t *)(base + (py + 1) * dp) + ox, s, w, a);
    }
}

static void picture(uint32_t *s, int w, int h, int t) { /* a moving test picture: bands, a grid, single-pixel dots */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t r = (uint32_t)((x + t) * 255 / w) & 255, g = (uint32_t)(y * 255 / h), b = ((x ^ y) & 8) ? 200 : 40;
            if (((x + t) % 32) == 0 || (y % 32) == 0) r = g = b = 255;
            if (((x * 7 + y * 13 + t) % 97) == 0) { r = 255; g = 0; b = 255; }
            s[y * w + x] = (r << 16) | (g << 8) | b; /* alpha 0, as a renderer may leave it */
        }
    /* an arrow in the top left corner: which way is up */
    for (int i = 0; i < 24; i++) for (int j = 0; j <= i / 2; j++) { s[(4 + i) * w + 16 + j] = 0xFFFF00; s[(4 + i) * w + 16 - j] = 0xFFFF00; }
}

static void fb_info(void) {
    const int fd = open("/dev/fb0", O_RDONLY);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fd < 0) { printf("fb0: cannot open /dev/fb0\n"); return; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0)
        printf("fb0: %ux%u %u bpp, virtual %ux%u, offset %u,%u, line %u bytes, memory %lx + %u\n", v.xres, v.yres,
               v.bits_per_pixel, v.xres_virtual, v.yres_virtual, v.xoffset, v.yoffset, f.line_length,
               (unsigned long)f.smem_start, f.smem_len);
    close(fd);
}

static int sRotMirror;

/* section 3: the CPU's 2x vs the engine's ways, in memory */
static void timings(void) {
    const int N = 60, dw = 2 * W, dh = 2 * H, dpitch = dw * 4;
    const uint32_t fb = (uint32_t)dpitch * dh;
    uint32_t *src = malloc(W * H * 4), *dst = malloc(fb);
    uint64_t mp = 0, sp = 0, bp = 0;
    uint32_t *mma = miyoo_hwblit_alloc(fb, &mp);
    uint32_t *stage = miyoo_hwblit_alloc(W * H * 4, &sp);
    uint32_t *big = miyoo_hwblit_alloc(752 * 560 * 4, &bp);
    double t0, c0, ms, cms;
    if (!src || !dst) { printf("timings: no memory\n"); return; }
    picture(src, W, H, 0);
    for (int pass = 0; pass < 2; pass++) {
        t0 = now_ms(); c0 = cpu_ms();
        for (int i = 0; i < N; i++) cpu_x2((uint8_t *)dst, dw, dh, dpitch, 0, 0, src, W, H, 1, 0xFF000000u);
        ms = (now_ms() - t0) / N; cms = (cpu_ms() - c0) / N;
        if (pass) printf("  CPU 2x turned, into ordinary memory:      %6.3f ms a frame (CPU %6.3f ms)\n", ms, cms);
    }
    if (!mma || !stage || !big) { printf("  (no engine memory: the engine's timings skipped)\n"); return; }
    for (int pass = 0; pass < 2; pass++) { /* (the first pass warms caches and pages up) */
        t0 = now_ms(); c0 = cpu_ms();
        for (int i = 0; i < N; i++) cpu_x2((uint8_t *)mma, dw, dh, dpitch, 0, 0, src, W, H, 1, 0xFF000000u);
        ms = (now_ms() - t0) / N; cms = (cpu_ms() - c0) / N;
        if (pass) printf("  CPU 2x turned, into engine memory (MMA):  %6.3f ms a frame (CPU %6.3f ms)\n", ms, cms);
        t0 = now_ms(); c0 = cpu_ms();
        for (int i = 0; i < N; i++) { for (int k = 0; k < W * H; k++) stage[k] = src[k] | 0xFF000000u; miyoo_hwblit_flush(stage, W * H * 4); }
        ms = (now_ms() - t0) / N; cms = (cpu_ms() - c0) / N;
        if (pass) printf("  frame copy into engine memory + flush:    %6.3f ms a frame (CPU %6.3f ms)\n", ms, cms);
        t0 = now_ms(); c0 = cpu_ms();
        for (int i = 0; i < N; i++) { miyoo_hwblit_flush(mma, fb); }
        ms = (now_ms() - t0) / N; cms = (cpu_ms() - c0) / N;
        if (pass) printf("  cache flush of a 640x480 panel:           %6.3f ms a frame (CPU %6.3f ms)\n", ms, cms);
    }
    /* the engine through the games' own path: miyoo_hwblit_frame onto an engine-memory "panel" */
    MiyooHwPanel p = { mma, (uint32_t)mp, dpitch, dw, dh };
    for (int how = 0; how < 3; how++) {
        miyoo_hwblit_reset(how, W, H);
        if (!miyoo_hwblit_frame(1, &p, src, W, H, 0, 0, dw, dh, 1, 0xFF000000u)) { printf("  engine way %d: did not run\n", how); continue; }
        miyoo_hwblit_sync();
        double kick = 0;
        t0 = now_ms(); c0 = cpu_ms();
        for (int i = 0; i < N; i++) {
            const double a = now_ms();
            miyoo_hwblit_frame(1, &p, src, W, H, 0, 0, dw, dh, 1, 0xFF000000u);
            kick += now_ms() - a;
            miyoo_hwblit_sync();
        }
        ms = (now_ms() - t0) / N; cms = (cpu_ms() - c0) / N;
        printf("  engine way %d: hand-over %6.3f ms, finished %6.3f ms (CPU %6.3f ms)\n", how, kick / N, ms, cms);
    }
    { /* the smooth fill of a Mini v4 / Flip: 320x240 onto all of 752x560 */
        MiyooHwPanel q = { big, (uint32_t)bp, 752 * 4, 752, 560 };
        miyoo_hwblit_reset(-1, 0, 0);
        if (miyoo_hwblit_frame(2, &q, src, W, H, 0, 0, 752, 560, 1, 0xFF000000u)) {
            t0 = now_ms(); c0 = cpu_ms();
            for (int i = 0; i < N; i++) { miyoo_hwblit_frame(2, &q, src, W, H, 0, 0, 752, 560, 1, 0xFF000000u); miyoo_hwblit_sync(); }
            ms = (now_ms() - t0) / N; cms = (cpu_ms() - c0) / N;
            printf("  engine smooth 320x240 -> 752x560, the whole present: %6.3f ms (CPU %6.3f ms)\n", ms, cms);
        } else printf("  engine smooth 752x560: did not run\n");
    }
    miyoo_hwblit_reset(-1, 0, 0);
    miyoo_hwblit_free(big, bp, 752 * 560 * 4);
    miyoo_hwblit_free(stage, sp, W * H * 4);
    miyoo_hwblit_free(mma, mp, fb);
    free(dst); free(src);
}

/* section 4: the panel as the games open it */
static void panel(void) {
    int pw = 640, ph = 480;
    {
        const int fd = open("/dev/fb0", O_RDONLY);
        struct fb_var_screeninfo v;
        if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.xres >= 320 && v.yres >= 240) { pw = (int)v.xres; ph = (int)v.yres; }
        if (fd >= 0) close(fd);
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return; }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_Surface *s = SDL_SetVideoMode(pw, ph, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!s) { printf("SDL_SetVideoMode %dx%d: %s\n", pw, ph, SDL_GetError()); SDL_Quit(); return; }
    {
        char drv[64] = "";
        SDL_VideoDriverName(drv, sizeof(drv));
        printf("SDL: driver %s, surface %dx%d pitch %d, %d bpp (A %08x), flags %08x (%s%s), pixels %p, unused1 %08x\n", drv,
               s->w, s->h, s->pitch, s->format->BitsPerPixel, s->format->Amask, s->flags,
               (s->flags & SDL_HWSURFACE) ? "hw" : "sw", (s->flags & SDL_DOUBLEBUF) ? " double" : "", s->pixels, s->unused1);
    }
    const int k = s->w / W < s->h / H ? s->w / W : s->h / H;
    const int ox = (s->w - W * 2) / 2, oy = (s->h - H * 2) / 2;
    uint32_t *src = malloc(W * H * 4);
    double t0, c0;
    long bad = 0, checked = 0;
    int on = 0, n = 0;
    void *firstpix = s->pixels;
    uint32_t firsthint = s->unused1;
    if (k < 2) { printf("panel smaller than 2x: skipped\n"); goto out; }
    {
        uint64_t phys;
        char desc[200];
        MiyooHwPanel p = { s->pixels, s->unused1, s->pitch, s->w, s->h };
        const int where = miyoo_hwblit_locate(&p, &phys, desc, sizeof(desc));
        printf("panel buffer: %s -> %s\n", desc, where == 1 ? "unused1" : where == 2 ? "/dev/fb0" : "not usable");
    }
    miyoo_hwblit_reset(-1, 0, 0);
    /* 120 frames by the engine (nearest), the first 20 read back and compared with the CPU's 2x */
    t0 = now_ms(); c0 = cpu_ms();
    double hwms = 0, hwcpu = 0;
    for (n = 0; n < 120; n++) {
        picture(src, W, H, n);
        if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
        MiyooHwPanel p = { s->pixels, s->unused1, s->pitch, s->w, s->h };
        const double a = now_ms(), b = cpu_ms();
        on = miyoo_hwblit_frame(1, &p, src, W, H, ox, oy, 2 * W, 2 * H, 1, s->format->Amask);
        miyoo_hwblit_sync();
        hwms += now_ms() - a; hwcpu += cpu_ms() - b;
        if (!on) { if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s); break; }
        if (n < 20) {
            for (int y = 0; y < 2 * H; y++)
                for (int x = 0; x < 2 * W; x++) {
                    const int px = ox + x, py = oy + y, dx = s->w - 1 - px, dy = s->h - 1 - py;
                    const uint32_t want = src[(y / 2) * W + x / 2] | s->format->Amask;
                    const uint32_t got = ((const uint32_t *)((const uint8_t *)s->pixels + dy * s->pitch))[dx];
                    if (got != want && bad++ < 5) printf("  frame %d (%d,%d): %08x, want %08x\n", n, x, y, got, want);
                    checked++;
                }
        }
        if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
        SDL_Flip(s);
    }
    if (on)
        printf("engine nearest on the panel: 120 frames, %.1f s; present %.3f ms a frame (CPU %.3f ms); read back: %ld of %ld "
               "pixels differ from the CPU's 2x\n", (now_ms() - t0) / 1000, hwms / 120, hwcpu / 120, bad, checked);
    else printf("engine nearest on the panel: not used (see the hwblit: line above)\n");
    printf("pixels %s, unused1 %s across flips\n", s->pixels == firstpix ? "unchanged" : "changed", s->unused1 == firsthint ? "unchanged" : "changed");
    { /* the same 120 frames by the CPU */
        double cms = 0, ccpu = 0;
        for (n = 0; n < 120; n++) {
            picture(src, W, H, n);
            if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
            const double a = now_ms(), b = cpu_ms();
            cpu_x2((uint8_t *)s->pixels, s->w, s->h, s->pitch, ox, oy, src, W, H, 1, s->format->Amask);
            cms += now_ms() - a; ccpu += cpu_ms() - b;
            if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
            SDL_Flip(s);
        }
        printf("CPU 2x on the panel: present %.3f ms a frame (CPU %.3f ms)\n", cms / 120, ccpu / 120);
    }
    if (on) { /* 60 frames of the smooth scaler, for the eye */
        double sms = 0;
        miyoo_hwblit_reset(-1, 0, 0);
        for (n = 0; n < 60; n++) {
            picture(src, W, H, n);
            if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
            MiyooHwPanel p = { s->pixels, s->unused1, s->pitch, s->w, s->h };
            const double a = now_ms();
            const int r = miyoo_hwblit_frame(2, &p, src, W, H, ox, oy, 2 * W, 2 * H, 1, s->format->Amask);
            miyoo_hwblit_sync();
            sms += now_ms() - a;
            if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
            if (!r) break;
            SDL_Flip(s);
        }
        printf("engine smooth on the panel: %d frames, present %.3f ms a frame\n", n, n ? sms / n : 0.0);
    }
    (void)c0;
out:
    free(src);
    SDL_Quit();
}

int main(int argc, char **argv) {
    const char *why = "";
    struct utsname u;
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(1, 2); /* the module's own lines (hwblit: ...) in the same file, in order */
    printf("hwblit_test (kit-hwblit): the 2D engine for the games' present\n");
    if (uname(&u) == 0) printf("system: %s %s %s\n", u.sysname, u.release, u.machine);
    fb_info();
    printf("\n1. libraries\n");
    if (!miyoo_hwblit_open(&why)) { printf("MI_GFX: not usable: %s\nRESULT: the games keep the CPU (hwblit has no effect here)\n", why); return 0; }
    printf("MI_GFX: libmi_sys.so and libmi_gfx.so opened\n");
    printf("\n2. checks (320x240 -> 640x480, every pixel against the CPU's)\n");
    const int t = miyoo_hwblit_check_turn();
    if (t < 0) printf("turn 180 at 1:1: %s\n", t == -1 ? "no engine memory (MI_SYS_MMA_Alloc failed)" : "every blit refused");
    else printf("turn 180 at 1:1: rotate %s, mirror both %s\n", (t & 1) ? "right" : "WRONG", (t & 2) ? "right" : "WRONG");
    sRotMirror = t > 0 && !(t & 1) && (t & 2);
    int exact = -1;
    for (int rot = 1; rot >= 0; rot--)
        for (int how = 0; how < 3; how++) {
            double ms = 0;
            const long bad = miyoo_hwblit_check_x2(how, W, H, rot, sRotMirror, &ms);
            printf("  way %d (%s), %s: %s", how, how == 0 ? "one scaled blit" : how == 1 ? "rows by stride, columns scaled" : "two quarter turns",
                   rot ? "turned" : "not turned", bad < 0 ? "the engine failed" : bad == 0 ? "EXACT" : "differs");
            if (bad > 0) printf(" (%ld of %d pixels)", bad, 4 * W * H);
            if (bad >= 0) printf(", %.3f ms", ms);
            printf("\n");
            if (rot && bad == 0 && exact < 0) exact = how;
        }
    printf("\n3. timings in memory (average of 60 frames; CPU = this thread's own CPU time)\n");
    timings();
    if (!(argc > 1 && strcmp(argv[1], "nosdl") == 0)) {
        printf("\n4. the panel, opened as the games open it\n");
        panel();
    }
    printf("\nRESULT: %s\n", t < 0 ? "the engine is not usable here: the games keep the CPU (hwblit has no effect)" : exact >= 0 ? "the engine can draw the games' 2x exactly (hwblit=1 should be used by the games)"
                                        : "no exact 2x: hwblit=1 leaves the CPU drawing; hwblit=2 (smooth) may still work");
    return 0;
}
