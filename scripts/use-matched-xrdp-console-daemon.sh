#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Use the persistent optimized xrdp 0.10.6.1 daemon for the physical x11vnc
# console.  It contains the VNC scheduling/encoding optimizations and the
# upstream resize-state fix.  The service drop-in is backed up and restored on
# failure; no source or binary is copied through /tmp.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prefix=$workspace_root/build/prefix/xrdp-optimized-resize
daemon=$prefix/sbin/xrdp
module=$prefix/lib/xrdp/libvnc.so
dropin=/etc/systemd/system/xrdp.service.d/upstream-local.conf
if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script with sudo: sudo $0" >&2
    exit 1
fi
if [ ! -x "$daemon" ]; then
    echo "Missing optimized daemon $daemon" >&2
    echo "Build it first with $workspace_root/scripts/build-optimized-xrdp.sh" >&2
    exit 1
fi
if ! "$daemon" --version 2>/dev/null | grep -q '^xrdp 0\.10\.6\.1'; then
    echo "$daemon is not the expected xrdp 0.10.6.1 build" >&2
    exit 1
fi
if [ ! -f "$module" ]; then
    echo "Missing optimized module $module" >&2
    exit 1
fi
if ! strings "$daemon" | grep -q 'XRDP_VNC_GFX_PROFILE'; then
    echo "$daemon does not contain the VNC optimization profile" >&2
    exit 1
fi
if ! strings "$daemon" | grep -q "Disabling GFX as 'drdynvc' isn't available"; then
    echo "$daemon does not contain the GFX compatibility path" >&2
    exit 1
fi

stamp=$(date +%Y%m%d-%H%M%S)
backup="$dropin.xrdp-console-before-optimized-resize-$stamp"
candidate_dropin="$dropin.xrdp-console-optimized-resize-$stamp.new"

install -d -m 0755 "$(dirname "$dropin")"
cat >"$candidate_dropin" <<EOF
[Service]
ExecStart=
ExecStart=$daemon --nodaemon --config /etc/xrdp/xrdp.ini
EOF

if [ -f "$dropin" ] && cmp -s -- "$candidate_dropin" "$dropin"; then
    unlink "$candidate_dropin"
    echo "Optimized resize-fixed xrdp daemon is already active in the service drop-in."
    systemctl is-active --quiet xrdp
    systemctl --no-pager --full status xrdp | sed -n '1,18p'
    exit 0
fi

if [ -f "$dropin" ]; then
    cp -a -- "$dropin" "$backup"
else
    backup=""
fi
mv -- "$candidate_dropin" "$dropin"

if ! systemctl daemon-reload || ! systemctl restart xrdp ||
   ! systemctl is-active --quiet xrdp; then
    echo "Optimized xrdp did not remain active; restoring the previous drop-in" >&2
    if [ -n "$backup" ]; then
        cp -a -- "$backup" "$dropin"
    else
        unlink "$dropin"
    fi
    systemctl daemon-reload || true
    systemctl restart xrdp || true
    exit 1
fi

echo "Using optimized resize-fixed xrdp daemon: $daemon"
echo "Using matching VNC module: $module"
echo "The existing global/Console GFX-disabled settings were preserved."
echo "Optimizations and source remain under: $workspace_root"
if [ -n "$backup" ]; then
    echo "Backup: $backup"
fi
systemctl --no-pager --full status xrdp | sed -n '1,18p'
