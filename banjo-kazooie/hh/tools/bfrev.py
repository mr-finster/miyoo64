# bfrev.py <file> <start_line> <end_line>: in lines [start, end] (1-based), every run of bit-fields is given a
# little-endian twin (#ifdef TARGET_HANDHELD): each storage unit's fields in reverse order, with a leading pad when the
# unit is not full, so a unit swapped to host order gives each field its big-endian (N64) value.
import re, sys
fn, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
L = open(fn).read().split('\n')
BF = re.compile(r'^(\s*)((?:unsigned |signed )?(?:u8|s8|u16|s16|u32|s32|int|unsigned int|unsigned|signed int|u64|s64))\s+(\w*)\s*:\s*(\d+)\s*;(.*)$')
SZ = {'u8':8,'s8':8,'u16':16,'s16':16,'u32':32,'s32':32,'int':32,'unsigned int':32,'unsigned':32,'signed int':32,'u64':64,'s64':64}
out = L[:a-1]
i = a - 1
while i < b:
    m = BF.match(L[i])
    if not m:
        out.append(L[i]); i += 1; continue
    run = []
    while i < b and BF.match(L[i]):
        run.append((L[i], BF.match(L[i]))); i += 1
    # group into units
    units = []; cur = []; used = 0; ty = None
    for line, mm in run:
        t = mm.group(2); w = int(mm.group(4)); size = SZ[t]
        if cur and (SZ[ty] != size or used + w > size):
            units.append((ty, cur, used)); cur = []; used = 0
        ty = t if not cur else ty
        cur.append((line, mm)); used += w
        if used == size:
            units.append((ty, cur, used)); cur = []; used = 0
    if cur: units.append((ty, cur, used))
    ind = run[0][1].group(1)
    out.append('#ifdef TARGET_HANDHELD /* little-endian: each unit\'s fields reversed (hh/tools/bfrev.py) */')
    for ty, cur, used in units:
        size = SZ[ty]
        if used < size:
            out.append(f'{ind}{ty} :{size-used};')
        for line, mm in reversed(cur):
            out.append(line)
    out.append('#else')
    for line, mm in run: out.append(line)
    out.append('#endif')
out += L[b:]
open(fn, 'w').write('\n'.join(out))
