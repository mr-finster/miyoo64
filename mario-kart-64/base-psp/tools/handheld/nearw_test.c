/* nearw_test.c: MK64_NEARW (2026-10-04, the near-camera audit after the GoldenEye port c20b9ee) through the real
 * renderer front end: includes base-psp/src/port/gfx/gfx_pc.c (its statics reachable); no ROM, no SDL.
 *
 *   nearw_test wmin                 the w plane (W_MIN) gfx_sp_matrix leaves for each of the game's projections:
 *                                   guPerspective (the game's own guPerspectiveF, its fovy and aspect) with every
 *                                   near/far set_perspective_and_aspect_ratio uses, the menus' and the ceremony's,
 *                                   loaded then multiplied by a random rigid lookAt as the game does, as fixed-point
 *                                   Mtx (s15.16). With MK64_NEARW=1: 2.0 bit for bit wherever near >= 2, and the
 *                                   -0.97w plane (+1e-4) where near < 2 (Koopa Troopa Beach, near 1); =0: 2.0 always
 *   nearw_test clip <near> [seed] [n]  n random triangles passing the eye under that projection (far 5000), through
 *                                   the vertex flags, gfx_sp_tri1's reject and trigger and gfx_clip_single_vert, against
 *                                   the N64's near-plane microcode rule (F3DEX: drawn where w > 0, -w <= z <= w), 48
 *                                   points a triangle on the screen, points within 1e-4 of a plane left out. Prints
 *                                   the disagreements by w: nearer than W_MIN (the port's cut; with the switch on and
 *                                   near 1 only the N64's w 1..1.015, where the port's -0.97w plane cuts) and beyond
 *                                   it (near >= 2: the clipper's parked B1/B3, the same with the switch off).
 * Build and run: base-psp/tools/handheld/nearw_test.sh (ARM under qemu-arm and x86 -m32). */
#include "gfx_pc.c"
#include <stdarg.h>

/* the rest of the game and port gfx_pc.c names: the data these paths read */
s32 gGamestate = 4;
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
int gfx_soft_last_frame_had_3d;
int handheld_cfg_unsquish3d(void) { return 1; }
int handheld_cfg_nearw(void) { return -1; } /* no settings file: MK64_NEARW or on */
char _ftext[16], _fbss[16], __assets_start[16], __assets_end[16];
/* the rasterizer's and texture manager's entry points: never reached by these paths (linked as unresolved, 0) */
u32 gPortProfFrame[PROF_COUNT];
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
u32 port_time_us(void) { return 0; }

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

/* guPerspectiveF (libultra, as src/os/guPerspectiveF.c) and guMtxF2L's s15.16 layout */
static void persp(float m[4][4], float fovy, float aspect, float n, float f, float scale) {
    const float c = 1.0f / tanf(fovy * 3.14159265358979f / 360.0f);
    memset(m, 0, 64);
    m[0][0] = c / aspect; m[1][1] = c; m[2][2] = (n + f) / (n - f); m[2][3] = -1; m[3][2] = 2 * n * f / (n - f);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[i][j] *= scale;
}
static void f2l(int32_t out[16], const float m[4][4]) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j += 2) {
        const int32_t a = (int32_t)(m[i][j] * 65536.0f), b = (int32_t)(m[i][j + 1] * 65536.0f);
        out[i * 2 + j / 2] = (int32_t)(((uint32_t)a & 0xffff0000u) | ((uint32_t)b >> 16));
        out[8 + i * 2 + j / 2] = (int32_t)(((uint32_t)a << 16) | ((uint32_t)b & 0xffffu));
    }
}
static void lookat(float m[4][4]) { /* a rigid view: random rotation, eye anywhere on a course */
    const float a = frnd(-3.2f, 3.2f), b = frnd(-1.2f, 1.2f);
    const float ex = frnd(-3000, 3000), ey = frnd(-200, 600), ez = frnd(-3000, 3000);
    const float fx = cosf(b) * sinf(a), fy = sinf(b), fz = cosf(b) * cosf(a); /* forward */
    float rx = fz, ry = 0, rz = -fx; const float rl = sqrtf(rx * rx + rz * rz); rx /= rl; rz /= rl; /* right */
    const float ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;     /* up */
    float r[3][3] = { { rx, ux, -fx }, { ry, uy, -fy }, { rz, uz, -fz } };
    memset(m, 0, 64);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = r[i][j];
    m[3][0] = -(ex * rx + ey * ry + ez * rz); m[3][1] = -(ex * ux + ey * uy + ez * uz); m[3][2] = ex * fx + ey * fy + ez * fz;
    m[3][3] = 1;
}
static void load_proj(float n, float f, float fovy, int with_lookat) {
    float P[4][4], L[4][4]; int32_t mtx[16];
    persp(P, fovy, 4.0f / 3.0f, n, f, 1.0f);
    f2l(mtx, P); gfx_sp_matrix(G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH, mtx);
    if (with_lookat) { lookat(L); f2l(mtx, L); gfx_sp_matrix(G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH, mtx); }
}

