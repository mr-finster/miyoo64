/* gfx_soft_halfwater.inc.c -- half-resolution water (2026-10-02, a LOOK TRADE kept as a test switch, default OFF):
 *   OOT_HALFWATER=1 (debug.cfg halfwater=1)  the constant-alpha see-through surfaces drawn at half resolution and
 *                                            scaled up: the Deku Tree basement water (both layers) and other fogged
 *                                            surfaces of one alpha (the combiner's constant alpha, no texel alpha).
 * Off, nothing here runs (raster_tri tests one int per triangle) and every pixel is as before. (The same layer once
 * took the see-through effects too; the user found them too soft, and that part was removed on 2026-10-02.)
 *
 * Included by gfx_soft.c inside its NEON section (HH_CM_NEON: the device and the ARM harnesses; the desktop proxy's
 * scalar build has no layer), after the row functions and before raster_tri; it uses that file's statics and macros.
 *
 * How it works:
 * - Which draws: raster_tri's triangles that are fogged with a constant alpha and no texel alpha, blended (sUseAlpha),
 *   z-tested, write no z, are not decals (z bias), not texture-edge cut-outs, not the sky (between its tags or the
 *   half-res sky pass), not UI / HUD, in the 3D phase, textured (any combine but TEXEL0_ALPHA as colour), not
 *   fastwater's top layer, the detail tile only on a 32-bit repeating texture (the water's grain). Texrects / 2D never
 *   qualify.
 * - The layer: a quarter-size buffer (half the output's width and height) holding premultiplied colour (B G R) and coverage (A), 0 =
 *   nothing. A qualifying triangle is drawn into it by raster_tri itself, called again with its x, y halved and the
 *   layer as the target (as the half-res sky pass does): the same setup, the same fetches, combine and fog as the
 *   full-res rows (the lean rows' macros), the constant alpha, then the blend over the layer with the source's alpha
 *   lane forced to 255 -- so B G R accumulate C*a + P*(1-a) and A accumulates 1 - (1-a)(1-a').. -- the "over"
 *   operator, which is associative: both water layers compose into one P, cov. The blend-twice fold is two such blends.
 * - Depth: each half pixel tests against the FARTHEST of its 2x2 full-res depths (built per 16x16 tile when the water
 *   first touches it), and remembers the NEAREST water depth drawn there (ZE). The composite (bilinear 2x upscale,
 *   weights 9/3/3/1) then leaves a full-res pixel alone when the nearest water among its four samples lies behind it
 *   (ZE > z + HFX_ZTOL): a solid in front of the water keeps a sharp edge, and there is no gap on the far side of that
 *   edge (the farthest-depth test drew the water there). Testing the nearest sample, not each one, keeps the water
 *   lying along the floor from failing on the floor's own slope between the samples.
 * - Compositing: out = P + D * (255 - cov) / 255 at full resolution, alpha 255; for one flat blend that is the full-res
 *   blend within 1 level (floor of a sum vs sum of floors).
 * - Order: the layer is composited (hfx_flush) before anything that could see the pixels under it in the wrong order:
 *   a non-qualifying triangle whose box touches a drawn tile (anything else waits, it touches other pixels), and every
 *   colour read or target change the deferred sky already settles at (sky_settle: 2D draws, composites, copies, the
 *   pause capture, PICK, dumps, the frame's end, target switches, the sky pass). The deferred sky (OOT_SKYDEFER) is
 *   drawn first (the composite reads the colour under it); a triangle drawn into the layer reads nothing at full res,
 *   so the sky stays kept across it. The z buffer only changes through raster_tri (gfx_pc skips depth fills), so a
 *   drawn tile's half depth stays valid until the flush. */

static int raster_tri(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z);
/* out of line, so raster_tri and the rest of this file compile as they did before (the hook costs one test) */
#define HFX_COLD __attribute__((noinline, cold))

static int sHfxMode SAVESTATE_EXCLUDE = -1;     /* -1 unread, 0 off, 1 OOT_HALFWATER */
#define HFX_ZTOL 16                             /* the composite's depth tolerance (16-bit z) */
static bool sHfxCompPx SAVESTATE_EXCLUDE;       /* the composite pixel by pixel (set by halfwater_test: its reference) */
static bool sHfxIn SAVESTATE_EXCLUDE;           /* raster_tri is drawing into the layer */
static bool sHfxOpen SAVESTATE_EXCLUDE;         /* the layer holds draws not yet composited */
static uint32_t *sHfxCol SAVESTATE_EXCLUDE;     /* B G R premultiplied, A coverage; 0 = nothing */
static uint16_t *sHfxZ SAVESTATE_EXCLUDE;       /* the 2x2 block's farthest depth, per drawn tile */
static uint16_t *sHfxZE SAVESTATE_EXCLUDE;      /* the nearest water depth drawn at the half pixel; 0xFFFF none */
static uint8_t *sHfxTile SAVESTATE_EXCLUDE;     /* per 8x8 half-res tile (16x16 full res): 1 = drawn, depth built */
static int sHfxW SAVESTATE_EXCLUDE, sHfxH SAVESTATE_EXCLUDE, sHfxTW SAVESTATE_EXCLUDE, sHfxTH SAVESTATE_EXCLUDE;
static uint32_t *sHfxDst SAVESTATE_EXCLUDE;     /* the target the layer composites into */
static uint16_t *sHfxDstZ SAVESTATE_EXCLUDE;
static int sHfxDW SAVESTATE_EXCLUDE, sHfxDH SAVESTATE_EXCLUDE;
static uint8_t *sHfxCov SAVESTATE_EXCLUDE;      /* OOT_CRACKFILL's coverage for the triangle being drawn (full res), or NULL */
static int sHfxTx0 SAVESTATE_EXCLUDE, sHfxTy0 SAVESTATE_EXCLUDE, sHfxTx1 SAVESTATE_EXCLUDE, sHfxTy1 SAVESTATE_EXCLUDE;
uint32_t gfx_soft_hfx_flushes SAVESTATE_EXCLUDE;  /* composites done (stats) */
uint64_t gfx_soft_hfx_px SAVESTATE_EXCLUDE;       /* full-res pixels composited (stats) */
#define HFX_T 8 /* tile side in half-res pixels */

