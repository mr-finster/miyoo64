#!/usr/bin/env python3
"""snowcheck.py -- Surface 2's near snow as numbers, with the limits the sweep holds it to (2026-10-04).

    tools/snowcheck.py <sweep dir>            (tools/sweep.sh's snow_* scenarios)
    tools/snowcheck.py --dir <frames dir>...  (just the numbers for any run)

The user's complaint (rounds 40-43, again on 2026-10-04): the snow in front of Bond waves in brightness
while walking. Per frame, the mean luma of the crop x 0-110, y 150-232 (the near snow left of the gun);
"flicker std" = the standard deviation of that mean's frame-to-frame step, "max step" its largest jump.
Camera motion moves the mean too, so a number is only compared with the same run on another build.

The scenarios (every frame from 320; the Bench 1 child's timeline: B at VI 600, then the pad):
  snow_game   Bench 1 "Surface 2, game fog" (GE_FOGPX=0, the default look); checked over its straight
              walk, frames 320-455 (VI 700-1000; the turn after it moves the mean far more than the fog)
  snow_split  Bench 1 "Surface 2, fog split" (GE_FOGPX=1), the same frames
  walk_game   the same level walked straight ahead (u from VI 700), game fog, frames 320-607
  walk_split  the same walk with the fog split

Limits: the 2026-10-04 numbers (std / max step: snow_game 2.16 / 6.4, snow_split 1.29 / 6.2, walk_game
2.68 / 11.2, walk_split 1.45 / 6.2) plus a margin under the known regressions: GE_PSHADE=0 (snow_game 4.12 /
24.3, walk_game 3.86 / 24.3) and the split's round-43 screen-position cut colour under GE_PSHADE
(GE_FOGPXSHADE=0: snow_split 2.44 / 11.7, walk_split 2.09 / 11.7). And the split must stay steadier than
the game's fog on both (it is the fix the user asked for in round 41).
Any frame missing, or a limit passed: exit 1."""
import glob
import os
import sys

import numpy as np
from PIL import Image

CROP = (0, 150, 110, 232)  # x0, y0, x1, y1
FIRST = 320
# scenario: (flicker std limit, max step limit, last frame)
LIMITS = {
    'snow_game': (2.60, 9.0, 455),
    'snow_split': (1.80, 9.0, 455),
    'walk_game': (3.10, 14.0, 10 ** 9),
    'walk_split': (1.80, 9.0, 10 ** 9),
}


def stats(d, last=10 ** 9):
    fs = sorted(f for f in glob.glob(os.path.join(d, 'frame_*.p[np][mg]'))
                if FIRST <= int(os.path.basename(f)[6:12]) <= last)
    if len(fs) < 3:
        return None
    x0, y0, x1, y1 = CROP
    m = np.array([np.asarray(Image.open(f).convert('L'), dtype=np.float32)[y0:y1, x0:x1].mean() for f in fs])
    st = np.diff(m)
    return len(fs), float(m.mean()), float(st.std()), float(np.abs(st).max())


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    if a[0] == '--dir':
        for d in a[1:]:
            s = stats(d)
            print('%-40s %s' % (d, 'no frames' if s is None else
                                'n %3d mean %6.1f flicker std %5.2f max step %5.1f' % s))
        return 0
    bad = 0
    got = {}
    for name, (lim_std, lim_step, last) in LIMITS.items():
        s = stats(os.path.join(a[0], name), last)
        if s is None:
            print('snowcheck %-11s FAIL: no frames' % name)
            bad += 1
            continue
        got[name] = s
        ok = s[2] <= lim_std and s[3] <= lim_step
        bad += not ok
        print('snowcheck %-11s %s: n %d mean %.1f flicker std %.2f (limit %.2f) max step %.1f (limit %.1f)' % (
            name, 'ok' if ok else 'FAIL', s[0], s[1], s[2], lim_std, s[3], lim_step))
    for split, game in (('snow_split', 'snow_game'), ('walk_split', 'walk_game')):
        if split in got and game in got and got[split][2] > got[game][2]:
            print('snowcheck %-11s FAIL: the fog split flickers more than the game fog (%.2f > %.2f)' % (
                split, got[split][2], got[game][2]))
            bad += 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