static int wmin_mode(void) {
    static const struct { const char *what; float n, f; } k[] = {
        { "Koopa Troopa Beach", 1, 5000 }, { "Bowser's Castle, Banshee, Rainbow, Block Fort, Skyscraper", 2, 2700 },
        { "Choco Mountain, Double Deck", 2, 1500 }, { "Wario Stadium", 10, 4800 }, { "Mario Raceway and 8 more", 9, 4500 },
        { "Kalamari Desert", 10, 7000 }, { "other courses, menus' 3D, ceremony", 3, 6800 }, { "menu_items", 100, 12800 } };
    int bad = 0;
    rsp.modelview_matrix_stack_size = 1;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        float lo = 1e9f, hi = -1e9f;
        for (int t = 0; t < 2000; t++) {
            load_proj(k[i].n, k[i].f, frnd(30.0f, 70.0f), t > 0);
            if (W_MIN < lo) lo = W_MIN; if (W_MIN > hi) hi = W_MIN;
        }
        const float want = !gfx_nearw_on || k[i].n >= 2.0f ? 2.0f : 0.0f;
        const bool ok = want == 2.0f ? (lo == 2.0f && hi == 2.0f) : (lo > k[i].n && hi < k[i].n * 1.02f);
        printf("wmin: near %g far %g (%s): W_MIN %.6f .. %.6f %s\n", k[i].n, k[i].f, k[i].what, lo, hi, ok ? "ok" : "WRONG");
        bad += !ok;
    }
    printf("wmin: MK64_NEARW=%d, %d wrong\n", gfx_nearw_on, bad);
    return bad != 0;
}

