/* tpe_test.c: gfx_soft_eval_texels (cycle 1 of the colour combiner evaluated per texel: the torch flames, get-item
 * models, ...) with OOT_TPEFAST's operand sources against the old per-texel switch loop, byte for byte (2026-10-02, FPS
 * round 2). Includes src/port/hh/gfx/gfx_soft.c; random calls over every mux of every slot (rgb 0..31, alpha 0..7),
 * random constants, tile 0 and tile 1 32-bit or 16-bit (expanded or kept), any size, tile-1 shifts and corners, with
 * and without tile 1, keep_alpha on and off; the fast loop with its row reads (OOT_TPEROWS) and without; and
 * gfx_soft_blend_textures with its NEON loops (OOT_BLENDNEON) against its scalar loops. tools/handheld/tpe_test.sh builds it for x86 -m32 and ARM (qemu-arm).
 *   tpe_test [seed] [calls]   exit 1 on any difference; prints a hash of the outputs (ARM and x86 must agree) */
#include "gfx_soft.c"

#include <stdarg.h>

uint32_t gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
bool gfx_depth_full_range(void) { return false; }
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
int gfx_depth_read_x(int x, int w, int h) { (void)h; return x * w / 320; } /* gfx_pc.c (OOT_FLARE); unused here */
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static uint32_t new_tex(int w, int h, int fmt) {
    static uint8_t buf[128 * 128 * 4];
    const uint32_t id = gfx_soft_new_texture();
    const int am = rnd() % 4;
    for (int i = 0; i < w * h * 4; i++) buf[i] = (uint8_t)rnd();
    if (fmt == 0 && am) for (int i = 0; i < w * h; i++) buf[i * 4 + 3] = am == 1 ? 255 : am == 2 ? 0 : (rnd() & 1) * 255;
    gfx_soft_select_texture(0, id);
    gfx_soft_upload_texture(buf, w, h, fmt == 0 ? GU_PSM_8888 : GU_PSM_5551);
    return id;
}
static int pick_size(void) { static const int s[] = { 1, 2, 3, 4, 8, 16, 32, 64, 128, 5, 24, 48 }; return s[rnd() % 12]; }