static HFX_COLD void hfx_init(void) {
    const char *e = getenv("OOT_HALFWATER");
    sHfxMode = !(e != NULL && e[0] == '0'); /* on by default since 2026-10-03 (the user: "turn it on for next test") */
    if (sHfxMode) port_log("gfx: half-resolution water (halfwater=0 turns it off)\n");
}

/* the layer for a W x H target: everything cleared (colour 0, ZE 0xFFFF, no tile drawn) */
static HFX_COLD bool hfx_alloc(int W, int H) {
    const int w = (W + 1) / 2, h = (H + 1) / 2;
    if (sHfxCol && w == sHfxW && h == sHfxH) return true;
    free(sHfxCol); free(sHfxZ); free(sHfxZE); free(sHfxTile);
    sHfxTW = (w + HFX_T - 1) / HFX_T; sHfxTH = (h + HFX_T - 1) / HFX_T;
    sHfxCol = (uint32_t *)calloc((size_t)w * h, 4);
    sHfxZ = (uint16_t *)malloc((size_t)w * h * 2);
    sHfxZE = (uint16_t *)malloc((size_t)w * h * 2);
    sHfxTile = (uint8_t *)calloc((size_t)sHfxTW * sHfxTH, 1);
    if (!sHfxCol || !sHfxZ || !sHfxZE || !sHfxTile) {
        free(sHfxCol); free(sHfxZ); free(sHfxZE); free(sHfxTile);
        sHfxCol = NULL; sHfxZ = sHfxZE = NULL; sHfxTile = NULL; sHfxW = sHfxH = 0;
        return false;
    }
    memset(sHfxZE, 0xFF, (size_t)w * h * 2);
    sHfxW = w; sHfxH = h;
    return true;
}

/* tile (tx, ty)'s half depth from the target's: the farthest of each 2x2 block, rows and columns past an
 * odd edge clamped */
static HFX_COLD void hfx_build_tile(int tx, int ty) {
    const int W = sHfxDW, H = sHfxDH, hw = sHfxW;
    const int hx0 = tx * HFX_T, hx1 = imin(hx0 + HFX_T, sHfxW), hy0 = ty * HFX_T, hy1 = imin(hy0 + HFX_T, sHfxH);
    for (int hy = hy0; hy < hy1; hy++) {
        const uint16_t *r0 = sHfxDstZ + (2 * hy) * W, *r1 = sHfxDstZ + imin(2 * hy + 1, H - 1) * W;
        uint16_t *d = sHfxZ + hy * hw;
        if (hx1 - hx0 == HFX_T && 2 * hx1 <= W) {
            const uint16x8_t a0 = vld1q_u16(r0 + 2 * hx0), a1 = vld1q_u16(r0 + 2 * hx0 + 8);
            const uint16x8_t b0 = vld1q_u16(r1 + 2 * hx0), b1 = vld1q_u16(r1 + 2 * hx0 + 8);
            const uint16x8_t m0 = vmaxq_u16(a0, b0), m1 = vmaxq_u16(a1, b1);
            const uint16x4_t p0 = vpmax_u16(vget_low_u16(m0), vget_high_u16(m0));
            const uint16x4_t p1 = vpmax_u16(vget_low_u16(m1), vget_high_u16(m1));
            vst1q_u16(d + hx0, vcombine_u16(p0, p1));
            continue;
        }
        for (int hx = hx0; hx < hx1; hx++) {
            const int c0 = 2 * hx, c1 = imin(2 * hx + 1, W - 1);
            uint16_t z = r0[c0];
            if (r0[c1] > z) z = r0[c1];
            if (r1[c0] > z) z = r1[c0];
            if (r1[c1] > z) z = r1[c1];
            d[hx] = z;
        }
    }
}

/* a qualifying triangle's full-res box [minx, maxx) x [miny, maxy): the tiles under it, one half pixel wider each way
 * (the upscale's reach), get their depth (once) and count as drawn */
static HFX_COLD void hfx_mark(int minx, int miny, int maxx, int maxy) {
    const int hx0 = imax((minx >> 1) - 1, 0), hx1 = imin(((maxx + 1) >> 1) + 1, sHfxW);
    const int hy0 = imax((miny >> 1) - 1, 0), hy1 = imin(((maxy + 1) >> 1) + 1, sHfxH);
    if (hx0 >= hx1 || hy0 >= hy1) return;
    const int tx0 = hx0 / HFX_T, tx1 = (hx1 - 1) / HFX_T + 1, ty0 = hy0 / HFX_T, ty1 = (hy1 - 1) / HFX_T + 1;
    for (int ty = ty0; ty < ty1; ty++)
        for (int tx = tx0; tx < tx1; tx++) {
            uint8_t *const t = &sHfxTile[ty * sHfxTW + tx];
            if (!*t) { hfx_build_tile(tx, ty); *t = 1; }
        }
    if (sHfxTx0 >= sHfxTx1) { sHfxTx0 = tx0; sHfxTx1 = tx1; sHfxTy0 = ty0; sHfxTy1 = ty1; }
    else { sHfxTx0 = imin(sHfxTx0, tx0); sHfxTx1 = imax(sHfxTx1, tx1); sHfxTy0 = imin(sHfxTy0, ty0); sHfxTy1 = imax(sHfxTy1, ty1); }
}

