/* miyoo_hwblit: see miyoo_hwblit.h. The game frame is copied (alpha forced) into memory the 2D engine reads, and
 * the engine scales and turns it into the panel's back buffer, which the port's SDL then flips as before.
 *
 * Found elsewhere on the Mini (other open ports and Onion's tools): MI_SYS_MMA_Alloc + MI_SYS_Mmap give memory with
 * a physical address; /dev/fb0's smem_start is an address the engine takes as is; some Miyoo SDL builds keep their
 * screen surface's physical address in SDL_Surface.unused1; the engine turns 180 degrees with ROTATE_180 (or
 * MIRROR_BOTH on some firmware), and its scaler blends neighbouring pixels. Each of these is checked here before
 * use, on the device itself, and anything unexpected leaves the CPU drawing. */
#if defined(MIYOO) || defined(MIYOO_HWBLIT_TEST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "miyoo_hwblit.h"
#if defined(MIYOO_HWBLIT_TEST)
#define SAVESTATE_EXCLUDE
#else
#include "savestate.h"
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* The engine's types, as libmi_gfx.so takes them (ARM EABI: 4-byte enums, an 8-byte-aligned 64-bit address). */
typedef struct { int32_t x, y; uint32_t w, h; } HwRect;
typedef struct { uint64_t phys; int32_t fmt; uint32_t w, h, stride; } HwSurface;
typedef struct { uint8_t on; int32_t op, fmt; uint32_t lo, hi; } HwColorKey;
typedef struct {
    HwRect clip;
    HwColorKey srckey, dstkey;
    int32_t srcop, dstop, mirror, rotate, blendflags;
    uint32_t srcconst, dstconst;
} HwOpt;
#define HW_FMT_ARGB8888 11
#define HW_BLD_ZERO 0
#define HW_BLD_ONE 1
#if __has_include(<mi_gfx.h>) && !defined(MIYOO_HWBLIT_NO_SDK_CHECK)
/* the toolchain's own headers, when it has them: the mirror above must match them byte for byte */
#include <mi_gfx.h>
#include <stddef.h>
_Static_assert(sizeof(HwRect) == sizeof(MI_GFX_Rect_t), "rect");
_Static_assert(sizeof(HwSurface) == sizeof(MI_GFX_Surface_t), "surface");
_Static_assert(offsetof(HwSurface, stride) == offsetof(MI_GFX_Surface_t, u32Stride), "surface stride");
_Static_assert(sizeof(HwOpt) == sizeof(MI_GFX_Opt_t), "opt");
_Static_assert(offsetof(HwOpt, rotate) == offsetof(MI_GFX_Opt_t, eRotate), "opt rotate");
_Static_assert(offsetof(HwOpt, mirror) == offsetof(MI_GFX_Opt_t, eMirror), "opt mirror");
_Static_assert(offsetof(HwOpt, srcconst) == offsetof(MI_GFX_Opt_t, u32GlobalSrcConstColor), "opt const");
_Static_assert(HW_FMT_ARGB8888 == E_MI_GFX_FMT_ARGB8888, "fmt");
_Static_assert(HW_BLD_ONE == E_MI_GFX_DFB_BLD_ONE && HW_BLD_ZERO == E_MI_GFX_DFB_BLD_ZERO, "bld");
#endif

static struct {
    int32_t (*sys_init)(void);
    int32_t (*mma_alloc)(uint8_t *heap, uint32_t bytes, uint64_t *phys);
    int32_t (*mma_free)(uint64_t phys);
    int32_t (*mmap)(uint64_t phys, uint32_t bytes, void **va, uint8_t cached);
    int32_t (*munmap)(void *va, uint32_t bytes);
    int32_t (*flush)(void *va, uint32_t bytes);
    int32_t (*gfx_open)(void);
    int32_t (*blit)(HwSurface *s, HwRect *sr, HwSurface *d, HwRect *dr, HwOpt *o, uint16_t *fence);
    int32_t (*wait)(uint8_t all, uint16_t fence);
} mi SAVESTATE_EXCLUDE;
static int sOpen SAVESTATE_EXCLUDE; /* 0 not tried, 1 open, -1 failed */
static const char *sOpenWhy SAVESTATE_EXCLUDE = "";

