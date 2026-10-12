#!/bin/sh
# M2 soak: 15 min autopilot (logo -> menus -> GP race, then continuous
# driving), RSS sampled every 5 s.  Runs INSIDE the container via dev.sh:
#   ./dev.sh 'sh /work/soak.sh'
# Pass/fail: process alive the whole 900 s, RSS plateau (no monotonic climb),
# final RSS comfortably under 64 MB.
set -u
OUT=/work/trace
mkdir -p "$OUT"
cd "$OUT"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MK64_AUTO=1 MK64_QUIET=1 \
  timeout 900 /work/base-psp/build/pc/mk64 > "$OUT/soak.log" 2>&1 &
WRAP=$!
: > "$OUT/rss.log"
START=$(date +%s)
sleep 2
GAMEPID=$(pgrep -n mk64) # the game itself, not the timeout wrapper
while kill -0 "$WRAP" 2>/dev/null; do
    RSS=$(grep VmRSS "/proc/$GAMEPID/status" 2>/dev/null | awk '{print $2}')
    [ -n "${RSS:-}" ] && echo "$(( $(date +%s) - START )) $RSS" >> "$OUT/rss.log"
    sleep 5
done
wait "$WRAP"
EXIT=$?
echo "exit $EXIT after $(( $(date +%s) - START )) s" >> "$OUT/rss.log"
echo "=== soak done: exit $EXIT ==="
echo "RSS kB first/min/max/last:"
awk 'NR==1{first=$2} {if($2>max)max=$2; if(min==0||$2<min)min=$2; last=$2} END{print first, min, max, last}' "$OUT/rss.log"
tail -3 "$OUT/soak.log"
