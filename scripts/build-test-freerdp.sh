#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build an isolated H.264-capable FreeRDP client for the optional CTest smoke.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_FREERDP_BUILD_DIR:-$workspace_root/build-test-freerdp}
case "$build_root" in
    /*) ;;
    *) build_root=$workspace_root/$build_root ;;
esac
source_root=$build_root/source
freerdp_build_root=$build_root/build
install_root=$build_root/install
freerdp_version=3.31.0
freerdp_commit=aa8650b300aa4cabd85d9c72b431301509b9043f
freerdp_repository=https://github.com/FreeRDP/FreeRDP.git
build_jobs=${XRDP_CONSOLE_FREERDP_BUILD_JOBS:-1}

for required in cmake ninja git pkg-config; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "Missing required build program: $required" >&2
        exit 1
    fi
done

if ! pkg-config --exists libavcodec libavutil libswscale; then
    cat >&2 <<'EOF'
Missing FFmpeg development libraries required for FreeRDP H.264 GFX.
On Ubuntu, install them with:
  sudo apt install libavcodec-dev libavutil-dev libswscale-dev
EOF
    exit 1
fi

if ! pkg-config --exists libusb-1.0; then
    cat >&2 <<'EOF'
Missing libusb development files required by the isolated FreeRDP client build.
On Ubuntu, install them with:
  sudo apt install libusb-1.0-0-dev
EOF
    exit 1
fi

if [ ! -d "$source_root/.git" ]; then
    if [ -e "$source_root" ]; then
        echo "Refusing to replace non-Git source path: $source_root" >&2
        exit 1
    fi
    mkdir -p "$build_root"
    git clone --depth 1 --branch "$freerdp_version" \
        "$freerdp_repository" "$source_root"
else
    current_version=$(git -C "$source_root" describe --tags --exact-match 2>/dev/null || true)
    if [ "$current_version" != "$freerdp_version" ]; then
        echo "Existing FreeRDP source is not the expected $freerdp_version tag: $source_root" >&2
        echo "Choose a fresh directory with XRDP_CONSOLE_FREERDP_BUILD_DIR; existing files were left untouched." >&2
        exit 1
    fi
    current_commit=$(git -C "$source_root" rev-parse HEAD)
    if [ "$current_commit" != "$freerdp_commit" ]; then
        echo "Existing FreeRDP source is not the pinned $freerdp_version commit: $source_root" >&2
        echo "Choose a fresh directory with XRDP_CONSOLE_FREERDP_BUILD_DIR; existing files were left untouched." >&2
        exit 1
    fi
    if [ -n "$(git -C "$source_root" status --porcelain)" ]; then
        echo "FreeRDP source tree has local changes: $source_root" >&2
        echo "Choose a fresh directory with XRDP_CONSOLE_FREERDP_BUILD_DIR; existing files were left untouched." >&2
        exit 1
    fi
fi

actual_commit=$(git -C "$source_root" rev-parse HEAD)
if [ "$actual_commit" != "$freerdp_commit" ]; then
    echo "FreeRDP tag $freerdp_version resolved to unexpected commit $actual_commit" >&2
    echo "Expected pinned commit: $freerdp_commit" >&2
    exit 1
fi

cmake -S "$source_root" -B "$freerdp_build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_root" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    "-DCMAKE_INSTALL_RPATH=$install_root/lib" \
    -DWITH_CLIENT=ON \
    -DWITH_CLIENT_SDL=OFF \
    -DWITH_X11=ON \
    -DWITH_WAYLAND=OFF \
    -DWITH_SERVER=OFF \
    -DWITH_PROXY=OFF \
    -DWITH_FFMPEG=ON \
    -DWITH_VIDEO_FFMPEG=ON \
    -DWITH_SWSCALE=ON \
    -DWITH_KRB5=OFF \
    -DWITH_CUPS=OFF \
    -DWITH_SMARTCARD_PCSC=OFF \
    -DWITH_UNICODE_BUILTIN=ON

cmake --build "$freerdp_build_root" --parallel "$build_jobs"
cmake --install "$freerdp_build_root"

freerdp_client=
for candidate in \
    "$install_root/bin/xfreerdp3" \
    "$install_root/bin/xfreerdp"; do
    if [ -x "$candidate" ]; then
        freerdp_client=$candidate
        break
    fi
done

if [ -z "$freerdp_client" ]; then
    echo "Could not find the installed FreeRDP client under $install_root/bin" >&2
    exit 1
fi

client_build=$("$freerdp_client" /buildconfig 2>&1)
if ! printf '%s\n' "$client_build" | grep -q 'WITH_GFX_H264=ON' ||
   ! printf '%s\n' "$client_build" | grep -q 'WITH_VIDEO_FFMPEG=ON'; then
    echo "Built FreeRDP client does not report the required H.264 GFX/FFmpeg features:" >&2
    printf '%s\n' "$client_build" >&2
    exit 1
fi

printf '%s\n' \
    "H.264-capable FreeRDP test client built and verified." \
    "Client: $freerdp_client" \
    "System FreeRDP packages were not replaced."
printf 'Run the direct-X11 CTest build with:\n  XRDP_CONSOLE_FREERDP_EXECUTABLE=%s scripts/build-direct-console.sh\n' \
    "$freerdp_client"
