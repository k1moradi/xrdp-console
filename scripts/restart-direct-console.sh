#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Restart direct-console services without touching the physical X11 session.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root, for example: sudo $0" >&2
    exit 1
fi

established=$(ss -tnH state established 'sport = :3389') || {
    echo "Could not inspect RDP connections; refusing to restart services" >&2
    exit 1
}
if [ -n "$established" ]; then
    echo "An RDP client is connected. Disconnect it before restarting xrdp." >&2
    exit 1
fi

systemctl restart xrdp.service
systemctl is-active --quiet xrdp.service
systemctl restart xrdp-console-chansrv.service
systemctl is-active --quiet xrdp-console-chansrv.service

attempt=0
while [ "$attempt" -lt 50 ]; do
    if ss -ltnH 'sport = :3389' | grep -q .; then
        echo "Direct-console services are active; xrdp is listening on port 3389."
        exit 0
    fi
    attempt=$((attempt + 1))
    sleep 0.2
done

echo "xrdp restarted but no port 3389 listener appeared." >&2
journalctl -u xrdp.service -n 60 --no-pager >&2 || true
exit 1
