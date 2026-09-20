#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the persistent optimized xrdp candidate used by the shared-console
# profile.  This deliberately builds from the checked-in patched tree; it
# never downloads into /tmp and never modifies the system installation.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_root=$workspace_root/third_party/xrdp-0.10.6.1-optimized
build_root=$workspace_root/build/xrdp-0.10.6.1-optimized
prefix=$workspace_root/build/prefix/xrdp-optimized-resize
xkb_sysroot=$workspace_root/deps/sysroot-xkbfile
x264_sysroot=${XRDP_X264_SYSROOT:-${HOME}/.local/lib/xrdp-deps/sysroot-x264}
flags_stamp=$build_root/.optimized-build-fingerprint

# This host has only 3.7 GiB of RAM.  Keep the build serial so a rebuild does
# not compete with the live desktop or the benchmark client.
jobs=1
if [ "${XRDP_BUILD_JOBS:-1}" != 1 ]; then
    echo "XRDP_BUILD_JOBS is ignored: this profile is intentionally serial" >&2
fi

for required in "$source_root/configure.ac" "$source_root/bootstrap" \
               "$xkb_sysroot/usr/include/X11/extensions/XKBfile.h" \
               "$xkb_sysroot/usr/lib/x86_64-linux-gnu/libxkbfile.a" \
               "$x264_sysroot/usr/include/x264.h" \
               "$x264_sysroot/usr/lib/x86_64-linux-gnu/libx264.so"; do
    if [ ! -e "$required" ]; then
        echo "Missing build input: $required" >&2
        exit 1
    fi
done

if [ ! -x "$source_root/configure" ] ||
   [ "$source_root/configure.ac" -nt "$source_root/configure" ]; then
    echo "Regenerating xrdp configure files from the persistent source tree"
    (cd "$source_root" && ./bootstrap)
fi

mkdir -p "$build_root" "$prefix"

pkg_config_path=$xkb_sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:
pkg_config_path=$pkg_config_path$x264_sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:
pkg_config_path=$pkg_config_path/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig

export CFLAGS='-O3 -march=native'
export CPPFLAGS="-I$xkb_sysroot/usr/include -I$x264_sysroot/usr/include"
export LDFLAGS="-L$xkb_sysroot/usr/lib/x86_64-linux-gnu -L$x264_sysroot/usr/lib/x86_64-linux-gnu"
export PKG_CONFIG_PATH=$pkg_config_path

source_revision=$(sha256sum \
    "$source_root/configure.ac" \
    "$source_root/common/os_calls.c" \
    "$source_root/common/os_calls.h" \
    "$source_root/libxrdp/libxrdp.c" \
    "$source_root/libxrdp/libxrdpinc.h" \
    "$source_root/vnc/vnc.c" \
    "$source_root/vnc/vnc.h" \
    "$source_root/xrdp/xrdp_cache.c" \
    "$source_root/xrdp/xrdp_mm.c" \
    "$source_root/xrdp/xrdp_painter.c" | sha256sum | awk '{print $1}')
build_fingerprint="source=$source_revision\nCFLAGS=$CFLAGS\nCPPFLAGS=$CPPFLAGS\nLDFLAGS=$LDFLAGS"
if [ "${XRDP_CLEAN_BUILD:-0}" = 1 ] ||
   [ ! -f "$flags_stamp" ] ||
   [ "$(cat "$flags_stamp" 2>/dev/null || true)" != "$build_fingerprint" ]; then
    if [ -f "$build_root/Makefile" ]; then
        echo "Cleaning stale objects before applying the optimized build flags"
        make -C "$build_root" clean -j"$jobs"
    fi
fi

configure_args="--prefix=$prefix
--sysconfdir=$prefix/etc
--localstatedir=$prefix/var
--runstatedir=/run
--with-socketdir=/run/xrdp/sockdir
--enable-strict-locations
--enable-x264
--enable-jpeg
--enable-ipv6
--enable-vsock
--enable-utmp
--with-freetype2=yes
--disable-neutrinordp"

echo "Configuring optimized xrdp in $build_root"
# shellcheck disable=SC2086
(cd "$build_root" && "$source_root/configure" $configure_args)
echo "Building optimized xrdp with -j$jobs, -O3, and -march=native"
make -C "$build_root" -j"$jobs"
make -C "$build_root" install

if [ "${XRDP_BUILD_CHECK:-1}" = 1 ]; then
    echo "Running the xrdp unit tests"
    check_attempt=1
    while ! make -C "$build_root" check -j"$jobs"; do
        # xrdp's SIGCHLD timing test can miss the first asynchronous delivery
        # under load. Retry once, but still fail the build if the second run
        # fails so a real regression is never hidden.
        if [ "$check_attempt" -ge 2 ]; then
            exit 1
        fi
        echo "The xrdp test suite had a transient failure; retrying once"
        check_attempt=$((check_attempt + 1))
    done
fi

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
if ! grep -q -- '-O3' "$build_root/Makefile" ||
   ! grep -q -- '-march=native' "$build_root/Makefile"; then
    echo "The generated build does not contain the requested native -O3 flags" >&2
    exit 1
fi
printf '%s\n' "$build_fingerprint" >"$flags_stamp"

echo "Built optimized resize-fixed xrdp:"
echo "  source: $source_root"
echo "  daemon: $daemon"
echo "  module: $module"
echo "  flags:  -O3 -march=native"
