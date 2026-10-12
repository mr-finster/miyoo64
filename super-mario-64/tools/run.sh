#!/usr/bin/env bash
# Headless proxy run in the container: frames + log into refs/<tag>/.
# usage: tools/run.sh <tag> <frames> [VAR=value ...]
#   e.g. tools/run.sh bob1 900 SM64_LEVEL=9
# Defaults: autopilot on, audio synced to game frames (repeatable), a square output,
# a dump every 30 frames. Extra VAR=value pairs
# override them (SM64_DUMP_EVERY=10, SM64_W=320 ...).
set -euo pipefail
tag=$1; frames=$2; shift 2
cd "$(dirname "$0")/.."
rm -rf "refs/$tag"; mkdir -p "refs/$tag"
./dev.sh "cd /work/port && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SM64_DATA=/tmp/sm64d SM64_AUTO=1 SM64_AUDIO_SYNC=1 \
  SM64_W=240 SM64_H=240 SM64_DUMP=/work/refs/$tag SM64_DUMP_EVERY=30 SM64_EXIT_AT=$frames $* \
  timeout 600 ./build/us_pc/sm64 > /work/refs/$tag/run.log 2>&1; echo exit=\$? >> /work/refs/$tag/run.log"
tail -1 "refs/$tag/run.log"
