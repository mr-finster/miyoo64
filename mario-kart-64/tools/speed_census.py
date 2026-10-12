#!/usr/bin/env python3
"""The 2026-10-07 speed round's path census (BUILDLOG "2026-10-07: SM64's speed round ported"), for a census build only.

Writes a copy of gfx_soft.c (as of origin/main 412fb3e) with per-path counters printed at exit ("CENSUS <name> <n>"):
triangles / rows / spans / pixels on the NEON four-pixel spans, their scalar tails, smooth and repeat-wrapped groups,
depth-tested spans wholly behind the depth buffer, and the no-depth texel-alone pixels SM64's NEONNOTO targets.

  python3 tools/speed_census.py <origin/main's gfx_soft.c> <out.c>
  # build that file in place of gfx_soft.c (PLAT=miyoo), run two qemu runs that exit at F1 and F2, diff the lines
"""
import sys
src, out = sys.argv[1], sys.argv[2]
s = open(src).read()
def rep(a, b):
    global s
    assert s.count(a) == 1, a
    s = s.replace(a, b)
rep('''#define PERSP_SPAN 16
#if defined(GFX_SOFT_NEON_SPAN)
#include "gfx_soft_row_neon.h"
#endif
''', '''#define PERSP_SPAN 16
#if defined(GFX_SOFT_NEON_SPAN)
#include "gfx_soft_row_neon.h"
#endif
enum { CN_TRI, CN_TRI_NEON, CN_TRI_NEON_OWN, CN_TRI_NEON_AROWS, CN_ROW, CN_ROW_NEON, CN_SPAN_NEON, CN_SPAN_NEON_CALL,
       CN_SPAN_TAIL, CN_PX_ALL, CN_PX_NEON, CN_PX_TAIL, CN_GRP, CN_GRP_SMOOTH, CN_GRP_RR, CN_ZSPAN4, CN_ZBEHIND, CN_ZBEHIND_PX,
       CN_PX_NOZ_TO, CN_PX_NOZ_TO_NEONABLE, CN_PX_NOZ_TEX, CN_PX_Z_OTHER, CN_PX_UNTEX, CN_SPAN_NOFIT, CN_SPAN_SKIP, CN_N };
static uint64_t sCen[CN_N];
static const char *const sCenName[CN_N] = { "tri", "tri_neon", "tri_neon_own", "tri_neon_arows", "row", "row_neon", "span_neon",
    "span_neon_call", "span_tail", "px_all", "px_neon", "px_tail", "grp", "grp_smooth", "grp_rr", "zspan4", "zbehind", "zbehind_px",
    "px_noz_to", "px_noz_to_neonable", "px_noz_tex", "px_z_other", "px_untex", "span_nofit", "span_skip" };
static void census_print(void) {
    for (int i = 0; i < CN_N; i++) port_log_always("CENSUS %s %llu\\n", sCenName[i], (unsigned long long)sCen[i]);
}
#define CEN(i, n) (sCen[i] += (uint64_t)(n))
''')
rep('''    OwnTri oe; /* MK64_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;''', '''    OwnTri oe; /* MK64_FILLRULE: the owned-edge test (see own_edge) */
    const bool own = sOwnTri;
    { static int reg; if (!reg) { reg = 1; atexit(census_print); } }
    CEN(CN_TRI, 1);
#if defined(GFX_SOFT_NEON_SPAN)
    if (neon_cm) { CEN(CN_TRI_NEON, 1); if (own) CEN(CN_TRI_NEON_OWN, 1); if (arows) CEN(CN_TRI_NEON_AROWS, 1); }
    const bool cen_neon = neon_cm;
#else
    const bool cen_neon = false;
#endif
    const int cen_cat = cen_neon ? 0 : !tex ? CN_PX_UNTEX : ztest ? CN_PX_Z_OTHER
                      : (rgb_tex && !rgb_in && !rgb_texa && !fogtri) ? CN_PX_NOZ_TO : CN_PX_NOZ_TEX;
    const bool cen_noto_ok = !t16 && tmask_u >= 0 && tmask_v >= 0;''')
rep('''        tri_px += (uint32_t)(xe - xs); /* fill/overdraw proxy (before z-reject) */''', '''        tri_px += (uint32_t)(xe - xs); /* fill/overdraw proxy (before z-reject) */
        CEN(CN_ROW, 1); CEN(CN_PX_ALL, xe - xs);
        if (cen_neon) CEN(CN_ROW_NEON, 1);
        else { CEN(cen_cat, xe - xs); if (cen_cat == CN_PX_NOZ_TO && cen_noto_ok) CEN(CN_PX_NOZ_TO_NEONABLE, xe - xs); }''')
rep('''            int span_left = span;
            if (arows) {''', '''            int span_left = span;
            if (cen_neon) {
                CEN(CN_SPAN_NEON, 1);
                if (span >= 4) {
                    CEN(CN_ZSPAN4, 1);
                    const int uz0 = clamp16_sh8(zq), uz1 = clamp16_sh8(zq + (span - 1) * zdq);
                    int mz = 0; for (int i = 0; i < span; i++) if (zb[idx + i] > mz) mz = zb[idx + i];
                    mz += zbias; if (mz > 65535) mz = 65535;
                    if ((uz0 < uz1 ? uz0 : uz1) > mz) { CEN(CN_ZBEHIND, 1); CEN(CN_ZBEHIND_PX, span); }
                }
            }
            if (arows) {''')
rep('''                    gfx_soft_spanskip_spans++; gfx_soft_spanskip_px += (uint64_t)span;''', '''                    gfx_soft_spanskip_spans++; gfx_soft_spanskip_px += (uint64_t)span;
                    CEN(CN_SPAN_SKIP, 1);''')
rep('''                    span_left = span - n4;
                }
            }
#endif''', '''                    span_left = span - n4;
                    CEN(CN_SPAN_NEON_CALL, 1); CEN(CN_PX_NEON, n4); CEN(CN_GRP, n4 / 4);
                    if (!flat) CEN(CN_GRP_SMOOTH, n4 / 4);
                    if (ws == WRAP_REPEAT && wt == WRAP_REPEAT) CEN(CN_GRP_RR, n4 / 4);
                } else CEN(CN_SPAN_NOFIT, 1);
            }
            if (neon_cm && span_left > 0) { CEN(CN_SPAN_TAIL, 1); CEN(CN_PX_TAIL, span_left); }
#endif''')
open(out, 'w').write(s)
