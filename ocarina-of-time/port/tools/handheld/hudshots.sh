# hudshots.sh <tag> [env...]: the HUD/text capture set (desktop square output: kokiri, field 0xCD, title, sign 0x0300, choice 0x0018, 40-char line 0x2043), frames every 20 from 60 into refs/q2d_<tag>; round 55 (X27) before/after: OOT_PIXEXACT=0 vs default
cd /work/port; T=$1; shift
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OOT_W=240 OOT_H=240 OOT_ASSETS=build/hh-gen OOT_SEED=1234
cp build/hh-pc/embedded.bin build/hh-gen/
O=/work/refs/q2d_$T; rm -rf $O; mkdir -p $O
run() { n=$1; shift; mkdir -p $O/$n /tmp/od_$n; env OOT_DATA=/tmp/od_$n "$@" OOT_DUMP_FROM=60 OOT_DUMP_EVERY=20 OOT_DUMP=$O/$n timeout -s KILL 400 ./build/hh-pc/oot > $O/$n.log 2>&1; }
run kokiri "$@" OOT_ENTRANCE=0xEE OOT_EXIT_AT=121 &
run field "$@" OOT_ENTRANCE=0xCD OOT_DAYTIME=0x8000 OOT_EXIT_AT=141 &
run title "$@" OOT_AUTO=1 OOT_EXIT_AT=601 OOT_DUMP_FROM=200 OOT_DUMP_EVERY=100 &
run sign "$@" OOT_ENTRANCE=0xEE OOT_MSG=0x0300,40 OOT_EXIT_AT=141 &
run choice "$@" OOT_ENTRANCE=0xEE OOT_MSG=0x0018,40 OOT_EXIT_AT=141 &
run line40 "$@" OOT_ENTRANCE=0xEE OOT_MSG=0x2043,40 OOT_EXIT_AT=141 &
wait
