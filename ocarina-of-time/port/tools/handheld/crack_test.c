/* crack_test.c: OOT_CRACKSCAN (gfx_soft.c crack_fill's prefiltered scan, 2026-10-03) against the old scan, byte for
 * byte. Includes src/port/hh/gfx/gfx_soft.c and calls crack_fill on random depth / coverage / colour buffers: depth
 * surfaces (planes, steep and flat) with 1-px gaps along random lines (rows, columns, diagonals), the T-junction
 * kind of run, large depth-less areas (the sky, with and without coverage: the backdrops), coverage rectangles,
 * noise, and checkerboards that hit the fill cap (CF_MAX); sizes 240 square, 320x240 and random small ones (8..70,
 * so the columns past the last group of eight are exercised). Each scene is filled twice from the same start, the
 * old scan (sCrackScan 0) and the new (1): colour, depth and the fill count must match.
 *
 *   crack_test [seed] [scenes]     prints "<scenes> scenes, <fills> fills, <capped> capped, <n> differ, hash <h>"
 * Build and run (ARM under qemu: the NEON prefilter; x86: the per-pixel prefilter): tools/handheld/crack_test.sh. */
#include "gfx_soft.c"

#include <stdarg.h>

uint32_t gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
bool gfx_depth_full_range(void) { return false; }
int gfx_depth_read_x(int x, int w, int h) { (void)h; return x * w / 320; }
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int rr(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); } /* a..b */

enum { MW = 320, MH = 240 };
static uint32_t fb0[MW * MH], fbA[MW * MH], fbB[MW * MH];
static uint16_t zb0[MW * MH], zbA[MW * MH], zbB[MW * MH];
static uint8_t cv0[MW * MH];

static void scene(int w, int h) {
    const int n = w * h;
    const int bg = rr(0, 3); /* 0: all clear, 1: one far plane, 2: noise, 3: half sky */
    for (int i = 0; i < n; i++) {
        fb0[i] = rnd() | 0xFF000000u;
        zb0[i] = 0xFFFF;
        cv0[i] = 0;
    }
    if (bg == 1) for (int i = 0; i < n; i++) zb0[i] = (uint16_t)(60000 + (i % w));
    if (bg == 2) for (int i = 0; i < n; i++) if (rnd() & 1) zb0[i] = (uint16_t)(rnd() % 0xFFFF);
    if (bg == 3) for (int i = n / 2; i < n; i++) zb0[i] = (uint16_t)(30000 + i / w * 7);
    const int nsurf = rr(0, 8);
    for (int s = 0; s < nsurf; s++) { /* planes over random boxes; slope steep or flat */
        const int x0 = rr(0, w - 1), y0 = rr(0, h - 1), x1 = rr(x0, w - 1), y1 = rr(y0, h - 1);
        const int z0 = rr(0, 60000), dx = rr(-300, 300) >> rr(0, 8), dy = rr(-300, 300) >> rr(0, 8);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                int z = z0 + dx * (x - x0) + dy * (y - y0);
                z = z < 0 ? 0 : z > 0xFFFE ? 0xFFFE : z;
                zb0[y * w + x] = (uint16_t)z;
            }
    }
    const int ngap = rr(0, 12);
    for (int g = 0; g < ngap; g++) { /* 1-px gaps (depth clear again) along a line: rows, columns, slopes, runs */
        int x = rr(0, w - 1), y = rr(0, h - 1);
        const int kind = rr(0, 3), len = rr(1, w + h);
        for (int k = 0; k < len; k++) {
            if (x < 0 || x >= w || y < 0 || y >= h) break;
            zb0[y * w + x] = 0xFFFF;
            if (kind == 0) x++;
            else if (kind == 1) y++;
            else if (kind == 2) { x++; if ((rnd() & 3) == 0) y++; } /* a shallow edge: runs on a row */
            else { y++; if ((rnd() & 3) == 0) x++; }
        }
    }
    const int ncov = rr(0, 4);
    for (int c = 0; c < ncov; c++) { /* coverage boxes: the alpha / depth-less draws */
        const int x0 = rr(0, w - 1), y0 = rr(0, h - 1), x1 = rr(x0, w - 1), y1 = rr(y0, h - 1);
        for (int y = y0; y <= y1; y++) memset(cv0 + y * w + x0, 1, (size_t)(x1 - x0 + 1));
    }
    if (rr(0, 9) == 0) { /* a checkerboard of holes in a plane: thousands of candidates, past CF_MAX */
        const int x0 = rr(0, w / 2), y0 = rr(0, h / 2), x1 = rr(x0, w - 1), y1 = rr(y0, h - 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) zb0[y * w + x] = ((x + y) & 1) ? 0xFFFF : (uint16_t)(20000 + x + y);
    }
    const int nnoise = rr(0, 3) == 0 ? rr(0, n / 4) : 0;
    for (int k = 0; k < nnoise; k++) { /* stray clear pixels and coverage bytes */
        const int i = rr(0, n - 1);
        if (rnd() & 1) zb0[i] = 0xFFFF; else cv0[i] = (uint8_t)(rnd() & 1);
    }
}

static uint32_t run(int scan, int w, int h, uint32_t *fb, uint16_t *zb, uint32_t *nfill) {
    const int n = w * h;
    memcpy(fb, fb0, (size_t)n * 4); memcpy(zb, zb0, (size_t)n * 2);
    sCrackScan = scan;
    sCrackFill = 1;
    sFB = fb; sZB = zb; sW = w; sH = h;
    sCovMark = cv0; sCovFB = fb;
    const uint32_t before = gfx_soft_crack_fills;
    crack_fill();
    *nfill = gfx_soft_crack_fills - before;
    uint32_t hsh = 2166136261u;
    for (int i = 0; i < n; i++) { hsh = (hsh ^ fb[i]) * 16777619u; hsh = (hsh ^ zb[i]) * 16777619u; }
    return hsh;
}

int main(int argc, char **argv) {
    rs = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const int scenes = argc > 2 ? atoi(argv[2]) : 2000;
    unsigned long long fills = 0;
    int capped = 0, differ = 0;
    uint32_t total = 2166136261u;
    for (int s = 0; s < scenes; s++) {
        const int sz = rr(0, 3);
        const int w = sz == 0 ? 240 : sz == 1 ? 320 : rr(8, 70), h = sz == 0 || sz == 1 ? 240 : rr(8, 70);
        scene(w, h);
        uint32_t na, nb;
        const uint32_t ha = run(0, w, h, fbA, zbA, &na);
        const uint32_t hb = run(1, w, h, fbB, zbB, &nb);
        if (ha != hb || na != nb || memcmp(fbA, fbB, (size_t)w * h * 4) || memcmp(zbA, zbB, (size_t)w * h * 2)) {
            if (differ < 5) printf("scene %d (%dx%d): old %u fills, new %u: differ\n", s, w, h, na, nb);
            differ++;
        }
        fills += na; capped += na == 2048;
        total = (total ^ ha) * 16777619u;
    }
    printf("%d scenes, %llu fills, %d capped, %d differ, hash %08x\n", scenes, fills, capped, differ, total);
    return differ != 0;
}