/* does a full-res box touch a drawn tile? */
static HFX_COLD bool hfx_hits(int minx, int miny, int maxx, int maxy) {
    const int tx0 = imax(minx >> 4, sHfxTx0), tx1 = imin(((maxx - 1) >> 4) + 1, sHfxTx1);
    const int ty0 = imax(miny >> 4, sHfxTy0), ty1 = imin(((maxy - 1) >> 4) + 1, sHfxTy1);
    for (int ty = ty0; ty < ty1; ty++)
        for (int tx = tx0; tx < tx1; tx++)
            if (sHfxTile[ty * sHfxTW + tx]) return true;
    return false;
}

/* The composite of the drawn tiles onto the target: per full-res pixel the bilinear 2x upscale of the layer (its own
 * half pixel 9/16, the horizontal and vertical neighbours on its side 3/16 each, the diagonal 1/16; edges clamped),
 * P + D (255 - cov) / 255 -- unless the nearest water among those four samples lies behind the pixel (ZE > z + tol):
 * a solid pixel in front of the water around it (a ledge rising through it) keeps its colour; so does a
 * pixel whose upscaled coverage rounds to 0 (alpha included: an internal target's undrawn pixels stay undrawn).
 * hfx_comp_px is one pixel; hfx_composite takes 8 pixels (4 half pixels) at a time in NEON lanes -- the same sums
 * (separably: 3 x the own row + the neighbour row, then 3 x the own column + the neighbour's), the same rounding, the
 * same test. Then the drawn tiles are cleared again (colour 0, ZE 0xFFFF). */
