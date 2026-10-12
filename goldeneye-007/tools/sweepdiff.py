#!/usr/bin/env python3
"""sweepdiff.py -- compare two tools/sweep.sh runs frame by frame (bit-exact).

    tools/sweepdiff.py <before> <after> [--list]

Prints, per scenario, how many frames differ and the first/last ones (--list: every one with
its changed-pixel count). Frames present in only one run are counted as differing. Scenarios
are the subdirectories the runs have (tools/sweep.sh: play, menu, intro; tools/levelsweep.sh:
lv<id> per mission)."""
import os
import sys


def ppm_pixels(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P6 header: magic, width, height, maxval, then one whitespace byte
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    return data[pos + 1:]


def changed_pixels(a, b):
    pa, pb = ppm_pixels(a), ppm_pixels(b)
    if len(pa) != len(pb):
        return -1
    return sum(1 for i in range(0, len(pa), 3) if pa[i:i + 3] != pb[i:i + 3])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'm2scratch', 'sweep')
    total = changed = 0
    scens = sorted(set(d for n in args for d in os.listdir(os.path.join(root, n))
                       if os.path.isdir(os.path.join(root, n, d))))
    for scen in scens:
        da, db = (os.path.join(root, n, scen) for n in args)
        if not (os.path.isdir(da) and os.path.isdir(db)):
            print('%-6s only in one run' % scen)
            continue
        names = sorted(set(os.listdir(da)) | set(os.listdir(db)))
        diffs = []
        for n in names:
            a, b = os.path.join(da, n), os.path.join(db, n)
            if not (os.path.exists(a) and os.path.exists(b)):
                diffs.append((n, -1))
                continue
            with open(a, 'rb') as fa, open(b, 'rb') as fb:
                if fa.read() == fb.read():
                    continue
            diffs.append((n, changed_pixels(a, b)))
        total += len(names)
        changed += len(diffs)
        span = ' (%s .. %s)' % (diffs[0][0], diffs[-1][0]) if diffs else ''
        print('%-6s %3d of %3d frames differ%s' % (scen, len(diffs), len(names), span))
        if '--list' in sys.argv:
            for n, px in diffs:
                print('    %s %s' % (n, 'only in one run' if px < 0 else '%d px' % px))
    print('total  %d of %d frames differ' % (changed, total))


if __name__ == '__main__':
    main()
