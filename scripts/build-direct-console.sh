#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and test the native direct-X11 module with its pinned xrdp runtime.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_BUILD_DIR:-$workspace_root/build-direct-console}
freerdp_client=${XRDP_CONSOLE_FREERDP_EXECUTABLE:-}

if [ -n "$freerdp_client" ] && [ ! -x "$freerdp_client" ]; then
    echo "XRDP_CONSOLE_FREERDP_EXECUTABLE is not executable: $freerdp_client" >&2
    exit 1
fi

for required in cmake ninja ctest; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "Missing required build program: $required" >&2
        exit 1
    fi
done

if [ -f "$build_root/CMakeCache.txt" ]; then
    configured_source=$(sed -n \
        's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
        "$build_root/CMakeCache.txt")
    if [ -n "$configured_source" ] &&
       [ "$configured_source" != "$workspace_root" ]; then
        echo "Build directory belongs to a different checkout: $configured_source" >&2
        echo "Choose a fresh directory with XRDP_CONSOLE_BUILD_DIR; existing files were left untouched." >&2
        exit 1
    fi

    if ! grep -Fq 'CMAKE_GENERATOR:INTERNAL=Ninja' "$build_root/CMakeCache.txt"; then
        echo "$build_root already uses a non-Ninja CMake generator" >&2
        echo "Choose another XRDP_CONSOLE_BUILD_DIR; existing files were left untouched." >&2
        exit 1
    fi
fi

cmake -S "$workspace_root" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DXRDP_CONSOLE_NATIVE=ON \
    -DXRDP_CONSOLE_BUILD_XRDP=ON \
    "-DXRDP_CONSOLE_FREERDP_EXECUTABLE=$freerdp_client"

# The pinned xrdp build is serialized to stay within the target laptop's
# memory budget. First-party targets are also built serially for predictability.
cmake --build "$build_root" --parallel 1
ctest --test-dir "$build_root" --output-on-failure

printf '%s\n' \
    "Native direct-X11 build and tests passed." \
    "Build directory: $build_root" \
    "Activate only after reviewing the test results:"
printf "sudo env XRDP_CONSOLE_BUILD_DIR='%s' \\\n  '%s/scripts/activate-direct-console.sh'\n" \
    "$build_root" "$workspace_root"
