#!/usr/bin/env bash
# kit-hwblit/sync.sh: copy the 2D-engine present module (miyoo_hwblit.c/.h, the canonical copy here) into each game's
# kit that uses it, next to its miyoo_panel.c, each game building on its own. --check: only report copies that
# differ (exit 1). So far: Super Mario 64 (experimental, off unless asked for: hwblit=1).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIRS="super-mario-64/port/kit/platform"
bad=0
for d in $DIRS; do
  for f in miyoo_hwblit.c miyoo_hwblit.h; do
    if [ "${1:-}" = --check ]; then
      cmp -s "$ROOT/kit-hwblit/$f" "$ROOT/$d/$f" || { echo "differs: $d/$f"; bad=1; }
    else
      cp "$ROOT/kit-hwblit/$f" "$ROOT/$d/$f"
    fi
  done
done
exit $bad
