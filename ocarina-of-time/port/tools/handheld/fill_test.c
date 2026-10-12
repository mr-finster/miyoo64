/* fill_test.c: OOT_FILLRULE and OOT_SHADELERP through the real renderer (2026-10-03, the Deku Tree's vision). Includes
 * src/port/hh/gfx/gfx_soft.c and drives raster_tri through gfx_soft_draw_triangles.
 *
 * own : random meshes whose triangles share edges -- convex quads split on a diagonal, convex polygons as fans (the
 *       clip fan's shape), the guard-band square (-120..360, its diagonals through pixel centres), grids on half-pixel
 *       steps (centres exactly on the edges); the convex outlines are hulls of random points -- drawn blended (alpha 128, white over black, no z write), untextured
 *       (the scalar loops) and textured with a white texture (the NEON rows on ARM). With gfx_soft_set_own_edges(1)
 *       no pixel may be drawn twice (191 = two blends of 128) and every pixel centre inside the mesh's outline by more
 *       than 1e-3 px must be drawn once (no crack); outside by more than 1e-3, not at all. The same frames with the
 *       switch off count the doubled pixels the old rule leaves (reported, expected > 0).
 * lerpv: gfx_scegu_set_tex_lerpv: a flat shade S toward K by a uniform texel T, opaque, every pixel S + (K - S) * T
 *       / 255 per channel (C division), bilinear and nearest, z-tested and not.
 *
 *   fill_test [seed] [frames]   prints the counts and a hash of the frames; exit 1 on any failure
 * Build and run: tools/handheld/fill_test.sh (ARM under qemu-arm and x86 -m32; the hashes must agree). */
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
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
static struct ShaderProgram gProg;
static uint32_t gTex;

static void upload_uniform(uint32_t argb) {
    static uint8_t buf[32 * 32 * 4];
    for (int i = 0; i < 32 * 32; i++) {
        buf[i * 4 + 0] = (uint8_t)(argb >> 16); buf[i * 4 + 1] = (uint8_t)(argb >> 8);
        buf[i * 4 + 2] = (uint8_t)argb; buf[i * 4 + 3] = (uint8_t)(argb >> 24);
    }
    gfx_soft_select_texture(0, gTex);
    gfx_soft_upload_texture(buf, 32, 32, GU_PSM_8888);
    gfx_soft_set_sampler_parameters(0, true, 0, 0);
}

static SoftVtx vtx(float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SoftVtx v; memset(&v, 0, sizeof(v));
    v.x = x; v.y = y; v.z = 0.5f; v.invw = 1.0f; v.u = frnd(0, 1); v.v = frnd(0, 1);
    v.r = r; v.g = g; v.b = b; v.a = a;
    return v;
}

