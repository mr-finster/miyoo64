#!/usr/bin/env bash
# Run a command inside the Smash build container (x86_64 via Colima+Rosetta).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -C port PLAT=miyoo -j6
#
# The project root is mounted at /work. The image (docker/Dockerfile) is the
# MK64 image plus qemu-user; it is built here the first time.
# Note: use plain `bash -c`, NOT `bash -lc`: a login shell re-sources
# /etc/profile and drops the SDK's PATH addition.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="ssb64hh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building $IMAGE (on top of mk64hh:build)..." >&2
  docker build --platform linux/amd64 -t "$IMAGE" "$PROJECT_ROOT/docker" >&2
fi

# git runs as root on a mount the host user owns: without safe.directory it
# refuses the repo, and the provenance gate (git ls-files) fails.
GITENV=(-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*')
# One argument is a command line for bash (./dev.sh "sh tools/build_miyoo.sh"). Several
# are run as given, each word intact: bash -c "$*" joined them and re-parsed the result, so
# inner quotes were lost and ./dev.sh sh -c 'a & b' ran 'sh -c a' (the lead's T12, found on
# DKR; here it silently dropped one of four parallel runs in round 15).
if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash
elif [ "$#" -eq 1 ]; then
  exec docker run --rm --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$1"
else
  exec docker run --rm --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c 'exec "$@"' bash "$@"
fi
