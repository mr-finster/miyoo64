/* raster_tri's row loop (gfx_soft.c), rows miny..maxy-1 of one triangle, over raster_tri's locals by name.
 * Included in raster_tri itself, and on the Miyoo Mini (MIYOO) in raster_rows, which draws one share of a
 * triangle's rows on either core (miyoo_worker): every row's edge bases and interpolants come from y alone
 * (TRI_ROW_SETUP: no row-to-row accumulation), so a share that starts mid-triangle draws exactly the pixels
 * the whole loop would. */
    for (int y = miny; y < maxy; y++) {
        TRI_ROW_SETUP
        if (sPld && y + 1 < maxy) { /* OOT_PLD: the next row's lines (z: 32 px a line, colour: 16) */
            const int nx = idx + sW, ne = nx + (xe - xs);
            for (int k = nx; k < ne; k += 32) __builtin_prefetch(zb + k);
            for (int k = nx; k < ne; k += 16) __builtin_prefetch(fbuf + k);
            __builtin_prefetch(zb + ne - 1); __builtin_prefetch(fbuf + ne - 1); /* the last line, when unaligned */
        }
        if (cov) memset(cov + idx, 1, (size_t)(xe - xs));
#if defined(HH_CM_NEON)
        if (cm_rows) { /* the whole row; flat triangles: zero colour steps, so rq.. clamp to the fs* */
            if (cmt.lean) cm_row_lean(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else if (cmt.lean2 == 1) cm_row_leana(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else if (cmt.lean2 == 2) cm_row_leans(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else if (cmt.lean2 == 3) cm_row_leanl(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else if (cmt.lean2 == 4) cm_row_leanz(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            else cm_row_neon(&cmt, fbuf + idx, zb + idx, xe - xs, zf, cr, cg, cb, ca, uw, vw, sw, rw);
            continue;
        }
#endif
        int x = xs;
        while (x < xe) {
            int span = xe - x;
            if (span > PERSP_SPAN) span = PERSP_SPAN;
            const float fspan = (float)span;
            const float sw_b = sw + swdx * fspan;           /* == the old end-of-span  sw += swdx*span */
            float rw_b = 0.0f;
            /* Perspective: exact u,v at span start and end, lerp between. */
            float u_a, v_a, du, dv;
            if (tex) {
                if (persp) {
                    rw_b = (sw_b > 1e-12f) ? 1.0f / sw_b : 0.0f;
                    u_a = uw * rw; v_a = vw * rw;
                    const float nu = (uw + uwdx * fspan) * rw_b - u_a;
                    const float nv = (vw + vwdx * fspan) * rw_b - v_a;
                    /* /16 is an exact power of two: multiplying by 0.0625f is
                     * bit-identical and skips two divides on every full span.
                     * Only the last, partial span still divides. */
                    if (span == PERSP_SPAN) { du = nu * (1.0f / PERSP_SPAN); dv = nv * (1.0f / PERSP_SPAN); }
                    else                    { du = nu / fspan;               dv = nv / fspan; }
                } else {
                    u_a = uw; v_a = vw; du = uwdx; dv = vwdx;
                }
            } else { u_a = v_a = du = dv = 0.0f; }

            /* The pixel loop is integer-only (device round 6): every per-pixel
             * interpolant -- texel u/v, z, r/g/b/a -- steps in fixed point,
             * converted once per 16-pixel span. The float->int conversions
             * and compares that were in the loop stall the Cortex-A7's VFP;
             * z is 24.8 of the 16-bit depth, colours 16.16. */
            int32_t uf = 0, vf = 0, duf = 0, dvf = 0;
            if (tex) {
                uf = to_fix16(u_a * (float)tw); duf = to_fix16(du * (float)tw);
                vf = to_fix16(v_a * (float)th); dvf = to_fix16(dv * (float)th);
            }
            int32_t zq = span_z_out(zf) ? span_zq_far(zf, zdx, span, zdq) /* OOT_NEARW */
                                        : (int32_t)((zf < -8.0f ? -8.0f : zf > 2.0f ? 2.0f : zf) * (65535.0f * 256.0f));
            int32_t rq = (int32_t)(cr * 65536.0f), gq = (int32_t)(cg * 65536.0f), bq = (int32_t)(cb * 65536.0f), aq = (int32_t)(ca * 65536.0f);
            int32_t rdq = rdq_t, gdq = gdq_t, bdq = bdq_t, adq = adq_t;
            if (pshade && persp && sPsVcvt) { /* the same values: 1/16 exact on full spans; on ARM sat_i32 by vcvt */
                const float is = span == PERSP_SPAN ? 1.0f / PERSP_SPAN : 1.0f / fspan;
                const float ar = cr * rw, ag = cg * rw, ab = cb * rw, aa = ca * rw;
                const float fr4[4] = { ar * 65536.0f, ag * 65536.0f, ab * 65536.0f, aa * 65536.0f };
                const float fd4[4] = { ((cr + crdx * fspan) * rw_b - ar) * is * 65536.0f, ((cg + cgdx * fspan) * rw_b - ag) * is * 65536.0f,
                                       ((cb + cbdx * fspan) * rw_b - ab) * is * 65536.0f, ((ca + cadx * fspan) * rw_b - aa) * is * 65536.0f };
#if defined(HH_CM_NEON)
                const int32x4_t q4 = vcvtq_s32_f32(vld1q_f32(fr4)), d4 = vcvtq_s32_f32(vld1q_f32(fd4));
                rq = vgetq_lane_s32(q4, 0); gq = vgetq_lane_s32(q4, 1); bq = vgetq_lane_s32(q4, 2); aq = vgetq_lane_s32(q4, 3);
                rdq = vgetq_lane_s32(d4, 0); gdq = vgetq_lane_s32(d4, 1); bdq = vgetq_lane_s32(d4, 2); adq = vgetq_lane_s32(d4, 3);
#else
                rq = sat_i32(fr4[0]); gq = sat_i32(fr4[1]); bq = sat_i32(fr4[2]); aq = sat_i32(fr4[3]);
                rdq = sat_i32(fd4[0]); gdq = sat_i32(fd4[1]); bdq = sat_i32(fd4[2]); adq = sat_i32(fd4[3]);
#endif
            } else if (pshade && persp) { /* OOT_PSHADE: colour/w at both span ends (rw, rw_b), linear between */
                const float is = 1.0f / fspan;
                const float ar = cr * rw, ag = cg * rw, ab = cb * rw, aa = ca * rw;
                rq = sat_i32(ar * 65536.0f); gq = sat_i32(ag * 65536.0f); bq = sat_i32(ab * 65536.0f); aq = sat_i32(aa * 65536.0f);
                rdq = sat_i32(((cr + crdx * fspan) * rw_b - ar) * is * 65536.0f);
                gdq = sat_i32(((cg + cgdx * fspan) * rw_b - ag) * is * 65536.0f);
                bdq = sat_i32(((cb + cbdx * fspan) * rw_b - ab) * is * 65536.0f);
                adq = sat_i32(((ca + cadx * fspan) * rw_b - aa) * is * 65536.0f);
            }

/* BILIN=1: 3-tap-style bilinear (4 texel taps, blended by the 16.16 fraction).
 * Only the 3D world uses it (2D/HUD/text stay nearest -- bilinear blurs text). */
#define FETCH_NEAREST                                                                                  \
                    const int iu = wrapf(uf >> 16, tw, ws, tmask_u);                                    \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_at(texpx, t16, iv * tw + iu);
/* FETCH_NEAREST on a span whose every texel coordinate lies inside the texture (nin, below): wrapf is the identity
 * there for every wrap mode (a repeat's mask or modulo, a clamp), and the storage is a compile-time choice */
#define FETCH_NEARIN32  const uint32_t t = ((const uint32_t *)texpx)[(vf >> 16) * tw + (uf >> 16)];
#define FETCH_NEARIN16  const uint32_t t = texel_at(texpx, true, (vf >> 16) * tw + (uf >> 16));
#define FETCH_BILINEAR  const uint32_t t = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);
/* The detail layer (sDetail, copied to dd for the span): tile 1 beside tile 0, mixed per channel. */
#define DETAIL_TEXEL(T0)                                                                               \
                    const int32_t du1 = (int32_t)((((uint32_t)(uf >> dd.srs) << dd.sls) + dd.offs) & dpmu); \
                    const int32_t dv1 = (int32_t)((((uint32_t)(vf >> dd.srt) << dd.slt) + dd.offt) & dpmv); \
                    const uint32_t t1d = dbilin ? bilerp(dpx, d16, dw1, dh1, WRAP_REPEAT, WRAP_REPEAT, du1, dv1, dw1 - 1, dh1 - 1) \
                                                : texel_at(dpx, d16, (dv1 >> 16) * dw1 + (du1 >> 16));   \
                    const uint32_t t = detail_mix(T0, t1d, dd.w);
#define FETCH_DBILINEAR                                                                                \
                    const uint32_t t0d = bilerp(texpx, t16, tw, th, ws, wt, uf, vf, tmask_u, tmask_v);  \
                    DETAIL_TEXEL(t0d)
#define FETCH_DNEAREST                                                                                 \
                    const uint32_t t0d = texel_at(texpx, t16, wrapf(vf >> 16, th, wt, tmask_v) * tw + wrapf(uf >> 16, tw, ws, tmask_u)); \
                    DETAIL_TEXEL(t0d)
/* UI quads squeezed 4:3 -> 1:1 like the 2D sprites: area average along S. */
#define FETCH_BOX                                                                                      \
                    const int iv = wrapf(vf >> 16, th, wt, tmask_v);                                    \
                    const uint32_t t = texel_box(texpx, t16, iv * tw, uf, duf, tw, ws, tmask_u, sUseAlpha);
/* FLAT=1: one shade colour for the whole triangle (fsr..fsa, clamped once per
 * triangle above) -- the four colour accumulators and their steps drop out of
 * the pixel loop. Same values: flat => every step is 0.
 * CM=1: the combiner is plain texture x shade (cm_mod), so the per-pixel walk
 * of the combine flags drops out. (Both MK64 perf round 24, bit-exact.)
 * CM=3 / CM=4: CM=1 / CM=0 fogged per pixel after the combine (sFogPx, fogtri):
 * lerp(colour, fog, the vertex alpha = the fog factor); a constant combiner alpha
 * the factor displaced comes back as fga (MK64 c400d1b + 7d23507). */
#define COLSTEP_0 , rq += rdq, gq += gdq, bq += bdq, aq += adq
#define COLSTEP_1
#define SPAN_LOOP(TEX, ZTEST, ALPHA, BILIN) SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, 0, 0)
#define SPAN_LOOP_CM(TEX, ZTEST, ALPHA, BILIN, FLAT, CM)                                               \
            for (int i = 0; i < span; i++, idx++, uf += duf, vf += dvf, zq += zdq COLSTEP_##FLAT) {     \
                const int uz = clamp16_sh8(zq);                                                         \
                if (ZTEST) { if (uz > zb[idx] + zbias) continue; }                                      \
                const int sr = FLAT ? fsr : clamp8_sh16(rq), sg = FLAT ? fsg : clamp8_sh16(gq);         \
                const int sb = FLAT ? fsb : clamp8_sh16(bq), sa = FLAT ? fsa : clamp8_sh16(aq);         \
                int fr, fg, fb, fa, fta = 255; /* fta: the texel alpha a fogged constant alpha multiplies */ \
                if (TEX) {                                                                              \
                    FETCH_##BILIN                                                                       \
                    const int ta = (t>>24)&0xff;                                                        \
                    if (a_tex) fta = ta;                                                                \
                    const int tr = (CM % 4 == 0 && rgb_texa) ? ta : (t>>16)&0xff, tg = (CM % 4 == 0 && rgb_texa) ? ta : (t>>8)&0xff, tb = (CM % 4 == 0 && rgb_texa) ? ta : t&0xff; \
                    if (CM == 1 || CM == 3) { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }   \
                    else if (CM == 5)   { fr=lkr+((sr-lkr)*tr)/255; fg=lkg+((sg-lkg)*tg)/255; fb=lkb+((sb-lkb)*tb)/255; } \
                    else if (!rgb_tex)  { fr=sr; fg=sg; fb=sb; }                                        \
                    else if (!rgb_in)   { fr=tr; fg=tg; fb=tb; }                                        \
                    else if (texlerp)   { fr=lkr+((sr-lkr)*tr)/255; fg=lkg+((sg-lkg)*tg)/255; fb=lkb+((sb-lkb)*tb)/255; } \
                    else if (texlerpv)  { fr=sr+((lkr-sr)*tr)/255; fg=sg+((lkg-sg)*tg)/255; fb=sb+((lkb-sb)*tb)/255; } \
                    else if (texadd)    { fr=tr+sr; fg=tg+sg; fb=tb+sb;                                 \
                                          fr = fr>255?255:fr; fg = fg>255?255:fg; fb = fb>255?255:fb; } \
                    else                { fr=div255(tr*sr); fg=div255(tg*sg); fb=div255(tb*sb); }       \
                    fa = !a_tex ? sa : (!a_in ? ta : div255(ta*sa));                                    \
                } else { fr=sr; fg=sg; fb=sb; fa=sa; }                                                  \
                if (CM == 3 || CM == 4) { fr=div255(fr*(255-sa) + fgr*sa); fg=div255(fg*(255-sa) + fgg*sa); \
                               fb=div255(fb*(255-sa) + fgb*sa);                                         \
                               if (ALPHA && fga >= 0) fa = fta < 255 ? div255(fta*fga) : fga; }        \
                if (ALPHA) {                                                                            \
                    if (fa < amin) { OVD_REJ(TEX); continue; }                                            \
                    if (fa < 255 && (ablend || zb[idx] != 0xFFFF || (fbuf[idx] & cmask) != clr)) {      \
                        const uint32_t d = fbuf[idx];                                                   \
                        const int dr=(d>>16)&0xff, dg=(d>>8)&0xff, db=d&0xff;                           \
                        fr = div255(fr*fa + dr*(255-fa));                                               \
                        fg = div255(fg*fa + dg*(255-fa));                                               \
                        fb = div255(fb*fa + db*(255-fa));                                               \
                    } else if (enote && fa < 255) edge_note(fbuf + idx, fa, (ZTEST && dmask) ? uz : 0xFFFF); \
                }                                                                                       \
                fbuf[idx] = argb((uint8_t)fr,(uint8_t)fg,(uint8_t)fb,255);                              \
                OVD_NOTE(fbuf + idx - sFB, ALPHA && fa < 255);                                          \
                if (ZTEST && dmask) zb[idx] = (uint16_t)uz;                                             \
            }
            const bool boxspan = tex && !use_bilin && sUiMode && gfx_soft_hud_box > 0 && duf > 66846 && duf <= 196608;
            if (dtex) { /* the detail layer; the NEON rows take the z-tested texture x shade case */
                const SoftDetail dd = sDetail; /* locals: the framebuffer stores can't alias them */
                const void *const dpx = dtex->px; const bool d16 = dtex->is16;
                const int dw1 = dtex->w, dh1 = dtex->h;
                const uint32_t dpmu = ((uint32_t)dw1 << 16) - 1u, dpmv = ((uint32_t)dh1 << 16) - 1u;
                if (fogtri) {
                    if (use_bilin) {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, DBILINEAR, 0, 4) else SPAN_LOOP_CM(1, 1, 0, DBILINEAR, 0, 4) }
                        else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, DBILINEAR, 0, 4) else SPAN_LOOP_CM(1, 0, 0, DBILINEAR, 0, 4) }
                    } else {
                        if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, DNEAREST, 0, 4) else SPAN_LOOP_CM(1, 1, 0, DNEAREST, 0, 4) }
                        else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, DNEAREST, 0, 4) else SPAN_LOOP_CM(1, 0, 0, DNEAREST, 0, 4) }
                    }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, DBILINEAR) else SPAN_LOOP(1, 1, 0, DBILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, DBILINEAR) else SPAN_LOOP(1, 0, 0, DBILINEAR) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, DNEAREST) else SPAN_LOOP(1, 1, 0, DNEAREST) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, DNEAREST) else SPAN_LOOP(1, 0, 0, DNEAREST) }
                }
            } else if (tex && ztest && cm_mod && !boxspan && fogtri) {
                /* the fogged world: z-tested texture x shade, fogged per pixel */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 3) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 3) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 3)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 3) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 3) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 3) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 3)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 3) }
                }
            } else if (tex && fogtri && !boxspan) {
                /* fogged, not texture x shade (decals, lerps, texture edges): the general loops + the fog step */
                if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 4) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 4) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, BILINEAR, 0, 4) else SPAN_LOOP_CM(1, 0, 0, BILINEAR, 0, 4) }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 4) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 4) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEAREST, 0, 4) else SPAN_LOOP_CM(1, 0, 0, NEAREST, 0, 4) }
                }
            } else if (tex && ztest && cm_mod && !boxspan) {
                /* the hot path: z-tested texture x shade */
                if (flat) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 1) }
                } else {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 0, 1) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 0, 1) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 1)  else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 1) }
                }
            } else if (tex) {
                /* CM 5 (OOT_UILOOP, 2026-10-01): the (A - K) * TEXEL0 + K tint as a compile-time combine -- the HUD's
                 * buttons and labels (UI triangles: nearest, mostly no z). The generic loops walk rgb_texa, rgb_tex,
                 * rgb_in and texlerp per pixel from the stack to reach the same lines; =0: the generic loops. */
                const bool tl5 = sUiLoop && texlerp && rgb_tex && rgb_in && !rgb_texa && !use_bilin;
                /* OOT_NEARIN (2026-10-01): a nearest span whose texel coordinates all lie inside the texture -- u and v
                 * step linearly, so their two ends tell (in 64 bits: no int32 wrap between them either) -- reads the
                 * texel without wrapf (an sdiv and a multiply a pixel on the HUD's odd-width textures) */
                bool nin = false;
                if (sNearIn && !use_bilin) {
                    const int64_t ua = uf, ub = (int64_t)uf + (int64_t)(span - 1) * duf;
                    const int64_t va = vf, vb = (int64_t)vf + (int64_t)(span - 1) * dvf;
                    nin = (ua < ub ? ua : ub) >= 0 && (ua < ub ? ub : ua) < ((int64_t)tw << 16) &&
                          (va < vb ? va : vb) >= 0 && (va < vb ? vb : va) < ((int64_t)th << 16);
                }
                if (boxspan && tl5) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BOX, 0, 5) else SPAN_LOOP_CM(1, 1, 0, BOX, 0, 5) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, BOX, 0, 5) else SPAN_LOOP_CM(1, 0, 0, BOX, 0, 5) }
                } else if (boxspan) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BOX) else SPAN_LOOP(1, 1, 0, BOX) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BOX) else SPAN_LOOP(1, 0, 0, BOX) }
