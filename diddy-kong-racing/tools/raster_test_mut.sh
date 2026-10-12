#!/bin/sh
# raster_test_mut.sh: is every new path of this round's rows exercised by tools/raster_test.c? Each mutation below is
# applied to a copy of port/sdl/gfx_soft.c, the ARM harness is built against the copy and run (under qemu-arm); a
# mutation is caught when some variant then differs from the defaults (the old rows, the scalar loops). Every one must
# be caught.   sh tools/raster_test_mut.sh [frames]   (in the build image; MUT=<text>: only the mutations named so;
# the speed round's ZSKIP decal window needs more frames: MUT=xspeed ... 40)
# (Turning a no-z branch-free blend into the branching one is an equivalent change, not a mutation: no z is written.)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
N=${1:-12}
out=${TMPDIR:-/tmp}/dkr_raster_mut; rm -rf "$out"; mkdir -p "$out"
SDK=${SDK:-/opt/miyoomini-toolchain/usr}
SR="$SDK/arm-linux-gnueabihf/sysroot"
python3 - "$here/port/sdl/gfx_soft.c" "$out" <<'PY'
import sys
src = open(sys.argv[1]).read()
muts = [
    ("rr pair: the wrap test", "if (iu0 < mu) { AB = vld1_u32", "if (iu0 <= mu) { AB = vld1_u32"),
    ("rr wrap: lane 1", "CD = vld1_lane_u32(px32 + b1, vld1_dup_u32(px32 + b1 + iu0), 1); }", "CD = vld1_lane_u32(px32 + b1 + 1, vld1_dup_u32(px32 + b1 + iu0), 1); }"),
    ("gen taps: iu1", "vld1_lane_u32(px32 + b1 + iu1, vld1_dup_u32(px32 + b0 + iu1), 1)", "vld1_lane_u32(px32 + b1 + iu0, vld1_dup_u32(px32 + b0 + iu1), 1)"),
    ("lerp: u weight", "vdupq_n_u16((uint16_t)((su >> 8) & 0xff))), 8);", "vdupq_n_u16((uint16_t)((su >> 7) & 0xff))), 8);"),
    ("shade clamp", "const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 16), v255);", "const uint16x4_t S = vmin_u16(vqshrun_n_s32(acc, 15), v255);"),
    ("fog factor lane", "const uint16x4_t fs = vdup_lane_u16(S, 3);", "const uint16x4_t fs = vdup_lane_u16(S, 2);"),
    ("alpha source M", "F = vbsl_u16(selM, M, vbsl_u16(selT, Tx, S));", "F = vbsl_u16(selM, Tx, vbsl_u16(selT, Tx, S));"),
    ("alpha source S", "F = vbsl_u16(selM, M, vbsl_u16(selT, Tx, S));", "F = vbsl_u16(selM, M, vbsl_u16(selT, Tx, M));"),
    ("bf: the reject", "M = vbsl_u16(vclt_u16(F, aminv), D, vorr_u16(Bl, a255));", "M = vbsl_u16(vcle_u16(F, aminv), D, vorr_u16(Bl, a255));"),
    ("bf: the store", "vst1_lane_u32(pf, vreinterpret_u32_u8(vtbl1_u8(vreinterpret_u8_u16(M), lo8)), 0);", "vst1_lane_u32(pf, vreinterpret_u32_u8(vtbl1_u8(vreinterpret_u8_u16(vorr_u16(M, a255)), lo8)), 0);"),
    ("alpha: the test", "if (fa < amin) continue;", "if (fa <= amin) continue;"),
    ("alpha: the blend limit", "const int amin = T->amin, ablim = T->ablend ? 255 : 0;", "const int amin = T->amin, ablim = 255;"),
    ("store: alpha byte", "lo8a = vcreate_u8(0x0000000008040200ull)", "lo8a = vcreate_u8(0x0000000006040200ull)"),
    ("z: LEANZ test", "if (uz > pz[-1] + (Z == 2 ? 0 : zbias)) continue; }", "if (uz > pz[-1] + (Z == 2 ? 1 : zbias)) continue; }"),
    ("z: the bias", "if (uz > pz[-1] + (Z == 2 ? 0 : zbias)) continue; }", "if (uz > pz[-1] + (Z == 2 ? 0 : 0)) continue; }"),
    ("z: the write flag", "if (Z == 2 || (Z == 1 && dmask)) pz[-1] = (uint16_t)uz;", "if (Z == 2 || Z == 1) pz[-1] = (uint16_t)uz;"),
    ("noz: the sky mask", "if (MASK && (*pf >> 24)) continue;", "if (0 && MASK && (*pf >> 24)) continue;"),
    ("noz: the span mask", "if (!MASK || !span_drawn(fb, span)) {", "if (!MASK || span_drawn(fb, span)) {"),
    ("fog alpha: amode 3", "cm_div255_lean(vmul_u16(Tx, fgav))", "cm_div255_lean(vmul_u16(S, fgav))"),
    ("fog alpha: the constant", "F = vbsl_u16(selF, vbsl_u16(selA3,", "F = vbsl_u16(selA3, vbsl_u16(selA3,"),
    ("noz fog: the fog step", "if (T->fog) { if (rr) { CM_ROWL_NOZ(0, 0, 0, 1, CM_LBIL_RR) }", "if (T->fog) { if (rr) { CM_ROWL_NOZ(0, 0, 0, 0, CM_LBIL_RR) }"),
    ("noz alpha: the fog step", "if (T->fog) { if (T->bf) { CM_ROWL_NOZ(0, 1, 1, 1, CM_LBIL_RR) }", "if (T->fog) { if (T->bf) { CM_ROWL_NOZ(0, 1, 1, 0, CM_LBIL_RR) }"),
    ("noz alpha: the span skip", "if (ALPHA && arows && span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                  \\", "if (ALPHA && arows && !span_clear(arows, uf, vf, duf, dvf, span, ws, wt)) {                 \\"),
    ("dispatch: noz fog on any wrap", "&& cm_fog && use_bilin && !t16 && (!sUseAlpha || rrtex);", "&& cm_fog && use_bilin && !t16;"),
    ("dispatch: noz alpha on any wrap", "cm_noz ? (sLeanS && (!sUseAlpha || (sLeanA && rrtex)))", "cm_noz ? (sLeanS && (!sUseAlpha || sLeanA))"),
    # the cross-port speed round (2026-10-07): DKR_ZSKIP, DKR_SPANVEC, DKR_FASTROWS (whole file, every occurrence)
    ("xspeed zskip: the decal window", "> (int)vget_lane_u16(m4, 0) + (Z == 2 ? 0 : zbias)) { zskip = 1;", "> (int)vget_lane_u16(m4, 0) + (Z == 2 ? 0 : 0)) { zskip = 1;"),
    ("xspeed zskip: the far end", "uz1 = clamp16_sh8(zq + (span - 1) * zdq);", "uz1 = clamp16_sh8(zq + (span - 2) * zdq);"),
    ("xspeed zskip: the stored max", "vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp + span - 8))", "vmaxq_u16(vld1q_u16(zp), vld1q_u16(zp))"),
    ("xspeed zskip: the short max", "else m4 = vmax_u16(vld1_u16(zp), vld1_u16(zp + span - 4));", "else m4 = vld1_u16(zp);"),
    ("xspeed spanvec: the affine step", "ua = vget_low_f32(vextq_f32(B, B, 1)); duv = DUV;", "ua = vget_low_f32(vextq_f32(B, B, 1)); duv = vget_low_f32(DA);"),
    ("xspeed spanvec: the 1/16 step", "duv = vmul_n_f32(nuv, 1.0f / PERSP_SPAN);", "duv = vmul_n_f32(nuv, 1.0f / 15);"),
    ("xspeed spanvec: the short step", "vdup_n_f32(vget_lane_f32(nuv, 0) / fspan), 1);", "vdup_n_f32(vget_lane_f32(nuv, 1) / fspan), 1);"),
    ("xspeed spanvec: 1/w carried", "A = An; B = Bn; rw = rw_b;", "A = An; B = Bn;"),
    ("xspeed spanvec: the shade step", "vmlaq_f32(A, DA, fsv)", "vmlaq_f32(A, vrev64q_f32(DA), fsv)"),
    ("xspeed spanvec: z", "vmin_f32(vmax_f32(vdup_lane_f32(vget_low_f32(B), 0)", "vmin_f32(vmax_f32(vdup_lane_f32(vget_low_f32(B), 1)"),
    ("xspeed fastrows: the snap lanes", "const float32x4_t D3 = { w0dx, w1dx, w2dx, 0.0f };", "const float32x4_t D3 = { w0dx, w1dx, w1dx, 0.0f };"),
    ("xspeed fastrows: the right edge", "const int a = minx + 1 + ifloorf(q);", "const int a = minx + ifloorf(q);"),
    ("xspeed fastrows: the owned right end", "while (xe > xs && !own_px(T->own, xe - 1, py)) xe--;", "while (xe > xs && !own_px(T->own, xe - 2, py)) xe--;"),
    ("xspeed fastrows: the third vertex", "B = vmlaq_lane_f32(B, T->vb2, vget_high_f32(W), 0);", "B = vmlaq_lane_f32(B, T->vb2, vget_low_f32(W), 0);"),
    ("xspeed fastrows: the shade sums", "A = vmlaq_lane_f32(A, T->va1, vget_low_f32(W), 1);", "A = vmlaq_lane_f32(A, T->va0, vget_low_f32(W), 1);"),
    ("xspeed fastrows: the held sky row", "r->tri = T->skytri;", "r->tri = 0;"),
    ("xspeed fastrows: the owned start", "if (xs >= xe) continue;\n            W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs - minx)));", "if (xs >= xe) continue;\n            W = vmlaq_f32(B3, D3, vdupq_n_f32((float)(xs + 1 - minx)));"),
]
lo = src.index("/* The lean rows (2026-10-04"); hi = src.index("#undef CM_LLERP2")
import os
sel = os.environ.get("MUT")
for i, (name, a, b) in enumerate(muts):
    if sel and sel not in name: continue
    if name.startswith("xspeed"):
        assert src.count(a) >= 1, name
        m = src.replace(a, b)
    elif name.startswith("dispatch"):
        assert src.count(a) == 1, name
        m = src.replace(a, b)
    else: # only inside the lean rows
        assert src[lo:hi].count(a) >= 1, name
        m = src[:lo] + src[lo:hi].replace(a, b) + src[hi:]
    open(f"{sys.argv[2]}/m{i:02d}.c", "w").write(m)
    open(f"{sys.argv[2]}/m{i:02d}.name", "w").write(name)
PY
missed=0
for m in "$out"/m*.c; do
  b=${m%.c}; d=$b.dir; mkdir -p "$d"; cp "$m" "$d/gfx_soft.c"
  if RT_SRC="$d" sh "$here/tools/raster_test.sh" build-arm-only "$d" > "$d/build.log" 2>&1; then :; fi
  r=0
  for k in 6 2 3 4 1 0; do
    if ! qemu-arm -L "$SR" "$d/rt_arm" 3 "$N" $k > "$d/run_$k.txt" 2>&1; then r=1; break; fi
  done
  if [ $r = 1 ]; then echo "caught: $(cat $b.name) (kind $k: $(grep -m1 differs "$d/run_$k.txt" || tail -1 "$d/run_$k.txt"))"
  else echo "MISSED: $(cat $b.name)"; missed=1; fi
done
rm -rf "$out"
[ $missed = 0 ] && echo "raster_test_mut: every mutation caught" || { echo "raster_test_mut: FAIL"; exit 1; }
