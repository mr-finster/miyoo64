/*
 * pm64bench: M1 renderer gate for the Paper Mario handheld port.
 *
 * Replays captured Paper Mario gfx tasks (.pms scenes, made locally from the player's
 * ROM by tools/cap + tools/pms; never shipped) through the mario-kart-64 renderer kit
 * (gfx_pc.c F3DEX2 front end + gfx_soft.c rasterizer on a square output) and logs, per scene:
 * ms per game frame split into interp / r3d / r2d / tex / clear / present, the fps that
 * implies, the worst frame, and VmRSS / MemAvailable.
 *
 *   pm64bench [-s seconds] [-o shotdir] [-d scenedir] [scene.pms ...]
 * With no scene arguments it runs every *.pms in the scene directory
 * (default: $HOME/.pm64bench, i.e. the OS's home folder/.pm64bench on the device).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "gfx_pc.h"
#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"
#include "port.h"
#ifdef PM64_SDL12
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#ifndef SDL_TRIPLEBUF
#define SDL_TRIPLEBUF SDL_DOUBLEBUF /* as mario-kart-64 gfx_sdl12.c */
#endif
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#endif

#define SCREEN 240
#define RDRAM_BYTES (4u << 20)

uint8_t *pm64_rdram;
u32 gPortProfFrame[PROF_COUNT];
u32 gPortProfLast[PROF_COUNT];
volatile u32 gPortProfFlipUs;
/* gfx_pc's static-memory test reads these linker regions; the bench has no asset region. */
char __assets_start[1], __assets_end[1];
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

static FILE *sLog;

/* SYS_clock_gettime32 on the device: musl 1.2 tries clock_gettime64 first, which the handheld's
 * 4.14 kernel lacks (ENOSYS), so every read cost two kernel entries -- and the kit reads the
 * clock twice per draw batch for its profile (MK64 perf round 24, via ocarina-of-time d267ca3). */
u32 port_time_us(void) {
#if defined(SYS_clock_gettime32)
    struct { long s, ns; } t;
    if (syscall(SYS_clock_gettime32, CLOCK_MONOTONIC, &t) == 0) return (u32)((uint64_t)t.s * 1000000u + (uint64_t)(t.ns / 1000));
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
}
static void vlog(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    vfprintf(stderr, fmt, ap);
    if (sLog) { vfprintf(sLog, fmt, ap2); fflush(sLog); }
    va_end(ap2);
}
void port_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
void port_log_always(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
void port_profile_add(int slot, uint32_t us) { if (slot == 0 || slot == 4 || slot == 5 || slot == 6) (void)us; }
void *port_seg_to_ptr(uintptr_t addr) { return (void *)addr; }
void port_gfx_overlay(void) {}

/* ---- window manager: the bench presents itself, outside gfx_run, so present is timed apart ---- */
static void wm_init(const char *n, bool fs) { (void)n; (void)fs; }
static void wm_set_kb(bool (*a)(int), bool (*b)(int), void (*c)(void)) { (void)a; (void)b; (void)c; }
static void wm_set_fs_cb(void (*cb)(bool)) { (void)cb; }
static void wm_set_fs(bool e) { (void)e; }
static void wm_main_loop(void (*f)(void)) { (void)f; }
/* Screen fit (round 3, your choice): the game renders its own 320x240 frame and the
 * present fits it to the square panel.
 *   PM64_FIT=squeeze (default, "B"): area filter, every 4 source columns -> 3 (3:1, 2:2, 1:3);
 *   PM64_FIT=crop ("D"): a 1:1 window 240 wide, centred (PM64_CROP_X = its left edge, 0..80).
 * PM64_SCREEN=WxH renders at another size, presented unfitted (experiments). */
static int sOutW = 320, sOutH = 240;
static int sFitCrop, sCropX = 40;
/* One 240-px output row from one source row of the rendered frame. */
static void fit_row(uint32_t *dst, const uint32_t *src, int sw) {
    if (sw == SCREEN) { memcpy(dst, src, SCREEN * 4); return; }
    if (sFitCrop || sw != 320) { const int x0 = sw - SCREEN < sCropX ? sw - SCREEN : sCropX; memcpy(dst, src + (x0 > 0 ? x0 : 0), SCREEN * 4); return; }
    for (int i = 0; i < 80; i++, src += 4, dst += 3) {
        const uint32_t a = src[0], b = src[1], c = src[2], d = src[3];
#define CH(p, sh) (((p) >> (sh)) & 255u)
#define MIX(p, q, wp, wq) ((((CH(p, 16) * wp + CH(q, 16) * wq + 2) >> 2) << 16) | (((CH(p, 8) * wp + CH(q, 8) * wq + 2) >> 2) << 8) | ((CH(p, 0) * wp + CH(q, 0) * wq + 2) >> 2) | 0xFF000000u)
        dst[0] = MIX(a, b, 3, 1); dst[1] = MIX(b, c, 2, 2); dst[2] = MIX(c, d, 1, 3);
#undef MIX
#undef CH
    }
}
static void wm_dims(uint32_t *w, uint32_t *h) { *w = (uint32_t)sOutW; *h = (uint32_t)sOutH; }
static void wm_events(void) {}
static bool wm_start(void) { return true; }
static void wm_swap(void) {}
static double wm_time(void) { return port_time_us() / 1e6; }
static struct GfxWindowManagerAPI bench_wapi = { wm_init, wm_set_kb, wm_set_fs_cb, wm_set_fs, wm_main_loop, wm_dims,
                                                 wm_events, wm_start, wm_swap, wm_swap, wm_time };
extern struct GfxRenderingAPI gfx_soft_api;
extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);