/* the outline (convex, counter-clockwise or clockwise) and the triangles */
static double gPx[16], gPy[16]; static int gNP;
static SoftVtx gT[3 * 256]; static int gNT;
static void tri(SoftVtx a, SoftVtx b, SoftVtx c) {
    if (rnd() & 1) { SoftVtx t = b; b = c; c = t; } /* either winding: the rasterizer takes both */
    gT[gNT * 3] = a; gT[gNT * 3 + 1] = b; gT[gNT * 3 + 2] = c; gNT++;
}
static float snap(float v, int mode) { return mode ? floorf(v * 2.0f) * 0.5f : v; } /* half pixels: centres on edges */
/* a convex polygon: the hull of random points in an ellipse-ish box (no trig: hh_compat.h's sinf/cosf are the game's) */
static void convex(int n, float cx, float cy, float rx, float ry, int sm) {
    double qx[32], qy[32], hx[64], hy[64];
    int m = 0, k = 0;
    for (int i = 0; i < 3 * n && m < 32; i++) { qx[m] = snap(cx + rx * frnd(-1, 1), sm); qy[m] = snap(cy + ry * frnd(-1, 1), sm); m++; }
    for (int i = 1; i < m; i++) /* sort by x, then y */
        for (int j = i; j > 0 && (qx[j] < qx[j - 1] || (qx[j] == qx[j - 1] && qy[j] < qy[j - 1])); j--) {
            double t = qx[j]; qx[j] = qx[j - 1]; qx[j - 1] = t; t = qy[j]; qy[j] = qy[j - 1]; qy[j - 1] = t;
        }
    for (int i = 0; i < m; i++) { /* Andrew's monotone chain, collinear points dropped */
        while (k >= 2 && ((hx[k-1]-hx[k-2])*(qy[i]-hy[k-2]) - (hy[k-1]-hy[k-2])*(qx[i]-hx[k-2])) <= 0) k--;
        hx[k] = qx[i]; hy[k] = qy[i]; k++;
    }
    for (int i = m - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && ((hx[k-1]-hx[k-2])*(qy[i]-hy[k-2]) - (hy[k-1]-hy[k-2])*(qx[i]-hx[k-2])) <= 0) k--;
        hx[k] = qx[i]; hy[k] = qy[i]; k++;
    }
    k--; /* the last point is the first */
    if (k > 16) k = 16; /* (a hull of more than 16 points: rare; the first 16 stay convex) */
    gNP = k;
    for (int i = 0; i < k; i++) { gPx[i] = hx[i]; gPy[i] = hy[i]; }
}
static int make_mesh(void) {
    const int kind = (int)(rnd() % 4), sm = (int)(rnd() % 2);
    gNT = 0;
    if (kind == 0 || kind == 1) { /* 0: a quad on a diagonal; 1: a fan (the clip fan) */
        const int n = kind == 0 ? 2 : 3 + (int)(rnd() % 4); /* random points: about 4 (quads) or more on the hull */
        const float big = (rnd() % 3 == 0) ? 3.0f : 1.0f; /* past the screen: the guard band's sizes */
        const float cx = frnd(40, 200), cy = frnd(40, 200), rx = frnd(10, 110) * big, ry = frnd(10, 110) * big;
        convex(n, cx, cy, rx, ry, sm);
        SoftVtx v[16];
        if (gNP < 3) return make_mesh();
        for (int i = 0; i < gNP; i++) v[i] = vtx((float)gPx[i], (float)gPy[i], 255, 255, 255, 128);
        const int o = (int)(rnd() % gNP); /* the fan's apex */
        for (int i = 1; i + 1 < gNP; i++) tri(v[o], v[(o + i) % gNP], v[(o + i + 1) % gNP]);
    } else if (kind == 2) { /* the guard-band square, both diagonals in turn */
        const float lo = -120.0f, hi = 360.0f;
        gNP = 4; gPx[0] = lo; gPy[0] = lo; gPx[1] = hi; gPy[1] = lo; gPx[2] = hi; gPy[2] = hi; gPx[3] = lo; gPy[3] = hi;
        SoftVtx v[4];
        for (int i = 0; i < 4; i++) v[i] = vtx((float)gPx[i], (float)gPy[i], 255, 255, 255, 128);
        if (rnd() & 1) { tri(v[0], v[1], v[2]); tri(v[0], v[2], v[3]); } else { tri(v[1], v[2], v[3]); tri(v[1], v[3], v[0]); }
    } else { /* a grid on half-pixel steps, the diagonals mixed */
        const int nx = 2 + (int)(rnd() % 6), ny = 2 + (int)(rnd() % 6);
        float x0 = floorf(frnd(-20, 150)), y0 = floorf(frnd(-20, 150));
        if (rnd() & 1) x0 += 0.5f;
        if (rnd() & 1) y0 += 0.5f;
        const float sx = (float)(1 + rnd() % 30) * 0.5f, sy = (float)(1 + rnd() % 30) * 0.5f;
        gNP = 4; gPx[0] = x0; gPy[0] = y0; gPx[1] = x0 + nx * sx; gPy[1] = y0; gPx[2] = x0 + nx * sx; gPy[2] = y0 + ny * sy;
        gPx[3] = x0; gPy[3] = y0 + ny * sy;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                SoftVtx a = vtx(x0 + i * sx, y0 + j * sy, 255, 255, 255, 128), b = vtx(x0 + (i + 1) * sx, y0 + j * sy, 255, 255, 255, 128);
                SoftVtx c = vtx(x0 + (i + 1) * sx, y0 + (j + 1) * sy, 255, 255, 255, 128), d = vtx(x0 + i * sx, y0 + (j + 1) * sy, 255, 255, 255, 128);
                if (rnd() & 1) { tri(a, b, c); tri(a, c, d); } else { tri(a, b, d); tri(b, c, d); }
            }
    }
    return kind;
}
/* signed distance-ish of a pixel centre to the outline: > 0 inside by that much (the outline is convex) */
static double inside_by(double px, double py) {
    double area = 0, m = 1e30;
    for (int i = 0; i < gNP; i++) { const int j = (i + 1) % gNP; area += gPx[i] * gPy[j] - gPx[j] * gPy[i]; }
    const double s = area > 0 ? 1.0 : -1.0;
    for (int i = 0; i < gNP; i++) {
        const int j = (i + 1) % gNP;
        const double ex = gPx[j] - gPx[i], ey = gPy[j] - gPy[i], len = sqrt(ex * ex + ey * ey);
        if (len == 0) continue;
        const double d = s * (ex * (py - gPy[i]) - ey * (px - gPx[i])) / len;
        if (d < m) m = d;
    }
    return m;
}

