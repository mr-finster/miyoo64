/* tjfill_test.c: GE_TJFILL (gfx_soft.c tj_fill / tj_mark_batch) through the real rasterizer (2026-10-04, the Dam tunnel
 * ceiling's hairline). Includes hh/gfx/gfx_soft.c. Each frame clears the target to a sky (no depth, 0xFFFF) and draws
 * a T-junction the way GoldenEye's meshes have them: a triangle T1 = (A, C, P) on one side of its long edge A-C, and on
 * the other side two triangles T2 = (A, T, Q), T3 = (T, C, Q) that meet at a vertex T which should sit on A-C but
 * misses it by d pixels (d < 0: a sliver gap between A-C and A-T-C, the crack; d > 0: an overlap). Half the meshes are
 * creased along A-C (T2 and T3 on a second plane through A-C, as the tunnel's portal band against its ceiling). Around
 * them: decoys -- separate opaque triangles at other depths, alpha cut-out triangles with 1-texel holes over the sky,
 * blended triangles -- and a watertight copy of the mesh (T on A-C exactly, T1 split at T) elsewhere in some frames.
 *
 * Checks:
 *   1. every sliver pixel is filled: after tj_fill no pixel with its centre inside the gap (A, T, C) keeps the sky
 *      (|d| <= 0.6 px, the gap away from the screen's edges and from the decoys);
 *   2. nothing else changes: every pixel tj_fill changed is a sliver pixel (a pixel no triangle wrote, inside the gap
 *      triangle grown by half a pixel); a frame with no gap (d >= 0 or the watertight mesh only) is untouched; pixels
 *      with depth, the cut-outs' holes, the blended triangles' pixels and the sky away from the mesh never change;
 *   3. GE_TJFILL=0 draws the same frame as before the change (no marks, no fill);
 *   4. the row loops agree: GE_LEANTRI on and off draw the same frame and the same marks (ARM: the lean triangle
 *      loop); the frame hash after the fill is the same on ARM (NEON rows) and x86 (scalar).
 * Build and run: port/hh/tools/tjfill_test.sh (x86 -m32 and ARM under qemu-arm). */
#include "gfx_soft.c"
#include "gfx_cc.c"

#include <stdarg.h>

int gfx_trace_frames;
void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
struct GfxDimensions gfx_current_dimensions = { 240, 240, 1.0f };
int gfx_pc_frame_w(void) { return 240; }
int bossGetStageNum(void) { return 33; }
void port_fb_tile_note(void *t, unsigned int b) { (void)t; (void)b; }
uint32_t gPortProfFrame[PROF_COUNT];

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frnd(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }

#define W 240
#define H 240
static uint32_t fbA[W * H], fbB[W * H], fbPre[W * H];
static uint16_t zbA[W * H], zbB[W * H], zbPre[W * H];
static uint8_t mkA[W * H];
static uint8_t holeMask[W * H]; /* pixels a cut-out or blended decoy marked (never filled, never changed) */

static struct ShaderProgram *sh_tex, *sh_shade, *sh_cut;
static uint32_t tid, cutid;

typedef struct { float a, b, c; } Plane; /* z = a x + b y + c (screen space) */
static float pz(const Plane *p, float x, float y) { return p->a * x + p->b * y + p->c; }
static SoftVtx vtx(float x, float y, const Plane *p) {
    SoftVtx v;
    memset(&v, 0, sizeof(v));
    v.x = x; v.y = y; v.z = pz(p, x, y); v.invw = 1.0f;
    v.u = x / 37.0f; v.v = y / 29.0f;
    v.r = (uint8_t)(90 + ((int)x * 3) % 120); v.g = (uint8_t)(70 + ((int)y * 5) % 150); v.b = 60; v.a = 255;
    return v;
}
/* the pixel centre (x + 0.5, y + 0.5) inside triangle (a, b, c), grown by g pixels (distance to each edge line) */
static bool inside_grown(const SoftVtx *a, const SoftVtx *b, const SoftVtx *c, int x, int y, float g) {
    const float px = x + 0.5f, py = y + 0.5f;
    const SoftVtx *v[3] = { a, b, c };
    const float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0.0f) return false;
    const float s = area > 0 ? 1.0f : -1.0f;
    for (int k = 0; k < 3; k++) {
        const SoftVtx *p = v[k], *q = v[(k + 1) % 3];
        const float ex = q->x - p->x, ey = q->y - p->y, len = sqrtf(ex * ex + ey * ey);
        if (len == 0.0f) return false;
        const float e = s * (ex * (py - p->y) - ey * (px - p->x)) / len; /* signed distance, inside > 0 */
        if (e < -g) return false;
    }
    return true;
}

