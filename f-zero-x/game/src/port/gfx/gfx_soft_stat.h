/* Overdraw census (make PLAT=pc STAT=1, into build/pc-stat; never in a release build).
 * Included at the end of gfx_soft.c. For every batch it records the pixels it changed
 * (colour, or depth only) and, at the end of the frame, how many of its colour writes
 * are still visible. Summed by batch signature over VIs [FZX_STAT_FROM, FZX_STAT_TO]
 * (default 1200-1500) and logged at FZX_STAT_TO; FZX_STAT_DUMP=<vi> also logs that
 * frame batch by batch. Unified targets only (3D at 100%). Slow: it copies both
 * buffers around every batch. It measures the display lists' own overdraw, so the
 * deferred backgrounds are off in this build unless FZX_DEFER=1 (with them on, a batch
 * that triggers a resolve is charged the background pixels it resolves). */
#include <stdarg.h>
extern unsigned int port_os_vi_count(void);
extern char gfx_probe_note[256];

typedef struct { char sig[160]; uint32_t tris, wr, zo, sv, n; } StatRow;
#define STAT_MAXB 1024
#define STAT_MAXSIG 512
static uint32_t *sStPrevFB; static uint16_t *sStPrevZB; static uint16_t *sStOwner;
static StatRow sStB[STAT_MAXB]; static int sStN;
static StatRow sStSum[STAT_MAXSIG]; static int sStNSig;
static int sStFrom = -1, sStTo, sStDump, sStOn, sStFrames, sStDone;
static char sStCur[160];
/* Span pixels by raster path (raster_tri's own count, before the depth test): kind 0-3
 * are hot_span's HOT_MOD / TL2 / TEX / DECAL, 10 = textured x shade on the generic loops,
 * 11 = other textured, 12 = untextured. */
static double sStPath[2][16][2][2][2][2][2];
static void stat_path(uint32_t px, bool is3d, int kind, bool bilin, bool zt, bool alpha, bool zw, bool fog) {
    if (sStOn && kind >= 0 && kind < 16) sStPath[is3d][kind][bilin][zt][alpha][zw][fog] += px;
}
/* hot_span's pass 2, by texel path: [kind][alpha][0 nearest, 1 inside, 2 repeat, 3 clamp, 4 other] */
static double sStBil[5][2][5];
static void stat_bilin(bool bilin, bool inside, bool rr, bool cc, int n, int kind, bool alpha) {
    if (!sStOn || kind < 0 || kind > 4) return;
    sStBil[kind][alpha][!bilin ? 0 : inside ? 1 : rr ? 2 : cc ? 3 : 4] += n;
}
/* spans whose texel coordinates don't move: [kind][0 duf==0&&dvf==0, 1 duf==0 on a 1-row texture, 2 other] */
static double sStConst[5][3];
static void stat_const(int kind, int th, int32_t duf, int32_t dvf, int n) {
    if (!sStOn || kind < 0 || kind > 4) return;
    sStConst[kind][duf == 0 && dvf == 0 ? 0 : duf == 0 && th == 1 ? 1 : 2] += n;
}
/* The 2026-10-07 speed round (SM64's six switches): rows and spans by the paths they touch.
 * sStRow: [0] rows of hot triangles (hot_row), [1] of them with an occluder cut (visible-only), [2] with owned edges
 * (FZX_FILLRULE), [3] rows of the other triangles (raster_tri's own span loop); [4] hot triangles, [5] of them flat.
 * sStZ (hot_span's pass 1, spans of 4+ pixels): [0] spans, [1] depth-tested, [2] depth-tested with no pixel in front,
 * [3] of those, the ones ZSKIP's span test (nearest z > farthest stored + the decal window) finds, [4] their pixels,
 * [5] untested (Z_UPD alone, the track). */
