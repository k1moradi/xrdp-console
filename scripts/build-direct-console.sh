#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and test the native direct-X11 module with its pinned xrdp runtime.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_BUILD_DIR:-$workspace_root/build-direct-console}
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

for required in cmake ninja ctest git strings; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "Missing required build program: $required" >&2
        exit 1
    fi
done


source_head=$(git -C "$workspace_root" rev-parse --verify HEAD)
source_short=$(git -C "$workspace_root" rev-parse --short=12 HEAD)
source_branch=$(git -C "$workspace_root" symbolic-ref --quiet --short HEAD || printf '%s' '(detached)')
source_status=$(git -C "$workspace_root" status --porcelain --untracked-files=all)
source_revision=$source_short
if [ -n "$source_status" ]; then
    source_revision=$source_revision-dirty
fi

printf '%s\n' \
    "Source branch: $source_branch" \
    "Source HEAD: $source_head" \
    "Expected module revision: $source_revision"

if [ "${XRDP_CONSOLE_REQUIRE_CLEAN:-0}" = "1" ] && [ -n "$source_status" ]; then
    echo "XRDP_CONSOLE_REQUIRE_CLEAN=1 but the source worktree is dirty:" >&2
    printf '%s\n' "$source_status" >&2
    exit 1
fi

if [ -n "${XRDP_CONSOLE_EXPECT_REVISION:-}" ]; then
    if ! expected_head=$(git -C "$workspace_root" rev-parse --verify "${XRDP_CONSOLE_EXPECT_REVISION}^{commit}" 2>/dev/null); then
        echo "XRDP_CONSOLE_EXPECT_REVISION does not resolve to a commit: ${XRDP_CONSOLE_EXPECT_REVISION}" >&2
        exit 1
    fi
    if [ "$source_head" != "$expected_head" ]; then
        echo "Source HEAD $source_head does not match XRDP_CONSOLE_EXPECT_REVISION $expected_head" >&2
        exit 1
    fi
fi

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
    "-DXRDP_CONSOLE_XRDP_INSTALL_DIR=$xrdp_install_root" \
    "-DXRDP_CONSOLE_FREERDP_EXECUTABLE=$freerdp_client"

# The pinned xrdp build is serialized to stay within the target laptop's
# memory budget. First-party targets are also built serially for predictability.
cmake --build "$build_root" --parallel 1

post_head=$(git -C "$workspace_root" rev-parse --verify HEAD)
post_status=$(git -C "$workspace_root" status --porcelain --untracked-files=all)
post_revision=$(git -C "$workspace_root" rev-parse --short=12 HEAD)
if [ -n "$post_status" ]; then
    post_revision=$post_revision-dirty
fi
if [ "$post_head" != "$source_head" ] || [ "$post_revision" != "$source_revision" ]; then
    echo "Source provenance changed during the build." >&2
    echo "Before: HEAD=$source_head revision=$source_revision" >&2
    echo "After:  HEAD=$post_head revision=$post_revision" >&2
    exit 1
fi

revision_header=$build_root/generated/build_revision.h
module_path=$build_root/src/libxrdp_console.so
if [ ! -f "$revision_header" ]; then
    echo "Missing generated build revision header: $revision_header" >&2
    exit 1
fi
generated_revision=$(sed -n 's/^#define XRDP_CONSOLE_BUILD_REVISION "\\(.*\\)"$/\\1/p' "$revision_header")
if [ "$generated_revision" != "$source_revision" ]; then
    echo "Generated module revision '$generated_revision' does not match source revision '$source_revision'." >&2
    exit 1
fi
if [ ! -f "$module_path" ]; then
    echo "Missing built module: $module_path" >&2
    exit 1
fi
if ! strings "$module_path" | grep -Fqx "$source_revision"; then
    echo "Built module does not embed expected source revision: $source_revision" >&2
    exit 1
fi

printf '%s\n' "Verified built module revision: $source_revision"

ctest --test-dir "$build_root" --output-on-failure

printf '%s\n' \
    "Native direct-X11 build and tests passed." \
    "Build directory: $build_root" \
    "Preflight (read-only):"
printf "sudo env XRDP_CONSOLE_BUILD_DIR='%s' '%s/scripts/activate-direct-console.sh' --preflight\n" \
    "$build_root" "$workspace_root"
printf '%s\n' "Activate only after reviewing the preflight result:"
printf "sudo env XRDP_CONSOLE_BUILD_DIR='%s' '%s/scripts/activate-direct-console.sh'\n" \
    "$build_root" "$workspace_root"
