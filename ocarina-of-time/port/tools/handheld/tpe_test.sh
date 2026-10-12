#!/bin/sh
# Build tools/handheld/tpe_test.c (gfx_soft_eval_texels: OOT_TPEFAST against the old loop; see the file's header) for ARM
# (qemu-arm) and x86 -m32 and run both; then the mutation runs (each perturbs the new loop in a copy of gfx_soft.c; every
# one must be caught). 2026-10-02.
#   sh tools/handheld/tpe_test.sh [calls]   (HH_SDK=<Miyoo toolchain> picks the device compiler; else the distro's gcc)
# Exit 0 when the outputs matched on both, the ARM and x86 hashes agree and every mutation was caught.
set -e
P=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-20000}
O=${TT_OUT:-/tmp/tpe_test}; mkdir -p "$O"
FLAGS="-std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-math-errno -ffp-contract=off -w \
 -include $P/src/port/hh/hh_compat.h -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -DDEBUG_FEATURES=0 -DOOT_VERSION=NTSC_1_0 \
 -DOOT_REVISION=0 -DOOT_REGION=REGION_US -DLIBULTRA_VERSION=LIBULTRA_VERSION_L -DLIBULTRA_PATCH=0 -DPLATFORM_PSP=1 \
 -DPLATFORM_N64=0 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE \
 -I$P/src/port/hh/gfx -I$P/kit/platform -iquote $P/include -iquote $P/include/libc -iquote $P/src -iquote $P \
 -I$P/src/port/hh/shim -I$P/src/port/hh"
ARMF="-O2 -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -DTARGET_HANDHELD=1 -DHANDHELD=1"
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
SDK=${MIYOO_TOOLCHAIN:-/opt/miyoomini-toolchain}
SR=$SDK/usr/arm-linux-gnueabihf/sysroot
if [ -x $SDK/usr/bin/arm-linux-gnueabihf-gcc ]; then
        CC="$SDK/usr/bin/arm-linux-gnueabihf-gcc"; ST=; QEMU="qemu-arm -L $SR"
else
    CC=arm-linux-gnueabihf-gcc; ST=-static; QEMU=qemu-arm
fi
$CC $ARMF $FLAGS $ST -o $O/tt_arm $P/tools/handheld/tpe_test.c -lm
gcc -O1 -m32 -fno-pie -no-pie -malign-data=abi -msse2 -mfpmath=sse -DTARGET_HANDHELD=1 -DTARGET_PC=1 $FLAGS \
    -o $O/tt_x86 $P/tools/handheld/tpe_test.c -lm
fail=0
a=$($QEMU $O/tt_arm 7 $N | tail -1) || fail=1
x=$($O/tt_x86 7 $N | tail -1) || fail=1
echo "ARM: $a"; echo "x86: $x"
# (the "|| fail" above sees tail's status, not the test's: check the count itself, 2026-10-02)
for r in "$a" "$x"; do case "$r" in *" 0 texels differ"*) ;; *) echo "  texels differ (or no result)"; fail=1 ;; esac; done
[ "${a##*hash }" = "${x##*hash }" ] || { echo "  ARM and x86 hashes differ"; fail=1; }
# mutations: each must make the runs differ (ARM build, stopping at the first differing call): the operand sources (shared), then the
# NEON loop's (the device path; x86 runs the scalar fast loop, compared above)
i=0
while read -r from to; do
    i=$((i + 1)); M=$O/mut$i; mkdir -p $M
    python3 - "$P/src/port/hh/gfx/gfx_soft.c" "$M/gfx_soft.c" "$from" "$to" <<'PY' || { echo "mut $i: text gone"; fail=1; continue; }
import sys
s = open(sys.argv[1]).read(); a = sys.argv[3].replace('~', ' '); b = sys.argv[4].replace('~', ' ')
if s.count(a) != 1: sys.exit(1)
open(sys.argv[2], 'w').write(s.replace(a, b))
PY
    $CC $ARMF -iquote $M $FLAGS $ST -o $M/tt_arm $P/tools/handheld/tpe_test.c -lm
    if TT_FIRST=1 $QEMU $M/tt_arm 9 $N >$M/out.txt 2>&1; then echo "mut $i ($from): NOT caught"; fail=1; else echo "mut $i ($from): caught"; fi
