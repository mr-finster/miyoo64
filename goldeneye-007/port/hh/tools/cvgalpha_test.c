/* cvgalpha_test.c: GE_CVGALPHA (gfx_soft.c raster_tri) through the real rasterizer (2026-10-01, the Dam's
 * see-through destroyed crate). Includes hh/gfx/gfx_soft.c and draws random textured triangles over a "wall"
 * in the destroyed-object mode: texture x shade, the alpha from the shade only (the env alpha 150/200/250 added
 * per vertex by the combiner), texture edge (CVG_X_ALPHA), z tested and written.
 *
 * The RDP rule (angrylion combiner.c cvg_times_alpha, zbuffer.c z_compare, blender.c): an interior pixel's
 * coverage becomes ((a * 8 + 4) >> 3) >> 5 (a 255 counted as 256): 0 for a < 32, else 1..8; over the wall's
 * full coverage the sum overflows, blend_en is off (no FORCE_BL), so the pixel is written as its colour.
 * Reference: the triangle drawn opaque (sUseAlpha off) when its three vertex alphas are >= 32, the wall when all
 * are below (the RDP's shade is linear in screen space: the alpha stays within the three; the port's perspective
 * shade can overshoot at slivers, which must not decide); a mixed triangle per pixel by the port's interpolated
 * alpha, taken from a probe: the same triangle with a white texture and the vertex alpha as its shade colour,
 * drawn opaque (the same pshade/stepping code).
 *
 * Checks, over random frames:
 *   new  == reference (every pixel, colour and depth)       -- GE_CVGALPHA=1 (default)
 *   old  == the pre-change path                              -- GE_CVGALPHA=0 (A/B): the blended draw, counted
 *   texture-alpha triangles: new == old                      -- foliage/fences untouched
 * Build and run: port/hh/tools/cvgalpha_test.sh (x86 -m32 scalar and ARM under qemu-arm, NEON rows). */
#include "gfx_soft.c"
#include "gfx_cc.c"
#ifdef CV_OLDFILE /* -DCV_OLDFILE with the tree before GE_CVGALPHA: only the old path's hash is meaningful */
static int sCvgAlpha;
static unsigned long gfx_soft_cvg_tris[3];
#endif

#include <stdarg.h>

int gfx_trace_frames;
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
int gfx_pc_frame_w(void) { return 240; }
int bossGetStageNum(void) { return 33; }
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
uint32_t gPortProfFrame[PROF_COUNT]; /* gfx_soft.c adds its handed-back time here (GE_RSPYIELD, 2026-10-02) */

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
static uint32_t wallFB[W * H], fbA[W * H], fbB[W * H], probeFB[W * H];
static uint16_t wallZB[W * H], zbA[W * H], zbB[W * H], probeZB[W * H];

static struct ShaderProgram *shader(int a_tex) {
    /* rgb (TEXEL0 - 0) * SHADE + 0; alpha SHADE, or (TEXEL0 - 0) * SHADE + 0 */
    uint32_t rgb = SHADER_TEXEL0 | (SHADER_0 << 3) | (SHADER_INPUT_1 << 6) | (SHADER_0 << 9);
    uint32_t al = a_tex ? (SHADER_TEXEL0 | (SHADER_0 << 3) | (SHADER_INPUT_1 << 6) | (SHADER_0 << 9)) : (SHADER_INPUT_1 << 9);
    return gfx_soft_create_and_load_new_shader(rgb | (al << 12) | SHADER_OPT_ALPHA | SHADER_OPT_TEXTURE_EDGE);
}

static void target(uint32_t *fb, uint16_t *zb) {
    memcpy(fb, wallFB, sizeof(wallFB)); memcpy(zb, wallZB, sizeof(wallZB));
    sFB = fb; sZB = zb; sW = W; sH = H;
}