int main(int argc, char **argv) {
    setenv("OOT_HALFWATER", "0", 1); /* the renderer at full resolution: the water layer is ARM-only and has its own test (halfwater_test) */
    rs = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const long calls = argc > 2 ? atol(argv[2]) : 20000;
    gfx_soft_init();
    static uint32_t a[256 * 256];
    long bad = 0, texels = 0, hits[2][4][32] = { { { 0 } } };
    uint32_t h = 2166136261u;
    for (long it = 0; it < calls; it++) {
        sArenaUsed = 0; sNextTexId = 1; memset(sTex, 0, sizeof(sTex));
        const int w0 = pick_size(), h0 = pick_size(), w1 = 1 << (rnd() % 8), h1 = 1 << (rnd() % 8);
        const uint32_t id0 = new_tex(w0, h0, (int)(rnd() % 2));
        const uint32_t id1 = rnd() % 4 ? new_tex(w1, h1, rnd() % 2) : 0;
        const uint32_t da = gfx_soft_new_texture(), db = gfx_soft_new_texture();
        int W = 1 + (int)(rnd() % 96), H = 1 + (int)(rnd() % 64);
        if (rnd() % 8 == 0) { W = 1 + (int)(rnd() % 256); H = 1 + (int)(rnd() % 256); }
        const int base_s = (int)(rnd() % 4096) - 1024, base_t = (int)(rnd() % 4096) - 1024;
        const int uls1 = (int)(rnd() % 2048) - 512, ult1 = (int)(rnd() % 2048) - 512;
        const int shs1 = rnd() % 3 ? 0 : (int)(rnd() % 16), sht1 = rnd() % 3 ? 0 : (int)(rnd() % 16);
        uint8_t m[8], k[9];
        static const uint8_t kRgb[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 31 };
        for (int s = 0; s < 4; s++) {
            m[s] = rnd() % 4 ? kRgb[rnd() % 17] : (uint8_t)(rnd() % 32);
            m[4 + s] = (uint8_t)(rnd() % 8);
            hits[0][s][m[s]]++; hits[1][s][m[4 + s]]++;
        }
        for (int i = 0; i < 9; i++) k[i] = (uint8_t)(rnd() % 5 ? rnd() : (rnd() & 1) * 255);
        const bool keep = rnd() & 1;
        sTpeFast = 0;
        const bool ra = gfx_soft_eval_texels(da, id0, id1, W, H, base_s, base_t, uls1, ult1, shs1, sht1, m, k, keep);
        if (ra) memcpy(a, sTex[da].px, (size_t)W * H * 4);
        sTpeFast = 1;
        sTpeRows = 1;
        const bool rb = gfx_soft_eval_texels(db, id0, id1, W, H, base_s, base_t, uls1, ult1, shs1, sht1, m, k, keep);
        if (ra != rb) { bad++; printf("call %ld: returns %d vs %d\n", it, ra, rb); if (getenv("TT_FIRST")) return 1; continue; }
        if (!ra) continue;
        const uint32_t *b = (const uint32_t *)sTex[db].px;
        long d = 0;
        for (int i = 0; i < W * H; i++) { d += a[i] != b[i]; h = (h ^ a[i]) * 16777619u; }
        /* OOT_TPEROWS=0 (2026-10-04): the fast loop's per-texel reads, against the old loop too */
        static uint32_t c[256 * 256];
        memcpy(c, b, (size_t)W * H * 4);
        sTpeRows = 0;
        const bool rc = gfx_soft_eval_texels(db, id0, id1, W, H, base_s, base_t, uls1, ult1, shs1, sht1, m, k, keep);
        sTpeRows = 1;
        if (rc != ra) { bad++; printf("call %ld: returns %d vs %d (rows off)\n", it, ra, rc); if (getenv("TT_FIRST")) return 1; continue; }
        for (int i = 0; i < W * H; i++) d += a[i] != ((const uint32_t *)sTex[db].px)[i];
        texels += W * H;
        if (d && getenv("TT_FIRST")) { printf("call %ld: %ld texels differ\n", it, d); return 1; } /* the mutation runs */
        if (d) {
            bad += d;
            if (bad < 2000) printf("call %ld: %ld texels differ (W %d H %d tex %dx%d / %dx%d m %u %u %u %u / %u %u %u %u keep %d)\n", it, d, W, H,
                                   w0, h0, w1, h1, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], keep);
        }
    }
    /* OOT_BLENDNEON (2026-10-04): gfx_soft_blend_textures's NEON loops against its scalar loops (sBlendNeon 0 / 1),
     * every mode, factor source and constant, tile 1 filtered (shifts 1..8, quarter-texel corners) or not */
    for (long it = 0; it < calls / 2; it++) {
        sArenaUsed = 0; sNextTexId = 1; memset(sTex, 0, sizeof(sTex));
        const int w0 = pick_size(), h0 = pick_size(), w1 = 1 << (rnd() % 8), h1 = 1 << (rnd() % 8);
        const uint32_t id0 = new_tex(w0, h0, (int)(rnd() % 2)), id1 = new_tex(w1, h1, (int)(rnd() % 2));
        const uint32_t da = gfx_soft_new_texture(), db = gfx_soft_new_texture();
        int W = 1 + (int)(rnd() % 96), H = 1 + (int)(rnd() % 64);
        if (rnd() % 8 == 0) { W = 1 + (int)(rnd() % 256); H = 1 + (int)(rnd() % 256); }
        const int base_s = (int)(rnd() % 4096) - 1024, base_t = (int)(rnd() % 4096) - 1024;
        const int uls1 = (int)(rnd() % 2048) - 512, ult1 = (int)(rnd() % 2048) - 512;
        const int shs1 = rnd() % 2 ? 0 : (int)(rnd() % 12), sht1 = rnd() % 2 ? 0 : (int)(rnd() % 12);
        const int rm = (int)(rnd() % 4), am = (int)(rnd() % 4), ks = (int)(rnd() % 3), kas = (int)(rnd() % 3);
        uint8_t kr[3]; for (int i = 0; i < 3; i++) kr[i] = (uint8_t)(rnd() % 4 ? rnd() : (rnd() & 1) * 255);
        const uint8_t ka = (uint8_t)rnd();
        const bool filt = rnd() % 3 != 0; const uint32_t qf = rnd() & 0xff;
        sBlendNeon = 0;
        const bool ra = gfx_soft_blend_textures(da, id0, id1, W, H, base_s, base_t, uls1, ult1, shs1, sht1, rm, am, ks, kr, kas, ka, filt, qf);
        sBlendNeon = 1;
        const bool rb = gfx_soft_blend_textures(db, id0, id1, W, H, base_s, base_t, uls1, ult1, shs1, sht1, rm, am, ks, kr, kas, ka, filt, qf);
        if (ra != rb) { bad++; printf("blend %ld: returns %d vs %d\n", it, ra, rb); if (getenv("TT_FIRST")) return 1; continue; }
        if (!ra) continue;
        const uint32_t *pa = (const uint32_t *)sTex[da].px, *pb = (const uint32_t *)sTex[db].px;
        long d = 0;
        for (int i = 0; i < W * H; i++) { d += pa[i] != pb[i]; h = (h ^ pa[i]) * 16777619u; }
        texels += W * H;
        if (d && getenv("TT_FIRST")) { printf("blend %ld: %ld texels differ\n", it, d); return 1; }
        if (d) { bad += d; if (bad < 2000) printf("blend %ld: %ld texels differ (W %d H %d modes %d %d src %d %d filt %d)\n", it, d, W, H, rm, am, ks, kas, filt); }
    }
    long unhit = 0;
    for (int s = 0; s < 4; s++) { for (int v = 0; v < 32; v++) unhit += hits[0][s][v] == 0; for (int v = 0; v < 8; v++) unhit += hits[1][s][v] == 0; }
    printf("tpe_test: %ld calls, %ld texels, every mux of every slot %s, %ld texels differ, hash %08x\n", calls, texels,
           unhit ? "NOT reached" : "reached", bad, (unsigned)h);
    return bad != 0 || unhit != 0;
}
