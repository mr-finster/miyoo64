/* fill_test.c: GE_FILLRULE (gfx_soft.c own_edge) through the real rasterizer (2026-10-03; the OoT port's
 * tools/handheld/fill_test.c, the Deku Tree's flame line, ported). Includes port/hh/gfx/gfx_soft.c and drives raster_tri.
 *
 * Random meshes whose triangles share edges -- convex quads split on a diagonal, convex polygons as fans (the clip
 * fan's shape), the guard-band square (-120..360, its diagonals through pixel centres), grids on half-pixel steps
 * (centres exactly on the edges), explosion-like rotated quads (explosion.c's billboards); the convex outlines are
 * hulls of random points -- drawn blended (alpha 128, white over black, z-tested, no z write), untextured (the scalar
 * loops) and textured with a white texture (texture x shade: on ARM the lean blended rows, GE_LEANBLEND).
 * With gfx_soft_set_own_edges(1):
 *   - no pixel may be drawn twice (> 140: two blends of 128), every pixel centre inside the mesh's outline by more
 *     than 1e-3 px must be drawn once (no crack), outside by more than 1e-3 not at all;
 *   - every row loop draws the same pixels: on ARM the scalar loops (scal), the build's defaults (base: the lean
 *     rows a call a row from raster_tri_body), the row loop in hot_tri_lean (leantri, GE_LEANTRI=1) and a mix that
 *     picks one of the three per triangle (mix: a quad drawn half by each path) -- byte for byte;
 *   - the frame hash (scal) is the same on ARM and x86.
 * The same frames with the switch off count the doubled pixels the old rule leaves (reported, expected > 0).
 *
 *   fill_test [seed] [frames]   prints the counts and a hash of the frames; exit 1 on any failure
 * Build and run: port/hh/tools/fill_test.sh (ARM under qemu-arm and x86 -m32; the hashes must agree). */
#include "gfx_soft.c"

#include <stdarg.h>

uint32_t gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
int gfx_trace_frames;
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
void gfx_cc_get_features(uint32_t id, struct CCFeatures *cc) { (void)id; memset(cc, 0, sizeof(*cc)); }
int bossGetStageNum(void) { return 33; }
int gfx_pc_frame_w(void) { return 320; }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
static struct ShaderProgram gProg;
static uint32_t gTex;

static void upload_white(void) {
    static uint8_t buf[32 * 32 * 4];
    memset(buf, 0xFF, sizeof(buf));
    gfx_soft_select_texture(0, gTex);
    gfx_soft_upload_texture(buf, 32, 32, GU_PSM_8888);
    gfx_soft_set_sampler_parameters(0, true, 0, 0);
}

static SoftVtx vtx(float x, float y) {
    SoftVtx v; memset(&v, 0, sizeof(v));
    v.x = x; v.y = y; v.z = 0.5f; v.invw = 1.0f; v.u = frnd(0, 1); v.v = frnd(0, 1);
    v.r = v.g = v.b = 255; v.a = 128;
    return v;
}