static void *open_lib(const char *name) {
    static const char *const dirs[] = { "", "/customer/lib/", "/config/lib/", "/lib/", "/usr/lib/" };
    char path[96];
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (h) return h;
    }
    return NULL;
}

int miyoo_hwblit_open(const char **why) {
    if (sOpen == 0) {
        void *hs = open_lib("libmi_sys.so"), *hg = open_lib("libmi_gfx.so");
        sOpen = -1;
        if (!hs || !hg) sOpenWhy = "no libmi_sys.so / libmi_gfx.so";
        else {
            *(void **)&mi.sys_init = dlsym(hs, "MI_SYS_Init");
            *(void **)&mi.mma_alloc = dlsym(hs, "MI_SYS_MMA_Alloc");
            *(void **)&mi.mma_free = dlsym(hs, "MI_SYS_MMA_Free");
            *(void **)&mi.mmap = dlsym(hs, "MI_SYS_Mmap");
            *(void **)&mi.munmap = dlsym(hs, "MI_SYS_Munmap");
            *(void **)&mi.flush = dlsym(hs, "MI_SYS_FlushInvCache");
            *(void **)&mi.gfx_open = dlsym(hg, "MI_GFX_Open");
            *(void **)&mi.blit = dlsym(hg, "MI_GFX_BitBlit");
            *(void **)&mi.wait = dlsym(hg, "MI_GFX_WaitAllDone");
            if (!mi.sys_init || !mi.mma_alloc || !mi.mma_free || !mi.mmap || !mi.munmap || !mi.flush || !mi.gfx_open ||
                !mi.blit || !mi.wait)
                sOpenWhy = "a MI_SYS / MI_GFX function is missing";
            else {
                /* the Mini's SDL has usually done both already: a second call is harmless, and a refusal is not
                 * final (the checks that follow decide) */
                const int32_t rs = mi.sys_init(), rg = mi.gfx_open();
                if (rs || rg) fprintf(stderr, "hwblit: MI_SYS_Init %d, MI_GFX_Open %d (carrying on: the checks decide)\n", (int)rs, (int)rg);
                sOpen = 1;
            }
        }
    }
    if (why) *why = sOpenWhy;
    return sOpen > 0;
}

void *miyoo_hwblit_alloc(uint32_t bytes, uint64_t *phys) {
    void *va = NULL;
    uint64_t p = 0;
    bytes = (bytes + 4095u) & ~4095u;
    if (sOpen <= 0 || mi.mma_alloc(NULL, bytes, &p) != 0 || !p) return NULL;
    if (mi.mmap(p, bytes, &va, 1) != 0 || !va) { mi.mma_free(p); return NULL; }
    *phys = p;
    return va;
}
void miyoo_hwblit_free(void *va, uint64_t phys, uint32_t bytes) {
    bytes = (bytes + 4095u) & ~4095u;
    if (sOpen <= 0) return;
    if (va) mi.munmap(va, bytes);
    if (phys) mi.mma_free(phys);
}
void miyoo_hwblit_flush(void *va, uint32_t bytes) { if (sOpen > 0 && va && bytes) mi.flush(va, bytes); }

static uint16_t sFence SAVESTATE_EXCLUDE;
static int blit_nowait(uint64_t sp, int sw, int sh, int sstride, int sx, int sy, int srw, int srh, uint64_t dp, int dw,
                       int dh, int dstride, int dx, int dy, int drw, int drh, int rotate, int mirror) {
    HwSurface s = { sp, HW_FMT_ARGB8888, (uint32_t)sw, (uint32_t)sh, (uint32_t)sstride };
    HwSurface d = { dp, HW_FMT_ARGB8888, (uint32_t)dw, (uint32_t)dh, (uint32_t)dstride };
    HwRect sr = { sx, sy, (uint32_t)srw, (uint32_t)srh }, dr = { dx, dy, (uint32_t)drw, (uint32_t)drh };
    HwOpt o;
    memset(&o, 0, sizeof(o));
    o.clip = dr;
    o.srcop = HW_BLD_ONE; /* a plain copy: the source as it is, nothing of what was there */
    o.dstop = HW_BLD_ZERO;
    o.mirror = mirror;
    o.rotate = rotate;
    return mi.blit(&s, &sr, &d, &dr, &o, &sFence) != 0 ? -1 : 0;
}
static void blit_wait(void) { mi.wait(1, sFence); }
int miyoo_hwblit_blit(uint64_t sp, int sw, int sh, int sstride, int sx, int sy, int srw, int srh, uint64_t dp, int dw,
                      int dh, int dstride, int dx, int dy, int drw, int drh, int rotate, int mirror) {
    if (sOpen <= 0) return -1;
    if (blit_nowait(sp, sw, sh, sstride, sx, sy, srw, srh, dp, dw, dh, dstride, dx, dy, drw, drh, rotate, mirror)) return -1;
    blit_wait();
    return 0;
}

