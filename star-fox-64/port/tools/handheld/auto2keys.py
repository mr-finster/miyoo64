#!/usr/bin/env python3
"""auto2keys.py [reads] -- the port's SF64_AUTO=2 pad as an m64ref key script (M64REF_KEYS), so the
real game in mupen64plus follows the same presses as a port run and their frames line up (the
owner's before | after | real game sheets, BUILDLOG round 10).

  M64REF_KEYS="$(python3 tools/handheld/auto2keys.py)" m64ref.sh <rom> <out> <frames> z64

The port's autopilot (controller_sdl.c) taps START and A on a 90-read cadence until the level
starts (read 875 under SF64_VCLOCK), then fires (A two reads on, two off). The N64 polls twice
per 30 fps frame and the port's read n sees the N64's poll 2n - 72 (SF64_KEYS_OFFSET), so a state
that starts at read n starts at poll 2n - 73. The real game then runs a few frames behind in the
intro (its loads take real time) and about 60 in the menus: match by content, not frame number."""
import sys
T7 = 875
reads = int(sys.argv[1]) if len(sys.argv) > 1 else 6000

def state(t):
    if t < T7:
        m = t % 90
        return {'start': m < 4, 'a': 45 <= m < 49}
    return {'start': False, 'a': (t & 3) < 2}

ev = []
prev = {'start': False, 'a': False}
for t in range(reads):
    s = state(t)
    p = max(1, 2 * (t + 1) - 73)
    for k in ('start', 'a'):
        if s[k] != prev[k]:
            ev.append((p, '%s%s' % (k, '+' if s[k] else '-')))
    prev = s
ev.sort(key=lambda e: e[0])
print(','.join('%d:%s' % e for e in ev))
