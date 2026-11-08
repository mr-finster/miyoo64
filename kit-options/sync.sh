#!/usr/bin/env bash
# kit-options/sync.sh: copy the shared OPTIONS page (miyoo_options.c/.h, the canonical copy here) next to the
# miyoo_menu.c of every port that has the page, each port building on its own. --check: only report copies
# that differ (exit 1). A port adopting the page adds its kit folder here (docs/OPTIONS.md).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIRS="ocarina-of-time/port/kit/platform majoras-mask/hh/kit/platform"
bad=0
for d in $DIRS; do
  for f in miyoo_options.c miyoo_options.h; do
    if [ "${1:-}" = --check ]; then
      cmp -s "$ROOT/kit-options/$f" "$ROOT/$d/$f" || { echo "differs: $d/$f"; bad=1; }
    else
      cp "$ROOT/kit-options/$f" "$ROOT/$d/$f"
    fi
  done
done
exit $bad