#if defined(HH_CM_NEON)
                } else if (tl5 && !ztest && nin && sUiNeon) { /* OOT_UINEON: the same span in NEON lanes */
                    const uint16x4_t kk = { (uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0 };
                    ui_tint_n(fbuf + idx, zb + idx, span, texpx, t16, tw, uf, duf, vf, dvf, (int32x4_t){ bq, gq, rq, aq },
                              (int32x4_t){ bdq, gdq, rdq, adq }, kk, !a_tex ? 2 : !a_in ? 1 : 0, sUseAlpha, amin, ablend, clr);
                    idx += span;
#endif
                } else if (tl5 && !(flat && ztest) && nin) {
                    if (t16) { if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEARIN16, 0, 5) else SPAN_LOOP_CM(1, 1, 0, NEARIN16, 0, 5) }
                               else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEARIN16, 0, 5) else SPAN_LOOP_CM(1, 0, 0, NEARIN16, 0, 5) } }
                    else     { if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEARIN32, 0, 5) else SPAN_LOOP_CM(1, 1, 0, NEARIN32, 0, 5) }
                               else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEARIN32, 0, 5) else SPAN_LOOP_CM(1, 0, 0, NEARIN32, 0, 5) } }
                } else if (tl5 && !(flat && ztest)) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 0, 5) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 0, 5) }
                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEAREST, 0, 5) else SPAN_LOOP_CM(1, 0, 0, NEAREST, 0, 5) }
                } else if (flat && ztest) {
                    if (use_bilin) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BILINEAR, 1, 0) else SPAN_LOOP_CM(1, 1, 0, BILINEAR, 1, 0) }
                    else           { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, NEAREST, 1, 0) else SPAN_LOOP_CM(1, 1, 0, NEAREST, 1, 0) }
                } else if (use_bilin) {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, BILINEAR) else SPAN_LOOP(1, 1, 0, BILINEAR) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, BILINEAR) else SPAN_LOOP(1, 0, 0, BILINEAR) }
                } else if (nin) {
                    if (t16) { if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEARIN16) else SPAN_LOOP(1, 1, 0, NEARIN16) }
                               else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEARIN16) else SPAN_LOOP(1, 0, 0, NEARIN16) } }
                    else     { if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEARIN32) else SPAN_LOOP(1, 1, 0, NEARIN32) }
                               else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEARIN32) else SPAN_LOOP(1, 0, 0, NEARIN32) } }
                } else {
                    if (ztest) { if (sUseAlpha) SPAN_LOOP(1, 1, 1, NEAREST) else SPAN_LOOP(1, 1, 0, NEAREST) }
                    else       { if (sUseAlpha) SPAN_LOOP(1, 0, 1, NEAREST) else SPAN_LOOP(1, 0, 0, NEAREST) }
                }
            } else if (fogtri) {
                if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(0, 1, 1, NEAREST, 0, 4) else SPAN_LOOP_CM(0, 1, 0, NEAREST, 0, 4) }
                else       { if (sUseAlpha) SPAN_LOOP_CM(0, 0, 1, NEAREST, 0, 4) else SPAN_LOOP_CM(0, 0, 0, NEAREST, 0, 4) }
            } else if (!ztest && !sUseAlpha && rdq == 0 && gdq == 0 && bdq == 0) {
                /* Constant-colour run: with no z, no alpha and zero steps every
                 * pixel of the span is argb(clamp(rq), clamp(gq), clamp(bq), 255). */
                const uint32_t c = argb((uint8_t)clamp8_sh16(rq), (uint8_t)clamp8_sh16(gq), (uint8_t)clamp8_sh16(bq), 255);
                uint32_t *const row = fbuf + idx;
                for (int i = 0; i < span; i++) { row[i] = c; OVD_NOTE(row + i - sFB, 0); }
                idx += span;
            } else {
                if (ztest) { if (sUseAlpha) SPAN_LOOP(0, 1, 1, NEAREST) else SPAN_LOOP(0, 1, 0, NEAREST) }
                else       { if (sUseAlpha) SPAN_LOOP(0, 0, 1, NEAREST) else SPAN_LOOP(0, 0, 0, NEAREST) }
            }
#undef SPAN_LOOP
#undef SPAN_LOOP_CM
#undef COLSTEP_0
#undef COLSTEP_1
#undef FETCH_NEAREST
#undef FETCH_BILINEAR
#undef FETCH_NEARIN32
#undef FETCH_NEARIN16
#undef FETCH_BOX
#undef DETAIL_TEXEL
#undef FETCH_DBILINEAR
#undef FETCH_DNEAREST
            zf += zdx * fspan;
            cr += crdx * fspan; cg += cgdx * fspan; cb += cbdx * fspan; ca += cadx * fspan;
            uw += uwdx * fspan;
            vw += vwdx * fspan;
            sw = sw_b; rw = rw_b; /* identical to sw += swdx*span; reciprocal carried */
            x += span;
        }
    }