/* ---- the 2x ways. src: w x h (stride w * 4) at sp; the destination's 2w x 2h rectangle at (tx, ty) (already turned)
 * in a dw x dh surface of stride dstride at dp. turn: 0 none, 1 rotate 180, 2 mirror both. tmp/tmp2: way 2's two
 * w*h*4 and w*h*8 buffers. Rows doubled by stride: a surface of every other row (stride twice the panel's) at row
 * parity p is written 1:1 twice, once per parity. */
static int turn_rot(int turn) { return turn == 1 ? 2 : 0; }
static int turn_mir(int turn) { return turn == 2 ? 3 : 0; }
static int rows_half(uint64_t dp, int dw, int dh, int dstride, int ty, int i, uint64_t *vp, int *vy) {
    const int row0 = ty + i;
    *vp = dp + (uint64_t)(row0 & 1) * (uint64_t)dstride;
    *vy = row0 >> 1;
    (void)dw; (void)dh;
    return 0;
}
static int x2_way(int how, uint64_t sp, int w, int h, uint64_t dp, int dw, int dh, int dstride, int tx, int ty, int turn,
                  uint64_t tmp, uint64_t tmp2) {
    uint64_t vp;
    int vy, i;
    if (how == 0) /* one scaled blit */
        return blit_nowait(sp, w, h, w * 4, 0, 0, w, h, dp, dw, dh, dstride, tx, ty, 2 * w, 2 * h, turn_rot(turn), turn_mir(turn));
    if (how == 1) { /* rows by stride, columns by the scaler */
        for (i = 0; i < 2; i++) {
            rows_half(dp, dw, dh, dstride, ty, i, &vp, &vy);
            if (blit_nowait(sp, w, h, w * 4, 0, 0, w, h, vp, dw, dh / 2, dstride * 2, tx, vy, 2 * w, h, turn_rot(turn), turn_mir(turn))) return -1;
        }
        return 0;
    }
    /* how 2: a quarter turn (columns become rows: h x w), its rows doubled by stride (h x 2w), the other quarter turn
     * (three quarters, or one with the 180 turn on top) back into the panel's rows doubled by stride: only 1:1 copies.
     * Which way the engine counts its quarter turns does not matter: 90 then 270 is none, 90 then 90 is 180. */
    if (blit_nowait(sp, w, h, w * 4, 0, 0, w, h, tmp, h, w, h * 4, 0, 0, h, w, 1, 0)) return -1;
    for (i = 0; i < 2; i++)
        if (blit_nowait(tmp, h, w, h * 4, 0, 0, h, w, tmp2 + (uint64_t)i * (uint64_t)h * 4u, h, w, h * 8, 0, 0, h, w, 0, 0)) return -1;
    for (i = 0; i < 2; i++) {
        rows_half(dp, dw, dh, dstride, ty, i, &vp, &vy);
        if (blit_nowait(tmp2, h, 2 * w, h * 4, 0, 0, h, 2 * w, vp, dw, dh / 2, dstride * 2, tx, vy, 2 * w, h, turn ? 1 : 3, 0)) return -1;
    }
    return 0;
}

