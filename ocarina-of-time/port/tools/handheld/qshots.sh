#!/bin/sh
# Frames from the DEVICE binary under qemu-arm (what the handheld draws), for
# screenshots: debug boot at an entrance and time of day, PPM dumps every N
# frames into refs/shots_<tag>/<name>/.
#   ./dev.sh 'sh /work/port/tools/handheld/qshots.sh <tag> <builddir> <name>:<entrance>[:<daytime>] ...'
# e.g. kokiri:0xEE:0x8000 field:0xCD:0x6000 ; FROM/EVERY/TO override the frame window.
set -u
TAG=$1; BD=/work/port/$2; shift 2
FROM=${FROM:-150}; EVERY=${EVERY:-30}; TO=${TO:-271}
SR=/opt/miyoomini-toolchain/usr/arm-linux-gnueabihf/sysroot
command -v qemu-arm >/dev/null || { apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq qemu-user >/dev/null 2>&1; }
OUT=/work/refs/shots_$TAG; mkdir -p $OUT
for spec in "$@"; do
    name=${spec%%:*}; rest=${spec#*:}; ent=${rest%%:*}; day=${rest#*:}; [ "$day" = "$rest" ] && day=
    A=/tmp/qshot_$name; rm -rf $A; mkdir -p $A/od $OUT/$name
    ln -s /work/port/build/hh-gen/data $A/data; cp $BD/embedded.bin $A/
    ( env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_ASSETS=$A OOT_DATA=$A/od OOT_SEED=1234 OOT_AUDIO_SYNC=1 \
        OOT_ENTRANCE=$ent ${day:+OOT_DAYTIME=$day} OOT_EXIT_AT=$TO OOT_DUMP_FROM=$FROM OOT_DUMP_EVERY=$EVERY OOT_DUMP=$OUT/$name \
        timeout -s KILL 3000 qemu-arm -L $SR $BD/oot > $OUT/$name.log 2>&1; echo "$name ($ent ${day:-night}): exit $?" ) &
done
wait
ls $OUT/*/ | grep -c ppm
