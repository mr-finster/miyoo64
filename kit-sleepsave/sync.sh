#!/usr/bin/env bash
# kit-sleepsave/sync.sh: copy the shared sleep-save module (miyoo_sleepsave.c/.h, the canonical copy here) into every
# game's kit, next to its miyoo_clock.c, each game building on its own. --check: only report copies that differ
# (exit 1).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIRS="banjo-kazooie/hh/kit/platform diddy-kong-racing/port/sdl f-zero-x/game/src/port/miyoo goldeneye-007/port/kit/platform
harvest-moon-64/hh/kit majoras-mask/hh/kit/platform mario-kart-64/base-psp/src/port/sdl ocarina-of-time/port/kit/platform
paper-mario/kit/platform perfect-dark/port/miyoo/kit pilotwings-64/hh/kit/platform star-fox-64/port/src/port/sdl
super-mario-64/port/kit/platform super-smash-bros/port/kit/platform the-new-tetris/hh/kit/platform"
bad=0
for d in $DIRS; do
  for f in miyoo_sleepsave.c miyoo_sleepsave.h; do
    if [ "${1:-}" = --check ]; then
      cmp -s "$ROOT/kit-sleepsave/$f" "$ROOT/$d/$f" || { echo "differs: $d/$f"; bad=1; }
    else
      cp "$ROOT/kit-sleepsave/$f" "$ROOT/$d/$f"
    fi
  done
done
exit $bad
