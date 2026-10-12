#!/bin/bash
# Save-state check, desktop proxy or the device binary under qemu-arm: a state saved in one
# process and loaded in a fresh one, with the host heap moved (OOT_HEAPSHIFT, as the
# device's address randomisation does every boot), must carry on exactly as the process
# that saved it did.
#   A: debug boot at <entrance>, save slot 1 at loop iteration SAVE_AT, keep going and dump
#      every frame from SAVE_AT - 5 to SAVE_AT + RUN.
#   B: a fresh process, heap moved, --loadStateFile <A's state>; the kit loads it at the
#      third iteration boundary. Dump every frame for RUN + 10 iterations.
#   Then B's frames after the load must equal A's frames after the save, pixel for pixel
#   (the offset between the two numberings is found, not assumed), and so must the audio
#   both make from there (OOT_AUDIO_SYNC's virtual DAC; hh_audio.c marks the dump at the
#   save and at the load).
#   INPUT=<OOT_INPUT script> is played from the state point on in both runs (one input read
#   per loop iteration: A's script idles SAVE_AT reads first, B's 2). PRE=<script> is A's
#   input before the save instead of idling (it must add up to SAVE_AT reads), e.g. START
#   to be in the pause menu when the state is saved.
#     ./dev.sh 'cd /work/port && tools/handheld/qstate.sh <tag> [pc|handheld] [entrance] [extra env...]'
# Results in refs/qstate_<tag>/: a.log, b.log, frames/, and the verdict on stdout.
set -u
TAG=${1:?usage: qstate.sh <tag> [pc|handheld] [entrance] [VAR=value ...]}
PLAT=${2:-pc}
ENT=${3:-0xEE}
shift $(( $# < 3 ? $# : 3 ))
SAVE_AT=${SAVE_AT:-150}
RUN=${RUN:-90}
OUT=/work/refs/qstate_$TAG
rm -rf "$OUT" /tmp/qs_$TAG; mkdir -p "$OUT" /tmp/qs_$TAG/a /tmp/qs_$TAG/b /tmp/qs_$TAG/fa /tmp/qs_$TAG/fb
W=/tmp/qs_$TAG
BD=/work/port/build/hh-$PLAT
if [ "$PLAT" = handheld ]; then
    SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
    command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
        RUNBIN="qemu-arm -L $SR $BD/oot"
    TMO=3000
else
    export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    RUNBIN="$BD/oot"
    TMO=600
fi
mkdir -p $W/assets && ln -sfn /work/port/build/hh-gen/data $W/assets/data && cp $BD/embedded.bin $W/assets/
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_W=240 OOT_H=240 OOT_ASSETS=$W/assets OOT_SEED=1234 OOT_AUDIO_SYNC=1 OOT_QUIET=1
INPUT=${INPUT:-}
PRE=${PRE:-$SAVE_AT:}
echo "=== A: $PLAT, entrance $ENT, save at $SAVE_AT, frames to $((SAVE_AT + RUN)) ${INPUT:+input $INPUT} $* ==="
env "$@" OOT_DATA=$W/a OOT_ENTRANCE=$ENT OOT_SAVE_AT=$SAVE_AT OOT_EXIT_AT=$((SAVE_AT + RUN)) \
    OOT_INPUT=$PRE,${INPUT:-1:} OOT_AUDIO_DUMP=$W/a.pcm \
    OOT_DUMP=$W/fa OOT_DUMP_FROM=$((SAVE_AT - 5)) OOT_DUMP_EVERY=1 \
    timeout -s KILL $TMO $RUNBIN > $OUT/a.log 2>&1; echo "A exit $?"
grep -E "savestate" $OUT/a.log
test -f $W/a/state_01.sav || { echo "FAIL: A wrote no state"; exit 1; }
ls -l $W/a/state_01.sav | awk '{print "state: " $5 " bytes"}'
echo "=== B: a fresh process, heap moved, --loadStateFile ==="
env "$@" OOT_DATA=$W/b OOT_HEAPSHIFT=${HEAPSHIFT:-40961} OOT_EXIT_AT=$((RUN + 10)) \
    ${INPUT:+OOT_INPUT=2:,$INPUT} OOT_AUDIO_DUMP=$W/b.pcm \
    OOT_DUMP=$W/fb OOT_DUMP_FROM=0 OOT_DUMP_EVERY=1 \
    timeout -s KILL $TMO $RUNBIN --loadStateFile $W/a/state_01.sav > $OUT/b.log 2>&1; echo "B exit $?"
grep -E "savestate" $OUT/b.log
python3 - $W/fa $W/fb $OUT $SAVE_AT $W <<'PY'
import os, sys, shutil, re
fa, fb, out, save_at, w = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
def frames(d):
    r = {}
    for f in os.listdir(d):
        if f.endswith('.ppm'):
            r[int(f.split('_')[1].split('.')[0])] = os.path.join(d, f)
    return r
A, B = frames(fa), frames(fb)
def px(p):
    d = open(p, 'rb').read()
    return d.split(b'\n', 3)[3]
if not A or not B:
    print('FAIL: no frames (A %d, B %d)' % (len(A), len(B))); sys.exit(1)
# the offset: B frame b <-> A frame b + off; try every one that lines up B's early frames
# with A's frames from the save on, and keep the one with the most exact matches
best = None
for off in range(save_at - 20, save_at + 10):
    pairs = [(b, b + off) for b in sorted(B) if b + off in A and b + off >= save_at]
    if len(pairs) < 10:
        continue
    same = sum(1 for b, a in pairs if px(B[b]) == px(A[a]))
    if best is None or same > best[1]:
        best = (off, same, pairs)
if best is None:
    print('FAIL: the frame windows do not overlap'); sys.exit(1)
off, same, pairs = best
print('offset %d (A frame = B frame + %d): %d of %d frames identical, %d distinct pictures among them'
      % (off, off, same, len(pairs), len(set(px(A[a]) for b, a in pairs))))
# the audio from the mark on (the save in A, the load in B)
def mark(log):
    m = re.findall(r'AUDIO savestate mark (\d+)', open(log, errors='replace').read())
    return int(m[-1]) if m else None
ma, mb = mark(os.path.join(out, 'a.log')), mark(os.path.join(out, 'b.log'))
audio_ok = False
if ma is None or mb is None:
    print('audio: no savestate mark (A %s, B %s)' % (ma, mb))
else:
    da = open(os.path.join(w, 'a.pcm'), 'rb').read()[ma:]
    db = open(os.path.join(w, 'b.pcm'), 'rb').read()[mb:]
    n = min(len(da), len(db))
    first = next((i for i in range(n) if da[i] != db[i]), None)
    loud = sum(1 for i in range(0, n - 1, 2) if abs(int.from_bytes(da[i:i+2], 'little', signed=True)) > 256)
    if first is None:
        audio_ok = n > 0
        print('audio: %.2f s after the mark identical (%d of %d samples above -40 dB)' % (n / 128000.0, loud, n // 2))
    else:
        print('audio: differs from byte %d (%.3f s after the mark) of %d' % (first, first / 128000.0, n))
bad = []
for b, a in pairs:
    pa, pb = px(A[a]), px(B[b])
    if pa != pb:
        n = sum(1 for i in range(0, min(len(pa), len(pb)), 3) if pa[i:i+3] != pb[i:i+3])
        bad.append((a, b, n))
for a, b, n in bad[:12]:
    print('  A %d / B %d: %d px differ' % (a, b, n))
os.makedirs(os.path.join(out, 'frames'), exist_ok=True)
keep = [pairs[0], pairs[len(pairs) // 2], pairs[-1]] + [(b, a) for a, b, _ in bad[:3]]
for b, a in keep:
    shutil.copy(A[a], os.path.join(out, 'frames', 'A_%04d.ppm' % a))
    shutil.copy(B[b], os.path.join(out, 'frames', 'B_%04d.ppm' % b))
print('PASS' if not bad and audio_ok else 'FAIL: %d frames differ%s' % (len(bad), '' if audio_ok else ', audio'))
PY