int main(int argc, char **argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 2000;
    rs = argc > 2 ? (uint32_t)atoi(argv[2]) : 1;
    gfx_soft_init();
    sScX0 = 0; sScY0 = 0; sScX1 = W; sScY1 = H; sVpX = 0; sVpY = 0; sVpW = W; sVpH = H;
    sDepthTest = true; sDepthMask = true;
    static uint8_t texbuf[32 * 32 * 4], white[32 * 32 * 4];
    const uint32_t tid = gfx_soft_new_texture(), wid = gfx_soft_new_texture();
    for (int i = 0; i < 32 * 32 * 4; i++) { texbuf[i] = (uint8_t)rnd(); white[i] = 255; }
    for (int i = 0; i < 32 * 32; i++) texbuf[i * 4 + 3] = (i % 7) ? 255 : (uint8_t)rnd(); /* some texture alpha */
    gfx_soft_select_texture(0, tid); gfx_soft_upload_texture(texbuf, 32, 32, GU_PSM_8888);
    gfx_soft_select_texture(0, wid); gfx_soft_upload_texture(white, 32, 32, GU_PSM_8888);
    for (int i = 0; i < W * H; i++) { wallFB[i] = 0xFF000000u | (rnd() & 0x3F3F7F); wallZB[i] = 0xF000; }
    struct ShaderProgram *sh_a = shader(0), *sh_t = shader(1);
    uint32_t oldHash = 2166136261u;
    long bad_new = 0, bad_tex = 0, oldDiff = 0, px_total = 0, dropped = 0;
    unsigned long kinds[3] = { 0, 0, 0 };
    for (int f = 0; f < frames; f++) {
        SoftVtx v[3];
        const int kind = rnd() % 4; /* 0: destroyed (alpha >= 150), 1: all < 32, 2: mixed / anything, 3: texture alpha */
        const float cx = frnd(-20, 260), cy = frnd(-20, 260), sz = frnd(3, 140);
        for (int k = 0; k < 3; k++) {
            v[k].x = cx + frnd(-sz, sz); v[k].y = cy + frnd(-sz, sz); v[k].z = frnd(0.0f, 0.9f);
            v[k].invw = (rnd() % 8) ? frnd(0.05f, 3.0f) : 1.0f;
            v[k].u = frnd(-3, 3); v[k].v = frnd(-3, 3);
            v[k].r = (uint8_t)rnd(); v[k].g = (uint8_t)rnd(); v[k].b = (uint8_t)rnd();
            v[k].a = kind == 0 ? (uint8_t)(150 + rnd() % 106) : kind == 1 ? (uint8_t)(rnd() % 32) : (uint8_t)rnd();
            v[k].kr = v[k].kg = v[k].kb = v[k].ka = 0;
        }
        /* gfx_pc's per-vertex K (the combiner at texel 0): rgb 0, alpha = the alpha (it doesn't read the texture),
         * so the game's draws take the VK rows; half the frames as that, half plain texture x shade */
        if (kind != 3 && (rnd() & 1)) for (int k = 0; k < 3; k++) v[k].ka = v[k].a;
        if (rnd() % 5 == 0) { v[1].r = v[2].r = v[0].r; v[1].g = v[2].g = v[0].g; v[1].b = v[2].b = v[0].b; v[1].a = v[2].a = v[0].a; }
        const bool bil = rnd() & 1;
        gfx_soft_bilinear = bil;
        sTexOverride = NULL;
        if (kind == 3) {
            /* texture alpha: GE_CVGALPHA must not change it */
            sCurShader = sh_t; sBoundTex[0] = tid; sUseAlpha = true;
            sCvgAlpha = 1; target(fbA, zbA); raster_tri(&v[0], &v[1], &v[2], true);
            sCvgAlpha = 0; target(fbB, zbB); raster_tri(&v[0], &v[1], &v[2], true);
            if (memcmp(fbA, fbB, sizeof(fbA)) || memcmp(zbA, zbB, sizeof(zbA))) bad_tex++;
            continue;
        }
        /* probe: the interpolated alpha per pixel (white texture x shade (a, a, a), opaque) */
        SoftVtx p[3];
        for (int k = 0; k < 3; k++) { p[k] = v[k]; p[k].r = p[k].g = p[k].b = v[k].a; p[k].a = 255; }
        /* (p keeps v's ka: the probe takes the same rows; K rgb 0 + (S - 0) * 255 / 255 = S) */
        sCurShader = sh_a; sBoundTex[0] = wid; sUseAlpha = false;
        target(probeFB, probeZB); raster_tri(&p[0], &p[1], &p[2], true);
        /* the triangle opaque */
        sBoundTex[0] = tid; sUseAlpha = false;
        target(fbB, zbB); raster_tri(&v[0], &v[1], &v[2], true);
        /* new path */
        sUseAlpha = true; sCvgAlpha = 1;
        const unsigned long k0 = gfx_soft_cvg_tris[0], k1 = gfx_soft_cvg_tris[1], k2 = gfx_soft_cvg_tris[2];
        target(fbA, zbA); raster_tri(&v[0], &v[1], &v[2], true);
        kinds[0] += gfx_soft_cvg_tris[0] - k0; kinds[1] += gfx_soft_cvg_tris[1] - k1; kinds[2] += gfx_soft_cvg_tris[2] - k2;
        for (int i = 0; i < W * H; i++) {
            const bool covered = probeFB[i] != wallFB[i] || probeZB[i] != wallZB[i];
            const int a = (int)((probeFB[i] >> 16) & 0xFF);
            /* whole triangle by its vertex alphas (the RDP's shade is linear in screen space, so its alpha
             * stays within the three; the port's perspective shade can overshoot at slivers), per pixel
             * (the port's own interpolated alpha) only for a mixed triangle */
            const int amn = v[0].a < v[1].a ? (v[0].a < v[2].a ? v[0].a : v[2].a) : (v[1].a < v[2].a ? v[1].a : v[2].a);
            const int amx = v[0].a > v[1].a ? (v[0].a > v[2].a ? v[0].a : v[2].a) : (v[1].a > v[2].a ? v[1].a : v[2].a);
            const bool drawn = covered && (amn >= 32 || (amx >= 32 && a >= 32));
            const uint32_t ec = drawn ? fbB[i] : wallFB[i];
            const uint16_t ez = drawn ? zbB[i] : wallZB[i];
            if (covered) px_total++;
            if (covered && !drawn) dropped++;
            if (fbA[i] != ec || zbA[i] != ez) {
                if (bad_new < 12 && getenv("CV_V")) printf("  f%d kind %d px %d,%d a %d got %08X want %08X (opaque %08X wall %08X) amin/max %d %d %d\n", f, kind, i % W, i / W, a, fbA[i], ec, fbB[i], wallFB[i], v[0].a, v[1].a, v[2].a);
                bad_new++;
            }
        }
        /* old path (GE_CVGALPHA=0): counted, for the record */
        sCvgAlpha = 0;
        uint32_t *keep = (uint32_t *)malloc(sizeof(fbA)); memcpy(keep, fbA, sizeof(fbA));
        target(fbA, zbA); raster_tri(&v[0], &v[1], &v[2], true);
        for (int i = 0; i < W * H; i++) { oldDiff += fbA[i] != keep[i]; oldHash = (oldHash ^ fbA[i] ^ ((uint32_t)zbA[i] << 7)) * 16777619u; }
        free(keep);
    }
    printf("cvgalpha: %d frames, %ld covered px (%ld below 32), tris opaque %lu dropped %lu mixed %lu\n", frames, px_total,
           dropped, kinds[0], kinds[1], kinds[2]);
    printf("  new vs RDP reference: %ld px differ; texture-alpha new vs old: %ld frames differ; old path differs from new on %ld px\n",
           bad_new, bad_tex, oldDiff);
    printf("  old path (GE_CVGALPHA=0) frame hash %08x\n", (unsigned)oldHash);
    printf("%s\n", bad_new || bad_tex ? "cvgalpha_test: FAIL" : "cvgalpha_test: PASS");
    return bad_new || bad_tex;
}