/* the CPU's picture (miyoo_panel.c's 2x, turned or not) of pixel (x, y) of a 2w x 2h destination */
static uint32_t ref_x2(const uint32_t *src, int w, int h, int x, int y, int rot) {
    if (rot) { x = 2 * w - 1 - x; y = 2 * h - 1 - y; }
    return src[(y / 2) * w + x / 2];
}
static uint32_t pattern(int i) {
    uint32_t v = (uint32_t)i * 2654435761u;
    v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15;
    return v | 0xFF000000u;
}
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

long miyoo_hwblit_check_x2(int how, int w, int h, int rot, int rotmirror, double *ms) {
    const uint32_t sb = (uint32_t)w * h * 4, db = sb * 4;
    uint64_t sp = 0, dp = 0, t1 = 0, t2 = 0;
    uint32_t *s = miyoo_hwblit_alloc(sb, &sp), *d = miyoo_hwblit_alloc(db, &dp);
    void *v1 = how == 2 ? miyoo_hwblit_alloc(sb, &t1) : NULL, *v2 = how == 2 ? miyoo_hwblit_alloc(sb * 2, &t2) : NULL;
    long bad = -1;
    if (ms) *ms = 0;
    if (s && d && (how != 2 || (v1 && v2))) {
        for (int i = 0; i < w * h; i++) s[i] = pattern(i);
        memset(d, 0, db);
        mi.flush(s, sb);
        mi.flush(d, db);
        const double t0 = now_ms();
        if (x2_way(how, sp, w, h, dp, 2 * w, 2 * h, 2 * w * 4, 0, 0, rot ? (rotmirror ? 2 : 1) : 0, t1, t2) == 0) {
            blit_wait();
            if (ms) *ms = now_ms() - t0;
            mi.flush(d, db);
            bad = 0;
            for (int y = 0; y < 2 * h; y++)
                for (int x = 0; x < 2 * w; x++)
                    if (d[y * 2 * w + x] != ref_x2(s, w, h, x, y, rot)) bad++;
        } else blit_wait(); /* (a way's first blits may have gone out) */
    }
    if (v2) miyoo_hwblit_free(v2, t2, sb * 2);
    if (v1) miyoo_hwblit_free(v1, t1, sb);
    if (d) miyoo_hwblit_free(d, dp, db);
    if (s) miyoo_hwblit_free(s, sp, sb);
    return bad;
}

int miyoo_hwblit_check_turn(void) {
    const int w = 16, h = 8;
    uint64_t sp = 0, dp = 0;
    uint32_t *s = miyoo_hwblit_alloc(4096, &sp), *d = miyoo_hwblit_alloc(4096, &dp);
    int ok = -1, ran = 0;
    if (s && d) {
        ok = 0;
        for (int i = 0; i < w * h; i++) s[i] = pattern(i + 7);
        for (int t = 1; t <= 2; t++) {
            memset(d, 0, 4096);
            mi.flush(s, 4096);
            mi.flush(d, 4096);
            if (miyoo_hwblit_blit(sp, w, h, w * 4, 0, 0, w, h, dp, w, h, w * 4, 0, 0, w, h, turn_rot(t), turn_mir(t))) continue;
            ran++;
            mi.flush(d, 4096);
            int bad = 0;
            for (int i = 0; i < w * h; i++) if (d[i] != s[w * h - 1 - i]) bad++;
            if (!bad) ok |= 1 << (t - 1);
        }
        if (!ran) ok = -2;
    }
    if (d) miyoo_hwblit_free(d, dp, 4096);
    if (s) miyoo_hwblit_free(s, sp, 4096);
    return ok;
}

