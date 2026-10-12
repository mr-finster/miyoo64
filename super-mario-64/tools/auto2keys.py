#!/usr/bin/env python3
"""The SM64_AUTO autopilot's pad as a pad script for the emulator (M64REF_KEYS of
the emulator reference script (m64ref.sh)), so a sweep frame can be checked against the real game at
the same instant.

usage: auto2keys.py <mario_frame> <last_frame> [--mode 1|2]
  mario_frame: the frame Mario appeared, from the run's log ("autopilot: Mario appeared
  at frame N"); last_frame: the last frame to script. Prints the key string.

It replays src/hh/controller_sdl.c's autopilot: START held 4 frames of every 60 until
Mario exists; then, for SM64_AUTO=2, from 60 frames in, the stick at 80 turning a
quarter every 90 frames (up, right, down, left) and A for 3 frames of every 45. Polls
count from 1: the pad read in frame f is poll f + 1.

The level: SM64_LEVEL=N preselects the debug level select; in the emulator the same is
GameShark "A032D58C 0001;A032DDF9 00NN" (the level select on, gCurrLevelNum's low byte
forced to N every frame; N in hex).
"""
import argparse

ap = argparse.ArgumentParser()
ap.add_argument('mario_frame', type=int)
ap.add_argument('last_frame', type=int)
ap.add_argument('--mode', type=int, default=2)
a = ap.parse_args()

DIRS = ['up', 'right', 'down', 'left']   # dir[] = {0,80} {80,0} {0,-80} {-80,0}
held = {'start': False, 'a': False}
stick = None
ev = []
for f in range(0, a.last_frame + 1):
    poll = f + 1
    want = {'start': False, 'a': False}
    want_stick = None
    if f < a.mario_frame:
        want['start'] = (f % 60) < 4
    elif a.mode == 2:
        n = f - a.mario_frame + 1          # sInLevel after this frame's increment
        if n > 60:
            want_stick = DIRS[(n // 90) % 4]
            want['a'] = (n % 45) < 3
    for k in ('start', 'a'):
        if want[k] != held[k]:
            ev.append('%d:%s%s' % (poll, k, '+' if want[k] else '-'))
            held[k] = want[k]
    if want_stick != stick:
        if stick: ev.append('%d:%s-' % (poll, stick))
        if want_stick: ev.append('%d:%s+' % (poll, want_stick))
        stick = want_stick
print(','.join(ev))
