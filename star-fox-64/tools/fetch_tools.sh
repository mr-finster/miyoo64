#!/usr/bin/env bash
# Fetch the decomp's host tools (upstream git submodules) at the commits
# sf64 3c5d492 pins, into port/tools/. They are not vendored in this repo.
# Idempotent: an existing checkout at the right commit is left alone.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fetch() { # name url commit
    local dir="$ROOT/port/tools/$1"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$3" ]; then return; fi
    rm -rf "$dir" && git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$2" "$3"
    git -C "$dir" checkout -q FETCH_HEAD
    git -C "$dir" submodule update -q --init --recursive --depth 1 || true
    echo "fetched $1 @ ${3:0:7}"
}
fetch splat         https://github.com/ethteck/splat                441af8f682454154aae6d593d0938f23c747a1bc
fetch asm-processor https://github.com/simonlindholm/asm-processor  fed1e3ddb9967bd427f4599b1b8ededa296bd9f9
fetch asm-differ    https://github.com/simonlindholm/asm-differ     e8bddb02576e73aecb90f813bfb452417c37b257
fetch m2c           https://github.com/matt-kempster/m2c            000c5be4c403963ddcffee73a5e0a0f26fb9a221
fetch Torch         https://github.com/HarbourMasters/Torch         6ca699d508d2b62ab51d290e6a38a1a24f7d83cf
