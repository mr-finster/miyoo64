#!/usr/bin/env bash
# Run a command inside the build container (x86_64 via Colima+Rosetta),
# the same setup as ../mario-kart-64/dev.sh and ../ocarina-of-time/dev.sh.
# The image is OoT's oothh:build: MK64's mk64hh:build (the Miyoo toolchain at
# /opt/miyoomini-toolchain, 32-bit SDL2 and zlib, valgrind) plus qemu-user, which
# tools/sweep.sh needs for the ARM runs.
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh '<script>'      # one argument: a shell script, e.g. ./dev.sh 'make -j6 && ls build'
#   ./dev.sh <command...>    # several: one command's argv, quotes kept, e.g. ./dev.sh sh -c 'echo "a b"'
#
# The repo is mounted at /work (the working directory). Use plain `bash -c`,
# NOT `bash -lc`: a login shell re-sources /etc/profile and drops the image's
# PATH settings. Colima mounts only folders under $HOME, so keep scratch
# output in refs/ (untracked), not /tmp on the Mac.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${DKR_IMAGE:-oothh:build}"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi

# The mounted repo belongs to another uid: without safe.directory, git refuses
# it and the build stamp reads "unknown+".
PRE='git config --global --add safe.directory /work;'

# bash -c "$PRE $*" joined the arguments and dropped their inner quotes, so
# ./dev.sh sh -c '...' became "sh -c sh ..." and silently ran nothing (T12, round 16;
# Paper Mario's and MK64's 288a7bd fix). Several arguments now reach the command as its
# argv, after PRE.
if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$PRE exec bash"
elif [ "$#" -eq 1 ]; then
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$PRE $1"
else
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$PRE"' exec "$@"' dev "$@"
fi