static double sStRow[6], sStZ[6];
static void stat_row(bool hot, bool cut, bool own) {
    if (!sStOn) return;
    if (hot) { sStRow[0]++; if (cut) sStRow[1]++; if (own) sStRow[2]++; } else sStRow[3]++;
}
static void stat_tri(bool hot, bool flat) { if (sStOn && hot) { sStRow[4]++; if (flat) sStRow[5]++; } }
static void stat_zspan(bool ztest, int span, int n, int32_t zq, int32_t zdq, const uint16_t *zb, int zbias) {
    if (!sStOn || span < 4) return;
    sStZ[0]++;
    if (!ztest) { sStZ[5]++; return; }
    sStZ[1]++;
    if (n) return;
    sStZ[2]++;
    int mx = 0;
    for (int i = 0; i < span; i++) mx = zb[i] > mx ? zb[i] : mx;
    const int a = clamp16_sh8(zq), b = clamp16_sh8(zq + (span - 1) * zdq);
    if ((a < b ? a : b) > mx + zbias) { sStZ[3]++; sStZ[4] += span; }
}
static void stat_sig(bool is2d) {
    const SoftTex *tex = (sCurShader && (sCurShader->cc.used_textures[0] || sCurShader->cc.used_textures[1])) ? &sTex[sBoundTex[0]] : NULL;
    if (tex && !tex->valid) tex = NULL;
    const char *kind = "-";
    if (sCurShader && tex) {
        const struct ShaderProgram *p = sCurShader;
        const bool cm = p->rgb_tex && p->rgb_in && !p->rgb_decal && !sTexLerp2 && !sTexLerp && !sTexAdd && !p->rgb_texa;
        const bool tl2 = p->rgb_tex && p->rgb_in && !p->rgb_decal && sTexLerp2 && !p->rgb_texa;
        const bool to = p->rgb_tex && !p->rgb_in && !p->rgb_texa, de = p->rgb_tex && p->rgb_in && p->rgb_decal && !p->rgb_texa;
        kind = cm ? "MOD" : tl2 ? "TL2" : to ? "TEX" : de ? "DECAL" : sTexLerp ? "texlerp" : sTexAdd ? "texadd" : p->rgb_texa ? "texa" : !p->rgb_tex ? "shade" : "other";
    } else if (sCurShader) kind = "untex";
    const char *cc = strstr(gfx_probe_note, "cc ");
    char ccs[12] = "?";
    if (cc) { memcpy(ccs, cc + 3, 8); ccs[8] = 0; }
    snprintf(sStCur, sizeof(sStCur), "%s %-5s tex %3dx%-3d zt %d zw %d a %d fog %d cc %s", is2d ? "2D" : "3D", kind,
             tex ? tex->w : 0, tex ? tex->h : 0, (int)sDepthTest, (int)sDepthMask, (int)sUseAlpha, (int)(sPostFog && !sUseAlpha), ccs);
}
static void stat_batch_begin(bool is2d) {
    if (sStFrom < 0) {
        const char *e = getenv("FZX_STAT_FROM"); sStFrom = e ? atoi(e) : 1200;
        e = getenv("FZX_STAT_TO"); sStTo = e ? atoi(e) : 1500;
        e = getenv("FZX_STAT_DUMP"); sStDump = e ? atoi(e) : -1;
        gfx_probe_on = 1; /* gfx_pc writes gfx_probe_note (cc) per batch; the probe pixel stays off-target */
        sProbeX = sProbeY = 1 << 20;
    }
    if (!sStOn || !sUnified) return;
    stat_sig(is2d);
    memcpy(sStPrevFB, sFB, (size_t)sW * sH * 4);
    memcpy(sStPrevZB, sZB, (size_t)sW * sH * 2);
}
static void stat_batch_end(size_t tris) {
    if (!sStOn || !sUnified || sStN >= STAT_MAXB) return;
    StatRow *b = &sStB[sStN];
    memset(b, 0, sizeof(*b));
    snprintf(b->sig, sizeof(b->sig), "%s", sStCur);
    b->tris = (uint32_t)tris; b->n = 1;
    for (int i = 0; i < sW * sH; i++) {
        if (sFB[i] != sStPrevFB[i]) { b->wr++; sStOwner[i] = (uint16_t)(sStN + 1); }
        else if (sZB[i] != sStPrevZB[i]) b->zo++;
    }
    sStN++;
}
static void stat_frame_begin(void) {
    const unsigned vi = port_os_vi_count();
    sStOn = sStFrom >= 0 && (int)vi >= sStFrom && (int)vi < sStTo && sUnified;
    if (!sStOn) return;
    if (!sStOwner) {
        sStPrevFB = malloc(1024 * 1024 * 4); sStPrevZB = malloc(1024 * 1024 * 2); sStOwner = malloc(1024 * 1024 * 2);
    }
    memset(sStOwner, 0, (size_t)sW * sH * 2);
    sStN = 0;
}
static void stat_frame_end(void) {
    if (!sStOn) {
        if (!sStDone && sStFrom >= 0 && (int)port_os_vi_count() >= sStTo && sStFrames) {
            uint32_t wr = 0, sv = 0, zo = 0;
            sStDone = 1;
            for (int s = 0; s < sStNSig; s++) { wr += sStSum[s].wr; sv += sStSum[s].sv; zo += sStSum[s].zo; }
            port_log("STAT %d frames, VI %d-%d: per frame %.0f px written, %.0f visible (%.2fx overdraw of %d), %.0f depth-only\n",
                     sStFrames, sStFrom, sStTo, (double)wr / sStFrames, (double)sv / sStFrames, sv ? (double)wr / sv : 0.0,
                     sW * sH, (double)zo / sStFrames);
            {
                static const char *kn[16] = { "hot MOD", "hot TL2", "hot TEX", "hot DECAL", "hot SHADEA", "", "", "", "", "",
                                              "loop tex*shade", "loop tex other", "loop untex" };
                for (int d = 1; d >= 0; d--) for (int k = 0; k < 16; k++) for (int b = 0; b < 2; b++) for (int z = 0; z < 2; z++)
                    for (int a = 0; a < 2; a++) for (int w = 0; w < 2; w++) for (int f = 0; f < 2; f++) {
                        const double v = sStPath[d][k][b][z][a][w][f] / sStFrames;
                        if (v >= 50) port_log("STATP %s %-14s %s zt %d a %d zw %d fog %d: %6.0f px\n", d ? "3D" : "2D", kn[k],
                                              b ? "bilinear" : "nearest ", z, a, w, f, v);
                    }
            }
            {
                static const char *kn[5] = { "MOD", "TL2", "TEX", "DECAL", "SHADEA" }, *pn[5] = { "nearest", "inside", "repeat", "clamp", "other" };
                for (int k = 0; k < 5; k++) for (int a = 0; a < 2; a++) for (int q = 0; q < 5; q++) {
                    const double v = sStBil[k][a][q] / sStFrames;
                    if (v >= 50) port_log("STATB hot %-5s alpha %d %-7s: %6.0f px\n", kn[k], a, pn[q], v);
                }
            }
            port_log("STATR per frame: %.0f hot rows (%.0f with an occluder cut, %.0f owned edges), %.0f other rows; %.0f hot triangles (%.0f flat)\n",
                     sStRow[0] / sStFrames, sStRow[1] / sStFrames, sStRow[2] / sStFrames, sStRow[3] / sStFrames,
                     sStRow[4] / sStFrames, sStRow[5] / sStFrames);
            port_log("STATZ per frame: %.0f hot spans of 4+ px, %.0f untested, %.0f depth-tested, %.0f of them all behind, "
                     "%.0f found by the span test (%.0f px)\n", sStZ[0] / sStFrames, sStZ[5] / sStFrames, sStZ[1] / sStFrames,
                     sStZ[2] / sStFrames, sStZ[3] / sStFrames, sStZ[4] / sStFrames);
            for (int k = 0; k < 5; k++)
                port_log("STATC hot kind %d: still %6.0f, still on a 1-row texture %6.0f, moving %6.0f px\n", k,
                         sStConst[k][0] / sStFrames, sStConst[k][1] / sStFrames, sStConst[k][2] / sStFrames);
            for (int k = 0; k < sStNSig; k++) { /* by pixels written, descending */
                int best = k;
                for (int s = k + 1; s < sStNSig; s++) if (sStSum[s].wr > sStSum[best].wr) best = s;
                StatRow t = sStSum[k]; sStSum[k] = sStSum[best]; sStSum[best] = t;
                const StatRow *r = &sStSum[k];
                port_log("STAT %7.0f wr %7.0f vis (%3.0f%%) %6.0f zonly %5.1f tris %5.1f batches | %s\n", (double)r->wr / sStFrames,
                         (double)r->sv / sStFrames, r->wr ? 100.0 * r->sv / r->wr : 0.0, (double)r->zo / sStFrames,
                         (double)r->tris / sStFrames, (double)r->n / sStFrames, r->sig);
            }
        }
        return;
    }
    for (int i = 0; i < sW * sH; i++) if (sStOwner[i]) sStB[sStOwner[i] - 1].sv++;
    const int dump = (int)port_os_vi_count() == sStDump;
    for (int b = 0; b < sStN; b++) {
        const StatRow *r = &sStB[b];
        if (dump) port_log("STATF %3d %6u wr %6u vis %6u zonly %4u tris | %s\n", b, r->wr, r->sv, r->zo, r->tris, r->sig);
        int s = 0;
        while (s < sStNSig && strcmp(sStSum[s].sig, r->sig) != 0) s++;
        if (s == sStNSig) {
            if (sStNSig >= STAT_MAXSIG) continue;
            memset(&sStSum[s], 0, sizeof(sStSum[s]));
            snprintf(sStSum[s].sig, sizeof(sStSum[s].sig), "%s", r->sig);
            sStNSig++;
        }
        sStSum[s].wr += r->wr; sStSum[s].sv += r->sv; sStSum[s].zo += r->zo; sStSum[s].tris += r->tris; sStSum[s].n++;
    }
    sStFrames++;
}