/* ---- present ---- */
#ifdef PM64_SDL12
static SDL_Surface *sScreen;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
/* ARGB8888 -> RGB565, 8 px per iteration (from mario-kart-64 gfx_sdl12.c). */
static void argb_row_to_565(uint16_t *dst, const uint32_t *src, int n) {
    int x = 0;
    for (; x + 8 <= n; x += 8) {
        const uint8x8x4_t p = vld4_u8((const uint8_t *)(src + x));
        const uint16x8_t r = vshll_n_u8(p.val[2], 8), g = vshll_n_u8(p.val[1], 8), b = vshll_n_u8(p.val[0], 8);
        vst1q_u16(dst + x, vorrq_u16(vorrq_u16(vandq_u16(r, vdupq_n_u16(0xF800)), vshrq_n_u16(vandq_u16(g, vdupq_n_u16(0xFC00)), 5)),
                                     vshrq_n_u16(vandq_u16(b, vdupq_n_u16(0xF800)), 11)));
    }
    for (; x < n; x++) { const uint32_t s = src[x]; dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F)); }
}
#else
static void argb_row_to_565(uint16_t *dst, const uint32_t *src, int n) {
    for (int x = 0; x < n; x++) { const uint32_t s = src[x]; dst[x] = (uint16_t)(((s >> 8) & 0xF800) | ((s >> 5) & 0x07E0) | ((s >> 3) & 0x001F)); }
}
#endif
/* As every port's gfx_sdl12.c (mario-kart-64): SDL_TRIPLEBUF pans /dev/fb0 across three
 * pages, so pan back to page 1 on exit and on any crash, or the launcher draws to a page
 * the panel is not showing. */
