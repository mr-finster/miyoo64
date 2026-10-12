#!/usr/bin/env bash
# Run a command inside the MK64 build container (x86_64 via Colima+Rosetta).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -j
#
# The project root is mounted at /work. Note: use plain `bash -c`, NOT `bash -lc`
# — a login shell re-sources /etc/profile and drops the image's PATH settings.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="mk64hh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi

# One argument is a shell script (./dev.sh 'make -j && ls'); several are one command's argv,
# quotes kept (./dev.sh sh -c 'echo a b'). bash -c "$*" joined them and dropped the inner
# quotes (T12; Paper Mario's fix).
if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash
elif [ "$#" -eq 1 ]; then
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash -c "$1"
else
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" "$@"
fi
