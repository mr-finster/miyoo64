#!/bin/sh
# lazymix_test.sh: B16, the load-mixer thread started on the first backstop enable (DKR_LAZYMIX).
# The current audio_sdl.c against the one before it (git $BASE, default c9561ef), on ARM (handheld
# SDK gcc + its SDL 1.2, qemu-arm) and on x86 (host gcc, SDL 1.2 and SDL2). Pass: the same hash and
# 0 worker mixes everywhere, one thread fewer with the switch on, the backstop still mixing when
# enabled, the benchmark toggle leaving no thread behind, and ~200 fewer sleep syscalls a second.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDK:-/opt/miyoomini-toolchain/usr}
SR="$SDK/arm-linux-gnueabihf/sysroot"
BASE=${BASE:-c9561ef}
out=${TMPDIR:-/tmp}/dkr_lazymix_test; mkdir -p "$out"
git -C "$here" show "$BASE:port/sdl/audio_sdl.c" > "$out/audio_old.c"
cp "$here/port/sdl/audio_sdl.c" "$out/audio_new.c"
DEF="-D_LANGUAGE_C -D_MIPS_SZLONG=32 -DF3DDKR_GBI -DVERSION_us_v80 -DPORT=1"
INC="-I$here/port -I$here/port/sdl -iquote $here/decomp/include -idirafter $here/decomp/include -idirafter $here/decomp/include/PR"
FL="-std=gnu99 -O2 -fno-strict-aliasing -fwrapv -D_GNU_SOURCE -D_REENTRANT"
ARMFL="-march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"  # the port's MARCH
for v in old new; do D=""; [ $v = new ] && D=-DNEW
  "$SDK"/bin/arm-linux-gnueabihf-gcc $FL $ARMFL $DEF -DHANDHELD=1 -DTARGET_HANDHELD=1 $INC -I"$SR/usr/include" $D \
      -o "$out/arm_$v" "$here/tools/lazymix_test.c" "$out/audio_$v.c" -L"$SR/usr/lib" -lSDL -lpthread
  if [ -f /usr/include/SDL/SDL.h ]; then
    gcc $FL $DEF -DHANDHELD=1 -DTARGET_HANDHELD=1 $INC -I/usr/include/SDL $D -o "$out/x86_$v" "$here/tools/lazymix_test.c" "$out/audio_$v.c" -lSDL -lpthread
  fi
  if [ -f /usr/include/SDL2/SDL.h ]; then
    gcc $FL $DEF $INC $D -o "$out/x86s2_$v" "$here/tools/lazymix_test.c" "$out/audio_$v.c" -lSDL2 -lpthread
  fi
done
export SDL_AUDIODRIVER=dummy
for b in arm x86 x86s2; do
  [ -x "$out/${b}_new" ] || continue
  run() { if [ $b = arm ]; then qemu-arm -L "$SR" "$@"; else "$@"; fi; }
  for v in old new; do echo "== $b $v"; run "$out/${b}_$v" play; done
  echo "== $b new DKR_LAZYMIX=0"; DKR_LAZYMIX=0 run "$out/${b}_new" play
  for v in old new; do echo "== $b $v backstop"; run "$out/${b}_$v" backstop | tail -1; done
  echo "== $b new toggle"; run "$out/${b}_new" toggle | tail -2
  for v in old new; do
    printf "== %s %s idle 2 s: sleep syscalls " $b $v
    if [ $b = arm ]; then qemu-arm -L "$SR" -strace "$out/arm_$v" idle 2>&1 | grep -c nanosleep || true
    elif command -v strace >/dev/null; then strace -f -e trace=nanosleep,clock_nanosleep "$out/${b}_$v" idle 2>&1 | grep -c nanosleep || true
    else echo "(no strace)"; fi
  done
done