static void clear_target(uint32_t *fb, uint16_t *zb, uint32_t sky) {
    for (int i = 0; i < W * H; i++) { fb[i] = sky; zb[i] = 0xFFFF; }
    sFB = fb; sZB = zb; sW = W; sH = H;
}

typedef struct {
    SoftVtx t[16][3]; /* triangles in draw order */
    int kind[16];     /* 0 opaque textured, 1 opaque shade, 2 cut-out (texture alpha, z test, writes depth), 3 blended (no z write) */
    int n;
} Scene;

static void draw_scene(const Scene *s) {
    for (int i = 0; i < s->n; i++) {
        const int k = s->kind[i];
        sDepthTest = true; sDepthMask = k != 3;
        sUseAlpha = k >= 2;
        sCurShader = k == 0 ? sh_tex : k == 1 ? sh_shade : sh_cut;
        sBoundTex[0] = k == 2 ? cutid : tid;
        tj_mark_batch(s->t[i], 1); /* (as gfx_soft_draw_triangles does for each batch) */
        raster_tri(&s->t[i][0], &s->t[i][1], &s->t[i][2], true);
    }
}

int main(int argc, char **argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 2000;
    rs = argc > 2 ? (uint32_t)atoi(argv[2]) : 1;
    gfx_soft_init();
    sScX0 = 0; sScY0 = 0; sScX1 = W; sScY1 = H; sVpX = 0; sVpY = 0; sVpW = W; sVpH = H;
    static uint8_t texbuf[32 * 32 * 4], cutbuf[16 * 16 * 4];
    tid = gfx_soft_new_texture(); cutid = gfx_soft_new_texture();
    for (int i = 0; i < 32 * 32 * 4; i++) texbuf[i] = (uint8_t)(rnd() | 0x20);
    for (int i = 0; i < 32 * 32; i++) texbuf[i * 4 + 3] = 255;
    for (int i = 0; i < 16 * 16; i++) { /* a grate: every other texel row and column a hole */
        cutbuf[i * 4] = 200; cutbuf[i * 4 + 1] = 180; cutbuf[i * 4 + 2] = 40;
        cutbuf[i * 4 + 3] = ((i % 16) % 2 && (i / 16) % 3) ? 255 : 0;
    }
    gfx_soft_select_texture(0, tid); gfx_soft_upload_texture(texbuf, 32, 32, GU_PSM_8888);
    gfx_soft_select_texture(0, cutid); gfx_soft_upload_texture(cutbuf, 16, 16, GU_PSM_8888);
    const uint32_t rgb_t = SHADER_TEXEL0 | (SHADER_0 << 3) | (SHADER_INPUT_1 << 6) | (SHADER_0 << 9);
    sh_tex = gfx_soft_create_and_load_new_shader(rgb_t | ((SHADER_INPUT_1 << 9) << 12));
    sh_shade = gfx_soft_create_and_load_new_shader((SHADER_INPUT_1 << 9) | ((SHADER_INPUT_1 << 9) << 12));
    sh_cut = gfx_soft_create_and_load_new_shader(rgb_t | (rgb_t << 12) | SHADER_OPT_ALPHA | SHADER_OPT_TEXTURE_EDGE);
    sPhase3D = true; sUnified = true;
    long gaps = 0, gap_px = 0, left = 0, wrong = 0, untouched_bad = 0, off_bad = 0, lean_bad = 0, filled = 0, tight = 0, thin = 0;
    uint32_t hash = 2166136261u, hashPre = 2166136261u;
    for (int f = 0; f < frames; f++) {
        Scene s; s.n = 0;
        const uint32_t sky = 0xFF000000u | (0x40 + rnd() % 0x40) << 16 | (0x60 + rnd() % 0x40) << 8 | (0xA0 + rnd() % 0x50);
        /* the mesh's plane: depth 0.05..0.95 over the screen, with a slope */
        Plane p1 = { frnd(-0.0008f, 0.0008f), frnd(-0.0008f, 0.0008f), 0.0f }; /* (every vertex within 0..1) */
        p1.c = frnd(0.35f, 0.65f) - pz(&p1, 120, 120);
        /* the long edge A-C, P on one side, Q on the other */
        float dx = frnd(-1.0f, 1.0f), dy = frnd(-1.0f, 1.0f); /* (no libm trig: musl and glibc may round it apart) */
        if (fabsf(dx) + fabsf(dy) < 0.1f) dx = 1.0f;
        { const float l = sqrtf(dx * dx + dy * dy); dx /= l; dy /= l; }
        const float ang = dy, len = frnd(30.0f, 200.0f), mx = frnd(40, 200), my = frnd(40, 200);
        const float nx = -dy, ny = dx;
        const float ax = mx - dx * len * 0.5f, ay = my - dy * len * 0.5f, cx = mx + dx * len * 0.5f, cy = my + dy * len * 0.5f;
        const float hp = frnd(8.0f, 60.0f), hq = frnd(8.0f, 60.0f), tt = frnd(0.2f, 0.8f);
        const int gapcase = rnd() % 4;          /* 0, 1: a gap; 2: an overlap; 3: watertight */
        const float d = gapcase < 2 ? -frnd(0.02f, 0.6f) : gapcase == 2 ? frnd(0.02f, 0.6f) : 0.0f;
        const bool crease = rnd() & 1;
        Plane p2 = p1; /* the Q side: through A-C, its own slope across */
        if (crease) {
            const float k = frnd(-0.0008f, 0.0008f); /* depth per pixel of distance from A-C, toward Q (up to ~50 a pixel in 16-bit depth) */
            p2.a += k * -nx; p2.b += k * -ny; p2.c -= k * -nx * ax + k * -ny * ay;
        }
        const float px_ = mx + nx * hp, py_ = my + ny * hp;              /* P: the +n side */
        const float qx = mx - nx * hq + dx * frnd(-10, 10), qy = my - ny * hq + dy * frnd(-10, 10); /* Q: the -n side */
        const float Tx = ax + (cx - ax) * tt + nx * d, Ty = ay + (cy - ay) * tt + ny * d; /* d < 0: toward Q (-n), a gap */
        SoftVtx A1 = vtx(ax, ay, &p1), C1 = vtx(cx, cy, &p1), P = vtx(px_, py_, &p1);
        SoftVtx A2 = vtx(ax, ay, &p2), C2 = vtx(cx, cy, &p2), Q = vtx(qx, qy, &p2), T = vtx(Tx, Ty, &p2);
        const int mk0 = rnd() & 1 ? 0 : 1, mk1 = rnd() & 1 ? 0 : 1;
        if (gapcase == 3) { /* watertight: T on A-C, T1 split there */
            SoftVtx Tm1 = vtx(ax + (cx - ax) * tt, ay + (cy - ay) * tt, &p1), Tm2 = vtx(Tm1.x, Tm1.y, &p2);
            s.t[s.n][0] = A1; s.t[s.n][1] = Tm1; s.t[s.n][2] = P; s.kind[s.n++] = mk0;
            s.t[s.n][0] = Tm1; s.t[s.n][1] = C1; s.t[s.n][2] = P; s.kind[s.n++] = mk0;
            s.t[s.n][0] = A2; s.t[s.n][1] = Tm2; s.t[s.n][2] = Q; s.kind[s.n++] = mk1;
            s.t[s.n][0] = Tm2; s.t[s.n][1] = C2; s.t[s.n][2] = Q; s.kind[s.n++] = mk1;
        } else {
            s.t[s.n][0] = A1; s.t[s.n][1] = C1; s.t[s.n][2] = P; s.kind[s.n++] = mk0;
            s.t[s.n][0] = A2; s.t[s.n][1] = T; s.t[s.n][2] = Q; s.kind[s.n++] = mk1;
            s.t[s.n][0] = T; s.t[s.n][1] = C2; s.t[s.n][2] = Q; s.kind[s.n++] = mk1;
        }
        const float mbx0 = fminf(fminf(ax, cx), fminf(px_, qx)) - 32, mbx1 = fmaxf(fmaxf(ax, cx), fmaxf(px_, qx)) + 32;
        const float mby0 = fminf(fminf(ay, cy), fminf(py_, qy)) - 32, mby1 = fmaxf(fmaxf(ay, cy), fmaxf(py_, qy)) + 32; /* (and the caps) */
        /* the mesh goes on past A and C (A and C are inner vertices, as in a level: the crack is closed): two caps,
         * each a pair of triangles on the edges A-P, A-Q (C-P, C-Q) with the far point E beyond A (F beyond C) */
        {
            const float ex = ax - dx * frnd(6.0f, 30.0f), ey = ay - dy * frnd(6.0f, 30.0f);
            const float fx2 = cx + dx * frnd(6.0f, 30.0f), fy2 = cy + dy * frnd(6.0f, 30.0f);
            SoftVtx E1 = vtx(ex, ey, &p1), E2 = vtx(ex, ey, &p2), F1 = vtx(fx2, fy2, &p1), F2 = vtx(fx2, fy2, &p2);
            if (!crease) { E2 = E1; F2 = F1; }
            else { /* E and F on A-C's line: on both planes alike */
                E2.z = E1.z = pz(&p1, ex, ey); F2.z = F1.z = pz(&p1, fx2, fy2);
            }
            s.t[s.n][0] = A1; s.t[s.n][1] = P; s.t[s.n][2] = E1; s.kind[s.n++] = mk0;
            s.t[s.n][0] = A2; s.t[s.n][1] = E2; s.t[s.n][2] = Q; s.kind[s.n++] = mk1;
            s.t[s.n][0] = C1; s.t[s.n][1] = F1; s.t[s.n][2] = P; s.kind[s.n++] = mk0;
            s.t[s.n][0] = C2; s.t[s.n][1] = Q; s.t[s.n][2] = F2; s.kind[s.n++] = mk1;
        }
        /* decoys: separate triangles elsewhere at other depths, cut-outs and blended ones over the sky */
        const int nd = rnd() % 6;
        for (int k = 0; k < nd && s.n < 16; k++) {
            const int kind = rnd() % 4;
            Plane pd = { 0.0f, 0.0f, kind == 3 ? frnd(0.01f, 0.2f) : frnd(0.02f, 0.98f) };
            const float sz = frnd(3, 40);
            float ox, oy; /* outside the mesh's box by its size (the decoys are scenery: they never touch the mesh) */
            do { ox = frnd(-20, 260); oy = frnd(-20, 260); } while (ox + sz > mbx0 - 2 && ox - sz < mbx1 + 2 && oy + sz > mby0 - 2 && oy - sz < mby1 + 2);
            for (int j = 0; j < 3; j++) {
                s.t[s.n][j] = vtx(ox + frnd(-sz, sz), oy + frnd(-sz, sz), &pd);
                if (kind >= 2) { s.t[s.n][j].u = frnd(-2, 2); s.t[s.n][j].v = frnd(-2, 2); s.t[s.n][j].a = kind == 3 ? 128 : 255; }
            }
            s.kind[s.n++] = kind;
        }
        /* shuffle the draw order */
        for (int i = s.n - 1; i > 0; i--) {
            const int j = rnd() % (i + 1);
            SoftVtx tv[3]; memcpy(tv, s.t[i], sizeof(tv)); memcpy(s.t[i], s.t[j], sizeof(tv)); memcpy(s.t[j], tv, sizeof(tv));
            const int tk = s.kind[i]; s.kind[i] = s.kind[j]; s.kind[j] = tk;
        }
        const bool bil = rnd() & 1;
        gfx_soft_bilinear = bil;

        /* GE_TJFILL=0: nothing marked, nothing filled -- the frame as drawn */
        sTjFill = 0; tj_prepare(); free(sTjMask); free(sTjRow); sTjMask = sTjRow = NULL; sTjMaskW = sTjMaskH = 0;
        clear_target(fbB, zbB, sky); gfx_soft_depth_written = true; draw_scene(&s);
        memcpy(fbPre, fbB, sizeof(fbB)); memcpy(zbPre, zbB, sizeof(zbB));
        tj_fill();
        if (memcmp(fbB, fbPre, sizeof(fbB)) || memcmp(zbB, zbPre, sizeof(zbB))) off_bad++;

        /* GE_TJFILL=1, both triangle loops */
        sTjFill = 1;
        for (int lean = 0; lean < 2; lean++) {
#if defined(HH_CM_NEON)
            sLeanTri = lean; /* GE_LEANTRI (ARM): the triangle's rows in hot_tri_lean */
#endif
            clear_target(lean ? fbB : fbA, lean ? zbB : zbA, sky);
            tj_prepare();
            gfx_soft_depth_written = true;
            draw_scene(&s);
            if (!lean) {
                memcpy(mkA, sTjMask, sizeof(mkA));
                if (memcmp(fbA, fbPre, sizeof(fbA)) || memcmp(zbA, zbPre, sizeof(zbA))) off_bad++; /* the marks draw nothing */
            } else if (memcmp(mkA, sTjMask, sizeof(mkA)) || memcmp(fbB, fbPre, sizeof(fbB)) || memcmp(zbB, zbPre, sizeof(zbB))) {
                lean_bad++; /* the lean loop: the same pixels and the same marks */
            }
            memcpy(holeMask, sTjMask, sizeof(holeMask));
            tj_fill();
        }
#if defined(HH_CM_NEON)
        sLeanTri = 0;
#endif
        if (memcmp(fbA, fbB, sizeof(fbA)) || memcmp(zbA, zbB, sizeof(zbA))) lean_bad++;
        /* what changed */
        const SoftVtx *ga = &A1, *gc = &C1; /* the gap triangle (A, T, C) */
        bool near_decoy = false;
        for (int i = 0; i < W * H; i++) {
            const bool changed = fbA[i] != fbPre[i] || zbA[i] != zbPre[i];
            const int x = i % W, y = i / W;
            const bool in_gap = d < 0.0f && gapcase < 3 && inside_grown(ga, &T, gc, x, y, 0.5f);
            if (changed) {
                filled++;
                if (zbPre[i] != 0xFFFF || holeMask[i] || !in_gap) {
                    if (wrong < 8 && getenv("TJ_V")) {
                        printf("  f%d changed %d,%d (z %04x mark %d in_gap %d d %.2f case %d)\n", f, x, y, zbPre[i], holeMask[i], in_gap, d, gapcase);
                        if (getenv("TJ_VV") && x >= 3 && y >= 3 && x < W - 3 && y < H - 3) for (int yy = y - 3; yy <= y + 3; yy++) { printf("     ");
                            for (int xx = x - 3; xx <= x + 3; xx++) printf(" %04x%s", zbPre[yy * W + xx], xx == x && yy == y ? "*" : " "); printf("\n"); }
                    }
                    wrong++;
                }
            }
            if (in_gap && inside_grown(ga, &T, gc, x, y, 0.0f) && zbPre[i] == 0xFFFF && !holeMask[i]) gap_px++;
        }
        /* every sliver pixel filled: the gap's own pixels (centre strictly inside), away from the screen edges and with
         * nothing but the mesh around (the decoys can sit on either side: the gap then has a different neighbour) */
        if (d < 0.0f && gapcase < 3) {
            gaps++;
            for (int i = 0; i < W * H; i++) {
                const int x = i % W, y = i / W;
                if (x < 3 || y < 3 || x >= W - 3 || y >= H - 3) continue;
                if (!inside_grown(ga, &T, gc, x, y, 0.0f) || zbA[i] != 0xFFFF) continue;
                /* the 5x5 around it: only the mesh's own pixels or the gap (no decoy pixel, no cut-out mark) */
                bool clean = true;
                for (int yy = y - 2; yy <= y + 2 && clean; yy++)
                    for (int xx = x - 2; xx <= x + 2; xx++) {
                        const int j = yy * W + xx;
                        if (holeMask[j]) { clean = false; break; }
                        const bool mesh = inside_grown(&A1, &C1, &P, xx, yy, 0.01f) || inside_grown(&A2, &T, &Q, xx, yy, 0.01f) ||
                                          inside_grown(&T, &C2, &Q, xx, yy, 0.01f) || inside_grown(ga, &T, gc, xx, yy, 0.01f);
                        if (!mesh && zbPre[j] != 0xFFFF) { clean = false; break; } /* a decoy's pixel */
                    }
                if (!clean) continue;
                /* both faces there: the mesh's depth 3 px out on either side of A-C (a face thinner than that, or its
                 * outline, leaves the gap open to the sky on one side: not a crack between two faces) */
                {
                    const int x1 = (int)floorf(x + 0.5f + nx * 3.0f), y1 = (int)floorf(y + 0.5f + ny * 3.0f);
                    const int x2 = (int)floorf(x + 0.5f - nx * 3.0f), y2 = (int)floorf(y + 0.5f - ny * 3.0f);
                    if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 || x1 >= W || y1 >= H || x2 >= W || y2 >= H ||
                        zbPre[y1 * W + x1] == 0xFFFF || zbPre[y2 * W + x2] == 0xFFFF) { thin++; continue; }
                }
                /* within 2 px of T, A or C: the gap is a point there, and at A and C it meets the mesh's own outline */
                if ((fabsf(x + 0.5f - T.x) < 2.5f && fabsf(y + 0.5f - T.y) < 2.5f) ||
                    (fabsf(x + 0.5f - ax) < 2.5f && fabsf(y + 0.5f - ay) < 2.5f) ||
                    (fabsf(x + 0.5f - cx) < 2.5f && fabsf(y + 0.5f - cy) < 2.5f)) { tight++; continue; }
                if (left < 8 && getenv("TJ_V")) {
                    printf("  f%d left %d,%d d %.2f crease %d len %.0f ang %.2f\n", f, x, y, d, crease, len, ang);
                    if (getenv("TJ_VV")) for (int yy = y - 3; yy <= y + 3; yy++) { printf("     ");
                        for (int xx = x - 3; xx <= x + 3; xx++) printf(" %04x%s", zbPre[yy * W + xx], xx == x && yy == y ? "*" : " "); printf("\n"); }
                }
                left++;
            }
        }
        if (gapcase >= 2 && nd == 0)
            for (int i = 0; i < W * H; i++) if (fbA[i] != fbPre[i] || zbA[i] != zbPre[i]) { untouched_bad++; break; }
        for (int i = 0; i < W * H; i++) hash = (hash ^ fbA[i] ^ ((uint32_t)zbA[i] << 7)) * 16777619u;
        for (int i = 0; i < W * H; i++) hashPre = (hashPre ^ fbPre[i] ^ ((uint32_t)zbPre[i] << 7)) * 16777619u;
    }
    /* tj_fill alone on synthetic frames (no rasterizer: the ARM and x86 rows may round apart): planes of depth with
     * creases, holes of 1-3 pixels, runs of sky, random marks -- its result must hash the same on ARM and x86 */
    uint32_t fhash = 2166136261u;
    sTjFill = 1;
    for (int f = 0; f < frames / 4; f++) {
        clear_target(fbA, zbA, 0xFF4060C0u); tj_prepare(); gfx_soft_depth_written = true;
        const int a = (int)(rnd() % 600) - 300, b = (int)(rnd() % 600) - 300, c = 0x4000 + (int)(rnd() % 0x8000);
        const int k = (int)(rnd() % 400) - 200, cx0 = (int)(rnd() % W);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int z = c + (a * x + b * y) / 8 + (x > cx0 ? k * (x - cx0) / 4 : 0);
                if (z < 0) z = 0; if (z > 0xFFFE) z = 0xFFFE;
                zbA[y * W + x] = (uint16_t)z; fbA[y * W + x] = 0xFF000000u | (rnd() & 0xFFFFFF);
            }
        const int nh = (int)(rnd() % 400);
        for (int h = 0; h < nh; h++) { /* holes: a dot, a run across or down of 1-4, or a block */
            const int x = (int)(rnd() % W), y = (int)(rnd() % H), n = 1 + (int)(rnd() % 4), dir = (int)(rnd() % 3);
            for (int j = 0; j < n; j++) {
                const int xx = dir == 0 ? x + j : x, yy = dir == 1 ? y + j : y;
                if (xx < W && yy < H) { zbA[yy * W + xx] = 0xFFFF; if (dir == 2) for (int q = 0; q < n && xx + q < W; q++) zbA[yy * W + xx + q] = 0xFFFF; }
            }
            if (rnd() % 5 == 0 && x + 4 < W) { memset(sTjMask + y * W + x, 1, 4); sTjRow[y] = 1; }
        }
        tj_fill();
        for (int i = 0; i < W * H; i++) fhash = (fhash ^ fbA[i] ^ ((uint32_t)zbA[i] << 7)) * 16777619u;
    }
    printf("tjfill: %d frames, %ld with a gap (%ld gap px), %ld px filled; left in a gap %ld (skipped: %ld beside T, A, C, %ld with a face under 3 px); "
           "changed outside a gap %ld; no-gap frames touched %ld; GE_TJFILL=0 or marks changed a frame %ld; loops differ %ld; "
           "drawn hash %08x, filled %08x; fill-only hash %08x\n", frames, gaps, gap_px, filled, left, tight, thin, wrong, untouched_bad, off_bad, lean_bad, (unsigned)hashPre, (unsigned)hash, (unsigned)fhash);
    const bool fail = left || wrong || untouched_bad || off_bad || lean_bad;
    printf("%s\n", fail ? "tjfill_test: FAIL" : "tjfill_test: PASS");
    return fail;
}