static void draw(bool own, bool textured) {
    for (int i = 0; i < W * H; i++) { sFB[i] = 0xFF000000u; sZB[i] = 0xFFFF; }
    gfx_soft_set_own_edges(own);
    gfx_soft_bilinear = true;
    sDepthTest = textured; sDepthMask = false; sZDecal = false; sUseAlpha = true; sEdgeOpaque = false;
    gfx_soft_set_fog(false, 0, 0, 0, -1);
    gProg.rgb_tex = textured; gProg.rgb_in = true; gProg.a_tex = false; gProg.a_in = true; gProg.rgb_texa = false;
    gProg.cc.opt_texture_edge = false; gProg.cc.num_inputs = 1; gProg.cc.used_textures[0] = textured;
    sCurShader = &gProg;
    gfx_scegu_set_tex_lerpv(false); gfx_scegu_set_tex_lerp(false, 0, 0, 0);
    sTexAdd = false; sForceIn = false;
    gfx_scegu_set_ui_mode(false);
    sBoundTex[0] = gTex;
    gfx_soft_set_detail(0, 0, 0, 0, 0, NULL);
    gfx_soft_draw_triangles((float *)gT, (size_t)gNT * 3 * sizeof(SoftVtx) / 4, (size_t)gNT);
    sky_settle();
}

