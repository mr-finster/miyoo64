#!/usr/bin/env bash
# Run a command inside the F-Zero X build container (x86_64 via Colima+Rosetta),
# after mario-kart-64's dev.sh. The image is mk64hh:build plus qemu (docker/Dockerfile).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -C game -f Makefile.handheld PLAT=miyoo -j6
#   ./dev.sh '<a && b>'      # one quoted argument is a shell command line
#
# The repo root is mounted at /work. Use plain `bash -c`, NOT `bash -lc`: a login shell
# re-sources /etc/profile and drops the SDK's PATH addition.
# Several arguments keep each word intact (T12, DKR round 16): bash -c "$*" re-split them, so
# ./dev.sh sh -c '...' ran `sh -c sh ...` and silently did nothing.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="fzxhh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build --platform linux/amd64 -t "$IMAGE" "$PROJECT_ROOT/docker"

# git runs inside the container as root on a mount the host user owns: the build id
# (Makefile.handheld) and the provenance gate need it.
GITENV=(-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*')
if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash
elif [ "$#" -eq 1 ]; then
  exec docker run --rm --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$1"
else
  exec docker run --rm --platform linux/amd64 "${GITENV[@]}" -v "$PROJECT_ROOT":/work -w /work "$IMAGE" bash -c "$(printf '%q ' "$@")"
fi
