#!/usr/bin/env python3
"""bestmatch.py -- same-instant pairs of the port and mupen64plus (T13, after MK64's
bx/env/bestmatch.py). Run the port with FZX_PKEYS set to the M64REF_KEYS pad script that
m64ref.sh played, and FZX_DUMP_POLLS around the polls of the reference shots: both then get
the same input at the same controller read. For each reference shot this finds the port poll
whose picture matches it best over the 3D view (the HUD masked), since the two can still be
a few polls apart in a race.

  bestmatch.py <m64ref run.log> <refdir> <portdir> [poll ...] [--win N] [--shift S]
    run.log  m64ref.sh's log: its shot lines give each frame's controller-poll count
    refdir   the z64 frames (f<frame>.png, 320x240; box-filtered to the port's square frames)
    portdir  the port's poll_<p>.ppm dumps
    poll     only these reference polls (default: every shot in run.log)
    --shift  search around poll + S. On the autopilot's route (Mute City GP) the port's race
             starts 40 polls after the N64's (the N64 runs some screens below 60 fps and polls
             less on the way), and the N64 acts on a read 3 polls later than the port. So a
             race script keeps its menu keys, moves its race keys by +43, and pairs with
             --shift 40 (round 11: 95 of 107 shots paired at exactly +40).
  prints: frame, reference poll, best port poll, offset, mean abs error over the mask
Fails loudly when nothing is compared. PIL only.
"""
import os, re, sys
from PIL import Image, ImageChops, ImageStat, ImageDraw

args = [a for a in sys.argv[1:]]
win, shift = 12, 0
if '--win' in args:
    i = args.index('--win'); win = int(args[i + 1]); del args[i:i + 2]
if '--shift' in args:
    i = args.index('--shift'); shift = int(args[i + 1]); del args[i:i + 2]
if len(args) < 3:
    sys.exit(__doc__)
runlog, refdir, portdir = args[:3]
only = set(int(a) for a in args[3:])

# m64shot's shot lines: "m64shot: f000520.png poll=523"
shots = []
for l in open(runlog, errors='replace'):
    m = re.search(r'm64shot: f(\d+)\.png poll=(\d+)', l)
    if m:
        shots.append((int(m.group(1)), int(m.group(2))))
if not shots:
    sys.exit('bestmatch: no shot lines with a poll count in ' + runlog)

# The HUD on the port's square picture (the N64 layout squeezed to 240 wide): the rival
# portraits down the left, the lap/rank/time/energy rows along the top, the minimap and the
# speed at the lower right.
mask = Image.new('L', (240, 240), 255)
d = ImageDraw.Draw(mask)
d.rectangle((0, 0, 239, 44), fill=0)
d.rectangle((0, 0, 38, 239), fill=0)
d.rectangle((168, 140, 239, 239), fill=0)


def portframe(q):
    for ext in ('ppm', 'png'):
        p = os.path.join(portdir, 'poll_%06d.%s' % (q, ext))
        if os.path.exists(p):
            return Image.open(p).convert('RGB')
    return None


n = 0
for frame, poll in shots:
    if only and poll not in only:
        continue
    rp = os.path.join(refdir, 'f%06d.png' % frame)
    if not os.path.exists(rp):
        continue
    ref = Image.open(rp).convert('RGB')
    if ref.size == (320, 240):
        ref = ref.resize((240, 240), Image.BOX)
    best = None
    for q in range(poll + shift - win, poll + shift + win + 1):
        t = portframe(q)
        if t is None:
            continue
        v = sum(ImageStat.Stat(ImageChops.difference(ref, t), mask).mean) / 3
        if best is None or v < best[0]:
            best = (v, q)
    if best is None:
        print('f%06d poll %d: no port dump within %+d +-%d' % (frame, poll, shift, win))
        continue
    print('f%06d poll %d -> port poll %d (%+d), mae %.2f' % (frame, poll, best[1], best[1] - poll, best[0]))
    n += 1
if n == 0:
    sys.exit('bestmatch: nothing compared')