/* ---- where the panel's buffer is */
static struct { uintptr_t lo, hi; uint64_t base; } sMap SAVESTATE_EXCLUDE; /* a /dev/fb0 mapping: VA lo..hi is base.. */
static int map_fb(uintptr_t va, char *desc, int desclen) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[256];
    int found = 0;
    if (!f) { snprintf(desc, desclen, "no /proc/self/maps"); return 0; }
    snprintf(desc, desclen, "the buffer is in no mapping");
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi, off;
        char perm[8], path[160] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %159s", &lo, &hi, perm, &off, path) < 4) continue;
        if (va < lo || va >= hi) continue;
        snprintf(desc, desclen, "the buffer is in %lx-%lx +%lx %s", lo, hi, off, path[0] ? path : "(anonymous)");
        if (strcmp(path, "/dev/fb0") == 0) {
            const int fd = open("/dev/fb0", O_RDONLY);
            struct fb_fix_screeninfo fi;
            if (fd >= 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0 && fi.smem_start) {
                sMap.lo = lo; sMap.hi = hi; sMap.base = (uint64_t)fi.smem_start + off;
                found = 1;
            }
            if (fd >= 0) close(fd);
        }
        break;
    }
    fclose(f);
    return found;
}
int miyoo_hwblit_locate(const MiyooHwPanel *p, uint64_t *phys, char *desc, int desclen) {
    const uintptr_t va = (uintptr_t)p->pixels;
    if (p->phys_hint) {
        *phys = p->phys_hint;
        snprintf(desc, desclen, "the SDL surface's unused1 (%08x)", (unsigned)p->phys_hint);
        return 1;
    }
    if (!(va >= sMap.lo && va < sMap.hi) && !map_fb(va, desc, desclen)) return 0;
    *phys = sMap.base + (va - sMap.lo);
    snprintf(desc, desclen, "/dev/fb0 mapped at %lx (physical %llx)", (unsigned long)sMap.lo, (unsigned long long)sMap.base);
    return 2;
}

/* ---- the game's frames */
static int sState SAVESTATE_EXCLUDE;          /* 0 not set up, 1 on, -1 off for good */
static int sTurn SAVESTATE_EXCLUDE = -1;      /* the turn that works: 1 rotate, 2 mirror (when the picture is turned) */
static int sHow SAVESTATE_EXCLUDE = -1, sHowW SAVESTATE_EXCLUDE, sHowH SAVESTATE_EXCLUDE; /* nearest: the exact way at w x h; -2 none */
static int sWhere SAVESTATE_EXCLUDE;          /* miyoo_hwblit_locate's answer, checked once */
static uint32_t *sStage SAVESTATE_EXCLUDE;    /* the frame, opaque, where the engine reads it */
static uint64_t sStageP SAVESTATE_EXCLUDE, sTmpP SAVESTATE_EXCLUDE, sTmp2P SAVESTATE_EXCLUDE;
static void *sTmp SAVESTATE_EXCLUDE, *sTmp2 SAVESTATE_EXCLUDE;
static uint32_t sStageN SAVESTATE_EXCLUDE;    /* bytes allocated for each of stage, tmp; tmp2 twice that */

static uint8_t *sPendRow0 SAVESTATE_EXCLUDE;  /* a frame the engine may still be drawing: its rows in the panel */
static uint32_t sPendRows SAVESTATE_EXCLUDE;

int miyoo_hwblit_mode(const char *env, int cfg) {
    const char *e = env ? getenv(env) : NULL;
    if (!(e && e[0])) e = getenv("MIYOO_HWBLIT");
    if (e && e[0]) return e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0;
    return cfg == 1 || cfg == 2 ? cfg : 0;
}
int miyoo_hwblit_active(void) { return sState > 0; }

static void off_for_good(const char *why) {
    if (sState >= 0) fprintf(stderr, "hwblit: %s: the CPU draws the picture\n", why);
    sState = -1;
}

static void copy_opaque(uint32_t *d, const uint32_t *s, int n, uint32_t a) {
    int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32x4_t av = vdupq_n_u32(a);
    for (; i + 8 <= n; i += 8) {
        __builtin_prefetch(s + i + 64);
        vst1q_u32(d + i, vorrq_u32(vld1q_u32(s + i), av));
        vst1q_u32(d + i + 4, vorrq_u32(vld1q_u32(s + i + 4), av));
    }
#endif
    for (; i < n; i++) d[i] = s[i] | a;
}