static void fb_unpan(void) {
    const int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return;
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.yoffset != 0) { v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v); }
    close(fd);
}
static void fatal_handler(int sig) { fb_unpan(); signal(sig, SIG_DFL); raise(sig); }
static void present_init(void) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    /* the games' present path (mario-kart-64 gfx_sdl12.c): 16 bpp, HW surface, triple buffered */
    sScreen = SDL_SetVideoMode(SCREEN, SCREEN, 16, SDL_HWSURFACE | SDL_TRIPLEBUF);
    if (!sScreen) sScreen = SDL_SetVideoMode(SCREEN, SCREEN, 16, SDL_SWSURFACE);
    if (!sScreen) { port_log("SDL_SetVideoMode failed: %s\n", SDL_GetError()); exit(1); }
    port_log("video: %dx%d %d bpp, %s\n", sScreen->w, sScreen->h, sScreen->format->BitsPerPixel,
             (sScreen->flags & SDL_HWSURFACE) ? "HW surface" : "SW surface");
    atexit(fb_unpan);
    signal(SIGSEGV, fatal_handler); signal(SIGBUS, fatal_handler); signal(SIGFPE, fatal_handler);
    signal(SIGILL, fatal_handler);  signal(SIGABRT, fatal_handler);
}
/* ---- status screens in mario-kart-64's first-start layout (assets_handheld.c ui_draw) ---- */
#define RES_DIR "/usr/games/menu_resources/"
static TTF_Font *sFontBig, *sFontSmall;
static void ui_text(int y, const char *s, TTF_Font *f, int gray) {
    SDL_Color c = { (Uint8)gray, (Uint8)gray, (Uint8)gray, 0 };
    if (!f || !s || !s[0]) return;
    SDL_Surface *t = TTF_RenderText_Blended(f, s, c);
    if (!t) return;
    SDL_Rect p = { (Sint16)((sScreen->w - t->w) / 2), (Sint16)y, 0, 0 };
    SDL_BlitSurface(t, NULL, sScreen, &p);
    SDL_FreeSurface(t);
}
static void ui_draw(const char *sub, const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    if (!sFontBig && TTF_WasInit() == 0 && TTF_Init() == 0) {
        sFontBig = TTF_OpenFont(RES_DIR "OpenSans-Bold.ttf", 19);
        sFontSmall = TTF_OpenFont(RES_DIR "OpenSans-Bold.ttf", 12);
    }
    SDL_FillRect(sScreen, NULL, SDL_MapRGB(sScreen->format, 0, 0, 0));
    ui_text(22, "PAPER MARIO", sFontBig, 255);
    ui_text(50, sub, sFontSmall, 150);
    ui_text(86, l1, sFontSmall, 230);
    ui_text(104, l2, sFontSmall, 230);
    ui_text(122, l3, sFontSmall, 230);
    if (pct >= 0) {
        SDL_Rect frame = { 30, 158, 180, 12 }, fill = { 32, 160, (Uint16)(176 * pct / 100), 8 };
        char p[8];
        SDL_FillRect(sScreen, &frame, SDL_MapRGB(sScreen->format, 70, 70, 70));
        SDL_FillRect(sScreen, &fill, SDL_MapRGB(sScreen->format, 255, 255, 255));
        snprintf(p, sizeof p, "%d%%", pct);
        ui_text(176, p, sFontSmall, 200);
    }
    ui_text(212, foot, sFontSmall, 150);
    SDL_Flip(sScreen);
}
static void ui_wait_button(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { }
    for (;;) { if (!SDL_WaitEvent(&ev)) return; if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return; }
}
static void present(void) {
    int w, h;
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    static uint32_t row[SCREEN];
    if (SDL_MUSTLOCK(sScreen)) SDL_LockSurface(sScreen);
    for (int y = 0; y < h && y < SCREEN; y++) {
        fit_row(row, fb + y * w, w);
        argb_row_to_565((uint16_t *)((uint8_t *)sScreen->pixels + y * sScreen->pitch), row, SCREEN);
    }
    if (SDL_MUSTLOCK(sScreen)) SDL_UnlockSurface(sScreen);
    SDL_Flip(sScreen);
}
static bool sQuit;
static bool sQuitSeen(void);
/* The handheld MENU button arrives as SDLK_q (as in mario-kart-64 gfx_sdl12.c). Sticky once seen. */
static bool quit_requested(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_q || e.key.keysym.sym == SDLK_ESCAPE))) sQuit = true;
    return sQuit;
}
static bool sQuitSeen(void) { return sQuit; }
/* Keep a status screen up long enough to read (outside any timed window); MENU still quits (round 13). */
static void ui_hold(int ms) {
    const uint32_t t0 = port_time_us();
    while (!quit_requested() && port_time_us() - t0 < (uint32_t) ms * 1000u) SDL_Delay(50);
}
#else
static bool sQuitSeen(void) { return false; }
static void present_init(void) {}
static void present(void) {}
static bool quit_requested(void) { return false; }
static void ui_draw(const char *sub, const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    printf("screen: %s | %s | %s | %s | %d%% | %s\n", sub ? sub : "", l1 ? l1 : "", l2 ? l2 : "", l3 ? l3 : "", pct, foot ? foot : "");
}
static void ui_wait_button(void) {}
static void ui_hold(int ms) { (void) ms; }
#endif

static void save_ppm(const char *path) {
    int w, h;
    const uint32_t *fb = gfx_soft_get_framebuffer(&w, &h);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const bool fit = w >= SCREEN && (w == 320 || w == SCREEN) && h == 240; /* the panel's view of the frame */
    const int ow = fit ? SCREEN : w;
    fprintf(f, "P6\n%d %d\n255\n", ow, h);
    uint32_t row[640];
    for (int y = 0; y < h; y++) {
        if (fit) fit_row(row, fb + y * w, w); else memcpy(row, fb + y * w, w * 4);
        for (int x = 0; x < ow; x++) { uint8_t px[3] = { row[x] >> 16, row[x] >> 8, row[x] }; fwrite(px, 3, 1, f); }
    }
    fclose(f);
}

/* ---- scenes ---- */
typedef struct { uint32_t addr, len; const uint8_t *data; } Region;
typedef struct { uint32_t root, nreg, poll; bool fstart; Region *reg; } Task; /* fstart: the task begins a game frame */
typedef struct { char name[64]; uint8_t *blob; uint32_t ntask, nframes; bool stamped; Task *task; } Scene;