static HFX_COLD void hfx_comp_px(uint32_t *d, const uint16_t *dz, int x, const uint32_t *Lc, const uint32_t *Ln,
                        const uint16_t *Ec, const uint16_t *En, int hw, int tol) {
    const int i = x >> 1, in = (x & 1) ? imin(i + 1, hw - 1) : imax(i - 1, 0);
    const uint32_t a = Lc[i], b = Lc[in], c = Ln[i], e = Ln[in];
    if ((a | b | c | e) == 0) return;
    int zmin = Ec[i]; /* (0xFFFF: no water there) */
    if (Ec[in] < zmin) zmin = Ec[in];
    if (En[i] < zmin) zmin = En[i];
    if (En[in] < zmin) zmin = En[in];
    if (zmin > imin(dz[x] + tol, 0xFFFF)) return;
    const uint16x8_t ab = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)b << 32) | a)));
    const uint16x8_t ce = vmovl_u8(vreinterpret_u8_u32(vcreate_u32(((uint64_t)e << 32) | c)));
    uint16x4_t s = vmul_n_u16(vget_low_u16(ab), 9);
    s = vmla_n_u16(s, vget_high_u16(ab), 3);
    s = vmla_n_u16(s, vget_low_u16(ce), 3);
    s = vadd_u16(s, vget_high_u16(ce));
    const uint16x4_t P = vrshr_n_u16(s, 4), v255 = vdup_n_u16(255);
    if (vget_lane_u16(P, 3) == 0) return; /* no coverage left after the upscale: the pixel is not drawn */
    const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(d[x]))));
    const uint16x4_t inv = vsub_u16(v255, vdup_lane_u16(P, 3));
    uint16x4_t O = vmin_u16(vadd_u16(P, cm_div255_lean(vmul_u16(D, inv))), v255);
    O = vorr_u16(O, vcreate_u16(0x00FF000000000000ull));
    d[x] = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(O, O))), 0);
}
static HFX_COLD void hfx_composite(void) {
    const int W = sHfxDW, H = sHfxDH, hw = sHfxW, hh = sHfxH, tol = HFX_ZTOL;
    const uint16x8_t v255q = vdupq_n_u16(255), a255q = vreinterpretq_u16_u64(vdupq_n_u64(0x00FF000000000000ull));
    const uint16x8_t tolq = vdupq_n_u16((uint16_t)(tol < 0 ? 0 : tol > 65535 ? 65535 : tol));
    const uint8x8_t k3 = vdup_n_u8(3);
    uint64_t npx = 0;
    for (int ty = sHfxTy0; ty < sHfxTy1; ty++) {
        for (int tx = sHfxTx0; tx < sHfxTx1; tx++) {
            if (!sHfxTile[ty * sHfxTW + tx]) continue;
            const int x0 = tx * 2 * HFX_T, x1 = imin(x0 + 2 * HFX_T, W), y0 = ty * 2 * HFX_T, y1 = imin(y0 + 2 * HFX_T, H);
            for (int y = y0; y < y1; y++) {
                const int j = y >> 1, jn = (y & 1) ? imin(j + 1, hh - 1) : imax(j - 1, 0);
                const uint32_t *const Lc = sHfxCol + j * hw, *const Ln = sHfxCol + jn * hw;
                const uint16_t *const Ec = sHfxZE + j * hw, *const En = sHfxZE + jn * hw;
                uint32_t *const d = sHfxDst + y * W;
                const uint16_t *const dz = sHfxDstZ + y * W;
                int x = x0;
                for (; !sHfxCompPx && x + 8 <= x1 && (x >> 1) + 4 <= hw; x += 8) { /* x even: half pixels i .. i + 3 */
                    const int i = x >> 1, im = imax(i - 1, 0), ip = imin(i + 4, hw - 1);
                    const uint32x4_t C0 = vld1q_u32(Lc + i), N0 = vld1q_u32(Ln + i);
                    const uint32_t cm = Lc[im], cp = Lc[ip], nm = Ln[im], np = Ln[ip];
                    const uint64x2_t any = vreinterpretq_u64_u32(vorrq_u32(C0, N0));
                    if ((vgetq_lane_u64(any, 0) | vgetq_lane_u64(any, 1) | cm | cp | nm | np) == 0) continue;
                    npx += 8;
                    /* the depth test: the nearest water among each pixel's four samples in front of it (+ tol) */
                    const uint16x4_t zc = vld1_u16(Ec + i), zn = vld1_u16(En + i);
                    const uint16x4_t vc = vmin_u16(zc, zn);
                    const uint16x4_t vm = vmin_u16(vext_u16(vdup_n_u16(Ec[im]), zc, 3), vext_u16(vdup_n_u16(En[im]), zn, 3));
                    const uint16x4_t vp = vmin_u16(vext_u16(zc, vdup_n_u16(Ec[ip]), 1), vext_u16(zn, vdup_n_u16(En[ip]), 1));
                    const uint16x4x2_t zz = vzip_u16(vmin_u16(vc, vm), vmin_u16(vc, vp)); /* even, odd pixels in order */
                    const uint16x8_t bad = vcgtq_u16(vcombine_u16(zz.val[0], zz.val[1]), vqaddq_u16(vld1q_u16(dz + x), tolq));
                    const uint32x4_t Cm = vextq_u32(vdupq_n_u32(cm), C0, 3), Cp = vextq_u32(C0, vdupq_n_u32(cp), 1);
                    const uint32x4_t Nm = vextq_u32(vdupq_n_u32(nm), N0, 3), Np = vextq_u32(N0, vdupq_n_u32(np), 1);
#define HFX_V3(A, B, HALF) vaddw_u8(vmull_u8(vget_##HALF##_u8(vreinterpretq_u8_u32(A)), k3), vget_##HALF##_u8(vreinterpretq_u8_u32(B)))
                    const uint16x8_t vcl = HFX_V3(C0, N0, low), vch = HFX_V3(C0, N0, high);
                    const uint16x8_t El = vrshrq_n_u16(vmlaq_n_u16(HFX_V3(Cm, Nm, low), vcl, 3), 4);
                    const uint16x8_t Eh = vrshrq_n_u16(vmlaq_n_u16(HFX_V3(Cm, Nm, high), vch, 3), 4);
                    const uint16x8_t Ol = vrshrq_n_u16(vmlaq_n_u16(HFX_V3(Cp, Np, low), vcl, 3), 4);
                    const uint16x8_t Oh = vrshrq_n_u16(vmlaq_n_u16(HFX_V3(Cp, Np, high), vch, 3), 4);
#undef HFX_V3
                    const uint16x8_t P[4] = { vcombine_u16(vget_low_u16(El), vget_low_u16(Ol)), vcombine_u16(vget_high_u16(El), vget_high_u16(Ol)),
                                              vcombine_u16(vget_low_u16(Eh), vget_low_u16(Oh)), vcombine_u16(vget_high_u16(Eh), vget_high_u16(Oh)) };
                    const uint8x16_t D0 = vreinterpretq_u8_u32(vld1q_u32(d + x)), D1 = vreinterpretq_u8_u32(vld1q_u32(d + x + 4));
                    const uint16x8_t Dw[4] = { vmovl_u8(vget_low_u8(D0)), vmovl_u8(vget_high_u8(D0)), vmovl_u8(vget_low_u8(D1)), vmovl_u8(vget_high_u8(D1)) };
                    uint8x8_t ob[4], cb[4];
                    for (int k = 0; k < 4; k++) {
                        cb[k] = vmovn_u16(P[k]); /* the coverage bytes: a pixel left at 0 is not drawn */
                        const uint16x8_t inv = vsubq_u16(v255q, P[k]);
                        uint16x8_t m = vcombine_u16(vmul_lane_u16(vget_low_u16(Dw[k]), vget_low_u16(inv), 3),
                                                    vmul_lane_u16(vget_high_u16(Dw[k]), vget_high_u16(inv), 3));
                        m = vshrq_n_u16(vaddq_u16(vsraq_n_u16(m, m, 8), vdupq_n_u16(1)), 8);
                        ob[k] = vmovn_u16(vorrq_u16(vminq_u16(vaddq_u16(P[k], m), v255q), a255q));
                    }
                    /* a pixel kept as it was: the depth test failed, or no coverage after the upscale (its alpha too:
                     * an internal target's undrawn pixels stay undrawn) */
                    const int16x8_t badm = vreinterpretq_s16_u16(bad);
                    const uint32x4_t cvm = vdupq_n_u32(0xFF000000u);
                    const uint32x4_t keep0 = vorrq_u32(vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(badm))),
                                                       vceqq_u32(vandq_u32(vreinterpretq_u32_u8(vcombine_u8(cb[0], cb[1])), cvm), vdupq_n_u32(0)));
                    const uint32x4_t keep1 = vorrq_u32(vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(badm))),
                                                       vceqq_u32(vandq_u32(vreinterpretq_u32_u8(vcombine_u8(cb[2], cb[3])), cvm), vdupq_n_u32(0)));
                    vst1q_u32(d + x, vbslq_u32(keep0, vreinterpretq_u32_u8(D0), vreinterpretq_u32_u8(vcombine_u8(ob[0], ob[1]))));
                    vst1q_u32(d + x + 4, vbslq_u32(keep1, vreinterpretq_u32_u8(D1), vreinterpretq_u32_u8(vcombine_u8(ob[2], ob[3]))));
                }
                for (; x < x1; x++) hfx_comp_px(d, dz, x, Lc, Ln, Ec, En, hw, tol);
            }
        }
    }
    for (int ty = sHfxTy0; ty < sHfxTy1; ty++) {
        for (int tx = sHfxTx0; tx < sHfxTx1; tx++) {
            uint8_t *const t = &sHfxTile[ty * sHfxTW + tx];
            if (!*t) continue;
            *t = 0;
            const int hx0 = tx * HFX_T, n = imin(hx0 + HFX_T, hw) - hx0;
            for (int hy = ty * HFX_T; hy < imin(ty * HFX_T + HFX_T, hh); hy++) {
                memset(sHfxCol + hy * hw + hx0, 0, (size_t)n * 4);
                memset(sHfxZE + hy * hw + hx0, 0xFF, (size_t)n * 2);
            }
        }
    }
    sHfxTx0 = sHfxTx1 = sHfxTy0 = sHfxTy1 = 0;
    gfx_soft_hfx_px += npx;
}

