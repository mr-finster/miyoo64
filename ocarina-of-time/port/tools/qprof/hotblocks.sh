#!/bin/sh
# Hottest translation blocks of one function from a qprof run (in the container):
#   FN=<function> sh hotblocks.sh <tag> <builddir> [from] [to] [n]
# e.g. FN=raster_tri.constprop.0 sh hotblocks.sh kokiri_v7 build/qp_v7 60 120 25
# Per block: instructions executed in frames from..to, share of the function,
# block length, times executed, and the source line of its first instruction.
O=/work/refs/qprof_$1; BIN=/work/port/$2/oot; F1=${3:-60}; F2=${4:-120}; N=${5:-20}
FN=${FN:-raster_tri.constprop.0}
X=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf
lo=$(awk -v FN=$FN '$3==FN{print $1}' $O/syms.txt)
hi=$(awk -v FN=$FN 'found{print $1; exit} $3==FN{found=1}' $O/syms.txt)
[ -n "$lo" ] || { echo "no symbol $FN"; exit 1; }
$X-objdump -d --no-show-raw-insn --start-address=0x$lo --stop-address=0x$hi $BIN > /tmp/hb.dis
python3 - $O $F1 $F2 $N $lo $hi $BIN $X <<'PY'
import re, subprocess, sys
O, F1, F2, N, lo, hi, BIN, X = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5], 16), int(sys.argv[6], 16), sys.argv[7], sys.argv[8]
def load(p):
    d = {}
    for l in open(p):
        k, v = l.split()
        if k == "total":
            continue
        a = int(k, 16)
        if lo <= a < hi:
            d[a] = int(v)
    return d
a, b = load(f"{O}/prof_{F1}.txt"), load(f"{O}/prof_{F2}.txt")
diff = {k: b.get(k, 0) - a.get(k, 0) for k in set(a) | set(b)}
tot = sum(diff.values()) or 1
lines = [l.rstrip() for l in open("/tmp/hb.dis") if re.match(r"^\s*[0-9a-f]+:", l)]
addr = [int(l.split(":")[0], 16) for l in lines]
def tb(start):
    i = addr.index(start) if start in addr else 0
    out = []
    while i < len(lines):
        l = lines[i]; out.append(l); ins = l.split("\t")[1] if "\t" in l else ""
        if re.match(r"^(b|bl|bx|blx|b[a-z]{2}|pop|ldm)\b", ins):
            break
        i += 1
    return out
top = sorted(diff.items(), key=lambda kv: -kv[1])[:N]
src = subprocess.run([f"{X}-addr2line", "-e", BIN, "-i"] + [hex(k) for k, _ in top], capture_output=True, text=True).stdout.split("\n")
print(f"{sys.argv[5]}..{sys.argv[6]} {hi - lo} bytes, {tot:,} insns")
acc = 0
for n, (k, v) in enumerate(top):
    acc += v; t = tb(k)
    print(f"{k:#x} {v:>13,} {100 * v / tot:5.1f}% cum {100 * acc / tot:5.1f}%  len {len(t):3d} execs {v // max(len(t), 1):>11,}  {src[n].split('/')[-1] if n < len(src) else ''}")
PY
