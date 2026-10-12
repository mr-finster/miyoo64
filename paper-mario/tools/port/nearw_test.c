/* nearw_test.c: PM64_NEARW (2026-10-04, the near-camera audit after goldeneye-007 c20b9ee; ocarina-of-time's dl_test
 * nearw) through the renderer front end: includes bench/kit/gfx_pc.c (its statics reachable); no ROM, no SDL; the
 * rasterizer's and the bench's entry points these paths never reach are left unresolved.
 *
 *   nearw_test nearw [seed] [n]   n random triangles passing the eye (Paper Mario's camera: guPerspective 25 deg, 4:3,
 *                                 near 16 (CAM_NEAR_CLIP), far 4096; vertices from 3 units behind the eye to 40 in
 *                                 front, half of them within 4; side extents around the guard band; clip ratio 1 or 2),
 *                                 through the port's own decisions -- gfx_sp_vertex's flags (copied: they are inline
 *                                 there), gfx_sp_tri1's shared-flag reject (gfx_reject_keep) and trigger
 *                                 (gfx_clip_trigger), gfx_clip_single_vert and its fan -- against the N64's NoN rule
 *                                 (F3DZEX2 NoN: drawn where w > 0 and z <= w; the guard band only trims what is off the
 *                                 screen). 48 points a triangle (barycentric, away from its own edges); each on the
 *                                 screen and in front of the eye is drawn by the N64 or not, and by the port when it
 *                                 lies in one of the triangles the port emits (projected, within an eighth of a pixel:
 *                                 NEARW_EPS). Points within 1e-4 of a plane
 *                                 are left out. Prints the disagreements nearer than W_MIN (the sliver the port gives
 *                                 up: w < 1/64 with PM64_NEARW, w < 2 without) and beyond it (must be 0); exit 1 on any.
 * Build and run: tools/port/nearw_test.sh (ARM under qemu-arm and x86 -m32). */
#include "gfx_pc.c"
#include <stdarg.h>

u32 gPortProfFrame[PROF_COUNT];
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static int irnd(int n) { return (int)(rnd() % (uint32_t)n); }
static void persp(float m[4][4], float fovy, float aspect, float n, float f) { /* guPerspectiveF, scale 1 */
    const float c = 1.0f / tanf(fovy * 3.14159265f / 360.0f);
    memset(m, 0, 64);
    m[0][0] = c / aspect; m[1][1] = c; m[2][2] = (n + f) / (n - f); m[2][3] = -1; m[3][2] = 2 * n * f / (n - f);
}
/* gfx_sp_vertex's clip flags for a perspective vertex (the block in its loop, as it stands) */
static uint32_t vtx_flags(float x, float y, float z, float w) {
    const float aw = w < 0.0f ? -w : w, gw = GUARD_BAND * aw;
    uint32_t rej = 0;
    if (x < -gw) rej |= X_POS;
    if (x > gw) rej |= X_NEG;
    if (y < -gw) rej |= Y_POS;
    if (y > gw) rej |= Y_NEG;
    if (x < -w) rej |= X_OUT_L;
    if (x > w) rej |= X_OUT_R;
    if (y < -w) rej |= Y_OUT_B;
    if (y > w) rej |= Y_OUT_T;
    if (z < -w) rej |= Z_POS;
    if (z > w) rej |= Z_NEG;
    const float gb = gfx_clip_ratio * aw;
    if (x < -gb || x > gb || y < -gb || y > gb) rej |= GB_OUT;
    if (w < W_MIN) rej |= W_LOW;
    return rej;
}

/* nearw: the point (px, py) inside the projected triangle or within NEARW_EPS of it (NDC; 1/1024 is an eighth of a
 * pixel at 240): the clipper's vertices are floats, and a sliver's edge moves by their rounding */