static HFX_COLD void hfx_flush(void) {
    if (!sHfxOpen) return;
    sHfxOpen = false;
    if (sDsN) sky_flush(); /* the deferred sky first: the composite reads the colour under the layer */
    hfx_composite();
    gfx_soft_hfx_flushes++;
}
static inline void hfx_settle(void) { if (sHfxOpen) hfx_flush(); }

/* A qualifying triangle: into the layer (raster_tri again, x and y halved, the layer as the target). -2: no layer
 * memory, draw it at full resolution. */
static HFX_COLD int hfx_draw(const SoftVtx *v0, const SoftVtx *v1, const SoftVtx *v2, bool use_z, int minx, int miny, int maxx,
                    int maxy) {
    if (sHfxOpen && (sHfxDst != sFB || sHfxDW != sW || sHfxDH != sH)) hfx_flush();
    if (!sHfxOpen) {
        if (!hfx_alloc(sW, sH)) return -2;
        sHfxDst = sFB; sHfxDstZ = sZB; sHfxDW = sW; sHfxDH = sH;
        sHfxTx0 = sHfxTx1 = sHfxTy0 = sHfxTy1 = 0;
        sHfxOpen = true;
    }
    hfx_mark(minx, miny, maxx, maxy);
    /* OOT_CRACKFILL's coverage, as raster_tri marks it for a blend without a z write: each half-res span's 2x2 pixels */
    sHfxCov = (sCov && sFB == sCovTarget && sCovSz >= sW * sH && !sSkyTag && !sSkyPass) ? sCov : NULL;
    uint32_t *const sfb = sFB; uint16_t *const szb = sZB;
    const int sw = sW, sh = sH, vx = sVpX, vy = sVpY, vw = sVpW, vh = sVpH, cx0 = sScX0, cy0 = sScY0, cx1 = sScX1, cy1 = sScY1;
    sFB = sHfxCol; sZB = sHfxZ; sW = sHfxW; sH = sHfxH;
    if (vw > 0 && vh > 0) { /* the viewport and scissor on the half grid, rounded outward */
        const int hx0 = vx >> 1, hy0 = vy >> 1;
        sVpX = hx0; sVpY = hy0; sVpW = ((vx + vw + 1) >> 1) - hx0; sVpH = ((vy + vh + 1) >> 1) - hy0;
    }
    sScX0 = cx0 >> 1; sScY0 = cy0 >> 1; sScX1 = imin((cx1 + 1) >> 1, sHfxW); sScY1 = imin((cy1 + 1) >> 1, sHfxH);
    SoftVtx h[3] = { *v0, *v1, *v2 };
    for (int k = 0; k < 3; k++) { h[k].x *= 0.5f; h[k].y *= 0.5f; }
    sHfxIn = true;
    const int r = raster_tri(&h[0], &h[1], &h[2], use_z);
    sHfxIn = false;
    sFB = sfb; sZB = szb; sW = sw; sH = sh;
    sVpX = vx; sVpY = vy; sVpW = vw; sVpH = vh; sScX0 = cx0; sScY0 = cy0; sScX1 = cx1; sScY1 = cy1;
    return r;
}

/* wrapf and the 16-bit tap for the rows below, always inlined: calling the shared static inlines from here too made
 * GCC take them out of line for the whole file (the scalar span loops then called them per pixel) */
