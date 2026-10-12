#!/bin/sh
# sweep.sh <outdir> [pc|arm|scalar|all] [ENV=val ...]: render all 240 m1 frames (one warm-up pass, -s 0) with
# the current builds into <outdir>/pc, <outdir>/arm (qemu) and/or <outdir>/arm_scalar (qemu, PM64_CMNEON=0).
# Default: pc and arm. The ENV=val pairs apply to every run (A/B switches). Runs inside ./dev.sh.
# Fails loudly (exit 1): a missing build, a failed run, or a run that doesn't write exactly 240 frames.
# Compare two sweeps with tools/sweepcmp.py. Sweeps hold ROM-derived pictures: keep them in refs/.
set -eu
[ $# -ge 1 ] || { echo "usage: sweep.sh <outdir> [pc|arm|scalar|all] [ENV=val ...]" >&2; exit 1; }
out=$1; shift
which=both
case "${1:-}" in pc|arm|scalar|all) which=$1; shift;; esac
for kv in "$@"; do
    case "$kv" in *=*) export "$kv";; *) echo "sweep.sh: not ENV=val: $kv" >&2; exit 1;; esac
done
export SDL_VIDEODRIVER=dummy
set -- refs/cap/m1/*.pms
[ -f "$1" ] || { echo "sweep.sh: no scenes in refs/cap/m1" >&2; exit 1; }
run() { # run <subdir> <command...>
    d="$out/$1"; shift
    rm -rf "$d"; mkdir -p "$d"
    if ! "$@" -s 0 -o "$d" -d "$d" refs/cap/m1/*.pms > "$d.log" 2>&1; then
        echo "sweep.sh: $d: the run failed; tail of $d.log:" >&2; tail -5 "$d.log" >&2; exit 1
    fi
    n=$(ls "$d" | grep -c '\.ppm$' || true)
    [ "$n" -eq 240 ] || { echo "sweep.sh: $d has $n frames, not 240" >&2; exit 1; }
    echo "sweep.sh: $d: 240 frames"
}
case "$which" in pc|both|all)
    [ -x bench/build/pc/pm64bench ] || { echo "sweep.sh: no desktop build" >&2; exit 1; }
    run pc bench/build/pc/pm64bench;; esac
case "$which" in arm|both|all|scalar)
    [ -x bench/build/miyoo/pm64bench ] || { echo "sweep.sh: no ARM build" >&2; exit 1; };; esac
case "$which" in arm|both|all)
    run arm qemu-arm -L refs/hhroot bench/build/miyoo/pm64bench;; esac
case "$which" in scalar|all)
    run arm_scalar env PM64_CMNEON=0 qemu-arm -L refs/hhroot bench/build/miyoo/pm64bench;; esac
