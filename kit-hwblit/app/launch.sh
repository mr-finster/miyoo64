#!/bin/sh
# App/HwblitTest/launch.sh: the 2D engine check (kit-hwblit/hwblit_test.c). From Onion's Apps, or over SSH:
#   sh /mnt/SDCARD/App/HwblitTest/launch.sh
# A test picture shows for about 10 seconds; the results are written to result.txt in this folder.
DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR" || exit 1
{ echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor; } 2>/dev/null
chmod +x ./hwblit_test 2>/dev/null
./hwblit_test > result.txt 2>&1
echo "exit $?" >> result.txt
sync
