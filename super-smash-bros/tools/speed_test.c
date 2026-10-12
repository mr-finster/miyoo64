/* speed_test.c: the 2026-10-07 speed round's switches (gfx_soft.c SSB64_FASTROWS, SSB64_SPANVEC, SSB64_ZSKIP) through
 * the real renderer (gfx_soft.c, included whole through tools/lerprows_test.c's harness) -- no ROM.
 *
 *   speed_test rand [frames]    every frame twice over: lerprows_test's random frame (texel lerps, texture x shade, the
 *                               texel alone, untextured, fog, UI, nearest; z-tested or not, opaque / alpha-tested /
 *                               blended, flat and smooth, perspective and affine, slivers and huge off-screen
 *                               triangles, 32-bit and 5551, repeating and clamped), and an "occluded" frame (near
 *                               walls first, then batches at every depth behind, beside and in front of them, the
 *                               owned-edge rule (SSB64_FILLRULE) on for some, decals). Each is drawn from the same start
 *                               with the three switches off (the reference), all on, and each off in turn: every colour
 *                               and depth value must be equal. Prints an FNV hash of the all-on frames: ARM (NEON) must
 *                               equal x86 (scalar), and the build against an older gfx_soft.c (-DSPEED_OLDFILE) must
 *                               give the same hash.
 *   speed_test scene [frames]   lerprows_test's synthetic fight frame, the switches from the environment
 *                               (SSB64_FASTROWS / SSB64_SPANVEC / SSB64_ZSKIP): for qemu instruction counts. Its hash.
 */
#define main lerprows_main
#include "lerprows_test.c"
#undef main
#ifdef SPEED_OLDFILE
static int gfx_soft_fastrows, gfx_soft_spanvec, gfx_soft_zskip; /* (the old renderer has no switches) */
#endif

static void set_sw(int fr, int sv, int zs) { gfx_soft_fastrows = fr; gfx_soft_spanvec = sv; gfx_soft_zskip = zs; }

/* a fan of small triangles with corners on a quarter-pixel grid: edges through pixel centres, where the row solve's
 * bias and the snap decide */
static void grid_batch(void) {
    state_reset();
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = true; gSh.a_tex = chance(50); gSh.a_in = chance(50);
    if (chance(40)) { const uint8_t kr = (uint8_t)rnd(), kg = (uint8_t)rnd(), kb = (uint8_t)rnd(); gfx_scegu_set_tex_lerp(true, kr, kg, kb); }
    sUseAlpha = chance(40); sDepthTest = chance(85); sDepthMask = chance(70);
    const float cx = (float)(int)frnd(10, 230) + 0.5f, cy = (float)(int)frnd(10, 230) + 0.5f, z = frnd(0.1f, 0.9f);
    const int nf = 3 + (int)(rnd() % 8);
    float px = cx + (float)(int)frnd(-40, 40) * 0.25f, py = cy + (float)(int)frnd(-40, 40) * 0.25f;
    for (int i = 0; i < nf; i++) {
        const float nx = cx + (float)(int)frnd(-120, 120) * 0.25f, ny = cy + (float)(int)frnd(-120, 120) * 0.25f;
        const int r = (int)(rnd() & 255), g = (int)(rnd() & 255);
        /* (the random values in order: not as arguments, whose order the compilers pick differently) */
        const float z1 = z + frnd(-0.05f, 0.05f), u1 = frnd(0, 2), v1 = frnd(0, 2);
        const float z2 = z + frnd(-0.05f, 0.05f), u2 = frnd(0, 2), v2 = frnd(0, 2);
        gV[gNV++] = vx(cx, cy, z, 1.0f, 0.5f, 0.5f, r, g, 128, 255);
        gV[gNV++] = vx(px, py, z1, 0.9f, u1, v1, g, r, 64, 200);
        gV[gNV++] = vx(nx, ny, z2, 1.1f, u2, v2, 255, r, g, 255);
        px = nx; py = ny;
    }
    draw3d((int)(rnd() % 6));
}
/* near walls, then everything behind and around them: random batches (some with the owned-edge rule), grid fans, and
 * decals on the walls (a hair behind them: inside the decal window, outside the plain test) */
static void occl_frame(uint32_t seed) {
    rs = seed;
    gfx_soft_start_frame();
    state_reset();
    gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = true;
    const int nw = 1 + (int)(rnd() % 4);
    for (int i = 0; i < nw; i++) { /* walls: quads at a near depth, sloped a little; then a decal on each */
        const float x0 = frnd(-60, 200), y0 = frnd(-60, 200), w = frnd(30, 200), h = frnd(30, 200);
        const float z0 = frnd(0.05f, 0.5f), dz = chance(50) ? 0.0f : frnd(-0.1f, 0.1f), iw = frnd(0.2f, 1.5f);
        const int tex = (int)(rnd() % 6);
        for (int d = 0; d < 2; d++) {
            const float o = d ? (float)(1 + (int)(rnd() % 4)) / 65535.0f : 0.0f; /* 1..4 depth units behind */
            state_reset();
            gSh.cc.used_textures[0] = true; gSh.rgb_tex = gSh.rgb_in = true;
            sZDecal = d; sDepthMask = !d;
            if (d && chance(50)) gfx_scegu_set_tex_lerp(true, 200, 40, 90);
            quad(vx(x0, y0, z0 + o, iw, 0, 0, 200, 180, 160, 255), vx(x0 + w, y0, z0 + dz + o, iw, 2, 0, 120, 140, 160, 255),
                 vx(x0 + w, y0 + h, z0 + dz + o, iw, 2, 2, 90, 200, 60, 255), vx(x0, y0 + h, z0 + o, iw, 0, 2, 255, 255, 255, 255));
            draw3d(d ? (int)(rnd() % 6) : tex);
        }
    }
    const int nb = 20 + (int)(rnd() % 40);
    for (int i = 0; i < nb; i++) {
        sOwnTri = chance(30);
        if (chance(25)) grid_batch(); else rand_batch();
        sOwnTri = false;
    }
    gfx_soft_end_frame();
}

