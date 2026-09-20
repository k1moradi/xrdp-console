#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the pinned, patched xrdp dependency for this host.  The source,
# build, and install trees are generated below build/_deps; nothing is
# installed into the system by this script.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_BUILD_DIR:-$workspace_root/build}
prefix=${XRDP_CONSOLE_XRDP_INSTALL_DIR:-$build_root/_deps/xrdp-install}
source_root=${XRDP_CONSOLE_XRDP_SOURCE_DIR:-$build_root/_deps/xrdp-src}
dependency_build_root=${XRDP_CONSOLE_XRDP_BUILD_DIR:-$build_root/_deps/xrdp-build}
native_cflags=${XRDP_CONSOLE_XRDP_CFLAGS:--O3 -march=native}

# This host has only 3.7 GiB of RAM. Keep the build serial so a rebuild does
# not compete with the live desktop or the benchmark client.
jobs=1
if [ "${XRDP_BUILD_JOBS:-1}" != 1 ]; then
    echo "XRDP_BUILD_JOBS is ignored: this profile is intentionally serial" >&2
fi

for required in cmake ninja; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "Missing required build program: $required" >&2
        exit 1
    fi
done

if [ -f "$build_root/CMakeCache.txt" ] &&
   ! grep -Fq 'CMAKE_GENERATOR:INTERNAL=Ninja' "$build_root/CMakeCache.txt"; then
    echo "$build_root already uses a non-Ninja CMake generator" >&2
    echo "Choose another XRDP_CONSOLE_BUILD_DIR or reconfigure that directory" >&2
    exit 1
fi

cmake -S "$workspace_root" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DXRDP_CONSOLE_NATIVE=ON \
    -DXRDP_CONSOLE_BUILD_XRDP=ON \
    "-DXRDP_CONSOLE_XRDP_SOURCE_DIR=$source_root" \
    "-DXRDP_CONSOLE_XRDP_BUILD_DIR=$dependency_build_root" \
    "-DXRDP_CONSOLE_XRDP_INSTALL_DIR=$prefix" \
    "-DXRDP_CONSOLE_XRDP_CFLAGS=$native_cflags" \
    "-DXRDP_CONSOLE_XRDP_CPPFLAGS=${XRDP_CONSOLE_XRDP_CPPFLAGS:-}" \
    "-DXRDP_CONSOLE_XRDP_LDFLAGS=${XRDP_CONSOLE_XRDP_LDFLAGS:-}" \
    "-DXRDP_CONSOLE_XRDP_PKG_CONFIG_PATH=${XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH:-}"

cmake --build "$build_root" --target xrdp_upstream --parallel "$jobs"

daemon=$prefix/sbin/xrdp
module=$prefix/lib/xrdp/libvnc.so
if ! "$daemon" --version 2>/dev/null | grep -q '^xrdp 0\.10\.6\.1'; then
    echo "The installed candidate does not report xrdp 0.10.6.1" >&2
    exit 1
fi
if [ ! -f "$module" ]; then
    echo "The installed candidate is missing $module" >&2
    exit 1
fi
if ! grep -q 'XRDP_MM_IS_VNC' "$source_root/xrdp/xrdp_types.h" ||
   ! grep -q 'dynamic_resizing' "$source_root/vnc/vnc.h"; then
    echo "The pinned xrdp source was not patched with the retained console changes" >&2
    exit 1
fi
if ! grep -Fq "XRDP_CONSOLE_XRDP_CFLAGS:STRING=$native_cflags" \
   "$build_root/CMakeCache.txt"; then
    echo "The CMake build is not configured with the requested native xrdp flags" >&2
    exit 1
fi

echo "Built patched native xrdp:"
echo "  source: $source_root"
echo "  daemon: $daemon"
echo "  module: $module"
echo "  flags:  $native_cflags"