#define NEARW_EPS (1.0 / 1024.0)
static bool in_tri_ndc(double px, double py, double x0, double y0, double x1, double y1, double x2, double y2) {
    const double a = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (a == 0.0) return false;
    const double sg = a > 0.0 ? 1.0 : -1.0, vx[3] = { x0, x1, x2 }, vy[3] = { y0, y1, y2 };
    for (int e = 0; e < 3; e++) {
        const double ax = vx[e], ay = vy[e], bx = vx[(e + 1) % 3], by = vy[(e + 1) % 3];
        const double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        if (len == 0.0) return false;
        if (sg * ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / len < -NEARW_EPS) return false;
    }
    return true;
}
static int nearw_mode(int seed, int n) {
    float P[4][4];
    persp(P, 25.0f, 4.0f / 3.0f, 16.0f, 4096.0f);
    const float wmin = W_MIN;
    rs = (uint32_t)seed * 2654435761u + 7;
    long tris = 0, clipped = 0, rejected = 0, samples = 0, n64 = 0, near_bad = 0, far_bad = 0, near_pts = 0;
    for (int it = 0; it < n; it++) {
        gfx_clip_ratio = (float)(1 + irnd(2));
        const float r = gfx_clip_ratio;
        struct LoadedVertex v[3];
        memset(v, 0, sizeof(v));
        for (int k = 0; k < 3; k++) {
            const float w = irnd(2) ? frnd(-3.0f, 4.0f) : frnd(-3.0f, 40.0f), ze = -w;
            const float s = r * (fabsf(ze) + 0.5f) * (4.0f / 3.0f) / P[1][1];
            const float e[4] = { frnd(-1.4f, 1.4f) * s, frnd(-1.4f, 1.4f) * s * 0.75f, ze, 1.0f };
            float c[4];
            for (int j = 0; j < 4; j++) c[j] = e[0] * P[0][j] + e[1] * P[1][j] + e[2] * P[2][j] + e[3] * P[3][j];
            v[k].x = e[0]; v[k].y = e[1]; v[k].z = e[2]; v[k].w = 1.0f;
            v[k]._x = c[0]; v[k]._y = c[1]; v[k]._z = c[2]; v[k]._w = c[3];
            v[k].u = frnd(0, 64); v[k].v = frnd(0, 64);
            v[k].clip_rej = vtx_flags(c[0], c[1], c[2], c[3]);
        }
        tris++;
        /* the port's triangles: gfx_sp_tri1's shared-flag reject, then its trigger and the clipper */
        static struct LoadedVertex out[32];
        size_t nout = 0;
        struct LoadedVertex *va[3] = { &v[0], &v[1], &v[2] };
        if (v[0].clip_rej & v[1].clip_rej & v[2].clip_rej & ~gfx_reject_keep) { rejected++; nout = 0; }
        else if ((v[0].clip_rej | v[1].clip_rej | v[2].clip_rej) & gfx_clip_trigger) {
            gfx_clip_single_vert(out, &nout, va); clipped++;
        } else { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; nout = 3; }
        float sx[32], sy[32];
        for (size_t i = 0; i < nout; i++) { sx[i] = out[i]._x / out[i]._w; sy[i] = out[i]._y / out[i]._w; }
        for (int m = 0; m < 48; m++) {
            float a = frnd(0.002f, 1.0f), b = frnd(0.002f, 1.0f), c = frnd(0.002f, 1.0f);
            const float t = a + b + c; a /= t; b /= t; c /= t;
            if (a < 0.001f || b < 0.001f || c < 0.001f) continue;
            float p[4];
            for (int j = 0; j < 4; j++) p[j] = a * (&v[0]._x)[j] + b * (&v[1]._x)[j] + c * (&v[2]._x)[j];
            const float x = p[0], y = p[1], z = p[2], w = p[3];
            if (!(w > 0.0f)) continue;
            const float eps = 1e-4f * (w > 1.0f ? w : 1.0f);
            if (fabsf(x) > w - eps || fabsf(y) > w - eps) continue;                /* off the screen (or on its edge) */
            if (fabsf(z - w) < eps || fabsf(w - wmin) < 1e-4f * wmin || fabsf(w - 2.0f) < 2e-4f) continue; /* on a plane */
            samples++;
            const bool nd = z <= w;                                                 /* the N64 (NoN): w > 0, z <= w */
            n64 += nd;
            bool pd = false;
            const float nx = x / w, ny = y / w;
            for (size_t i = 0; i + 2 < nout && !pd; i += 3) {
                const float x0 = sx[i], y0 = sy[i], x1 = sx[i + 1], y1 = sy[i + 1], x2 = sx[i + 2], y2 = sy[i + 2];
                pd = in_tri_ndc(nx, ny, x0, y0, x1, y1, x2, y2);
            }
            if (w < 2.0f) near_pts++;
            if (pd != nd) { if (w < wmin) near_bad++; else { if (far_bad < 5) printf("  tri %d: point w %g z %g x %g y %g: N64 %d, port %d\n", it, w, z, x, y, nd, pd); far_bad++; } }
        }
    }
    printf("nearw: PM64_NEARW=%d, W_MIN %g: %ld triangles (%ld clipped, %ld rejected), %ld points on the screen in front of the "
           "eye (%ld within 2 units), the N64 draws %ld; the port differs at %ld nearer than W_MIN, %ld beyond\n",
           gfx_wmin < 1.0f, wmin, tris, clipped, rejected, samples, near_pts, n64, near_bad, far_bad);
    return far_bad != 0;
}


int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "nearw";
    const int seed = argc > 2 ? atoi(argv[2]) : 1, n = argc > 3 ? atoi(argv[3]) : 100000;
    { /* gfx_init's switch block (gfx_init itself opens the window): the clipper's planes and PM64_NEARW */
        const char *e;
        if ((e = getenv("PM64_CLIPEDGES")) && e[0] >= '0' && e[0] <= '2') gfx_clipedges = e[0] - '0';
        if ((e = getenv("PM64_NEARCLIP")) && e[0]) gfx_nearclip = e[0] != '0';
        gfx_clip_trigger = Z_NEG | W_LOW | (gfx_nearclip ? Z_POS : 0) | (gfx_clipedges == 2 ? GB_OUT : 0);
        gfx_reject_keep = XY_OUT | GB_OUT | (gfx_nearclip ? 0 : Z_POS);
        e = getenv("PM64_NEARW"); gfx_wmin = !(e && e[0] == '0') && !gfx_nearclip ? 1.0f / 64.0f : 2.0f;
    }
    if (!strcmp(mode, "nearw")) return nearw_mode(seed, n);
    fprintf(stderr, "usage: nearw_test nearw [seed] [n]\n");
    return 2;
}
