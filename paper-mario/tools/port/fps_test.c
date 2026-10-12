/* fps_test.c: the renderer (bench/kit/gfx_soft.c, included whole) on synthetic scenes (fx_scene.c), no ROM.
 *   fps_test hash [frames]          per kind and overall: an FNV hash of every pixel and depth of frames 0..n-1
 *   fps_test bench <kind> [frames]  frames of one kind (qemu instruction counts)
 *   fps_test scene <kind> <frame> <out.ppm>
 *   fps_test nearz [seed] [n]       PM64_NEARW's span depth (span_zdq + span_zq_far, 2026-10-04) against the exact depth:
 *                                   n random spans (start -1e4..1e4, slope -1e3..1e3, 1-16 pixels, a third in [-1, 2]),
 *                                   every pixel's 16-bit depth from the 24.8 steps against clamp(floor(exact * 65535)) in
 *                                   double, at most 1 apart; with PM64_NEARW=0 the count of the old path's misses
 *   env FPS_NEARZ=1 (matrix): near-eye depths in a quarter of the perspective vertices, every row variant */
#include "gfx_soft.c"
#include "gfx_cc.c"
#include "fps_scene.c"
#include <stdarg.h>

u32 gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { FXW, FXH, 4.0f / 3.0f };
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void port_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void port_log_always(const char *fmt, ...) { (void)fmt; }
u32 port_time_us(void) { return 0; }

