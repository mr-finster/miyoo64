#!/usr/bin/env python3
"""Mutations for raster_test.sh mut: each one perturbs one new rasterizer path in a copy of gfx_soft.c.
   raster_test_mut.py count | kind <i> | apply <i> <gfx_soft.c> <out>
   (apply prints the mutation's name, exit 1 if its text is gone; kind prints the H_KIND its frames need)"""
import sys

MUTS = [
    ('leana alpha test', '        if (fa < amin) continue;                                                                        \\',
     '        if (fa < amin + 1) continue;                                                                    \\'),
    ('leana AM 0 lane', 'const int fa = AM == 0 ? (int)vget_lane_u16(M, 3)',
     'const int fa = AM == 0 ? (int)vget_lane_u16(M, 2)'),
    ('leana AM 1 lane', '\n        const int fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 3)          \\',
     '\n        const int fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 2)          \\'),
    ('leana AM 2 lane', '\n                     : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\',
     '\n                     : AM == 2 ? (int)vget_lane_u16(S, 2) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\'),
    ('leana AM 3', '\n                     : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\',
     '\n                     : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac + 1 : div255(vget_lane_u16(Tx, 3) * fgac); \\'),
    ('leana AM 4', '\n                     : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\',
     '\n                     : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac + 255); \\'),
    ('leana TEX_EDGE clear test', '(*pf & 0xFFFFFFu) != clr)) {                        \\',
     '(*pf & 0xFFFFFFu) != clr + 1)) {                    \\'),
    ('leana z write', '        if (dmask) *pz = (uint16_t)uz;                                                                  \\\n    }\nstatic int sAlphaLean',
     '        if (dmask) *pz = (uint16_t)(uz ^ 1);                                                            \\\n    }\nstatic int sAlphaLean'),
    ('leana fog', 'fogv, fs));                    \\\n        }                                                                                               \\\n        if (fa < 255',
     'fogv, vadd_u16(fs, fs)));      \\\n        }                                                                                               \\\n        if (fa < 255'),
    ('leana detail', 'CM_LEANA_AM(1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV))',
     'CM_LEANA_AM(0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV))'),
    ('leana 16-bit repeating', '    if (rr) { if (T->t16) { CM_LEANA_AM(0, CM_LBILIN16) }',
     '    if (rr) { if (T->t16) { CM_LEANA_AM(0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }'),
    ('lean generic-wrap taps', 'iu1 = CM_WRAP_GU((su >> 16) + 1);',
     'iu1 = CM_WRAP_GU((su >> 16) + 2);'),
    ('leans store', 'pf++, uf += duf, vf += dvf) {                                     \\\n        uint16x4_t Tx; __VA_ARGS__                                                                      \\\n        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vorr_u16(Tx, a255), Tx))), 0);',
     'pf++, uf += duf, vf += dvf) {                                     \\\n        uint16x4_t Tx; __VA_ARGS__                                                                      \\\n        vst1_lane_u32(pf, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vorr_u16(vadd_u16(Tx, vdup_n_u16(1)), a255), Tx))), 0);'),
    ('ui loop CM 5 tint', 'else if (CM == 5)   { fr=lkr+((sr-lkr)*tr)/255;',
     'else if (CM == 5)   { fr=lkr+((sr-lkr)*tr)/256;'),
    ('ui loop CM 5 box', 'if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BOX, 0, 5) else SPAN_LOOP_CM(1, 1, 0, BOX, 0, 5) }\n                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, BOX, 0, 5)',
     'if (ztest) { if (sUseAlpha) SPAN_LOOP_CM(1, 1, 1, BOX, 0, 5) else SPAN_LOOP_CM(1, 1, 0, BOX, 0, 5) }\n                    else       { if (sUseAlpha) SPAN_LOOP_CM(1, 0, 1, NEAREST, 0, 5)'),
    ('scalar pshade vcvt', 'rdq = vgetq_lane_s32(d4, 0); gdq',
     'rdq = vgetq_lane_s32(d4, 1); gdq'),
    ('leanz z write', '        const int zbias = 0; const bool dmask = true; (void)zbias; (void)dmask;\n        if (sDsMarking) {',
     '        const int zbias = 0; const bool dmask = false; (void)zbias; (void)dmask;\n        if (sDsMarking) {'),
    ('leanz detail z bias', 'const int LX = 0; (void)LX; const int zbias = 0; const bool dmask = true; (void)zbias; (void)dmask;\n            if (fog)',
     'const int LX = 0; (void)LX; const int zbias = 256; const bool dmask = true; (void)zbias; (void)dmask;\n            if (fog)'),
    ('2d gen boxc', 'if (boxc)      { GEN2D_LOOP(F2D_BOXC, tl, tad, lr, lg, lb, 0) }',
     'if (boxc)      { GEN2D_LOOP(F2D_NEAR, tl, tad, lr, lg, lb, 0) }'),
    ('2d gen tint', 'const int lr = sLerpR, lg = sLerpG, lb = sLerpB;',
     'const int lr = sLerpR, lg = sLerpR, lb = sLerpB;'),
    ('2d gen add', 'else           { GEN2D_LOOP(F2D_NEAR, tl, tad, lr, lg, lb, 0) }',
     'else           { GEN2D_LOOP(F2D_NEAR, tl, 0, lr, lg, lb, 0) }'),
    ('2d lerp toward zero', 'n2d_div255(vmull_u8(s.val[2], pr))), n2d_div255(vmull_u8(s.val[2], qr)));',
     'n2d_div255(vmull_u8(s.val[2], pr))), n2d_div255(vmull_u8(s.val[2], pr)));'),
    ('2d lerp alpha', 'use_alpha, amin, sLerpR, sLerpG, sLerpB);',
     'use_alpha, amin + 1, sLerpR, sLerpG, sLerpB);'),
    ('ui nearin u end', '(ua < ub ? ub : ua) < ((int64_t)tw << 16)',
     '(ua < ub ? ub : ua) < ((int64_t)(tw + 1) << 16)'),
    ('ui nearin v start', '(va < vb ? va : vb) >= 0 &&',
     '(va < vb ? va : vb) >= -65536 &&'),
    ('ui nearin 16-bit', '#define FETCH_NEARIN16  const uint32_t t = texel_at(texpx, true,',
     '#define FETCH_NEARIN16  const uint32_t t = texel_at(texpx, false,'),
    ('ui neon tint toward zero', 'cm_div255_lean(vmul_u16(vqsub_u16(k, S), T)));',
     'cm_div255_lean(vmul_u16(vqsub_u16(S, k), T)));'),
    ('ui neon alpha source', ': amode == 1 ? (int)(t >> 24)',
     ': amode == 1 ? (int)((t >> 16) & 255)'),
    ('ui neon blend test', 'if (fa < 255 && (ablend || zb[i] != 0xFFFF',
     'if (fa < 254 && (ablend || zb[i] != 0xFFFF'),
    ('skydefer marked flush', 'if (sDsN && (sDsMarked || sDsN == DS_MAX || fbuf != sDsFB)) sky_flush();',
     'if (sDsN && (sDsN == DS_MAX || fbuf != sDsFB)) sky_flush();'),
    ('skydefer other draws flush', 'if (sDsN && !marking && !keep) sky_flush();',
     'if (sDsN && !marking && !keep && !cm_rows) sky_flush();'),
    ('skydefer mask skip', '        if (*pm) continue;                                                                              \\',
     '        if (*pm > 1) continue;                                                                          \\'),
    ('skydefer marking pixel', '        *pm = 1;                                                                                        \\',
     '        *pm = 0;                                                                                        \\'),
    ('skydefer texel rewrite', '    for (int k = 0; k < sDsN; k++) if (sDsTri[k].cmt.px == px) { sky_flush(); return; }',
     '    for (int k = 0; k < sDsN; k++) if (sDsTri[k].cmt.px == px + 1) { sky_flush(); return; }'),
    ('skydefer 2d settle', '    sky_settle(); /* OOT_SKYDEFER: the kept sky first (see raster_tri) */',
     '    /* removed */'),
    ('skydefer marking decals', 'const bool marking = cm_rows && cmt.lean && !cmt.alpha && sLeanZ && zbias == 0 && dmask',
     'const bool marking = cm_rows && cmt.lean && !cmt.alpha && sLeanZ && zbias >= 0 && dmask'),
    ('leanl AM 0 lane', '\n            fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 3)                \\',
     '\n            fa = AM == 0 ? (int)vget_lane_u16(M, 2) : AM == 1 ? (int)vget_lane_u16(Tx, 3)                \\'),
    ('leanl AM 1 lane', '\n            fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 3)                \\',
     '\n            fa = AM == 0 ? (int)vget_lane_u16(M, 3) : AM == 1 ? (int)vget_lane_u16(Tx, 2)                \\'),
    ('leanl AM 2 lane', '\n               : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\',
     '\n               : AM == 2 ? (int)vget_lane_u16(S, 2) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\'),
    ('leanl AM 3', '\n               : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\',
     '\n               : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac + 1 : div255(vget_lane_u16(Tx, 3) * fgac); \\'),
    ('leanl AM 4', '\n               : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac); \\',
     '\n               : AM == 2 ? (int)vget_lane_u16(S, 3) : AM == 3 ? fgac : div255(vget_lane_u16(Tx, 3) * fgac + 255); \\'),
    ('leanl tint toward zero', '\n                                cm_div255_lean(vmul_u16(vqsub_u16(lk, S), Tx)));                        \\',
     '\n                                cm_div255_lean(vmul_u16(vqsub_u16(S, lk), Tx)));                        \\'),
    ('leanl tint up', '\n        uint16x4_t M = vsub_u16(vadd_u16(lk, cm_div255_lean(vmul_u16(vqsub_u16(S, lk), Tx))),             \\',
     '\n        uint16x4_t M = vsub_u16(vadd_u16(lk, cm_div255_lean(vmul_u16(vqsub_u16(S, lk), vadd_u16(Tx, vdup_n_u16(1))))),             \\'),
    ('leanl alpha test', '\n            if (fa < amin) continue;                                                                    \\\n        }                                                                                               \\\n        if (FOG) {',
     '\n            if (fa < amin + 1) continue;                                                                    \\\n        }                                                                                               \\\n        if (FOG) {'),
    ('leanl blend test', '        if (ALPHA && fa < 255 && (ablend || *pz != 0xFFFF || (*pf & 0xFFFFFFu) != clr)) {               \\',
     '        if (ALPHA && fa < 254 && (ablend || *pz != 0xFFFF || (*pf & 0xFFFFFFu) != clr)) {               \\'),
    ('leanl TEX_EDGE clear test', '        if (ALPHA && fa < 255 && (ablend || *pz != 0xFFFF || (*pf & 0xFFFFFFu) != clr)) {               \\',
     '        if (ALPHA && fa < 255 && (ablend || *pz != 0xFFFF || (*pf & 0xFFFFFFu) != clr + 1)) {               \\'),
    ('leanl z write', '\n        if (dmask) *pz = (uint16_t)uz;                                                                  \\\n    }\nstatic int sLerpLean',
     '\n        if (dmask) *pz = (uint16_t)(uz ^ 1);                                                                  \\\n    }\nstatic int sLerpLean'),
    ('leanl fog', '            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, fs));                    \\\n        }                                                                                               \\\n        if (ALPHA && fa < 255',
     '            M = cm_div255_lean(vmla_u16(vmul_u16(M, vsub_u16(v255, fs)), fogv, vadd_u16(fs, fs)));                    \\\n        }                                                                                               \\\n        if (ALPHA && fa < 255'),
    ('leanl 16-bit repeating', '    if (rr) { if (T->t16) { CM_LEANL_AM(CM_LBILIN16) }',
     '    if (rr) { if (T->t16) { CM_LEANL_AM(CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }'),
    ('leanl opaque fog', 'if (fog) { CM_ROW(0, 0, 0, 1, FETCH) } else { CM_ROW(0, 0, 0, 0, FETCH) }',
     'if (fog) { CM_ROW(0, 0, 0, 0, FETCH) } else { CM_ROW(0, 0, 0, 0, FETCH) }'),
    ('leanl K lanes', '(uint16_t)lkb, (uint16_t)lkg, (uint16_t)lkr, 0 }; }',
     '(uint16_t)lkr, (uint16_t)lkg, (uint16_t)lkb, 0 }; }'),
    ('leanl twice excluded', '!rgb_texa && !(sTwiceReq && sUseAlpha && !dmask) &&',
     '!rgb_texa &&'),
    ('leanl rgb_texa excluded', '&& texlerp && rgb_tex && rgb_in &&\n                           !rgb_texa &&',
     '&& texlerp && rgb_tex && rgb_in &&\n                           1 &&'),
    ('neon pshade vcvt', 'if (sPsVcvt) { CM_PSHADE_VCVT } else {',
     'if (sPsVcvt) { CM_PSHADE_VCVT acc = vaddq_s32(acc, vdupq_n_s32(65536)); } else {'),
    ('detl leanl detail repeating', 'if (rr) { CM_LEANL_AM(1, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }',
     'if (rr) { CM_LEANL_AM(0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) }'),
    ('mag 2d row fraction', 'vsubq_f32(x, vcvtq_f32_s32(iu)), 256.0f)',
     'vsubq_f32(x, vcvtq_f32_s32(iu)), 255.0f)'),
    ('mag 2d row clamp', 'const int32x4_t c0 = rep ? vandq_s32(iu, vmask) : vminq_s32(vmaxq_s32(iu, vzero), vmax);',
     'const int32x4_t c0 = rep ? vandq_s32(iu, vmask) : vminq_s32(iu, vmax);'),
    ('mag 2d row rounding', 'half = vdupq_n_u32(32768);',
     'half = vdupq_n_u32(32767);'),
    ('mag 2d texel colour', 'combine_mod_n2d(row + cx0, tv, n, 255, 255, 255,',
     'combine_mod_n2d(row + cx0, tv, n, 255, 254, 255,'),
    ('noz alpha source', '                     : (int)vget_lane_u16(S, 3);                                                        \\',
     '                     : (int)vget_lane_u16(S, 2);                                                        \\'),
    ('noz alpha test', '                     : (int)vget_lane_u16(S, 3);                                                        \\\n        if (fa < amin) continue;',
     '                     : (int)vget_lane_u16(S, 3);                                                        \\\n        if (fa < amin + 1) continue;'),
    ('noz 16-bit taps', '    if (rr) { if (T->t16) { CM_LEANZ_AM(CM_LBILIN16) }',
     '    if (rr) { if (T->t16) { CM_LEANZ_AM(CM_LBILING(CM_TAP32)) }'),
    # 2026-10-04, the flame overlays: OOT_CLAMPTAP, OOT_ALPHASKIP (their frames: H_INSIDE); the generic-wrap fetch sites above it,
    # moved here (their text changed with CM_LBILINI)
    ('leana opaque generic-wrap 32 fog', 'else        { if (fog) { CM_ROW(0, 0, 0, 1, CM_LBILINI32(0)) }',
     'else        { if (fog) { CM_ROW(0, 0, 0, 0, CM_LBILINI32(0)) }'),
    ('leana opaque generic-wrap 16', '} else { CM_ROW(0, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } }',
     '} else { CM_ROW(0, 0, 0, 1, CM_LBILINI(CM_TAP16, 0)) } }'),
    ('leans 16-bit repeating', '    (void)th; (void)px; (void)zdq; (void)dacc0; (void)askip;\n    if (rr) { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILIN16) } else { CM_ROW(1, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }\n    else    { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(1, 0, 0, 0, CM_LBILINI32(0)) } }\n}\n/* OOT_UINEON',
     '    (void)th; (void)px; (void)zdq; (void)dacc0; (void)askip;\n    if (rr) { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } else { CM_ROW(1, 0, 0, 0, CM_LBILIN_RR32(px32, mu, CM_WRAP_RRV)) } }\n    else    { if (T->t16) { CM_ROW(1, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(1, 0, 0, 0, CM_LBILINI32(0)) } }\n}\n/* OOT_UINEON'),
    ('leans 32-bit generic wrap', 'CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(1, 0, 0, 0, CM_LBILINI32(0)) } }\n}\n/* OOT_UINEON',
     'CM_LBILINI(CM_TAP16, 0)) } else { CM_ROW(1, 0, 0, 0, CM_LBILINI(CM_TAP16, 0)) } }\n}\n/* OOT_UINEON'),
    ('leanl generic-wrap 32', '{ CM_LEANL_AM(0, CM_LBILINI(CM_TAP16, CM_ASKIP_AM)) } else { CM_LEANL_AM(0, CM_LBILINI32(CM_ASKIP_AM)) } }',
     '{ CM_LEANL_AM(0, CM_LBILINI(CM_TAP16, CM_ASKIP_AM)) } else { CM_LEANL_AM(0, CM_LBILINI(CM_TAP16, CM_ASKIP_AM)) } }'),
    ('detl leanl detail generic wrap', 'else { CM_LEANL_AM(1, CM_LBILINI32(0)) }',
     'else { CM_LEANL_AM(0, CM_LBILINI32(0)) }'),
    ('detc opaque detail', '} else { CM_ROW(0, 0, 1, 0, CM_LBILINI32(0)) }\n        return;',
     '} else { CM_ROW(0, 0, 0, 0, CM_LBILINI32(0)) }\n        return;'),
    ('detc alpha detail', 'else    { CM_LEANA_AM(1, CM_LBILINI32(0)) }',
     'else    { CM_LEANA_AM(0, CM_LBILINI32(0)) }'),
    ('ins 32 inside taps', 'const uint32_t *const p0 = px32 + y0 * tw + x0, *const p1 = p0 + tw;',
     'const uint32_t *const p0 = px32 + y0 * tw + x0, *const p1 = p0 + tw + 1;'),
    ('ins 32 inside skip', 'if ((SKIP) && (p0[0] | p0[1] | p1[0] | p1[1]) < askip) continue;',
     'if ((SKIP) && (p0[0] | p0[1] | p1[0]) < askip) continue;'),
    ('ins 32 inside test', '            const uint32_t *const p0 = px32',
     '            if (y0 == 0) continue; const uint32_t *const p0 = px32'),
    ('ins 32 edge skip', '            if ((SKIP) && (a | b | c | d) < askip) continue;',
     '            if ((SKIP) && (a | b | c) < askip) continue;'),
    ('ins 16 inside taps', 'a = TAP(i0); b = TAP(i0 + 1); c = TAP(i0 + tw); d = TAP(i0 + tw + 1);',
     'a = TAP(i0); b = TAP(i0 + 1); c = TAP(i0 + tw); d = TAP(i0 + tw);'),
    ('ins 16 skip', '        if ((SKIP) && (a | b | c | d) < askip) continue;                                                \\',
     '        if ((SKIP) && (a | b | c) < askip) continue;                                                    \\'),
    ('ins leanl skip threshold', '    const uint32_t askip = sAlphaSkip ? (uint32_t)amin << 24 : 0u;                         /* OOT_ALPHASKIP */\n    (void)th; (void)px;\n#undef CM_ROW_PIXELS\n#define CM_ROW_PIXELS CM_PIXELS_LEANL',
     '    const uint32_t askip = sAlphaSkip ? (uint32_t)(amin + 1) << 24 : 0u;                         /* OOT_ALPHASKIP */\n    (void)th; (void)px;\n#undef CM_ROW_PIXELS\n#define CM_ROW_PIXELS CM_PIXELS_LEANL'),
    ('ins skip alpha sources', '#define CM_ASKIP_AM (AM == 0 || AM == 1 || AM == 4)',
     '#define CM_ASKIP_AM (AM == 0 || AM == 1 || AM == 2 || AM == 4)'),
    ('ins leanl opaque no skip', '        const int AM = 2; (void)AM;',
     '        const int AM = 0; (void)AM;'),
    ('ins noz skip', 'CM_LEANZ_AM(CM_LBILINI32(CM_ASKIP_AM))',
     'CM_LEANZ_AM(CM_LBILINI32(1))'),
    ('ins leana skip sources forced', 'CM_LEANA_AM(0, CM_LBILINI32(CM_ASKIP_AM))',
     'CM_LEANA_AM(0, CM_LBILINI32(1))'),
]

def kind(name):
    if name.startswith("ins"):
        k = "lerp" if "leanl" in name else "alpha H_NOZ=1" if "noz" in name else "alpha"
        return k + " H_INSIDE=1"
    if name.startswith("detl"):
        return "lerp H_DETL=1"
    if name.startswith("mag"):
        return "2d H_MAG=1"
    if name.startswith("noz"):
        return "alpha H_NOZ=1"
    if name.startswith("detc"):
        return ("opq" if "opaque" in name else "alpha") + " H_DETC=1"
    if name.startswith("ui"):
        return "ui"
    if name.startswith("skydefer"):
        return "seq"
    if name.startswith("2d"):
        return "2d"
    if name.startswith("leanz"):
        return "opq"
    if name.startswith("leanl"):
        return "lerp"
    if name.startswith("scalar"):
        return "gen"
    return "sky" if name.startswith("leans") else "opq" if "opaque" in name else "" if name.startswith(("lean generic", "neon")) else "alpha"

if sys.argv[1] == "count":
    print(len(MUTS))
    sys.exit(0)
i = int(sys.argv[2])
name, a, b = MUTS[i]
if sys.argv[1] == "kind":
    print(kind(name))
    sys.exit(0)
print(name)
s = open(sys.argv[3]).read()
if s.count(a) != 1:
    sys.exit(1)
open(sys.argv[4], "w").write(s.replace(a, b))
