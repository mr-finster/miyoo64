#!/usr/bin/env bash
# Run a command inside the Star Fox 64 build container (x86_64 via Colima+Rosetta).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -C port -f Makefile.handheld PLAT=pc
#
# The project root is mounted at /work. The image is docker/Dockerfile.sf64 (tag sf64hh:build),
# a thin layer on the MK64 image; it is built here on first use. Use plain `bash -c`, NOT
# `bash -lc`: a login shell re-sources /etc/profile and resets the image's PATH.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="sf64hh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building $IMAGE (needs mk64hh:build)..." >&2
  docker build -t "$IMAGE" -f "$PROJECT_ROOT/docker/Dockerfile.sf64" "$PROJECT_ROOT/docker" >&2
fi

# One argument is a shell script (./dev.sh 'make -j && ls'); several are one command's argv,
# quotes kept (./dev.sh sh -c 'echo a b'). bash -c "$*" joined them and dropped the inner
# quotes, so ./dev.sh sh -c '...' silently ran nothing (T12; Paper Mario's fix, mario-kart-64 288a7bd).
if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash
elif [ "$#" -eq 1 ]; then
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$1"
else
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work -w /work "$IMAGE" "$@"
fi
