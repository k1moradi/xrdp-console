#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Read-only snapshot of the direct-X11 xrdp service and its recent logs.

set -u

display=${XRDP_CONSOLE_DISPLAY:-:0}
display_number=${display#*:}
display_number=${display_number%%.*}
display_socket=/tmp/.X11-unix/X$display_number

printf 'display=%s\n' "$display"
if [ -S "$display_socket" ]; then
    printf 'display_socket=ready (%s)\n' "$display_socket"
else
    printf 'display_socket=missing (%s)\n' "$display_socket"
fi

printf '\n%s\n' '--- xrdp service ---'
systemctl --no-pager --full status xrdp.service 2>&1 || true

printf '\n%s\n' '--- console chansrv service ---'
systemctl --no-pager --full status xrdp-console-chansrv.service 2>&1 || true

printf '\n%s\n' '--- RDP listener ---'
ss -ltnp 'sport = :3389' 2>&1 || true

printf '\n%s\n' '--- established RDP clients ---'
ss -tnp state established 'sport = :3389' 2>&1 || true

printf '\n%s\n' '--- recent xrdp journal ---'
journalctl -u xrdp.service -n 80 --no-pager 2>&1 || true
