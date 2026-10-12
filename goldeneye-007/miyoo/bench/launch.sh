#!/bin/sh
# The device bench on the Miyoo Mini: App/GoldenEyeBench/launch.sh. Runs the game's own bench
# (port/hh/hh_bench.c: the Dam, each variant twice, nothing to press; ~30-40 minutes) with the
# App/GoldenEye install. Results: BENCH lines in App/GoldenEye/data/console.log, samples in
# App/GoldenEye/data/bench_prof.bin. It returns to the menu when done.
export GE_BENCH=1
exec sh /mnt/SDCARD/App/GoldenEye/launch.sh