/* the outline (convex, either winding) and the triangles; each triangle textured or not (gTx) */
static double gPx[16], gPy[16]; static int gNP;
static SoftVtx gT[3 * 256]; static uint8_t gTx[256], gPath[256]; static int gNT, gTexMode;
static void tri(SoftVtx a, SoftVtx b, SoftVtx c) {
    if (gNT >= 256) return;
    if (rnd() & 1) { SoftVtx t = b; b = c; c = t; } /* either winding: the rasterizer takes both */
    gT[gNT * 3] = a; gT[gNT * 3 + 1] = b; gT[gNT * 3 + 2] = c;
    gTx[gNT] = gTexMode == 2 ? (uint8_t)(rnd() & 1) : (uint8_t)gTexMode; /* 2: mixed within the mesh */
    gPath[gNT] = (uint8_t)(rnd() % 3);
    gNT++;
}
static float snap(float v, int mode) { return mode ? floorf(v * 2.0f) * 0.5f : v; } /* half pixels: centres on edges */
/* a convex polygon: the hull of random points in a box (no trig: libm differs between the ARM and x86 builds) */
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
    const int kind = (int)(rnd() % 5), sm = (int)(rnd() % 2);
    gTexMode = (int)(rnd() % 3);
    gNT = 0;
    if (kind == 0 || kind == 1) { /* 0: a quad on a diagonal; 1: a fan (the clip fan) */
        const int n = kind == 0 ? 2 : 3 + (int)(rnd() % 4);
        const float big = (rnd() % 3 == 0) ? 3.0f : 1.0f; /* past the screen: the guard band's sizes */
        const float cx = frnd(40, 200), cy = frnd(40, 200), rx = frnd(10, 110) * big, ry = frnd(10, 110) * big;
        convex(n, cx, cy, rx, ry, sm);
        if (gNP < 3) return make_mesh();
        SoftVtx v[16];
        for (int i = 0; i < gNP; i++) v[i] = vtx((float)gPx[i], (float)gPy[i]);
        const int o = (int)(rnd() % gNP); /* the fan's apex */
        for (int i = 1; i + 1 < gNP; i++) tri(v[o], v[(o + i) % gNP], v[(o + i + 1) % gNP]);
    } else if (kind == 2) { /* the guard-band square, either diagonal */
        const float lo = -120.0f, hi = 360.0f;
        gNP = 4; gPx[0] = lo; gPy[0] = lo; gPx[1] = hi; gPy[1] = lo; gPx[2] = hi; gPy[2] = hi; gPx[3] = lo; gPy[3] = hi;
        SoftVtx v[4];
        for (int i = 0; i < 4; i++) v[i] = vtx((float)gPx[i], (float)gPy[i]);
        if (rnd() & 1) { tri(v[0], v[1], v[2]); tri(v[0], v[2], v[3]); } else { tri(v[1], v[2], v[3]); tri(v[1], v[3], v[0]); }
    } else if (kind == 3) { /* a grid on half-pixel steps, the diagonals mixed */
        const int nx = 2 + (int)(rnd() % 6), ny = 2 + (int)(rnd() % 6);
        float x0 = floorf(frnd(-20, 150)), y0 = floorf(frnd(-20, 150));
        if (rnd() & 1) x0 += 0.5f;
        if (rnd() & 1) y0 += 0.5f;
        const float sx = (float)(1 + rnd() % 30) * 0.5f, sy = (float)(1 + rnd() % 30) * 0.5f;
        gNP = 4; gPx[0] = x0; gPy[0] = y0; gPx[1] = x0 + nx * sx; gPy[1] = y0; gPx[2] = x0 + nx * sx; gPy[2] = y0 + ny * sy;
        gPx[3] = x0; gPy[3] = y0 + ny * sy;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                SoftVtx a = vtx(x0 + i * sx, y0 + j * sy), b = vtx(x0 + (i + 1) * sx, y0 + j * sy);
                SoftVtx c = vtx(x0 + (i + 1) * sx, y0 + (j + 1) * sy), d = vtx(x0 + i * sx, y0 + (j + 1) * sy);
                if (rnd() & 1) { tri(a, b, c); tri(a, c, d); } else { tri(a, b, d); tri(b, c, d); }
            }
    } else { /* explosion.c's billboard: a rotated square, 0-1-2 and 0-2-3, corners whole pixels at times */
        const float cx = frnd(-20, 260), cy = frnd(-20, 260), r = (rnd() % 4) ? frnd(5, 60) : frnd(60, 130);
        const float co = frnd(-1.0f, 1.0f), si = (rnd() & 1 ? 1.0f : -1.0f) * sqrtf(1.0f - co * co);
        const float ox[4] = { 1, -1, -1, 1 }, oy[4] = { -1, -1, 1, 1 };
        const int whole = rnd() % 3 == 0;
        SoftVtx c[4];
        for (int k = 0; k < 4; k++) {
            float x = cx + r * (ox[k] * co - oy[k] * si), y = cy + r * (ox[k] * si + oy[k] * co);
            if (whole) { x = floorf(x); y = floorf(y); }
            c[k] = vtx(x, y);
        }
        /* the outline: the quad as drawn (whole-pixel corners can make it non-convex by a hair: hull it) */
        double qx[4], qy[4];
        for (int k = 0; k < 4; k++) { qx[k] = c[k].x; qy[k] = c[k].y; }
        double ar = 0; for (int k = 0; k < 4; k++) ar += qx[k] * qy[(k + 1) % 4] - qx[(k + 1) % 4] * qy[k];
        gNP = 4;
        for (int k = 0; k < 4; k++) { gPx[k] = qx[k]; gPy[k] = qy[k]; }
        int convexq = 1; /* both triangles wind the quad's way: a convex quad */
        for (int k = 0; k < 4; k++) {
            const int a = k, b = (k + 1) % 4, d = (k + 2) % 4;
            const double cr = (qx[b] - qx[a]) * (qy[d] - qy[a]) - (qy[b] - qy[a]) * (qx[d] - qx[a]);
            if (cr * ar <= 0) convexq = 0;
        }
        if (!convexq) return make_mesh();
        tri(c[0], c[1], c[2]); tri(c[0], c[2], c[3]);
    }
    return kind;
}
/* signed distance of a pixel centre to the outline: > 0 inside by that much (the outline is convex) */
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

