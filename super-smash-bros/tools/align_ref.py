#!/usr/bin/env python3
"""align_ref.py REFDIR PORTDIR OFFSET SPAN [REF_FRAMES...]
Finds, for each reference frame (m64ref 640x480 PNG, fNNNNNN.png), the port frame (square PPM,
frame_NNNNNN.ppm) nearest in picture: the reference box-filtered to 240x180 (the port's uniform
0.75 of 320x240) against the port's rows 30..209, over port frames REF - OFFSET +- SPAN.
Round 14: the 1P "DK VS Metal Mario" run (SSB64_START_SCENE=52 SSB64_SPGAME_STAGE=10
SSB64_SPGAME_FKIND=2) is the reference's pad script minus 460 frames (the N64 logo and menus).
Prints ref, best port frame, mean abs difference per channel, and the runner-up."""
import os, sys
from PIL import Image

refdir, portdir, off, span = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
refs = [int(a) for a in sys.argv[5:]] or sorted(int(f[1:7]) for f in os.listdir(refdir) if f.startswith('f') and f.endswith('.png'))
ports = sorted(int(f[6:12]) for f in os.listdir(portdir) if f.startswith('frame_') and f.endswith('.ppm'))
cache = {}


def port(fr):
    if fr not in cache:
        cache[fr] = list(Image.open(os.path.join(portdir, 'frame_%06d.ppm' % fr)).convert('RGB').crop((0, 30, 240, 210)).getdata())
    return cache[fr]


for r in refs:
    rb = list(Image.open(os.path.join(refdir, 'f%06d.png' % r)).convert('RGB').resize((240, 180), Image.BOX).getdata())
    scores = []
    for fr in ports:
        if abs(fr - (r - off)) > span:
            continue
        pp = port(fr)
        d = sum(abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) for a, b in zip(rb, pp)) / (3.0 * len(rb))
        scores.append((d, fr))
    scores.sort()
    if scores:
        nxt = scores[1] if len(scores) > 1 else (-1, -1)
        print('ref %5d -> port %5d (diff %.1f), next %5d (%.1f)' % (r, scores[0][1], scores[0][0], nxt[1], nxt[0]))
