#!/usr/bin/env python3
# sweepcmp.py <dirA> <dirB> [expected frames, default 240]: byte-compare every .ppm of two sweeps (the
# verification standard: "cmp every PPM"). Prints the differing frames per scene and a total. Fails loudly (exit 2)
# when there is nothing to compare or the sweeps don't match up: an empty or missing directory, different frame
# names, or a frame count other than expected (Smash T11: a compare that finds no frames must not say "0 differ").
# Exit 0 = identical, 1 = frames differ. Standard library only.
import sys, os, glob, collections

def die(msg):
    print('sweepcmp.py: ' + msg, file=sys.stderr)
    sys.exit(2)

if len(sys.argv) not in (3, 4): die('usage: sweepcmp.py <dirA> <dirB> [expected frames]')
A, B = sys.argv[1], sys.argv[2]
want = int(sys.argv[3]) if len(sys.argv) == 4 else 240
for d in (A, B):
    if not os.path.isdir(d): die(f'not a directory: {d}')
na = sorted(os.path.basename(f) for f in glob.glob(A + '/*.ppm'))
nb = sorted(os.path.basename(f) for f in glob.glob(B + '/*.ppm'))
if not na: die(f'no .ppm frames in {A}')
if na != nb: die(f'{len(na)} frames in {A} but {len(nb)} in {B}, or different names')
if len(na) != want: die(f'{len(na)} frames, expected {want}')
diff = collections.OrderedDict()
for n in na:
    with open(f'{A}/{n}', 'rb') as fa, open(f'{B}/{n}', 'rb') as fb:
        if fa.read() != fb.read(): diff.setdefault(n.rsplit('_', 1)[0], []).append(n)
for sc, fs in diff.items(): print(f'{sc}: {len(fs)} differ ({fs[0]}{", ..." if len(fs) > 1 else ""})')
total = sum(len(v) for v in diff.values())
print(f'{total} of {len(na)} frames differ')
sys.exit(1 if total else 0)