/* the row loops: scal = the scalar loops (raster_tri_body's rows, as the desktop), base = the build's defaults (the
 * lean blended rows a call a row), leantri = GE_LEANTRI=1 (the row loop in hot_tri_lean_body), mix = one a triangle */
/* slow, baseslow (2026-10-04): scal and base with GE_OWNFAST=0 (own_px's row ends, the code before own_row) */
enum { V_SCAL, V_BASE, V_LEANTRI, V_MIX, V_SLOW, V_BSLOW, V_N };
static const char *kName[V_N] = { "scal", "base", "leantri", "mix", "slow", "baseslow" };
#if defined(HH_CM_NEON)
static int sFtDef[8];
#endif
static void set_path(int v) {
    sOwnFast = v != V_SLOW && v != V_BSLOW;
    if (v == V_SLOW) v = V_SCAL;
    if (v == V_BSLOW) v = V_BASE;
#if defined(HH_CM_NEON)
    const int neon = v != V_SCAL;
    sCmNeon = neon; sMipNeon = neon; sVkNeon = neon; sVkaNeon = neon; sCmbNeon = neon; sCmNoZ = neon; sHotRow = neon;
    sLeanRow = neon ? sFtDef[0] : 0; sLeanBlend = neon ? sFtDef[1] : 0; sLeanNoz = neon ? sFtDef[2] : 0;
    sLeanVk = neon ? sFtDef[3] : 0; sLeanNp2 = neon ? sFtDef[4] : 0;
    sLeanTri = v == V_LEANTRI ? 1 : v == V_BASE ? sFtDef[5] : 0;
    if (v == V_LEANTRI) { sLeanRow = 1; sLeanBlend = 1; }
#else
    (void)v;
#endif
}

static void draw(bool own, int variant) {
    for (int i = 0; i < W * H; i++) { sFB[i] = 0xFF000000u; sZB[i] = 0xFFFF; }
    gfx_soft_set_own_edges(own);
    gfx_soft_bilinear = 1;
    sDepthTest = true; sDepthMask = false; sZDecal = false; sUseAlpha = true; sUiMode = false;
    sTexAdd = false; sTexLerp = false; sForceIn = false;
    gProg.rgb_in = true; gProg.a_tex = false; gProg.a_in = true; gProg.rgb_texa = false;
    gProg.cc.opt_texture_edge = false; gProg.cc.num_inputs = 1;
    sCurShader = &gProg;
    sBoundTex[0] = gTex;
    for (int t = 0; t < gNT; t++) {
        gProg.rgb_tex = gTx[t]; gProg.cc.used_textures[0] = gTx[t];
        set_path(variant == V_MIX ? (int)gPath[t] : variant);
        raster_tri(&gT[t * 3], &gT[t * 3 + 1], &gT[t * 3 + 2], true);
    }
    set_path(V_BASE);
}