static int stage_for(int w, int h) {
    const uint32_t need = ((uint32_t)w * h * 4 + 4095u) & ~4095u;
    if (need <= sStageN) return 1;
    if (sStage) miyoo_hwblit_free(sStage, sStageP, sStageN);
    if (sTmp) miyoo_hwblit_free(sTmp, sTmpP, sStageN);
    if (sTmp2) miyoo_hwblit_free(sTmp2, sTmp2P, sStageN * 2);
    sStage = sTmp = sTmp2 = NULL; sStageN = 0;
    sStage = miyoo_hwblit_alloc(need, &sStageP);
    if (!sStage) return 0;
    sStageN = need;
    return 1;
}
static int tmp_for_way2(void) {
    if (sTmp && sTmp2) return 1;
    sTmp = miyoo_hwblit_alloc(sStageN, &sTmpP);
    sTmp2 = miyoo_hwblit_alloc(sStageN * 2, &sTmp2P);
    return sTmp && sTmp2;
}

/* The engine writes a known strip into the panel's buffer at the address found, and the CPU reads it back there:
 * the address is the buffer's. The strip lies inside the frame's rectangle, drawn over right after. */
static int check_where(const MiyooHwPanel *p, uint64_t dp, int tx, int ty) {
    const int w = 64, h = 4;
    uint64_t sp = 0;
    uint32_t *s = miyoo_hwblit_alloc(4096, &sp);
    int bad = -1;
    if (!s) return -1;
    for (int i = 0; i < w * h; i++) s[i] = pattern(i + 99);
    mi.flush(s, 4096);
    uint8_t *row0 = (uint8_t *)p->pixels + (size_t)ty * p->pitch;
    mi.flush(row0, (uint32_t)(h * p->pitch));
    if (miyoo_hwblit_blit(sp, w, h, w * 4, 0, 0, w, h, dp, p->w, p->h, p->pitch, tx, ty, w, h, 0, 0) == 0) {
        mi.flush(row0, (uint32_t)(h * p->pitch));
        bad = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (((const uint32_t *)(row0 + (size_t)y * p->pitch))[tx + x] != s[y * w + x]) bad++;
    }
    miyoo_hwblit_free(s, sp, 4096);
    return bad;
}

static int setup(int mode, const MiyooHwPanel *p, int w, int h, int tx, int ty, int rot) {
    const char *why;
    char desc[200];
    uint64_t dp;
    if (!miyoo_hwblit_open(&why)) { off_for_good(why); return 0; }
    if (rot && sTurn < 0) {
        const int t = miyoo_hwblit_check_turn();
        if (t <= 0) {
            off_for_good(t == -1 ? "no memory for the engine (MMA)" : t == -2 ? "the engine refused to copy"
                                                                             : "the engine does not turn the picture right");
            return 0;
        }
        sTurn = (t & 1) ? 1 : 2;
    }
    if (!stage_for(w, h)) { off_for_good("no memory for the engine (MMA)"); return 0; }
    sWhere = miyoo_hwblit_locate(p, &dp, desc, sizeof(desc));
    if (!sWhere) {
        char m[260];
        snprintf(m, sizeof(m), "the panel's buffer has no address the engine can use (%s)", desc);
        off_for_good(m);
        return 0;
    }
    const long bad = check_where(p, dp, tx, ty);
    if (bad) {
        char m[300];
        snprintf(m, sizeof(m), "the engine's copy did not land in the panel's buffer (%s: %ld pixels off)", desc, bad);
        off_for_good(m);
        return 0;
    }
    fprintf(stderr, "hwblit: on (%s), the panel's buffer at %s, turned by %s\n", mode == 1 ? "nearest" : "smooth", desc,
            !rot ? "nothing" : sTurn == 1 ? "rotate 180" : "mirror both");
    sState = 1;
    return 1;
}

/* nearest: the first of the three ways whose 2x at w x h matches the CPU's exactly (checked once per size) */
static int nearest_way(int w, int h, int rot) {
    if (sHow != -1 && sHowW == w && sHowH == h) return sHow;
    sHowW = w; sHowH = h; sHow = -2;
    long bad[3];
    double ms[3];
    for (int how = 0; how < 3; how++) {
        bad[how] = miyoo_hwblit_check_x2(how, w, h, rot, sTurn == 2, &ms[how]);
        if (bad[how] == 0) { sHow = how; break; }
    }
    if (sHow >= 0)
        fprintf(stderr, "hwblit: nearest %dx%d: way %d (%s) matches the CPU's 2x exactly (%.2f ms)\n", w, h, sHow,
                sHow == 0 ? "one scaled blit" : sHow == 1 ? "rows by stride, columns scaled" : "two quarter turns",
                ms[sHow]);
    else
        fprintf(stderr, "hwblit: nearest %dx%d: no way matches the CPU's 2x (pixels off: %ld %ld %ld; -1 failed): the CPU "
                "draws this size\n", w, h, bad[0], bad[1], bad[2]);
    return sHow;
}

