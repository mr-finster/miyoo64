#!/usr/bin/env bash
# Run a command inside the SM64 build container (x86_64 via Colima+Rosetta).
# Usage:
#   ./dev.sh                 # interactive bash shell in the container
#   ./dev.sh <command...>    # run a command, e.g. ./dev.sh make -j
#
# One argument is a script (./dev.sh 'cd /work/port && make'); several are one command
# whose arguments keep their quoting (./dev.sh sh -c 'echo a b'). bash -c "$*" joined
# them unquoted, so ./dev.sh sh -c '...' ran `sh -c <first word>`: nothing, silently
# (the lead's T12, found in DKR).
# The project root is mounted at /work. Note: use plain `bash -c`, NOT `bash -lc`
# — a login shell re-sources /etc/profile and drops the SDK's PATH addition.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="sm64hh:build"

# Ensure the VM is running.
if ! docker info >/dev/null 2>&1; then
  echo "Colima not running; starting..." >&2
  colima start --arch aarch64 --vm-type vz --vz-rosetta --cpu 6 --memory 8 --disk 30
fi

if [ "$#" -eq 0 ]; then
  exec docker run --rm -it --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash
else
  if [ "$#" -eq 1 ]; then cmd=$1; else cmd=$(printf '%q ' "$@"); fi
  exec docker run --rm --platform linux/amd64 -v "$PROJECT_ROOT":/work "$IMAGE" bash -c "$cmd"
fi