static bool load_scene(const char *path, Scene *s) {
    FILE *f = fopen(path, "rb");
    if (!f) { port_log("cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->blob = malloc(n);
    if (!s->blob || fread(s->blob, 1, n, f) != (size_t)n) { fclose(f); port_log("cannot read %s\n", path); return false; }
    fclose(f);
    const uint32_t *h = (const uint32_t *)s->blob;
    if (h[0] != 0x31534D50) { port_log("%s: not a PMS1 scene\n", path); return false; }
    s->ntask = h[1];
    s->task = calloc(s->ntask, sizeof(Task));
    const uint8_t *p = s->blob + 16, *end = s->blob + n;
    for (uint32_t i = 0; i < s->ntask; i++) {
        const uint32_t *fh = (const uint32_t *)p;
        Task *t = &s->task[i];
        t->root = fh[0]; t->nreg = fh[1]; t->poll = fh[3];
        t->reg = calloc(t->nreg, sizeof(Region));
        p += 16;
        for (uint32_t r = 0; r < t->nreg; r++) {
            const uint32_t *rh = (const uint32_t *)p;
            t->reg[r].addr = rh[0]; t->reg[r].len = rh[1]; t->reg[r].data = p + 8;
            p += 8 + rh[1];
            if (p > end || rh[0] + rh[1] > RDRAM_BYTES) { port_log("%s: corrupt at task %u\n", path, i); return false; }
        }
    }
    /* Game frames: tasks that share a controller-poll stamp. The canonical m1 scenes were captured before the stamp
     * reached the capture (every task has 0), so rounds 1-8 timed each whole 30-task pass as one "frame". Without
     * stamps, a frame starts where the task's framebuffer changes (nusys rotates three); the warm-up pass records it
     * (bench_scene). On a stamped capture this gives the stamps' frames exactly (round 9). */
    s->stamped = false;
    for (uint32_t i = 1; i < s->ntask; i++) if (s->task[i].poll != s->task[0].poll) s->stamped = true;
    if (getenv("PM64_NOSTAMP")) s->stamped = false; /* test the framebuffer rule on stamped scenes */
    for (uint32_t i = 0; i < s->ntask; i++) s->task[i].fstart = i == 0 || (s->stamped && s->task[i].poll != s->task[i - 1].poll);
    const char *b = strrchr(path, '/');
    snprintf(s->name, sizeof s->name, "%s", b ? b + 1 : path);
    char *dot = strrchr(s->name, '.');
    if (dot) *dot = 0;
    return true;
}
static void free_scene(Scene *s) {
    for (uint32_t i = 0; i < s->ntask; i++) free(s->task[i].reg);
    free(s->task); free(s->blob);
}

static void mem_report(const char *when) {
    char line[256];
    long rss = -1, avail = -1;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) rss = atol(line + 6); fclose(f); }
    f = fopen("/proc/meminfo", "r");
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) avail = atol(line + 13); fclose(f); }
    port_log("MEM %s: VmRSS %ld KB, MemAvailable %ld KB\n", when, rss, avail);
}

/* Render one task; returns the render time in us (interp + raster, excluding the RDRAM copy). */
static uint32_t run_task(const Task *t) {
    for (uint32_t r = 0; r < t->nreg; r++) memcpy(pm64_rdram + t->reg[r].addr, t->reg[r].data, t->reg[r].len);
    const uint32_t t0 = port_time_us();
    gfx_start_frame();
    gfx_run((Gfx *)(pm64_rdram + t->root));
    gfx_end_frame();
    return port_time_us() - t0;
}

/* ---- device diagnostics (round 4: the first device run was ~40x slower than the proxy
 * in every stage, memset and SDL_Flip included; these lines say why) ---- */
