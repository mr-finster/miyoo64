#!/usr/bin/env bash
# Run a command inside the PM64->handheld build container (x86_64 via Colima+Rosetta), as
# mario-kart-64's dev.sh does. The image is pm64hh:build, a thin layer on mk64hh:build
# (docker/Dockerfile); it is built here the first time.
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -C bench PLAT=miyoo
#   ./dev.sh '<shell line>'  # one argument runs as a shell line: ./dev.sh 'make -C bench && ls bench/build'
# Several arguments run as they are, quotes kept (T12, DKR round 16: bash -c "$*" re-split them, so
# ./dev.sh sh -c 'echo a b' ran `sh -c echo` and printed nothing).
#
# The project root is mounted at /work. Note: use plain `bash -c`, NOT `bash -lc`
# -- a login shell re-sources /etc/profile and drops the SDK's PATH addition.
# git inside the container sees /work owned by another user; safe.directory is passed in
# the environment so the provenance gate (git ls-files) works.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="pm64hh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "building $IMAGE (thin layer on mk64hh:build)..." >&2
  docker build --platform linux/amd64 -t "$IMAGE" "$PROJECT_ROOT/docker" >&2
fi

GITENV=(-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*')
if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work "$IMAGE" bash
elif [ "$#" -eq 1 ]; then
  exec docker run --rm --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work "$IMAGE" bash -c "$1"
else
  exec docker run --rm --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work "$IMAGE" "$@"
fi