int main(int argc, char **argv) {
    setenv("OOT_HALFWATER", "0", 1);
    rs = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const int frames = argc > 2 ? atoi(argv[2]) : 2000;
    gfx_soft_init();
    select_target(true);
    sNatStale = false;
    gTex = gfx_soft_new_texture();
    upload_uniform(0xFFFFFFFFu);
    long twice = 0, crack = 0, spill = 0, oldtwice = 0, oldframes = 0, drawn = 0, ties = 0;
    uint32_t hsum = 2166136261u;
    for (int fr = 0; fr < frames; fr++) {
        const int kind = make_mesh();
        bool textured = (rnd() & 1) != 0;
        if (getenv("FT_TEX")) textured = getenv("FT_TEX")[0] == '1'; /* (debug: one kind only) */
        draw(false, textured); /* the old rule: how many pixels it draws twice */
        long d0 = 0;
        for (int i = 0; i < W * H; i++) d0 += (sFB[i] & 0xFF) > 140;
        oldtwice += d0; oldframes += d0 > 0;
        draw(true, textured);
        long t = 0, c = 0, s = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int v = (int)(sFB[y * W + x] & 0xFF);
                const double ib = inside_by(x + 0.5, y + 0.5);
                if (v > 140) t++;
                if (ib > 1e-3 && v != 128) c++;
                if (ib < -1e-3 && v != 0) s++;
                if (v == 128) drawn++;
                if (fabs(ib) <= 1e-3) ties++;
                hsum = (hsum ^ sFB[y * W + x]) * 16777619u;
            }
        if ((t || c || s) && getenv("FT_DBG")) {
            for (int i = 0; i < gNP; i++) printf("  P%d %.3f %.3f\n", i, gPx[i], gPy[i]);
            for (int i = 0; i < gNT * 3; i++) printf("  V%d %.3f %.3f\n", i, gT[i].x, gT[i].y);
            int k = 0;
            for (int y = 0; y < H && k < 6; y++) for (int x = 0; x < W && k < 6; x++) {
                const int v = (int)(sFB[y * W + x] & 0xFF); const double ib = inside_by(x + 0.5, y + 0.5);
                if (ib < -1e-3 && v != 0) { printf("  out (%d,%d) v %d by %.3f\n", x, y, v, ib); k++; }
            }
        }
        if (t || c || s) printf("frame %d (mesh %d, %s, %d tris): %ld twice, %ld cracks, %ld outside\n", fr, kind,
                                textured ? "textured" : "untextured", gNT, t, c, s);
        twice += t; crack += c; spill += s;
        if (getenv("FT_HLOG")) printf("h %d %08x\n", fr, (unsigned)hsum);
    }
    /* lerpv: S + (K - S) * T / 255, exact */
    long lbad = 0;
    for (int fr = 0; fr < frames / 4; fr++) {
        uint8_t S[3], K[3]; /* (one statement each: an initializer list's or an argument list's order is the compiler's) */
        for (int ch = 0; ch < 3; ch++) S[ch] = (uint8_t)rnd();
        for (int ch = 0; ch < 3; ch++) K[ch] = (uint8_t)rnd();
        const uint32_t T = 0xFF000000u | (rnd() & 0xFFFFFF);
        const uint8_t Tc[3] = { (uint8_t)(T >> 16), (uint8_t)(T >> 8), (uint8_t)T };
        upload_uniform(T);
        gNT = 0;
        const float cx = frnd(40, 200), cy = frnd(40, 200), rx = frnd(5, 120), ry = frnd(5, 120);
        convex(2, cx, cy, rx, ry, 0);
        SoftVtx v[16];
        if (gNP < 3) { fr--; continue; }
        for (int i = 0; i < gNP; i++) v[i] = vtx((float)gPx[i], (float)gPy[i], S[0], S[1], S[2], 255);
        for (int i = 1; i + 1 < gNP; i++) tri(v[0], v[i], v[i + 1]);
        for (int i = 0; i < W * H; i++) { sFB[i] = 0xFF000000u; sZB[i] = 0xFFFF; }
        gfx_soft_set_own_edges(false);
        gfx_soft_bilinear = (rnd() & 1) != 0;
        sDepthTest = (rnd() & 1) != 0; sDepthMask = (rnd() & 1) != 0; sZDecal = false; sUseAlpha = false;
        gProg.rgb_tex = true; gProg.rgb_in = true; gProg.a_tex = false; gProg.a_in = true; gProg.rgb_texa = false;
        gProg.cc.used_textures[0] = true; sCurShader = &gProg; sForceIn = true;
        gfx_scegu_set_tex_lerpv(true); gfx_scegu_set_tex_lerp(true, K[0], K[1], K[2]);
        sBoundTex[0] = gTex;
        gfx_soft_draw_triangles((float *)gT, (size_t)gNT * 3 * sizeof(SoftVtx) / 4, (size_t)gNT);
        sky_settle();
        uint32_t want = 0xFF000000u;
        for (int ch = 0; ch < 3; ch++) want |= (uint32_t)(uint8_t)(S[ch] + ((K[ch] - S[ch]) * Tc[ch]) / 255) << (16 - 8 * ch);
        long n = 0, b = 0;
        for (int i = 0; i < W * H; i++) if (sFB[i] != 0xFF000000u || want == 0xFF000000u) { n++; if (sFB[i] != want && sFB[i] != 0xFF000000u) b++; }
        if (b) printf("lerpv frame %d: %ld of %ld px wrong (S %d,%d,%d K %d,%d,%d T %06X)\n", fr, b, n, S[0], S[1], S[2], K[0], K[1], K[2], (unsigned)(T & 0xFFFFFF));
        lbad += b;
        for (int i = 0; i < W * H; i++) hsum = (hsum ^ sFB[i]) * 16777619u;
    }
    gfx_scegu_set_tex_lerpv(false);
    printf("fill_test: %d frames, %ld px drawn once, %ld centres within 1e-3 of an outline; own: %ld twice, %ld cracks, %ld outside; "
           "old rule: %ld px twice in %ld frames; lerpv: %ld px wrong; hash %08x\n",
           frames, drawn, ties, twice, crack, spill, oldtwice, oldframes, lbad, (unsigned)hsum);
    return (twice || crack || spill || lbad) ? 1 : 0;
}