done <<'MUTS'
case~G_CCMUX_PRIMITIVE:~*kv~=~k[ch];~return~4~+~slot; case~G_CCMUX_PRIMITIVE:~*kv~=~k[ch~+~1];~return~4~+~slot;
case~G_CCMUX_ENVIRONMENT:~*kv~=~k[4~+~ch];~return~4~+~slot; case~G_CCMUX_ENVIRONMENT:~*kv~=~k[4];~return~4~+~slot;
case~G_CCMUX_TEXEL1_ALPHA:~return~3; case~G_CCMUX_TEXEL1_ALPHA:~return~2;
case~G_CCMUX_PRIM_LOD_FRAC:~*kv~=~k[8]; case~G_CCMUX_PRIM_LOD_FRAC:~*kv~=~k[3];
*kv~=~((slot~==~0~||~slot~==~3)~&&~mux~==~G_CCMUX_1)~?~255~:~0; *kv~=~(slot~==~0~&&~mux~==~G_CCMUX_1)~?~255~:~0;
case~G_ACMUX_1:~*kv~=~slot~==~2~?~k[8]~:~255; case~G_ACMUX_1:~*kv~=~255;
case~G_ACMUX_TEXEL1:~return~3; case~G_ACMUX_TEXEL1:~return~2;
sx~=~sl~==~3~?~0~:~4~+~sl; sx~=~sl~==~3~?~1~:~4~+~sl;
vbslq_s32(vcltq_s32(pr,~vdupq_n_s32(0)),~vnegq_s32(q),~q) vbslq_s32(vcltq_s32(pr,~vdupq_n_s32(0)),~q,~q)
m8081),~23)); m8081),~22));
vqmovn_u16(vcombine_u16(vqmovun_s32(v), vmovn_u16(vcombine_u16(vqmovun_s32(v),
vcreate_u8(((uint64_t)p1~<<~32)~|~p0); vcreate_u8(((uint64_t)p0~<<~32)~|~p0);
if~(++x0~==~w0)~x0~=~0; if~(++x0~>~w0)~x0~=~0;
(is16_1~?~texel_at(px1,~true,~r1~+~col1[x]) (is16_1~?~texel_at(px1,~false,~r1~+~col1[x])
is16_0~?~texel_at(px0,~true,~r0~+~x0) is16_0~?~texel_at(px0,~false,~r0~+~x0)
tab[8~+~4~*~sl~+~lane]~=~(uint8_t)val[ch][4~+~sl]; tab[8~+~4~*~sl~+~lane]~=~(uint8_t)val[ch][4];
static~const~uint8_t~kLaneCh[4]~=~{~2,~1,~0,~3~}; static~const~uint8_t~kLaneCh[4]~=~{~0,~1,~2,~3~};
if~(++x0~>=~w0)~x0~=~0; if~(++x0~>~w0)~x0~=~0;
sTpeRow0[i]~=~texel_at(px0,~true,~r0~+~i); sTpeRow0[i]~=~texel_at(px0,~true,~r0~+~(i^1));
sTpeRow1[i]~=~texel_at(px1,~true,~r1~+~i); sTpeRow1[i]~=~texel_at(px1,~false,~r1~+~i);
const~int~n0~=~W~<~w0~?~W~:~w0; const~int~n0~=~W~<~w0~?~W~-~1~:~w0;
const~int~w1~=~t1->w;~for const~int~w1~=~t1->w~-~1;~for
p1~=~q1[c1~!=~NULL~?~c1[x]~:~0]; p1~=~q1[c1~!=~NULL~?~c1[x]~^~1~:~0];
(const~uint32_t~*)px1~+~r1~:~sTpeRow1; (const~uint32_t~*)px1~+~r1~+~1~:~sTpeRow1;
P~=~(uint8_t)(swap_rgb~?~4~+~lane~:~lane); P~=~(uint8_t)(swap_rgb~?~lane~:~lane);
K~=~!lerp_a~?~12~:~ka_src~==~1~?~3~:~ka_src~==~2~?~7~:~11; K~=~!lerp_a~?~12~:~ka_src~==~1~?~7~:~ka_src~==~2~?~7~:~11;
K~=~!lerp_rgb~?~12~:~!krgb_src~?~(uint8_t)(8~+~lane)~:~krgb_src~==~1~?~3~:~7; K~=~!lerp_rgb~?~12~:~!krgb_src~?~(uint8_t)(10~-~lane)~:~krgb_src~==~1~?~3~:~7;
tab[11]~=~ka; tab[11]~=~ka~^~1;
vbslq_s32(vcgtq_s32(vdupq_n_s32(0),~pr),~vnegq_s32(q),~q) vbslq_s32(vcgtq_s32(vdupq_n_s32(0),~pr),~q,~q)
vreinterpret_u8_u64(vcreate_u64(((uint64_t)p1~<<~32)~|~p0)); vreinterpret_u8_u64(vcreate_u64(((uint64_t)p0~<<~32)~|~p1));
vdupq_n_u32(0x8081)),~23)); vdupq_n_u32(0x8081)),~24));
n1~=~t1->w; n1~=~t1->w~-~1;
blend_n_texel(&bn,~sBRow0[sCol0[x]],~sBRow1[sCol1[x]]); blend_n_texel(&bn,~sBRow0[sCol0[x]],~sBRow1[sCol1[x]~^~1]);
orow[x]~=~blend_n_texel(&bn,~sBRow0[sC0[x]],~hh_lerp_argb(sRow1[sCa[x]],~sRow1[sCb[x]],~sCw[x])); orow[x]~=~blend_n_texel(&bn,~sBRow0[sC0[x]],~hh_lerp_argb(sRow1[sCa[x]],~sRow1[sCa[x]],~sCw[x]));
MUTS
[ $fail = 0 ] && echo "tpe_test: PASS" || { echo "tpe_test: FAIL"; exit 1; }
