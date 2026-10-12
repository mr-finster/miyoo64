#!/bin/bash
# Deterministic frame sweep (after ocarina-of-time's sweep/qsweep): run the desktop
# build under the virtual clock (SF64_VCLOCK=1, SF64_FAST=1), dump frames, and
# compare with another sweep directory.
#
#   sweep.sh run  <out dir> [binary] [frames] [every]   -- dumps frame_*.ppm
#   sweep.sh diff <dir a> <dir b>                       -- counts differing frames
#
# Extra environment (e.g. SF64_TILEMASK=0, SF64_FASTSPAN=0) passes through.
# Run parallel sweeps of one binary only after one has prepared the data dir. The
# data dir needs a built sf64.rom: SF64_DATA (default ./sweep_data).
set -eu
cmd=${1:?run|diff}
case "$cmd" in
run)
    out=${2:?out dir}; bin=${3:-./build/pc/sf64}; frames=${4:-2400}; every=${5:-100}
    mkdir -p "$out"
    # one short start first: a new link changes the build stamp, so the image
    # is rebuilt -- here, not by two parallel sweeps writing it at once
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SF64_EXIT_AT=2 SF64_BASEROM=${SF64_BASEROM:-./baserom.us.rev1.z64} \
        SF64_DATA=${SF64_DATA:-./sweep_data} "$bin" > "$out/prep.log" 2>&1 || true
    find "$out" -maxdepth 1 -name 'frame_*.ppm' -delete
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SF64_VCLOCK=1 SF64_FAST=1 SF64_AUTO=${SF64_AUTO:-2} \
        SF64_EXIT_AT="$frames" SF64_DUMP="$out" SF64_DUMP_FROM=${SWEEP_FROM:-100} SF64_DUMP_EVERY="$every" \
        SF64_BASEROM=${SF64_BASEROM:-./baserom.us.rev1.z64} SF64_DATA=${SF64_DATA:-./sweep_data} "$bin" > "$out/run.log" 2>&1
    echo "sweep: $(ls "$out"/frame_*.ppm | wc -l) frames in $out"
    ;;
diff)
    a=${2:?dir a}; b=${3:?dir b}; n=0; d=0
    # an empty or missing sweep must fail, not compare nothing (super-smash-bros's lesson, T11)
    ls "$a"/frame_*.ppm >/dev/null 2>&1 || { echo "sweep diff: no frames in $a"; exit 1; }
    ls "$b"/frame_*.ppm >/dev/null 2>&1 || { echo "sweep diff: no frames in $b"; exit 1; }
    for f in "$a"/frame_*.ppm; do
        g="$b/$(basename "$f")"; n=$((n + 1))
        if ! cmp -s "$f" "$g"; then d=$((d + 1)); echo "differs: $(basename "$f")"; fi
    done
    echo "sweep diff: $n frames, $d differ"
    [ "$d" -eq 0 ]
    ;;
esac
