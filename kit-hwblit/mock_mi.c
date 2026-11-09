/* mock_mi.c: a software stand-in for the Mini's libmi_sys.so / libmi_gfx.so, for checking miyoo_hwblit.c's logic under
 * qemu (kit-hwblit/qemu_check.sh), never shipped. Memory: malloc, "physical" address = the virtual one. The blit:
 * scale, then turn (ROTATE_90 clockwise, as the SDK's pictures show) or mirror, nearest by default; MOCK_FILTER=1
 * blends 2x2 like a smoothing scaler (so a scaled 2x is not exact). MOCK_ROTATE180=0: ROTATE_180 refused (the
 * MIRROR_BOTH fallback). Built twice, as libmi_sys.so and libmi_gfx.so (both carry everything). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int32_t x, y; uint32_t w, h; } HwRect;
typedef struct { uint64_t phys; int32_t fmt; uint32_t w, h, stride; } HwSurface;
typedef struct { uint8_t on; int32_t op, fmt; uint32_t lo, hi; } HwColorKey;
typedef struct {
    HwRect clip;
    HwColorKey srckey, dstkey;
    int32_t srcop, dstop, mirror, rotate, blendflags;
    uint32_t srcconst, dstconst;
} HwOpt;

int32_t MI_SYS_Init(void) { return 0; }
int32_t MI_SYS_MMA_Alloc(uint8_t *heap, uint32_t bytes, uint64_t *phys) {
    void *p = NULL;
    (void)heap;
    if (posix_memalign(&p, 4096, bytes)) return -1;
    *phys = (uint64_t)(uintptr_t)p;
    return 0;
}
int32_t MI_SYS_MMA_Free(uint64_t phys) { free((void *)(uintptr_t)phys); return 0; }
int32_t MI_SYS_Mmap(uint64_t phys, uint32_t bytes, void **va, uint8_t cached) { (void)bytes; (void)cached; *va = (void *)(uintptr_t)phys; return 0; }
int32_t MI_SYS_Munmap(void *va, uint32_t bytes) { (void)va; (void)bytes; return 0; }
int32_t MI_SYS_FlushInvCache(void *va, uint32_t bytes) { (void)va; (void)bytes; return 0; }
int32_t MI_GFX_Open(void) { return 0; }
int32_t MI_GFX_WaitAllDone(uint8_t all, uint16_t fence) { (void)all; (void)fence; return 0; }

static uint32_t px(const HwSurface *s, int x, int y) {
    return ((const uint32_t *)(uintptr_t)(s->phys + (uint64_t)y * s->stride))[x];
}
static uint32_t lerp(uint32_t a, uint32_t b, int f) { /* f of 256 toward b, per byte */
    uint32_t r = 0;
    for (int k = 0; k < 32; k += 8) r |= (uint32_t)(((int)((a >> k) & 255) * (256 - f) + (int)((b >> k) & 255) * f) >> 8) << k;
    return r;
}

int32_t MI_GFX_BitBlit(HwSurface *s, HwRect *sr, HwSurface *d, HwRect *dr, HwOpt *o, uint16_t *fence) {
    static uint16_t f;
    const int rot = o->rotate, quarter = rot == 1 || rot == 3;
    const int sw = (int)sr->w, sh = (int)sr->h, rw = quarter ? sh : sw, rh = quarter ? sw : sh; /* the turned source */
    const char *e = getenv("MOCK_FILTER"), *r180 = getenv("MOCK_ROTATE180");
    const int filt = e && e[0] == '1';
    if (rot == 2 && r180 && r180[0] == '0') return -1;
    if (s->fmt != 11 || d->fmt != 11 || sr->x < 0 || sr->y < 0 || sr->x + sw > (int)s->w || sr->y + sh > (int)s->h ||
        dr->x < 0 || dr->y < 0 || dr->x + (int)dr->w > (int)d->w || dr->y + (int)dr->h > (int)d->h)
        return -1;
    for (int v = 0; v < (int)dr->h; v++)
        for (int u = 0; u < (int)dr->w; u++) {
            /* where in the turned source (fixed point 8 bits), centre-aligned */
            const int fx = (int)(((2 * u + 1) * rw * 128) / (int)dr->w) - 128, fy = (int)(((2 * v + 1) * rh * 128) / (int)dr->h) - 128;
            int xs[2], ys[2];
            int ax = filt ? (fx < 0 ? 0 : fx) : ((u * rw / (int)dr->w) << 8), ay = filt ? (fy < 0 ? 0 : fy) : ((v * rh / (int)dr->h) << 8);
            xs[0] = ax >> 8; ys[0] = ay >> 8;
            xs[1] = xs[0] + 1 < rw ? xs[0] + 1 : rw - 1; ys[1] = ys[0] + 1 < rh ? ys[0] + 1 : rh - 1;
            uint32_t c[2][2];
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int x = xs[i], y = ys[j], sx, sy; /* turned (x, y) back to the source */
                    switch (rot) {
                    case 1: sx = y; sy = sh - 1 - x; break;
                    case 2: sx = sw - 1 - x; sy = sh - 1 - y; break;
                    case 3: sx = sw - 1 - y; sy = x; break;
                    default: sx = x; sy = y;
                    }
                    if (o->mirror & 1) sx = sw - 1 - sx;
                    if (o->mirror & 2) sy = sh - 1 - sy;
                    c[j][i] = px(s, sr->x + sx, sr->y + sy);
                }
            const uint32_t out = filt ? lerp(lerp(c[0][0], c[0][1], ax & 255), lerp(c[1][0], c[1][1], ax & 255), ay & 255) : c[0][0];
            ((uint32_t *)(uintptr_t)(d->phys + (uint64_t)(dr->y + v) * d->stride))[dr->x + u] = out;
        }
    *fence = ++f;
    return 0;
}
