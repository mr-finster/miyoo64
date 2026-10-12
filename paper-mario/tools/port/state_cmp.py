#!/usr/bin/env python3
"""state_cmp.py <run A frames> <run B frames> [tail]: tools/port/state_test.sh's check. Run B's last <tail> frames
(default 100) must equal a run of consecutive frames of A byte for byte (B loaded the state A saved, so B's frames after
the load are A's after the save, shifted). Prints the offset and the first differing frame. Exit 0 = equal."""
import glob, os, sys
a = sorted(glob.glob(os.path.join(sys.argv[1], 'frame_*.ppm')))
b = sorted(glob.glob(os.path.join(sys.argv[2], 'frame_*.ppm')))
tail = int(sys.argv[3]) if len(sys.argv) > 3 else 100
if len(b) < tail or not a: sys.exit(f'state_cmp: too few frames (A {len(a)}, B {len(b)})')
A = [open(f, 'rb').read() for f in a]; B = [open(f, 'rb').read() for f in b[-tail:]]
starts = [i for i, x in enumerate(A) if x == B[0]]
best = None
for s in starts:
    n = 0
    while n < tail and s + n < len(A) and A[s + n] == B[n]: n += 1
    if best is None or n > best[1]: best = (s, n)
if best is None: sys.exit(f'state_cmp: B\'s frame {len(b) - tail} is nowhere in A')
s, n = best
print(f'state_cmp: B {os.path.basename(b[-tail])}.. = A {os.path.basename(a[s])}..: {n} of {tail} frames equal')
if n < tail: print(f'  first difference: B {os.path.basename(b[len(b) - tail + n])} vs A {os.path.basename(a[s + n]) if s + n < len(A) else "(A ended)"}')
sys.exit(0 if n == tail else 1)
