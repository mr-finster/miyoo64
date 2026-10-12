#!/usr/bin/env bash
# Run a command inside the Ocarina of Time build container (x86_64 via Colima+Rosetta).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -j
#   ./dev.sh '<a && b>'      # one quoted argument is a shell command line
#
# The project root is mounted at /work. Note: use plain `bash -c`, NOT `bash -lc`
# — a login shell re-sources /etc/profile and drops the SDK's PATH addition.
# Several arguments keep each word intact (T12, DKR round 16; F-Zero X 3a9c527): bash -c "$*"
# re-split them, so ./dev.sh sh -c 'a; b' ran `sh -c a` and then b.
# DEV_DOCKER_ARGS adds docker run options, e.g. DEV_DOCKER_ARGS="--cap-add SYS_NICE" for kit k1.6's
# nice -10 (a container without it keeps nice 0 and says so in the log).
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="oothh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi

if [ "$#" -eq 0 ]; then
  exec docker run --rm -it ${DEV_DOCKER_ARGS:-} --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash
elif [ "$#" -eq 1 ]; then
  exec docker run --rm ${DEV_DOCKER_ARGS:-} --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash -c "$1"
else
  exec docker run --rm ${DEV_DOCKER_ARGS:-} --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash -c "$(printf '%q ' "$@")"
fi