static int clip_mode(float near, int seed, int n) {
    float P[4][4];
    persp(P, 50.0f, 4.0f / 3.0f, near, 5000.0f, 1.0f);
    rs = (uint32_t)seed * 2654435761u + 3;
    rsp.modelview_matrix_stack_size = 1;
    load_proj(near, 5000.0f, 50.0f, 0);
    const float wmin = W_MIN;
    long tris = 0, clipped = 0, samples = 0, n64 = 0, near_bad = 0, far_bad = 0, sliver = 0;
    for (int it = 0; it < n; it++) {
        struct LoadedVertex v[3];
        memset(v, 0, sizeof(v));
        for (int k = 0; k < 3; k++) {
            const float w = (rnd() & 1) ? frnd(-2.0f, 3.0f * near + 2.0f) : frnd(-2.0f, 60.0f), ze = -w;
            const float s = (fabsf(ze) + 0.5f) * (4.0f / 3.0f) / P[1][1];
            const float e[4] = { frnd(-2.5f, 2.5f) * s, frnd(-2.5f, 2.5f) * s * 0.75f, ze, 1.0f };
            float c[4];
            for (int j = 0; j < 4; j++) c[j] = e[0] * P[0][j] + e[1] * P[1][j] + e[2] * P[2][j] + e[3] * P[3][j];
            v[k]._x = c[0]; v[k]._y = c[1]; v[k]._z = c[2]; v[k]._w = c[3];
            /* gfx_sp_vertex's flags (the CPU path) */
            const float x = c[0], y = c[1], z = c[2], ww = c[3], aw = ww < 0.0f ? -ww : ww, gw = GUARD_BAND * aw;
            uint32_t r = 0;
            if (x < -gw) r |= X_POS; if (x > gw) r |= X_NEG; if (y < -gw) r |= Y_POS; if (y > gw) r |= Y_NEG;
            if (x < -ww) r |= X_OUT_L; if (x > ww) r |= X_OUT_R; if (y < -ww) r |= Y_OUT_B; if (y > ww) r |= Y_OUT_T;
            if (z < -ww) r |= Z_POS;
            if (gfx_farclip_on ? (ww > 0.0f && z > ww) : (z > FAR_GUARD * aw)) r |= Z_NEG;
            v[k].clip_rej = r;
        }
        tris++;
        static struct LoadedVertex out[32];
        size_t nout = 0;
        struct LoadedVertex *va[3] = { &v[0], &v[1], &v[2] };
        float wm = v[0]._w; if (v[1]._w < wm) wm = v[1]._w; if (v[2]._w < wm) wm = v[2]._w;
        if (v[0].clip_rej & v[1].clip_rej & v[2].clip_rej) nout = 0; /* gfx_sp_tri1's reject */
        else if (wm < W_MIN || (wm < W_NEAR && (v[0]._z > v[0]._w || v[1]._z > v[1]._w || v[2]._z > v[2]._w))) {
            gfx_clip_single_vert(out, &nout, va); clipped++;                 /* its trigger and the clipper */
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
            if (fabsf(x) > w - eps || fabsf(y) > w - eps) continue;
            if (fabsf(z - w) < eps || fabsf(z + w) < eps || fabsf(z + 0.97f * w) < eps || fabsf(w - wmin) < 1e-4f * wmin) continue;
            samples++;
            const bool nd = z >= -w && z <= w;                  /* the N64: F3DEX's near and far planes */
            n64 += nd;
            bool pd = false;
            const float nx = x / w, ny = y / w;
            for (size_t i = 0; i + 2 < nout && !pd; i += 3) {
                const float x0 = sx[i], y0 = sy[i], x1 = sx[i + 1], y1 = sy[i + 1], x2 = sx[i + 2], y2 = sy[i + 2];
                const float d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (d == 0.0f) continue;
                const float l1 = ((nx - x0) * (y2 - y0) - (x2 - x0) * (ny - y0)) / d;
                const float l2 = ((x1 - x0) * (ny - y0) - (nx - x0) * (y1 - y0)) / d;
                pd = l1 >= -1e-5f && l2 >= -1e-5f && l1 + l2 <= 1.0f + 1e-5f;
            }
            if (pd != nd) {
                if (w < wmin) { near_bad++; if (z >= -w && z < -0.97f * w) sliver++; }
                else far_bad++;
            }
        }
    }
    printf("clip: MK64_NEARW=%d, near %g: W_MIN %.6f; %ld triangles (%ld clipped), %ld points on the screen in front of "
           "the eye, the N64 draws %ld; the port differs at %ld nearer than W_MIN (%ld of them between the N64's near "
           "plane and the port's -0.97w), %ld beyond\n", gfx_nearw_on, near, wmin, tris, clipped, samples, n64, near_bad,
           sliver, far_bad);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "wmin";
    gfx_nearw_on = -1; gfx_wmin_update(); /* reads MK64_NEARW */
    if (!strcmp(mode, "wmin")) return wmin_mode();
    if (!strcmp(mode, "clip")) return clip_mode(argc > 2 ? (float)atof(argv[2]) : 1.0f, argc > 3 ? atoi(argv[3]) : 1, argc > 4 ? atoi(argv[4]) : 100000);
    fprintf(stderr, "usage: nearw_test wmin | clip <near> [seed] [n]\n");
    return 2;
}