int main(int argc, char **argv) {
    rs = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u;
    if (!rs) rs = 1;
    const int frames = argc > 2 ? atoi(argv[2]) : 2000;
    gfx_soft_init();
    select_target(true);
    sNatStale = false;
#if defined(HH_CM_NEON)
    if (sHotRow < 0) { const char *e = getenv("GE_HOTROW"); sHotRow = !(e && e[0] == '0'); }
    if (sLeanRow < 0) { const char *e = getenv("GE_LEANROW"); sLeanRow = !(e && e[0] == '0'); }
    if (sLeanBlend < 0) { const char *e = getenv("GE_LEANBLEND"); sLeanBlend = !(e && e[0] == '0'); }
    if (sLeanNoz < 0) { const char *e = getenv("GE_LEANNOZ"); sLeanNoz = !(e && e[0] == '0'); }
    if (sLeanVk < 0) { const char *e = getenv("GE_LEANVK"); sLeanVk = !(e && e[0] == '0'); }
    if (sLeanNp2 < 0) { const char *e = getenv("GE_LEANNP2"); sLeanNp2 = !(e && e[0] == '0'); }
    if (sLeanTri < 0) { const char *e = getenv("GE_LEANTRI"); sLeanTri = e && e[0] == '1'; }
    if (sLeanAsm < 0) { const char *e = getenv("GE_LEANASM"); sLeanAsm = e && e[0] == '1'; }
    if (sVkaNeon < 0) { const char *e = getenv("GE_VKANEON"); sVkaNeon = !(e && e[0] == '0'); }
    if (sCmbNeon < 0) { const char *e = getenv("GE_CMBNEON"); sCmbNeon = !(e && e[0] == '0'); }
    sFtDef[0] = sLeanRow; sFtDef[1] = sLeanBlend; sFtDef[2] = sLeanNoz; sFtDef[3] = sLeanVk; sFtDef[4] = sLeanNp2; sFtDef[5] = sLeanTri;
    static const int vlist[] = { V_SCAL, V_BASE, V_LEANTRI, V_MIX, V_SLOW, V_BSLOW };
    const int nv = V_N;
#else
    static const int vlist[] = { V_SCAL, V_SLOW };
    const int nv = 2; /* x86: the scalar loops only (own_row and own_px) */
#endif
    gTex = gfx_soft_new_texture();
    upload_white();
    static uint32_t ref[W * H];
    long twice = 0, crack = 0, spill = 0, oldtwice = 0, oldframes = 0, drawn = 0, ties = 0, vdiff = 0;
    uint32_t hsum = 2166136261u;
    for (int fr = 0; fr < frames; fr++) {
        const int kind = make_mesh();
        draw(false, V_SCAL); /* the old rule: how many pixels it draws twice */
        long d0 = 0;
        for (int i = 0; i < W * H; i++) d0 += (sFB[i] & 0xFF) > 140;
        oldtwice += d0; oldframes += d0 > 0;
        for (int v = 0; v < nv; v++) {
            draw(true, vlist[v]);
            if (v == 0) { memcpy(ref, sFB, sizeof(ref)); continue; }
            long d = 0;
            for (int i = 0; i < W * H; i++) d += sFB[i] != ref[i];
            if (d) printf("frame %d (mesh %d): %s vs scal: %ld px differ\n", fr, kind, kName[vlist[v]], d);
            vdiff += d;
        }
        long t = 0, c = 0, s = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int v = (int)(ref[y * W + x] & 0xFF);
                const double ib = inside_by(x + 0.5, y + 0.5);
                if (v > 140) t++;
                if (ib > 1e-3 && v != 128) c++;
                if (ib < -1e-3 && v != 0) s++;
                if (v == 128) drawn++;
                if (fabs(ib) <= 1e-3) ties++;
                hsum = (hsum ^ ref[y * W + x]) * 16777619u;
            }
        if (t || c || s) printf("frame %d (mesh %d, %d tris): %ld twice, %ld cracks, %ld outside\n", fr, kind, gNT, t, c, s);
        twice += t; crack += c; spill += s;
    }
    printf("fill_test: %d frames, %d row loops, %ld px drawn once, %ld centres within 1e-3 of an outline; own: %ld twice, "
           "%ld cracks, %ld outside, %ld px differ between row loops; old rule: %ld px twice in %ld frames; hash %08x\n",
           frames, nv, drawn, ties, twice, crack, spill, vdiff, oldtwice, oldframes, (unsigned)hsum);
    return (twice || crack || spill || vdiff || oldtwice == 0) ? 1 : 0;
}