static uint32_t fnvh(uint32_t h) { h = fnv(h, sNatFB, (size_t)W * H * 4); return fnv(h, sNatZB, (size_t)W * H * 2); }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "rand";
    setup();
    if (!strcmp(mode, "rand")) {
        const int frames = argc > 2 ? atoi(argv[2]) : 200;
        static uint32_t fb0[W * H]; static uint16_t zb0[W * H];
        static const int combo[4][3] = { { 1, 1, 1 }, { 0, 1, 1 }, { 1, 0, 1 }, { 1, 1, 0 } };
        static const char *const name[4] = { "all on", "fastrows off", "spanvec off", "zskip off" };
        uint32_t h = 2166136261u; long bad[4] = { 0 }, badz[4] = { 0 };
        for (int fr = 0; fr < frames; fr++) {
            for (int kind = 0; kind < 2; kind++) {
                const uint32_t seed = 0x9E3779B9u * (uint32_t)(fr + 1) + (uint32_t)kind * 0x51ED27u;
                set_sw(0, 0, 0);
                if (kind) occl_frame(seed); else rand_frame(seed);
                memcpy(fb0, sNatFB, sizeof fb0); memcpy(zb0, sNatZB, sizeof zb0);
                for (int c = 0; c < 4; c++) {
                    set_sw(combo[c][0], combo[c][1], combo[c][2]);
                    if (kind) occl_frame(seed); else rand_frame(seed);
                    for (int i = 0; i < W * H; i++) { bad[c] += fb0[i] != sNatFB[i]; badz[c] += zb0[i] != sNatZB[i]; }
                    if (c == 0) h = fnvh(h);
                }
            }
        }
        long any = 0;
        printf("rand: %d frames x 2, hash %08x, against all off:", frames, (unsigned)h);
        for (int c = 0; c < 4; c++) { printf(" %s %ld/%ld;", name[c], bad[c], badz[c]); any += bad[c] + badz[c]; }
        printf(" (colour/depth values that differ)\n");
        return any != 0;
    }
#if defined(HH_CM_NEON)
    if (!strcmp(mode, "scal")) { /* diagnostics: the NEON rows against the scalar loops, per frame kind */
        const int frames = argc > 2 ? atoi(argv[2]) : 60;
        static uint32_t fb0[W * H]; static uint16_t zb0[W * H];
        for (int fr = 0; fr < frames; fr++) for (int kind = 0; kind < 2; kind++) {
            const uint32_t seed = 0x9E3779B9u * (uint32_t)(fr + 1) + (uint32_t)kind * 0x51ED27u;
            set_sw(1, 1, 1); sCmNeon = 0;
            if (kind) occl_frame(seed); else rand_frame(seed);
            memcpy(fb0, sNatFB, sizeof fb0); memcpy(zb0, sNatZB, sizeof zb0);
            sCmNeon = 1;
            if (kind) occl_frame(seed); else rand_frame(seed);
            long b = 0, bz = 0; int fx = -1, fy = -1;
            for (int i = 0; i < W * H; i++) { if (fb0[i] != sNatFB[i] || zb0[i] != sNatZB[i]) { if (fx < 0) { fx = i % W; fy = i / W; } } b += fb0[i] != sNatFB[i]; bz += zb0[i] != sNatZB[i]; }
            if (b || bz) printf("frame %d kind %d: %ld colour %ld depth differ, first at %d,%d (scalar %08x/%u neon %08x/%u)\n", fr, kind, b, bz, fx, fy,
                                (unsigned)fb0[fy * W + fx], zb0[fy * W + fx], (unsigned)sNatFB[fy * W + fx], sNatZB[fy * W + fx]);
        }
        return 0;
    }
#endif
    if (!strcmp(mode, "hashes")) { /* diagnostics: each frame's hash (all on), to compare two builds; "hashes N K B" dumps
                                       * frame N kind K's batch hashes after each batch from batch B */
        const int frames = argc > 2 ? atoi(argv[2]) : 60;
        for (int fr = 0; fr < frames; fr++) for (int kind = 0; kind < 2; kind++) {
            const uint32_t seed = 0x9E3779B9u * (uint32_t)(fr + 1) + (uint32_t)kind * 0x51ED27u;
            set_sw(1, 1, 1);
            if (kind) occl_frame(seed); else rand_frame(seed);
            printf("frame %d kind %d %08x\n", fr, kind, (unsigned)fnvh(2166136261u));
        }
        return 0;
    }
    if (!strcmp(mode, "scene")) {
        const int frames = argc > 2 ? atoi(argv[2]) : 4;
        const char *e;
        set_sw(!((e = getenv("SSB64_FASTROWS")) && e[0] == '0'), !((e = getenv("SSB64_SPANVEC")) && e[0] == '0'),
               !((e = getenv("SSB64_ZSKIP")) && e[0] == '0'));
        uint32_t h = 2166136261u;
        for (int fr = 0; fr < frames; fr++) { draw_scene(fr); h = fnv(h, sNatFB, (size_t)W * H * 4); }
        printf("scene: %d frames, switches %d%d%d, hash %08x\n", frames, gfx_soft_fastrows, gfx_soft_spanvec, gfx_soft_zskip, (unsigned)h);
        return 0;
    }
    fprintf(stderr, "modes: rand scene\n");
    return 2;
}
