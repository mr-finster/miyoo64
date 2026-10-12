#!/usr/bin/env bash
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

host_os="$(uname -s)"

# PSPSDK's tools are installed in $PSPDEV/bin. Make the common setup work even
# when PSPDEV is exported but its bin directory was not added to PATH.
if [[ -n "${PSPDEV:-}" && -d "${PSPDEV}/bin" ]]; then
    case ":${PATH}:" in
        *":${PSPDEV}/bin:"*) ;;
        *) export PATH="${PSPDEV}/bin:${PATH}" ;;
    esac
fi

# macOS ships GNU Make 3.81 as `make`, which is too old for this Makefile.
# Homebrew installs a current GNU Make as `gmake`.
if [[ -n "${MAKE:-}" ]]; then
    make_cmd="$MAKE"
elif [[ "$host_os" == "Darwin" ]]; then
    make_cmd="gmake"
else
    make_cmd="make"
fi

if ! command -v "$make_cmd" >/dev/null 2>&1; then
    if [[ "$host_os" == "Darwin" && "$make_cmd" == "gmake" ]]; then
        echo "error: GNU Make is required; install it with 'brew install make'" >&2
    else
        echo "error: unable to find build tool: $make_cmd" >&2
    fi
    exit 1
fi

for tool in psp-config psp-gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: unable to find $tool; install PSPSDK and add \$PSPDEV/bin to PATH" >&2
        exit 1
    fi
done

if [[ ! -f Makefile ]]; then
    echo "error: Makefile not found in $repo_root" >&2
    exit 1
fi

if [[ -n "${CLEAN_EXTRACTED:-}" ]]; then
    echo "Removing extracted/..."
    rm -rf extracted
fi

if [[ -n "${JOBS:-}" ]]; then
    jobs="$JOBS"
elif command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
else
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
fi

PSP_ENABLE_GPROF="${PSP_ENABLE_GPROF:-0}"
if [[ "${GPROF:-0}" == "1" ]]; then
    PSP_ENABLE_GPROF=1
fi

case "$PSP_ENABLE_GPROF" in
    1|ON|on|TRUE|true|YES|yes)
        PSP_ENABLE_GPROF=1
        ;;
    0|OFF|off|FALSE|false|NO|no|"")
        PSP_ENABLE_GPROF=0
        ;;
    *)
        echo "error: unsupported PSP_ENABLE_GPROF value: $PSP_ENABLE_GPROF" >&2
        exit 1
        ;;
esac

echo "Removing build/..."
rm -rf build

if [[ "$PSP_ENABLE_GPROF" == "1" ]]; then
    echo "Building psp-port gprof mode with $jobs job(s) using $make_cmd..."
else
    echo "Building psp-port with $jobs job(s) using $make_cmd..."
fi

"$make_cmd" -j"$jobs" psp-port PSP_ENABLE_GPROF="$PSP_ENABLE_GPROF" "$@"