void miyoo_hwblit_reset(int how, int w, int h) {
    miyoo_hwblit_sync();
    sState = 0; sWhere = 0;
    sHow = how >= 0 ? how : -1; sHowW = w; sHowH = h;
}

int miyoo_hwblit_frame(int mode, const MiyooHwPanel *p, const uint32_t *src, int w, int h, int ox, int oy, int pw,
                       int ph, int rot, uint32_t amask) {
    miyoo_hwblit_sync(); /* (normally done by the flip already) */
    if (mode <= 0 || sState < 0 || !p || !p->pixels || !src || w <= 0 || h <= 0) return 0;
    const int x2 = pw == 2 * w && ph == 2 * h;
    if (!x2 && (mode == 1 || pw != p->w || ph != p->h)) {
        /* nearest is the 2x (the Mini v4 / Flip fill is not); a smooth fill with black sides: the CPU draws those */
        static int sSaid SAVESTATE_EXCLUDE;
        if (!sSaid++) fprintf(stderr, "hwblit: %dx%d onto %dx%d is not a 2x%s: the CPU draws it\n", w, h, pw, ph,
                              mode == 1 ? " (hwblit=2 lets the engine's smooth scaler fill the panel)" : " and leaves black sides");
        return 0;
    }
    if (ox < 0 || oy < 0 || ox + pw > p->w || oy + ph > p->h) return 0;
    const int tx = rot ? p->w - ox - pw : ox, ty = rot ? p->h - oy - ph : oy;
    if (sState == 0 && !setup(mode, p, w, h, tx, ty, rot)) return 0;
    const int turn = rot ? sTurn : 0;
    int how = 0;
    if (mode == 1) {
        how = nearest_way(w, h, rot);
        if (how < 0) return 0;
        if (how == 2 && !tmp_for_way2()) { off_for_good("no memory for the engine's quarter turns (MMA)"); return 0; }
    }
    if (!stage_for(w, h)) { off_for_good("no memory for the engine (MMA)"); return 0; }
    uint64_t dp;
    char desc[200];
    if (miyoo_hwblit_locate(p, &dp, desc, sizeof(desc)) != sWhere) { off_for_good("the panel's buffer moved"); return 0; }

    copy_opaque(sStage, src, w * h, amask);
    mi.flush(sStage, (uint32_t)w * h * 4);
    uint8_t *row0 = (uint8_t *)p->pixels + (size_t)ty * p->pitch;
    const uint32_t rows = (uint32_t)ph * (uint32_t)p->pitch;
    mi.flush(row0, rows); /* nothing the CPU wrote there earlier lands on the engine's picture later */
    int r;
    if (mode == 1)
        r = x2_way(how, sStageP, w, h, dp, p->w, p->h, p->pitch, tx, ty, turn, sTmpP, sTmp2P);
    else
        r = blit_nowait(sStageP, w, h, w * 4, 0, 0, w, h, dp, p->w, p->h, p->pitch, tx, ty, pw, ph, turn_rot(turn), turn_mir(turn));
    if (r) { blit_wait(); off_for_good("a blit failed"); return 0; }
    /* the engine works on while the caller goes on; miyoo_hwblit_sync (the flip) waits for it */
    sPendRow0 = row0; sPendRows = rows;
    return 1;
}

void miyoo_hwblit_sync(void) {
    if (!sPendRow0) return;
    blit_wait();
    mi.flush(sPendRow0, sPendRows); /* the CPU (a panel dump or check, an SDL that copies) reads the engine's picture */
    sPendRow0 = NULL;
}

#endif /* MIYOO || MIYOO_HWBLIT_TEST */