static inline __attribute__((always_inline)) int hfx_wrapf(int c, int size, int mode, int mask) {
    if (mode == WRAP_REPEAT) {
        if (mask >= 0) return c & mask;
        c %= size; if (c < 0) c += size; return c;
    }
    return c < 0 ? 0 : (c >= size ? size - 1 : c);
}
static inline __attribute__((always_inline)) uint32_t hfx_tap16(const void *px, int idx) {
    const uint32_t c = ((const uint16_t *)px)[idx];
    return sUnpackLut[c & 0xff] | sUnpackLut[256 + (c >> 8)];
}
#undef CM_TAP16
#define CM_TAP16(i) hfx_tap16(px, (i))
#undef CM_WRAP_GU
#define CM_WRAP_GU(x) hfx_wrapf((x), tw, ws, mu)
#undef CM_WRAP_GV
#define CM_WRAP_GV(x) hfx_wrapf((x), th, wt, mv)
/* The layer's general row: CM_ROW's span walk (the lean rows' PSHADE setup), the full-res rows' fetch, combine and fog,
 * the constant alpha fgac, then the blend over the layer with the source alpha lane 255 (coverage), no z write, ZE
 * kept. */
typedef struct {
    CmTri c;         /* the full-res rows' per-triangle values (raster_tri's names) */
    uint16x4_t lk;   /* the (A - K) * TEXEL0 + K tint's K, B G R */
    uint8_t comb;    /* 0 texture x shade, 1 the K tint, 2 the texel alone, 3 the shade alone, 4 texture + shade */
    uint8_t fetch;   /* bilinear: 0 32-bit repeating on powers of two, 1 the same 16-bit, 2 32-bit any wrap, 3 16-bit
                      * any wrap; nearest: 4 32-bit, 5 16-bit */
} HfxTri;
#define CM_PIXELS_HFX(MODE, ALPHA, DETAIL, FOG, ...)                                                     \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, pe++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz) continue;                                                                         \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_LDETAIL_##DETAIL                                                                             \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M;                                                                                   \
        switch (comb) {                                                                                 \
        case 0: M = cm_div255_lean(vmul_u16(Tx, S)); break;                                             \
        case 1: M = vsub_u16(vadd_u16(lk, cm_div255_lean(vmul_u16(vqsub_u16(S, lk), Tx))),               \
                             cm_div255_lean(vmul_u16(vqsub_u16(lk, S), Tx))); break; /* toward zero, as C's / */ \
        case 2: M = Tx; break;                                                                          \
        case 3: M = S; break;                                                                           \
        default: M = vmin_u16(vadd_u16(Tx, S), v255); break;                                            \
        }                                                                                               \
        const int fa = fgac;                                                                            \
        if (fa < amin) continue;                                                                        \
        if (FOG) {                                                                                      \
            const uint16x4_t fs = vdup_lane_u16(S, 3);                                                  \
            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs));                    \
        }                                                                                               \
        M = vorr_u16(M, a255);                                                                          \
        const uint16x4_t SF = vmul_u16(M, vdup_n_u16((uint16_t)fa)), IF = vdup_n_u16((uint16_t)(255 - fa)); \
        const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));             \
        M = cm_div255_lean(vmla_u16(SF, D, IF));                                                        \
        if (extra) M = cm_div255_lean(vmla_u16(SF, M, IF));                                             \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (uz < *pe) *pe = (uint16_t)uz;                                                               \
    }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_HFX
#undef CM_ROW_PSHADE
#define CM_ROW_PSHADE CM_PSHADE_VCVT
static void __attribute__((noinline))
hfx_row(const HfxTri *Hx, uint32_t *fb, uint16_t *zp, int n,
        float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const CmTri *const T = &Hx->c;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull), v255 = vdup_n_u16(255), lk = Hx->lk;
    const void *const px = T->px;
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const int tw = T->w, th = T->h, ws = T->ws, wt = T->wt, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp;
    const int amin = T->amin, fgac = T->fgac, extra = T->extra, comb = Hx->comb;
    const uint16x4_t fogv = T->fogv;
    uint16_t *pe = sHfxZE + (zp - sHfxZ);
    (void)px; (void)px32; (void)th; (void)ws; (void)wt; (void)mv;
#define HFX_ROWF(DETAIL, ...) if (T->fog) { CM_ROW(0, 1, DETAIL, 1, __VA_ARGS__) } else { CM_ROW(0, 1, DETAIL, 0, __VA_ARGS__) }
    if (T->detail) { /* tile 0 32-bit repeating on a power of two, tile 1 bilinear (the water's grain) */
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        HFX_ROWF(1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV))
        return;
    }
    switch (Hx->fetch) {
    case 0: HFX_ROWF(0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) break;
    case 1: HFX_ROWF(0, CM_LBILIN16) break;
    case 2: HFX_ROWF(0, CM_LBILING(CM_TAP32)) break;
    case 3: HFX_ROWF(0, CM_LBILING(CM_TAP16)) break;
    case 4: HFX_ROWF(0, CM_NEAR(CM_TAP32, CM_WRAP_GU, CM_WRAP_GV)) break;
    default: HFX_ROWF(0, CM_NEAR(CM_TAP16, CM_WRAP_GU, CM_WRAP_GV)) break;
    }
#undef HFX_ROWF
}
/* The water's rows (OOT_HALFWATER's surfaces as the basement has them: fogged, the constant alpha fgac, texture x shade,
 * 32-bit repeating bilinear, with or without the grain tile, LX = blended twice): CM_PIXELS_HFX with those decided per
 * triangle, as cm_row_lean's water rows are -- fa and its two factor vectors constant, the blend always */
