#!/bin/bash
# Save-state check (M7), desktop proxy or the device binary under qemu-arm (ocarina-of-time's
# tools/qstate.sh, for Smash): a state saved in one process and loaded in a fresh one must
# carry on exactly as the process that saved it did, pictures and sound.
#   A: <scene env>, free-running; save slot 1 at the top of loop iteration SAVE_AT, keep going and
#      dump every picture from number SAVE_AT / 2 on (scene loads draw fewer pictures than ticks)
#      to tick SAVE_AT + RUN, and the sound (SSB64_PCM).
#   B: a fresh process with its own save dir and B_ENV (default: a plain boot, so the state lands
#      on another scene), --loadStateFile <A's state>; the kit loads it at the third iteration
#      boundary. Dump every frame for RUN + 10 iterations.
#   Then B's frames after the load must equal A's frames after the save, pixel for pixel (the
#   offset between the two numberings is found, not assumed), and so must the sound both make
#   from there (audio_sdl.c logs the PCM dump's offset at the save and at the load).
#     ./dev.sh 'bash tools/qstate.sh <tag> desktop|arm <binary> [VAR=value ...]'
# Results in refs/qstate_<tag>/: a.log, b.log, three frame pairs (and the first differing ones),
# and the verdict on stdout. The scratch frames live in /tmp inside the container.
set -u
TAG=${1:?usage: qstate.sh <tag> desktop|arm <binary> [VAR=value ...]}
MODE=$2; BIN=$(readlink -f "$3"); shift 3
SAVE_AT=${SAVE_AT:-600}
RUN=${RUN:-120}
B_ENV=${B_ENV:-}
ROM=${SSB64_ROM:-/work/refs/ssb-decomp-re/baserom.us.z64}
OUT=/work/refs/qstate_$TAG
W=/tmp/qs_$TAG
rm -rf "$OUT" "$W"; mkdir -p "$OUT" $W/a $W/b $W/fa $W/fb
if [ "$MODE" = arm ]; then
    ARMROOT=${ARMROOT:-/opt/armroot}
    RUNBIN="qemu-arm -L $ARMROOT $BIN"
else
    RUNBIN="$BIN"
fi
export SSB64_ROM=$ROM SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SSB64_W=240 SSB64_H=240 \
       SSB64_FREERUN=1 SSB64_AUDIOAIM=0 SSB64_QUIET=1
echo "=== A: $MODE, save at $SAVE_AT, frames to $((SAVE_AT + RUN)); $* ==="
env "$@" SSB64_DATA=$W/a SSB64_SAVE_AT=$SAVE_AT SSB64_FRAMES=$((SAVE_AT + RUN)) SSB64_PCM=$W/a.pcm \
    SSB64_DUMP=$W/fa SSB64_DUMP_FROM=$((SAVE_AT / 2)) SSB64_DUMP_EVERY=1 \
    timeout -s KILL 3000 $RUNBIN > $OUT/a.log 2>&1; echo "A exit $?"
grep -E "savestate|AUDIO savestate" $OUT/a.log
test -f $W/a/state_01.sav || { echo "FAIL: A wrote no state"; exit 1; }
ls -l $W/a/state_01.sav | awk '{print "state: " $5 " bytes"}'
ln -s $W/a/romdata.bin $W/b/romdata.bin # the extract is the same file: skip a second one
echo "=== B: a fresh process ($B_ENV), --loadStateFile ==="
env $B_ENV SSB64_DATA=$W/b SSB64_FRAMES=$((RUN + 10)) SSB64_PCM=$W/b.pcm \
    SSB64_DUMP=$W/fb SSB64_DUMP_FROM=0 SSB64_DUMP_EVERY=1 \
    timeout -s KILL 3000 $RUNBIN --loadStateFile $W/a/state_01.sav > $OUT/b.log 2>&1; echo "B exit $?"
grep -E "savestate|AUDIO savestate" $OUT/b.log
python3 - $W/fa $W/fb $OUT $SAVE_AT $W <<'PY'
import os, sys, shutil, re
fa, fb, out, save_at, w = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
def frames(d):
    return {int(f.split('_')[1].split('.')[0]): os.path.join(d, f) for f in os.listdir(d) if f.endswith('.ppm')}
A, B = frames(fa), frames(fb)
def px(p):
    return open(p, 'rb').read().split(b'\n', 3)[3]
if not A or not B:
    print('FAIL: no frames (A %d, B %d)' % (len(A), len(B))); sys.exit(1)
# A's frames are numbered by pictures since its boot, B's likewise: find the offset that lines up
# B's frames after the load with A's frames after the save, keeping the one with the most matches.
best = None
for off in range(min(A) - max(B), max(A) - min(B) + 1):
    pairs = [(b, b + off) for b in sorted(B) if b >= 5 and b + off in A]  # B loads at iteration 3
    if len(pairs) < 10:
        continue
    same = sum(1 for b, a in pairs if px(B[b]) == px(A[a]))
    if best is None or same > best[1]:
        best = (off, same, pairs)
if best is None:
    print('FAIL: the frame windows do not overlap'); sys.exit(1)
off, same, pairs = best
print('offset %d: %d of %d frames identical, %d distinct pictures among them'
      % (off, same, len(pairs), len(set(px(A[a]) for b, a in pairs))))
def mark(log, kind):
    m = re.findall(r'AUDIO savestate mark (\d+) \(%s\)' % kind, open(log, errors='replace').read())
    return int(m[-1]) if m else None
ma, mb = mark(os.path.join(out, 'a.log'), 'save'), mark(os.path.join(out, 'b.log'), 'load')
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
        bad.append((a, b, sum(1 for i in range(0, min(len(pa), len(pb)), 3) if pa[i:i+3] != pb[i:i+3])))
for a, b, n in bad[:12]:
    print('  A %d / B %d: %d px differ' % (a, b, n))
os.makedirs(os.path.join(out, 'frames'), exist_ok=True)
keep = [pairs[0], pairs[len(pairs) // 2], pairs[-1]] + [(b, a) for a, b, _ in bad[:2]]
for b, a in keep:
    shutil.copy(A[a], os.path.join(out, 'frames', 'A_%04d.ppm' % a))
    shutil.copy(B[b], os.path.join(out, 'frames', 'B_%04d.ppm' % b))
print('PASS' if not bad and audio_ok else 'FAIL: %d frames differ%s' % (len(bad), '' if audio_ok else ', audio'))
PY
rm -rf "$W"