static void log_file(const char *label, const char *path, int maxlines) {
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) { port_log("DIAG %s: (no %s)\n", label, path); return; }
    for (int n = 0; n < maxlines && fgets(line, sizeof line, f); n++) {
        size_t l = strlen(line); if (l && line[l - 1] == '\n') line[l - 1] = 0;
        port_log("DIAG %s: %s\n", label, line);
    }
    fclose(f);
}
static unsigned long long self_cpu_ticks(void) { /* utime + stime of this process, in clock ticks */
    FILE *f = fopen("/proc/self/stat", "r");
    unsigned long long ut = 0, st = 0;
    if (f) { if (fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) ut = st = 0; fclose(f); }
    return ut + st;
}
static unsigned long long total_cpu_ticks(unsigned long long *idle) {
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[8] = { 0 };
    if (f) { if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) {} fclose(f); }
    if (idle) *idle = v[3] + v[4];
    return v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
}
#define MAXP 256
typedef struct { int pid; unsigned long long t; char name[24]; } ProcT;
static int snap_procs(ProcT *p) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d)) && n < MAXP) {
        const int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        char path[64]; snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long long ut = 0, st = 0; char name[64] = "";
        if (fscanf(f, "%*d (%63[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", name, &ut, &st) == 3) {
            p[n].pid = pid; p[n].t = ut + st; snprintf(p[n].name, sizeof p[n].name, "%s", name); n++;
        }
        fclose(f);
    }
    if (d) closedir(d);
    return n;
}
static ProcT sProcA[MAXP]; static int sNProcA;
static unsigned long long sSelfA, sTotA, sIdleA;
static uint32_t sWallA;
static struct rusage sRuA;
static void diag_begin(void) {
    sNProcA = snap_procs(sProcA); sSelfA = self_cpu_ticks(); sTotA = total_cpu_ticks(&sIdleA); sWallA = port_time_us();
    getrusage(RUSAGE_SELF, &sRuA);
}
static double tv_s(struct timeval a, struct timeval b) { return (b.tv_sec - a.tv_sec) + (b.tv_usec - a.tv_usec) / 1e6; }
static void diag_end(const char *what) {
    static ProcT b[MAXP];
    const int nb = snap_procs(b);
    unsigned long long idle, tot = total_cpu_ticks(&idle);
    const double hz = 100.0, wall = (port_time_us() - sWallA) / 1e6;
    const double self = (self_cpu_ticks() - sSelfA) / hz, busy = ((tot - sTotA) - (idle - sIdleA)) / hz;
    port_log("DIAG %s: wall %.1f s, this process %.1f s CPU (%.0f%%), whole system busy %.0f%%\n", what, wall, self,
             wall > 0 ? 100 * self / wall : 0, wall > 0 ? 100 * busy / wall : 0);
    for (int k = 0; k < 3; k++) { /* the busiest other processes over the interval */
        int best = -1; unsigned long long bt = 0;
        for (int i = 0; i < nb; i++) {
            if (b[i].pid == getpid() || b[i].t == (unsigned long long)-1) continue;
            unsigned long long t0 = 0;
            for (int j = 0; j < sNProcA; j++) if (sProcA[j].pid == b[i].pid) { t0 = sProcA[j].t; break; }
            if (b[i].t - t0 > bt) { bt = b[i].t - t0; best = i; }
        }
        if (best < 0 || bt == 0) break;
        port_log("DIAG %s:   other: pid %d %s %.1f s CPU\n", what, b[best].pid, b[best].name, bt / hz);
        b[best].t = (unsigned long long)-1;
    }
    { /* round 9: user vs kernel time, page faults, context switches (kernel work is invisible to qemu counts) */
        struct rusage r;
        getrusage(RUSAGE_SELF, &r);
        port_log("DIAG %s: user %.2f s, system %.2f s, page faults %ld minor / %ld major, context switches %ld voluntary / %ld forced\n",
                 what, tv_s(sRuA.ru_utime, r.ru_utime), tv_s(sRuA.ru_stime, r.ru_stime), r.ru_minflt - sRuA.ru_minflt,
                 r.ru_majflt - sRuA.ru_majflt, r.ru_nvcsw - sRuA.ru_nvcsw, r.ru_nivcsw - sRuA.ru_nivcsw);
    }
    log_file(what, "/proc/cpu/alignment", 3);
}
static void diag_start(void) {
    log_file("cpu", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1);
    log_file("cpu", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", 1);
    log_file("cpu", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", 1);
    log_file("load", "/proc/loadavg", 1);
    log_file("start", "/proc/cpu/alignment", 6);
    { /* a dependent integer chain: about one cycle per step on the Cortex-A7 */
        volatile uint32_t sink;
        uint32_t x = 1, t0 = port_time_us();
        for (uint32_t i = 0; i < 20000000u; i++) x = x * 1664525u + 1013904223u;
        sink = x; (void)sink;
        const uint32_t us = port_time_us() - t0;
        port_log("DIAG calib: 20M dependent mul-adds in %u us (~%.0f M steps/s; ~2 cycles each on a Cortex-A7 at 1.2 GHz -> ~600)\n", us, us ? 20000.0 / (us / 1000.0) : 0);
    }
    { /* memory bandwidth */
        uint8_t *m = malloc(8 << 20);
        if (m) {
            memset(m, 1, 8 << 20);
            const uint32_t t0 = port_time_us();
            for (int r = 0; r < 4; r++) { memset(m, r, 8 << 20); __asm__ volatile("" : : "r"(m) : "memory"); } /* keep the stores */
            const uint32_t us = port_time_us() - t0;
            volatile uint8_t keep = m[12345]; (void)keep;
            port_log("DIAG calib: memset 32 MB in %u us (%.0f MB/s)\n", us, us ? 32.0 * 1e6 / us : 0);
            free(m);
        }
    }
}

typedef struct { double frame, interp, r3d, r2d, tex, clr, pres; uint32_t worst, frames, over; } Stats;

/* Device scene order (round 13): the user tends to stop after a scene or two, so the slow scenes go first, each
 * followed by its combiner check. Scenes not listed follow in name order. */
static int scene_rank(const char *path) {
    static const char *const order[] = { "7_battle", "3_letter", "8_battle_desert", "2_talk", "6_castle_hud", "4_toadtown", "5_castle", "1_field" };
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    for (int k = 0; k < (int)(sizeof order / sizeof *order); k++) {
        const size_t l = strlen(order[k]);
        if (!strncmp(b, order[k], l) && b[l] == '.') return k;
    }
    return 100;
}
/* The scenes with the most generic 2-cycle draws (3_letter 712 a pass, 7_battle 210, 8_battle_desert 120) run again
 * with it forced off (pm_gen_forbid, "/nogen"): wrong colours, same geometry, so the difference is its device cost. */
static bool gen_heavy(const char *name) { return !strcmp(name, "3_letter") || !strcmp(name, "7_battle") || !strcmp(name, "8_battle_desert"); }

static void bench_scene(Scene *s, double seconds, const char *shotdir) {
    /* Warm-up pass (texture cache fill, first-touch page faults) and screenshots. */
    diag_begin();
    extern uint32_t gfx_soft_phase_switches, gfx_pm64_main_cimg;
    const uint32_t sw0 = gfx_soft_phase_switches;
    uint32_t fb_prev = 0;
    for (uint32_t i = 0; i < s->ntask && !quit_requested(); i++) {
        run_task(&s->task[i]);
        if (!s->stamped && i > 0 && gfx_pm64_main_cimg != fb_prev) s->task[i].fstart = true; /* a new framebuffer opens a frame */
        fb_prev = gfx_pm64_main_cimg;
        /* one present per game frame; without stamps the next task's flag is not known yet, so this pass presents once */
        if (i + 1 == s->ntask || s->task[i + 1].fstart) present();
        if (shotdir) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%03u.ppm", shotdir, s->name, i);
            save_ppm(path);
        }
    }
    s->nframes = 0;
    for (uint32_t i = 0; i < s->ntask; i++) s->nframes += s->task[i].fstart;
    if (!s->stamped && s->nframes == 1 && s->ntask > 1) { /* one framebuffer throughout: fall back to one task a frame */
        for (uint32_t i = 0; i < s->ntask; i++) s->task[i].fstart = true;
        s->nframes = s->ntask;
    }
    port_log("SCENE %-16s %u gfx tasks = %u game frames (by %s)\n", s->name, s->ntask, s->nframes,
             s->stamped ? "poll stamp" : s->nframes == s->ntask ? "task" : "framebuffer");
    if (getenv("PM64_FRAMELOG")) { /* '|' = the task starts a frame */
        char pat[257]; uint32_t k = 0;
        for (; k < s->ntask && k < 256; k++) pat[k] = s->task[k].fstart ? '|' : '.';
        pat[k] = 0;
        port_log("SCENE %-16s frames %s\n", s->name, pat);
    }
    Stats st = { 0 };
    const uint32_t start = port_time_us();
    uint32_t i = 0;
    while (port_time_us() - start < (uint32_t)(seconds * 1e6) && !quit_requested()) {
        /* one game frame = its tasks up to the next frame start */
        uint32_t frame_us = 0, pres_us = 0;
        memset(gPortProfFrame, 0, sizeof gPortProfFrame);
        do { /* all the frame's tasks draw into one framebuffer: present once, at the end */
            frame_us += run_task(&s->task[i]);
            i = (i + 1) % s->ntask;
        } while (i != 0 && !s->task[i].fstart && !quit_requested());
        {
            const uint32_t tp = port_time_us();
            present();
            pres_us += port_time_us() - tp;
        }
        const uint32_t backend = gPortProfFrame[PROF_R3D] + gPortProfFrame[PROF_R2D] + gPortProfFrame[PROF_TEX] +
                                 gPortProfFrame[PROF_CLR] + gPortProfFrame[PROF_COMP];
        st.frame += frame_us + pres_us;
        st.interp += frame_us > backend ? frame_us - backend : 0;
        st.r3d += gPortProfFrame[PROF_R3D]; st.r2d += gPortProfFrame[PROF_R2D]; st.tex += gPortProfFrame[PROF_TEX];
        st.clr += gPortProfFrame[PROF_CLR] + gPortProfFrame[PROF_COMP]; st.pres += pres_us;
        if (frame_us + pres_us > st.worst) st.worst = frame_us + pres_us;
        if (frame_us + pres_us > 33333) st.over++; /* missed the 30 fps budget */
        st.frames++;
    }
    const double n = st.frames ? st.frames : 1;
    const double avg = st.frame / n / 1000.0;
    port_log("SCENE %-16s frames %5u  avg %6.2f ms (%5.1f fps)  over33 %3.0f%%  worst %6.2f ms | interp %5.2f r3d %5.2f r2d %5.2f tex %5.2f clr %5.2f present %5.2f\n",
             s->name, st.frames, avg, avg > 0 ? 1000.0 / avg : 0, 100.0 * st.over / n, st.worst / 1000.0, st.interp / n / 1000, st.r3d / n / 1000,
             st.r2d / n / 1000, st.tex / n / 1000, st.clr / n / 1000, st.pres / n / 1000);
    {
        extern size_t gfx_soft_arena_peak; extern size_t gfx_soft_arena_size(void); extern uint32_t gfx_soft_phase_switches;
        port_log("SCENE %-16s tex arena peak %zu KB of %zu KB (%.0f%%), 2D/3D switches %.1f per frame\n", s->name,
                 gfx_soft_arena_peak / 1024, gfx_soft_arena_size() / 1024, 100.0 * gfx_soft_arena_peak / gfx_soft_arena_size(),
                 st.frames ? (double)(gfx_soft_phase_switches - sw0) / (st.frames + s->nframes) : 0); /* timed + warm-up frames */
    }
    diag_end(s->name);
}

int main(int argc, char **argv) {
    double seconds = 5; /* per scene: ~150 game frames on the device (round 13; was 10) */
    const char *shotdir = NULL, *home = getenv("HOME");
    char scenedir[512];
    snprintf(scenedir, sizeof scenedir, "%s/.pm64bench", home ? home : ".");
    int first = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) shotdir = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) snprintf(scenedir, sizeof scenedir, "%s", argv[++i]);
        else { first = i; break; }
    }
    mkdir(scenedir, 0755);
    char logpath[600];
    snprintf(logpath, sizeof logpath, "%s/bench.log", scenedir);
    sLog = fopen(logpath, "w");
    extern const char pm64bench_stamp[]; /* bench/Makefile writes it at each link: the time and the commit */
    port_log("pm64bench M1 (%s), %.0f s per scene, scenes from %s\n", pm64bench_stamp, seconds, first < argc ? "argv" : scenedir);
    mem_report("start");
    diag_start();

    { const char *e = getenv("PM64_FIT"); if (e && !strcmp(e, "crop")) sFitCrop = 1; }
    { const char *e = getenv("PM64_CROP_X"); if (e) { sCropX = atoi(e); if (sCropX < 0) sCropX = 0; if (sCropX > 80) sCropX = 80; } }
    { const char *e = getenv("PM64_SCREEN"); int w, h; if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 640 && h <= 480) { sOutW = w; sOutH = h; } }
    port_log("render %dx%d, fit %s\n", sOutW, sOutH, sOutW == SCREEN ? "none" : sFitCrop ? "crop (1:1)" : "squeeze (area filter)");
    { extern int gfx_trace_frames; const char *e = getenv("PM64_TRACE"); if (e) gfx_trace_frames = atoi(e); } /* kit display-list trace, N tasks */
    pm64_rdram = aligned_alloc(64, RDRAM_BYTES);
    memset(pm64_rdram, 0, RDRAM_BYTES);
    present_init();
    gfx_init(&bench_wapi, &gfx_soft_api, "pm64bench", false);
    mem_report("after init");

    char *paths[64];
    int npaths = 0;
    if (first < argc) {
        for (int i = first; i < argc && npaths < 64; i++) paths[npaths++] = argv[i];
    } else {
        DIR *d = opendir(scenedir);
        struct dirent *e;
        while (d && (e = readdir(d)) && npaths < 64) {
            size_t l = strlen(e->d_name);
            if (e->d_name[0] != '.' && l > 4 && !strcmp(e->d_name + l - 4, ".pms")) { /* skips macOS ._ files */
                paths[npaths] = malloc(strlen(scenedir) + l + 2);
                sprintf(paths[npaths++], "%s/%s", scenedir, e->d_name);
            }
        }
        if (d) closedir(d);
        for (int i = 0; i < npaths; i++) /* the slow scenes first, then name order (round 13) */
            for (int j = i + 1; j < npaths; j++) {
                const int ri = scene_rank(paths[i]), rj = scene_rank(paths[j]);
                if (ri > rj || (ri == rj && strcmp(paths[i], paths[j]) > 0)) { char *t = paths[i]; paths[i] = paths[j]; paths[j] = t; }
            }
    }
    if (!npaths) {
        port_log("no scenes (*.pms) found in %s\n", scenedir);
        ui_draw("M1 benchmark", "No bench scenes found.", "Unzip pm64bench_scenes.zip into", "the bench's data folder.", -1, "Press any button to quit.");
        ui_wait_button();
    }
    if (npaths) { /* round 13: the user took the looping first clip for the game hanging */
        char l2[48], l3[48];
        const double runs = npaths + 8 + 12; /* + 3 combiner checks + 2 NEON checks + 3 row checks + 12 speed-round checks */
        snprintf(l2, sizeof l2, "%d clips play in turn, %.0f s each.", npaths, seconds);
        snprintf(l3, sizeof l3, "Leave it running (~%.0f min).", runs * (seconds + 1.5) / 60.0 + 0.49);
        ui_draw("M1 benchmark", "A speed test, not the game yet.", l2, l3, -1, "MENU quits.");
        ui_hold(3500);
    }
    /* PM64_BENCH_CHECKS=0: the scenes alone, without the check runs below (qemu instruction profiles of a scene) */
    const bool checks = !(getenv("PM64_BENCH_CHECKS") && getenv("PM64_BENCH_CHECKS")[0] == '0');
    for (int i = 0; i < npaths && !quit_requested(); i++) {
        Scene s = { 0 };
        if (!load_scene(paths[i], &s)) continue;
        port_log("scene %s: %u gfx tasks\n", s.name, s.ntask);
        {
            char l1[48], l3[48];
            snprintf(l1, sizeof l1, "Scene %d of %d", i + 1, npaths);
            snprintf(l3, sizeof l3, "Loops for %.0f s, then the next.", seconds);
            ui_draw("M1 benchmark", l1, s.name, l3, 100 * i / npaths, "MENU quits.");
            ui_hold(1200);
        }
        bench_scene(&s, seconds, shotdir);
        mem_report(s.name);
        char name[64];
        snprintf(name, sizeof name, "%s", s.name);
        free_scene(&s);
        if (checks && gen_heavy(name) && !quit_requested()) { /* its combiner check right away, so a short run has both (round 13) */
            extern int pm_gen_forbid;
            Scene g = { 0 };
            if (load_scene(paths[i], &g)) {
                strncat(g.name, "/nogen", sizeof g.name - strlen(g.name) - 1);
                ui_draw("M1 benchmark", "Combiner check", g.name, "Wrong colours on purpose.", 100 * i / npaths, "MENU quits.");
                ui_hold(1200);
                pm_gen_forbid = 1;
                bench_scene(&g, seconds, NULL);
                pm_gen_forbid = 0;
                free_scene(&g);
            }
        }
    }
    /* NEON A/B on the device (qemu's wall time cannot judge NEON): the two scenes with the most
     * bilinear texture x shade pixels again, with the scalar span loop (PM64_CMNEON=0 in-process). */
    {
        extern int gfx_soft_set_cmneon(int on);
        for (int i = 0; i < npaths && checks && !quit_requested() && gfx_soft_set_cmneon(0) >= 0; i++) {
            if (!strstr(paths[i], "1_field") && !strstr(paths[i], "5_castle")) continue;
            Scene s = { 0 };
            if (!load_scene(paths[i], &s)) continue;
            strncat(s.name, "/scalar", sizeof s.name - strlen(s.name) - 1);
            ui_draw("M1 benchmark", "NEON check", s.name, "", 100, "MENU quits.");
            bench_scene(&s, seconds, NULL);
            free_scene(&s);
        }
        gfx_soft_set_cmneon(1);
    }
    /* The rows of their own (PM64_CMSPLIT, the final exact speed check, 2026-10-03): the field, the castle and the
     * battle again with the rows inside cm_row_neon as before ("/nosplit", the same pixels); the difference to the
     * plain run is the change's device cost. */
    {
        extern int gfx_soft_set_cmsplit(int on);
        for (int i = 0; i < npaths && checks && !quit_requested() && gfx_soft_set_cmsplit(0) >= 0; i++) {
            if (!strstr(paths[i], "1_field") && !strstr(paths[i], "5_castle") && !strstr(paths[i], "7_battle")) continue;
            Scene s = { 0 };
            if (!load_scene(paths[i], &s)) continue;
            strncat(s.name, "/nosplit", sizeof s.name - strlen(s.name) - 1);
            ui_draw("M1 benchmark", "Row check", s.name, "", 100, "MENU quits.");
            bench_scene(&s, seconds, NULL);
            free_scene(&s);
        }
        gfx_soft_set_cmsplit(1);
    }
    /* The speed round (2026-10-07, from super-mario-64's): the field, the letter, the castle and the battle again with
     * its switches off in-process, the same pixels: "/speed-off" both (the build before the round), then each alone
     * ("/fastrows-off", "/zskip-off"). Each one's device gain = its run's avg against the plain run. */
    {
        extern int gfx_soft_set_speedsw(int mask);
        static const struct { const char *tag; int mask; } kOff[] = {
            { "/speed-off", 0 }, { "/fastrows-off", 2 }, { "/zskip-off", 1 } };
        const int on = gfx_soft_set_speedsw(3);
        for (int i = 0; i < npaths && checks && !quit_requested() && on >= 0; i++) {
            if (!strstr(paths[i], "1_field") && !strstr(paths[i], "3_letter") && !strstr(paths[i], "5_castle") &&
                !strstr(paths[i], "7_battle")) continue;
            for (int w = 0; w < 3 && !quit_requested(); w++) {
                Scene s = { 0 };
                if (!load_scene(paths[i], &s)) continue;
                strncat(s.name, kOff[w].tag, sizeof s.name - strlen(s.name) - 1);
                ui_draw("M1 benchmark", "Speed check", s.name, "", 100, "MENU quits.");
                gfx_soft_set_speedsw(kOff[w].mask);
                bench_scene(&s, seconds, NULL);
                gfx_soft_set_speedsw(on);
                free_scene(&s);
            }
        }
    }
    port_log("done\n");
    if (npaths && !sQuitSeen()) ui_draw("M1 benchmark", "Done.", "The log is in", "the bench's data folder.", 100, "");
    if (sLog) fclose(sLog);
#ifdef PM64_SDL12
    SDL_Quit();
#endif
    return 0;
}
