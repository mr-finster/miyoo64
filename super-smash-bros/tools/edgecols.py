#!/usr/bin/env python3
"""edgecols.py DIR [DIR ...] -- the picture's edge columns, per folder of PPM frames.

For columns 6-8 and 231-233 it averages, over the folder's frames, how many of rows
10..229 carry content (not black). Smash's scissor is N64 (10,10)-(310,230), which is
7.5..232.5 on the 240-wide screen. Under pixel centres columns 7..231 are inside, so
column 7 should be as full as column 8 and column 232 as empty as 233. Round 13: with
2D rectangles rounded half up, column 7 had content in only 9-157 of the 220 rows
(the triangles reach it, the backdrop and the clear don't). seamscan can't see this:
Smash's black borders keep it off the screen edge."""
import os, sys

COLS = (6, 7, 8, 231, 232, 233)


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, _ = fields
    return w, h, data[i + 1:i + 1 + w * h * 3]


for d in sys.argv[1:]:
    frames = sorted(f for f in os.listdir(d) if f.endswith('.ppm'))
    tot = dict((c, 0) for c in COLS)
    for f in frames:
        w, h, px = read_ppm(os.path.join(d, f))
        for c in COLS:
            tot[c] += sum(1 for y in range(10, 230) if sum(px[(y * w + c) * 3:(y * w + c) * 3 + 3]) > 30)
    n = max(len(frames), 1)
    print('%-28s %3d frames  ' % (d, len(frames)) + '  '.join('c%d %5.1f' % (c, tot[c] / n) for c in COLS))
