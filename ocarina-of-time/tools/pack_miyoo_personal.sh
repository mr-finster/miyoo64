#!/usr/bin/env bash
# PERSONAL TESTING ONLY: the Miyoo Mini release zip with your ROM already inside, for your own card.
# Never share its output: App/OcarinaOfTime/rom.z64 is your ROM.
#   tools/pack_miyoo_personal.sh [<your Ocarina of Time (USA 1.0) dump>] [out dir]
# The same zip as the public one (tools/build_miyoo.sh: its release gates, then
# miyoo/tools/miyoo_pack.py with --rom), so the same layout on every Miyoo OS: App/OcarinaOfTime/
# (Apps), Onion's Games entry (Emu/OCARINA, Roms/OCARINA/Ocarina of Time.oot, a custom-extension
# shortcut that takes Onion's normal game path; the old .miyoocmd entry never started), MinUI and
# Allium. Without a ROM argument, port/baseroms/ntsc-1.0/baserom.z64.
# Output: <out dir>/OcarinaOfTime-miyoo-PERSONAL.zip; unzip it onto the card's root.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -n "${2:-}" ] && export OUT_DIR="$2"
if [ -n "${1:-}" ]; then exec "$ROOT/tools/build_miyoo.sh" "$1"; fi
PERSONAL=1 exec "$ROOT/tools/build_miyoo.sh"