static uint32_t fnv(uint32_t h, const void *p, int n) {
    const uint8_t *b = p;
    for (int i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* every row variant: combine (texture x shade with each alpha source, fast paths 1, 1 with T0a, 7, 4, 10), texture
 * wrap (repeat at a power of two, not a power of two, clamped, clamped one way), TEXEL1's wrap, depth (tested and
 * written, tested, none), decal bias, alpha, texture edge (written unblended), shroud, bilinear/nearest; random
 * perspective and affine, flat and shaded triangles over a random frame and depth buffer */
static int sNearZ; /* FPS_NEARZ=1 (2026-10-04, PM64_NEARW): a quarter of the perspective vertices at z -1000..-1 with 1/w
                    * 2..64, the depths of faces within 16 units of the eye (z/w * 0.5 + 0.5 = 1 - 16 / w) */
static uint32_t matrix(int reps) {
    static const uint32_t m0id[3] = { 5u | 1u << 6 | (5u | 1u << 6) << 12, 5u | 1u << 6 | 5u << 12, 5u | 1u << 6 | 1u << 12 };
    static const int texs[4] = { T_SMOKE, T_NPOT, T_SPRITE, T_TILE }, tex1s[2] = { T_DUST1, T_NPOT };
    struct ShaderProgram *sh[3][2];
    for (int a = 0; a < 3; a++) for (int e = 0; e < 2; e++) sh[a][e] = gfx_soft_api.create_and_load_new_shader(m0id[a] | 1u << 24 | (e ? 1u << 26 : 0));
    uint32_t h = 2166136261u; long combos = 0;
    fxs_rs = 99;
    gfx_soft_api.start_frame(); gfx_scegu_set_phase(1);
    for (int mode = 0; mode < 8; mode++) for (int ti = 0; ti < 4; ti++) for (int t1 = 0; t1 < 2; t1++) for (int dep = 0; dep < 3; dep++)
    for (int dec = 0; dec < 2; dec++) for (int al = 0; al < 2; al++) for (int ed = 0; ed < 2; ed++) for (int shr = 0; shr < 2; shr++)
    for (int bl = 0; bl < 2; bl++) {
        if (mode < 3 && (t1 || shr)) continue;           /* texture x shade: one texture, no shroud */
        if ((mode == 3 || mode == 4 || mode == 5) && t1) continue;
        for (int i = 0; i < FXW * FXH; i++) { sNatFB[i] = fxs_rnd() | 0xFF000000u; sNatZB[i] = (uint16_t)(fxs_rnd() >> 3); }
        gfx_soft_bilinear = bl; sEdgeOpaque = ed;
        gfx_soft_api.set_use_alpha(al); gfx_soft_api.set_depth_test(dep < 2); gfx_soft_api.set_depth_mask(dep == 0); gfx_soft_api.set_zmode_decal(dec);
        gfx_soft_api.load_shader(sh[mode < 3 ? mode : 0][ed]);
        if (mode >= 3) {
            static const uint32_t w[5][2] = { { 0xFC121803u, 0xFFFFFF78u }, { 0xFC127E03u, 0xFFFFF378u }, { 0xFC60FEC1u, 0x44FE7339u },
                                              { 0xFC20A204u, 0xFF13FFFFu }, { 0xFC117E80u, 0xF5FFF438u } };
            static const uint8_t pr[4] = { 250, 220, 120, 200 }, en[4] = { 40, 90, 160, 120 };
            const int32_t t1map[6] = { 0, 3 << 16, 1, 0, 7 << 16, -1 };
            gfx_soft_pm_shroud(shr ? 12 : 0, 20, 10, 60);
            gfx_soft_pm_generic(true, w[mode - 3][0], w[mode - 3][1], mode != 5, pr, en, 0, 0, 0, t1map);
        } else gfx_soft_pm_generic(false, 0, 0, false, NULL, NULL, 0, 0, 0, NULL);
        gfx_soft_api.select_texture(0, fxs_tex[texs[ti]]); gfx_soft_api.select_texture(1, fxs_tex[tex1s[t1]]);
        for (int r = 0; r < reps; r++) {
            const int persp = (int)(fxs_rnd() & 1), flat = (int)(fxs_rnd() % 3 == 0);
            FxVtx v[3]; int c[4]; const float cx = fxs_frnd(-20, FXW + 20), cy = fxs_frnd(-20, FXH + 20), sz = fxs_frnd(2, 70);
            for (int k = 0; k < 4; k++) c[k] = (int)(fxs_rnd() & 255);
            for (int k = 0; k < 3; k++) { /* drawn in order: an argument list's order of evaluation is the compiler's */
                float f[6]; int q[4];
                for (int j = 0; j < 6; j++) f[j] = j < 2 ? fxs_frnd(-sz, sz) : j == 2 ? fxs_frnd(0.0f, 1.0f) : j == 3 ? fxs_frnd(0.2f, 3.0f) : fxs_frnd(-3, 3);
                for (int j = 0; j < 4; j++) q[j] = flat ? c[j] : (int)(fxs_rnd() & 255);
                if (sNearZ && persp && fxs_rnd() % 4 == 0) { f[2] = fxs_frnd(-1000.0f, -1.0f); f[3] = fxs_frnd(2.0f, 64.0f); } /* FPS_NEARZ */
                v[k] = fxs_vx(cx + f[0], cy + f[1], f[2], persp ? f[3] : 0.0f, f[4], f[5], q[0], q[1], q[2], q[3]);
            }
            gfx_soft_api.draw_triangles((float *)v, sizeof v / 4, 1);
        }
        h = fnv(h, sNatFB, FXW * FXH * 4); h = fnv(h, sNatZB, FXW * FXH * 2); combos++;
    }
    gfx_soft_pm_generic(false, 0, 0, false, NULL, NULL, 0, 0, 0, NULL); gfx_soft_pm_shroud(0, 0, 0, 0);
    printf("matrix: %ld combinations x %d triangles, hash %08x, span px %llu\n", combos, reps, (unsigned)h, (unsigned long long)gfx_soft_span_px);
    return h;
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "hash";
    gfx_soft_set_render_res(FXW, FXH, FXW, FXH);
    gfx_soft_api.init();
    fx_scene_init();
    { const char *e = getenv("PM64_CMNEON"); if (e && e[0] == '0') gfx_soft_set_cmneon(0); }
    { const char *e = getenv("FPS_SHARP"); if (e && e[0] == '1') gfx_soft_bilinear = 0; } /* nearest everywhere (SHARP) */
    if (!strcmp(mode, "scene")) {
        fx_scene_draw(argc > 2 ? atoi(argv[2]) : 0, argc > 3 ? atoi(argv[3]) : 0);
        FILE *f = fopen(argc > 4 ? argv[4] : "scene.ppm", "wb");
        fprintf(f, "P6\n%d %d\n255\n", FXW, FXH);
        for (int i = 0; i < FXW * FXH; i++) { const uint32_t c = sNatFB[i]; fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f); }
        fclose(f);
        return 0;
    }
    if (!strcmp(mode, "hash")) {
        const int frames = argc > 2 ? atoi(argv[2]) : 8;
        uint32_t all = 2166136261u;
        for (int k = 0; k < FX_KINDS; k++) {
            uint32_t h = 2166136261u;
            for (int fr = 0; fr < frames; fr++) {
                fx_scene_draw(k, fr);
                h = fnv(h, sNatFB, FXW * FXH * 4); h = fnv(h, sNatZB, FXW * FXH * 2);
            }
            printf("kind %d: %08x\n", k, (unsigned)h);
            all = fnv(all, &h, 4);
        }
        printf("hash: %d frames x %d kinds, hash %08x, span px %llu\n", frames, FX_KINDS, (unsigned)all, (unsigned long long)gfx_soft_span_px);
        return 0;
    }
    if (!strcmp(mode, "matrix")) { sNearZ = getenv("FPS_NEARZ") != NULL; matrix(argc > 2 ? atoi(argv[2]) : 3); return 0; }
#ifndef FPS_OLDTREE
    if (!strcmp(mode, "nearz")) { /* PM64_NEARW's span depth against the exact depth (goldeneye-007's rast_test nearz) */
        fxs_rs = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u;
        if (!fxs_rs) fxs_rs = 1;
        const long n = argc > 3 ? atol(argv[3]) : 1000000;
        long bad = 0, npx = 0, outside = 0;
        for (long i = 0; i < n; i++) {
            const int span = 1 + (int)(fxs_rnd() % 16);
            float zf = (fxs_rnd() % 3 == 0) ? fxs_frnd(-1.0f, 2.0f) : (fxs_rnd() & 1) ? fxs_frnd(-1e4f, 1e4f) : fxs_frnd(-30.0f, 30.0f);
            float zdx = (fxs_rnd() & 1) ? fxs_frnd(-2.0f, 2.0f) : (fxs_rnd() & 1) ? fxs_frnd(-30.0f, 30.0f) : fxs_frnd(-1e3f, 1e3f);
            if (fxs_rnd() % 8 == 0) zdx = 0.0f;
            const int32_t zdq = span_zdq(zdx);
            const int32_t zq = span_z_out(zf) ? span_zq_far(zf, zdx, span, zdq)
                                              : (int32_t)((zf < -8.0f ? -8.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            outside += zf < -1.0f || zf > 2.0f;
            for (int k = 0; k < span; k++) {
                const int ours = clamp16_sh8(zq + k * zdq);
                const double ez = ((double)zf + (double)k * (double)zdx) * 65535.0;
                const int ex = ez < 0.0 ? 0 : ez > 65535.0 ? 65535 : (int)ez;
                npx++;
                if (ours - ex > 1 || ex - ours > 1) {
                    if (bad < 5) printf("  span %d zf %g zdx %g pixel %d: %d, exact %d\n", span, zf, zdx, k, ours, ex);
                    bad++;
                }
            }
        }
        printf("nearz: PM64_NEARW=%d, %ld spans (%ld starting outside [-1, 2]), %ld px, %ld differ by more than 1\n",
               gfx_soft_nearw_on(), n, outside, npx, bad);
        return 0;
    }
#endif
    if (!strcmp(mode, "bench")) {
        const int kind = argc > 2 ? atoi(argv[2]) : 0, frames = argc > 3 ? atoi(argv[3]) : 1;
        for (int fr = 0; fr < frames; fr++) fx_scene_draw(kind, fr);
        printf("bench: kind %d, %d frames, span px %llu\n", kind, frames, (unsigned long long)gfx_soft_span_px);
        return 0;
    }
    return 2;
}
