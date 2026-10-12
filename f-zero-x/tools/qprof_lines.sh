#!/bin/sh
# Per-source-line attribution of one function from a tools/qprof.sh run.
#
#   sh tools/qprof_lines.sh <qprof dir> <function> [N]
#
# Recompiles gfx_soft.c with -g and the handheld flags into $OUT/gs_g.o (the code is
# identical, so offsets map 1:1 onto the linked binary), then charges each hot block
# to the OUTERMOST inlined frame -- for a SPAN_LOOP expansion that is the line of the
# macro invocation, i.e. which span variant is hot. SRC=gfx_pc: a function in gfx_pc.c
# (default gfx_soft); BUILD_DIR=: another build.
set -e
Q=$1; F=$2; N=${3:-15}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/refs}
O=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf
cd "$ROOT/game"
BD=${BUILD_DIR:-build/miyoo} # BUILD_DIR=: another build (its binary and flags)
SRC=${SRC:-gfx_soft}
CMD=$(make -f Makefile.handheld PLAT=miyoo BUILD_DIR=$BD -n -B $BD/src/port/gfx/$SRC.o | grep " -c " | tail -1)
eval "$(echo "$CMD" | sed "s| -o [^ ]*| -g -o $OUT/${SRC}_g.o|; s|^[@ ]*||")"
BIN=$BD/fzerox
S=$($O-nm $BIN | awk -v f=$F '$3==f{print $1}'); E=$($O-nm -n $BIN | awk -v f=$F 'p{print $1;exit} $3==f{p=1}')
SO=$($O-nm $OUT/${SRC}_g.o | awk -v f=$F '$3==f{print $1}')
python3 - "$Q" $S $E $SO $N "$OUT/${SRC}_g.o" "$O-addr2line" <<'PY'
import sys, subprocess, collections, glob
q,s,e,so,n,obj,a2l=sys.argv[1],int(sys.argv[2],16),int(sys.argv[3],16),int(sys.argv[4],16),int(sys.argv[5]),sys.argv[6],sys.argv[7]
def load(p):
    d={}
    for l in open(p):
        a=l.split()
        if len(a)==2 and a[0]!='total': d[int(a[0],16)]=int(a[1])
    return d
fs=sorted(glob.glob(q+'/prof_*.txt'), key=lambda p:int(p.split('_')[-1][:-4]))
a=load(fs[0]); b=load(fs[1])
rows=[(k,v-a.get(k,0)) for k,v in b.items() if s<=k<e and v-a.get(k,0)>0]
p=subprocess.run([a2l,'-i','-a','-e',obj]+[hex(k-s+so) for k,_ in rows],capture_output=True,text=True).stdout
agg=collections.Counter()
for (k,c),blk in zip(rows,p.split('0x')[1:]):
    ls=[x for x in blk.strip().split('\n')[1:] if x]
    agg[ls[-1].split('/')[-1].split(' ')[0]]+=c
tot=sum(agg.values())
for k,v in agg.most_common(n): print(f'{v:>14,} {100*v/tot:5.1f}% {k}')
PY
