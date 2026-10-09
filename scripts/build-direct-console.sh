#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and test the native direct-X11 module with its pinned xrdp runtime.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_BUILD_DIR:-$workspace_root/build-direct-console}
case "$build_root" in
    /*) ;;
    *) build_root=$workspace_root/$build_root ;;
esac
# Keep generated dependency scratch files and Python TemporaryDirectory
# instances beneath the selected build root, never /tmp or /var/tmp.
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

test_scratch_root=$build_root/test-artifacts/tmp
mkdir -p "$test_scratch_root"
TMPDIR=$test_scratch_root
export TMPDIR
# Low-memory-safe parallel compilation. Change with XRDP_CONSOLE_BUILD_JOBS.
# This governs both the top-level Ninja build and the pinned xrdp Make build.
build_jobs=${XRDP_CONSOLE_BUILD_JOBS:-2}
xrdp_make_jobs=${XRDP_CONSOLE_XRDP_BUILD_JOBS:-$build_jobs}
for jobs in "$build_jobs" "$xrdp_make_jobs"; do
    case "$jobs" in
        ""|*[!0-9]*) echo "Build job counts must be positive integers" >&2; exit 1 ;;
    esac
    if [ "$jobs" -lt 1 ] || [ "$jobs" -gt 64 ]; then
        echo "Build job counts must be between 1 and 64" >&2
        exit 1
    fi
done
xrdp_install_root=${XRDP_CONSOLE_XRDP_INSTALL_DIR:-$build_root/_deps/xrdp-install}
freerdp_client=${XRDP_CONSOLE_FREERDP_EXECUTABLE:-}
freerdp_build_root=${XRDP_CONSOLE_FREERDP_BUILD_DIR:-$workspace_root/build-test-freerdp}
case "$freerdp_build_root" in
    /*) ;;
    *) freerdp_build_root=$workspace_root/$freerdp_build_root ;;
esac

if [ -z "$freerdp_client" ]; then
    for candidate in \
        "$freerdp_build_root/install/bin/xfreerdp3" \
        "$freerdp_build_root/install/bin/xfreerdp"; do
        if [ -x "$candidate" ]; then
            freerdp_client=$candidate
            break
        fi
    done

    if [ -z "$freerdp_client" ]; then
        XRDP_CONSOLE_FREERDP_BUILD_JOBS=${XRDP_CONSOLE_FREERDP_BUILD_JOBS:-$build_jobs} \
            "$workspace_root/scripts/build-test-freerdp.sh"
        for candidate in \
            "$freerdp_build_root/install/bin/xfreerdp3" \
            "$freerdp_build_root/install/bin/xfreerdp"; do
            if [ -x "$candidate" ]; then
                freerdp_client=$candidate
                break
            fi
        done
    fi
fi

case "$freerdp_client" in
    /*) ;;
    *) freerdp_client=$workspace_root/$freerdp_client ;;
esac

if [ -n "$freerdp_client" ] && [ ! -x "$freerdp_client" ]; then
    echo "XRDP_CONSOLE_FREERDP_EXECUTABLE is not executable: $freerdp_client" >&2
    exit 1
fi

if [ -z "$freerdp_client" ]; then
    echo "The project-built H.264 FreeRDP client was not produced." >&2
    exit 1
fi

freerdp_buildconfig=$("$freerdp_client" /buildconfig 2>&1) || {
    echo "Could not run FreeRDP /buildconfig: $freerdp_client" >&2
    exit 1
}
if ! printf '%s\n' "$freerdp_buildconfig" | grep -q 'WITH_GFX_H264=ON' ||
   ! printf '%s\n' "$freerdp_buildconfig" | grep -Eq \
       'WITH_OPENH264=ON|WITH_FFMPEG=ON|WITH_VIDEO_FFMPEG=ON'; then
    echo "RDP loader CTests require FreeRDP with RDPGFX H.264 and a decoder backend." >&2
    echo "Selected client: $freerdp_client" >&2
    printf '%s\n' "$freerdp_buildconfig" >&2
    exit 1
fi

for required in cmake ninja ctest; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "Missing required build program: $required" >&2
        exit 1
    fi
done

cmake -S "$workspace_root" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DXRDP_CONSOLE_NATIVE=ON \
    -DXRDP_CONSOLE_BUILD_XRDP=ON \
    "-DXRDP_CONSOLE_XRDP_INSTALL_DIR=$xrdp_install_root" \
    "-DXRDP_CONSOLE_FREERDP_EXECUTABLE=$freerdp_client" \
    "-DXRDP_CONSOLE_XRDP_BUILD_JOBS=$xrdp_make_jobs"

# Only compilation is parallelized. Clipboard integration CTests intentionally
# remain serial because they share the xrdp per-user chansrv socket namespace.
printf 'Build jobs: Ninja=%s xrdp Make=%s\n' "$build_jobs" "$xrdp_make_jobs"
cmake --build "$build_root" --parallel "$build_jobs"
# One absent sesman-created socketdir otherwise causes 35 repeated clipboard
# startup failures. Fail before CTest with an actionable, read-only diagnosis;
# never skip tests or silently create a privileged runtime directory.
if ! python3 "$workspace_root/tools/diagnostics/xrdp_clipboard_test_doctor.py"; then
    echo "Clipboard test runtime unavailable; checking guarded activator recovery." >&2
    case "${XRDP_CONSOLE_AUTO_REPAIR_TEST_RUNTIME:-1}" in
        1)
            if [ "$(id -u)" -eq 0 ]; then
                "$workspace_root/scripts/activate-direct-console.sh" --prepare-test-runtime
            else
                command -v sudo >/dev/null 2>&1 || {
                    echo "sudo is required to restore the sesman test prerequisite." >&2
                    exit 1
                }
                sudo "$workspace_root/scripts/activate-direct-console.sh" --prepare-test-runtime
            fi
            # The recovery is not a waiver. Verify the real prerequisite again.
            python3 "$workspace_root/tools/diagnostics/xrdp_clipboard_test_doctor.py"
            ;;
        0)
            echo "Runtime auto-recovery disabled by XRDP_CONSOLE_AUTO_REPAIR_TEST_RUNTIME=0" >&2
            exit 1
            ;;
        *)
            echo "XRDP_CONSOLE_AUTO_REPAIR_TEST_RUNTIME must be 0 or 1" >&2
            exit 1
            ;;
    esac
fi
ctest --test-dir "$build_root" --output-on-failure --parallel 1

printf '%s\n' \
    "Native direct-X11 build and tests passed." \
    "Build directory: $build_root" \
    "Preflight (read-only):"
printf "sudo env XRDP_CONSOLE_BUILD_DIR='%s' '%s/scripts/activate-direct-console.sh' --preflight\n" \
    "$build_root" "$workspace_root"
printf '%s\n' "Activate only after reviewing the preflight result:"
printf "sudo env XRDP_CONSOLE_BUILD_DIR='%s' '%s/scripts/activate-direct-console.sh'\n" \
    "$build_root" "$workspace_root"