#define CM_PIXELS_HFXW(MODE, ALPHA, DETAIL, FOG, ...)                                                    \
    for (uint32_t *pf = fb; pf < end; pf++, pz++, pe++, uf += duf, vf += dvf, zq += zdq, acc = vaddq_s32(acc, dacc)) { \
        const int uz = clamp16_sh8(zq);                                                                 \
        if (uz > *pz) continue;                                                                         \
        uint16x4_t Tx; __VA_ARGS__                                                                      \
        CM_LDETAIL_##DETAIL                                                                             \
        const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);                                    \
        uint16x4_t M = cm_div255_lean(vmul_u16(Tx, S));                                                 \
        const uint16x4_t fs = vdup_lane_u16(S, 3);                                                      \
        M = vorr_u16(cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs)), a255);       \
        const uint16x4_t SF = vmul_u16(M, vfa);                                                         \
        const uint16x4_t D = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pf))));             \
        M = cm_div255_lean(vmla_u16(SF, D, vifa));                                                      \
        if (LX) M = cm_div255_lean(vmla_u16(SF, M, vifa));                                              \
        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(M, M))), 0);                         \
        if (uz < *pe) *pe = (uint16_t)uz;                                                               \
    }
#undef CM_ROW_PIXELS
#define CM_ROW_PIXELS CM_PIXELS_HFXW
static void __attribute__((noinline))
hfx_row_w(const HfxTri *Hx, uint32_t *fb, uint16_t *zp, int n,
          float zf, float cr, float cg, float cb, float ca, float uw, float vw, float sw, float rw) {
    const CmTri *const T = &Hx->c;
    const uint16x4_t a255 = vcreate_u16(0x00FF000000000000ull), v255 = vdup_n_u16(255);
    const uint16x4_t vfa = vdup_n_u16((uint16_t)T->fgac), vifa = vdup_n_u16((uint16_t)(255 - T->fgac));
    const uint32_t *const px32 = (const uint32_t *)T->px;
    const int tw = T->w, mu = T->mu, mv = T->mv;
    const float swdx = T->swdx, uwdx = T->uwdx, vwdx = T->vwdx, zdx = T->zdx;
    const float crdx = T->crdx, cgdx = T->cgdx, cbdx = T->cbdx, cadx = T->cadx;
    const float32x4_t tsz = { T->ftw, T->ftw, T->fth, T->fth };
    const int32x4_t dacc0 = T->dacc;
    const bool pshade = T->pshade && T->persp;
    const int32_t zdq = T->zdq;
    const bool persp = T->persp;
    const uint16x4_t fogv = T->fogv;
    uint16_t *pe = sHfxZE + (zp - sHfxZ);
    (void)mv;
    if (T->detail) {
        const uint32_t *const dpx32 = (const uint32_t *)T->dpx;
        const int dw1 = T->dw, dmu = T->dmu, dmv = T->dmv, dsls = T->dsls, dsrs = T->dsrs, dslt = T->dslt, dsrt = T->dsrt;
        const uint32_t dpmu = T->dpmu, dpmv = T->dpmv, doffs = T->doffs, dofft = T->dofft;
        const uint16x4_t dwt = T->dwt, dwinv = T->dwinv;
        if (T->extra) { const int LX = 1; CM_ROW(0, 1, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        else          { const int LX = 0; CM_ROW(0, 1, 1, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
        return;
    }
    if (T->extra) { const int LX = 1; CM_ROW(0, 1, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
    else          { const int LX = 0; CM_ROW(0, 1, 0, 1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }
}
#undef CM_PIXELS_HFXW
#undef CM_PIXELS_HFX
/* a layer triangle's rows (raster_tri's row setup, as the deferred sky's flush runs it): the water's row, or the general
 * one */
static HFX_COLD int hfx_tri_rows(const HfxTri *hx, const DsRows *Rp, bool water) {
    const DsRows R = *Rp;
    const float e0c = R.e0c, e0x = R.e0x, e0y = R.e0y, e1c = R.e1c, e1x = R.e1x, e1y = R.e1y;
    const float e2c = R.e2c, e2x = R.e2x, e2y = R.e2y, inv_area = R.inv_area;
    const float w0dx = R.w0dx, w1dx = R.w1dx, w2dx = R.w2dx, z0 = R.z0, z1 = R.z1, z2 = R.z2;
    const float r0 = R.r0, r1 = R.r1, r2 = R.r2, g0 = R.g0, g1 = R.g1, g2 = R.g2;
    const float bl0 = R.bl0, bl1 = R.bl1, bl2 = R.bl2, a0 = R.a0, a1 = R.a1, a2 = R.a2;
    const float uw0 = R.uw0, uw1 = R.uw1, uw2 = R.uw2, vw0 = R.vw0, vw1 = R.vw1, vw2 = R.vw2;
    const float iw0 = R.iw0, iw1 = R.iw1, iw2 = R.iw2;
    const int minx = R.minx, maxx = R.maxx, sW = R.stride;
    const bool flat = R.flat, persp = R.persp;
    uint32_t *const fbuf = sHfxCol; uint16_t *const zb = R.zbuf;
    uint32_t tri_px = 0;
    const bool own = false; OwnTri oe; (void)oe; /* (OOT_FILLRULE: not in the half-resolution layer) */
    for (int y = R.miny; y < R.maxy; y++) {
        TRI_ROW_SETUP
        if (sHfxCov) {
            const int fx0 = 2 * xs, fx1 = imin(2 * xe, sHfxDW);
            for (int fy = 2 * y; fy < imin(2 * y + 2, sHfxDH); fy++) memset(sHfxCov + fy * sHfxDW + fx0, 1, (size_t)(fx1 - fx0));
        }
        if (water) hfx_row_w(hx, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
        else hfx_row(hx, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
    }
    return (int)tri_px;
}

/* raster_tri's hook (after its row edge bases; its locals by name, as TRI_ROW_SETUP). Inside the layer: the rows above
 * (every triangle there qualified: fogged, the constant alpha, no texel alpha). Outside: a qualifying triangle goes to
 * the layer; any other that touches a drawn tile composites the layer first. */
#define HFX_TRI_HOOK                                                                                    \
    if (__builtin_expect(sHfxMode != 0, 0)) {                                                           \
        if (sHfxIn) {                                                                                   \
            HfxTri hx;                                                                                  \
            hx.c.px = texpx; hx.c.w = tw; hx.c.h = th; hx.c.ws = ws; hx.c.wt = wt;                      \
            hx.c.mu = tmask_u; hx.c.mv = tmask_v; hx.c.t16 = t16;                                       \
            hx.c.swdx = swdx; hx.c.uwdx = uwdx; hx.c.vwdx = vwdx; hx.c.zdx = zdx;                       \
            hx.c.crdx = crdx; hx.c.cgdx = cgdx; hx.c.cbdx = cbdx; hx.c.cadx = cadx;                     \
            hx.c.ftw = (float)tw; hx.c.fth = (float)th;                                                 \
            { const int32_t dq[4] = { bdq_t, gdq_t, rdq_t, adq_t }; hx.c.dacc = vld1q_s32(dq); }         \
            hx.c.zdq = zdq; hx.c.pshade = pshade; hx.c.persp = persp; hx.c.amin = amin; hx.c.fgac = fga; \
            hx.c.fog = fogtri;                                                                          \
            { const uint16_t fv[4] = { (uint16_t)fgb, (uint16_t)fgg, (uint16_t)fgr, 0 }; hx.c.fogv = vld1_u16(fv); } \
            hx.c.extra = sTwiceReq ? 1 : 0;                                                             \
            if (sTwiceReq) sTwiceDone = 1;                                                              \
            const bool hx_rr = ws == WRAP_REPEAT && wt == WRAP_REPEAT && tmask_u >= 0 && tmask_v >= 0;  \
            hx.c.detail = dtex && dbilin && !t16 && !dtex->is16 && hx_rr;                               \
            if (hx.c.detail) {                                                                          \
                const uint16_t winv[4] = { (uint16_t)(256 - sDetail.w[0]), (uint16_t)(256 - sDetail.w[1]), \
                                           (uint16_t)(256 - sDetail.w[2]), (uint16_t)(256 - sDetail.w[3]) }; \
                hx.c.dpx = dtex->px; hx.c.dw = dtex->w; hx.c.dmu = dtex->w - 1; hx.c.dmv = dtex->h - 1;  \
                hx.c.dpmu = ((uint32_t)dtex->w << 16) - 1u; hx.c.dpmv = ((uint32_t)dtex->h << 16) - 1u; \
                hx.c.dsls = sDetail.sls; hx.c.dsrs = sDetail.srs; hx.c.dslt = sDetail.slt; hx.c.dsrt = sDetail.srt; \
                hx.c.doffs = sDetail.offs; hx.c.dofft = sDetail.offt;                                   \
                hx.c.dwt = vld1_u16(sDetail.w); hx.c.dwinv = vld1_u16(winv);                            \
            }                                                                                           \
            hx.comb = !rgb_tex ? 3 : !rgb_in ? 2 : texlerp ? 1 : texadd ? 4 : 0;                        \
            { const uint16_t kv[4] = { (uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0 }; hx.lk = vld1_u16(kv); } \
            hx.fetch = use_bilin ? (hx_rr ? (t16 ? 1 : 0) : (t16 ? 3 : 2)) : (t16 ? 5 : 4);             \
            const bool hx_w = hx.comb == 0 && hx.fetch == 0;                                            \
            if (hx_w && fga < amin) return 0; /* (every pixel below the alpha test) */                  \
            const DsRows hx_R = { e0c, e0x, e0y, e1c, e1x, e1y, e2c, e2x, e2y, inv_area, w0dx, w1dx, w2dx,   \
                                  z0, z1, z2, r0, r1, r2, g0, g1, g2, bl0, bl1, bl2, a0, a1, a2, uw0, uw1, uw2, \
                                  vw0, vw1, vw2, iw0, iw1, iw2, minx, maxx, miny, maxy, sW, flat, persp, false, zb }; \
            return hfx_tri_rows(&hx, &hx_R, hx_w);                                                      \
        }                                                                                               \
        if (sHfxMode < 0) hfx_init();                                                                   \
        if (sHfxMode > 0) {                                                                             \
            if (fogtri && fga >= 0 && !a_tex && sDsAllow && sDsP3D && !sSkyPass && !sDsTag && !sUiMode &&  \
                ztest && sUseAlpha && !dmask && zbias == 0 && !tex_edge && tex && !rgb_texa && !fastw && !texlerpv && \
                (!dtex || (dbilin && !t16 && !dtex->is16 && ws == WRAP_REPEAT && wt == WRAP_REPEAT &&    \
                           tmask_u >= 0 && tmask_v >= 0))) {                                            \
                const int hx_r = hfx_draw(v0, v1, v2, use_z, minx, miny, maxx, maxy);                   \
                if (hx_r != -2) return hx_r;                                                            \
            }                                                                                           \
            if (sHfxOpen && fbuf == sHfxDst && hfx_hits(minx, miny, maxx, maxy)) hfx_flush();          \
        }                                                                                               \
    }
