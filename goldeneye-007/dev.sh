#!/usr/bin/env bash
# Run a command inside the GoldenEye build container (x86_64 via Colima+Rosetta).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -j
#
# The project root is mounted at /work. DEV_MOUNT=<host path>:<container path> adds one more bind
# mount (e.g. a ROM kept outside the tree: DEV_MOUNT=/path/rom:/rom:ro, then GE_ROM=/rom/<file>).
# Note: use plain `bash -c`, NOT `bash -lc`
# — a login shell re-sources /etc/profile and drops the image's PATH settings.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${GE_IMAGE:-gemm:build}" # the Miyoo Mini image (docker/Dockerfile.miyoo)

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi

if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash
else
  exec docker run --rm --platform linux/amd64 ${DEV_MOUNT:+-v "$DEV_MOUNT"} -v "$PROJECT_ROOT":/work "$IMAGE" bash -c "$*"
fi
