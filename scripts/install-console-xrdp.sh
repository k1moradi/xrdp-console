#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Install the generated patched xrdp build as the physical-console service.
# This deliberately keeps /etc/xrdp and the existing x11vnc service intact.

set -eu

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prefix=${XRDP_CONSOLE_XRDP_INSTALL_DIR:-$workspace_root/build/_deps/xrdp-install}
daemon=$prefix/sbin/xrdp
sesman=$prefix/sbin/xrdp-sesman
chansrv=$prefix/sbin/xrdp-chansrv
module=$prefix/lib/xrdp/libvnc.so
unit_dir=/etc/systemd/system
backup_root=/var/backups/xrdp-x11vnc
stamp=$(date +%Y%m%d-%H%M%S)
backup_dir=$backup_root/$stamp

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script in the authenticated terminal with: sudo $0" >&2
    exit 1
fi

if [ ! -x "$daemon" ] || [ ! -x "$sesman" ] || [ ! -x "$chansrv" ]; then
    echo "The patched xrdp build is incomplete under $prefix" >&2
    echo "Build it first with: $workspace_root/scripts/build-optimized-xrdp.sh" >&2
    exit 1
fi
if [ ! -f "$module" ]; then
    echo "Missing optimized VNC module: $module" >&2
    exit 1
fi
if ! "$daemon" --version 2>/dev/null | grep -q '^xrdp 0\.10\.6\.1'; then
    echo "$daemon is not the expected xrdp 0.10.6.1 build" >&2
    exit 1
fi
if [ ! -r /etc/xrdp/xrdp.ini ] || [ ! -r /etc/xrdp/sesman.ini ]; then
    echo "Expected existing configuration under /etc/xrdp was not found" >&2
    exit 1
fi
if ! awk '
    /^\[Console\]$/ { in_console = 1; next }
    /^\[/ { in_console = 0 }
    in_console && $0 == "lib=libvnc.so" { found_lib = 1 }
    in_console && $0 == "ip=127.0.0.1" { found_ip = 1 }
    in_console && $0 == "port=5900" { found_port = 1 }
    END { exit !(found_lib && found_ip && found_port) }
' /etc/xrdp/xrdp.ini; then
    echo "/etc/xrdp/xrdp.ini does not contain the expected x11vnc Console profile" >&2
    exit 1
fi

if dpkg-query -W -f='${Status}' xrdp 2>/dev/null | grep -q 'install ok installed'; then
    removal_plan=$(apt-get -s remove xrdp)
    removal_list=$(printf '%s\n' "$removal_plan" |
        sed -n '/^The following packages will be REMOVED:/,/^$/p')
    if printf '%s\n' "$removal_list" | grep -qw xorgxrdp; then
        echo "Refusing to remove xrdp because apt also selected xorgxrdp" >&2
        exit 1
    fi
else
    removal_plan=
    removal_list=
fi

mkdir -p "$backup_dir"
chmod 0700 "$backup_root" "$backup_dir"

backup_file()
{
    source=$1
    target=${source#/}
    if [ -e "$source" ] || [ -L "$source" ]; then
        mkdir -p "$backup_dir/$(dirname "$target")"
        cp -a -- "$source" "$backup_dir/$target"
    fi
}

restore_file()
{
    target=$1
    source=$backup_dir/${target#/}
    if [ -e "$source" ] || [ -L "$source" ]; then
        rm -f -- "$target"
        mkdir -p "$(dirname "$target")"
        cp -a -- "$source" "$target"
    else
        rm -f -- "$target"
    fi
}

write_unit()
{
    target=$1
    temporary=$(mktemp "$unit_dir/.xrdp-x11vnc.XXXXXX")
    cat >"$temporary"
    chmod 0644 "$temporary"
    install -m 0644 "$temporary" "$target"
    rm -f -- "$temporary"
}

wait_for_port()
{
    port=$1
    attempt=0
    while [ "$attempt" -lt 10 ]; do
        if ss -ltnH | awk -v wanted=":$port" \
            '$4 ~ wanted "$" { found = 1 } END { exit !found }'; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    return 1
}

xrdp_unit=$unit_dir/xrdp.service
sesman_unit=$unit_dir/xrdp-sesman.service
xrdp_dropin=$unit_dir/xrdp.service.d/upstream-local.conf
sesman_dropin=$unit_dir/xrdp-sesman.service.d/upstream-local.conf
xrdp_wanted=$unit_dir/multi-user.target.wants/xrdp.service
sesman_wanted=$unit_dir/multi-user.target.wants/xrdp-sesman.service

for path in \
    "$xrdp_unit" "$sesman_unit" "$xrdp_dropin" "$sesman_dropin" \
    "$xrdp_wanted" "$sesman_wanted" \
    /usr/local/sbin/xrdp /usr/local/sbin/xrdp-sesman \
    /usr/local/sbin/xrdp-chansrv; do
    backup_file "$path"
done

systemctl stop xrdp-console-chansrv.service 2>/dev/null || true
systemctl stop xrdp.service 2>/dev/null || true
systemctl stop xrdp-sesman.service 2>/dev/null || true
systemctl disable xrdp.service xrdp-sesman.service 2>/dev/null || true

# The existing drop-ins pointed at an older checkout and would override the
# complete units below. They are backed up above and intentionally removed.
rm -f -- "$xrdp_dropin" "$sesman_dropin" "$xrdp_wanted" "$sesman_wanted"
rmdir "$unit_dir/xrdp.service.d" "$unit_dir/xrdp-sesman.service.d" 2>/dev/null || true

# Keep the command-line daemons and the console chansrv helper on the same
# optimized build. Their compiled module/config paths remain in this stable
# checkout prefix, so the service units below use that prefix explicitly too.
install -d -m 0755 /usr/local/sbin
install -m 0755 "$daemon" /usr/local/sbin/xrdp
install -m 0755 "$sesman" /usr/local/sbin/xrdp-sesman
install -m 0755 "$chansrv" /usr/local/sbin/xrdp-chansrv

write_unit "$sesman_unit" <<EOF
[Unit]
Description=xrdp session manager (xrdp-x11vnc console)
Documentation=man:xrdp-sesman(8) man:sesman.ini(5)
After=network.target
StopWhenUnneeded=true
BindsTo=xrdp.service

[Service]
Type=exec
EnvironmentFile=-/etc/sysconfig/xrdp
EnvironmentFile=-/etc/default/xrdp
ExecStart=$sesman --nodaemon --config /etc/xrdp/sesman.ini
ExecReload=/bin/kill -HUP \$MAINPID

[Install]
WantedBy=multi-user.target
EOF

write_unit "$xrdp_unit" <<EOF
[Unit]
Description=xrdp daemon (xrdp-x11vnc physical console)
Documentation=man:xrdp(8) man:xrdp.ini(5)
Requires=xrdp-sesman.service
After=network-online.target xrdp-sesman.service

[Service]
Type=exec
EnvironmentFile=-/etc/sysconfig/xrdp
EnvironmentFile=-/etc/default/xrdp
ExecStart=$daemon --nodaemon --config /etc/xrdp/xrdp.ini
SystemCallArchitectures=native
SystemCallFilter=@system-service

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable xrdp-sesman.service xrdp.service
systemctl start xrdp.service

if ! systemctl is-active --quiet xrdp.service; then
    echo "The optimized xrdp service did not start; restoring the previous local state" >&2
    systemctl --no-pager --full status xrdp.service >&2 || true
    journalctl -u xrdp.service -n 40 --no-pager >&2 || true
    systemctl stop xrdp.service xrdp-sesman.service 2>/dev/null || true
    rm -f -- "$xrdp_unit" "$sesman_unit"
    restore_file "$xrdp_dropin"
    restore_file "$sesman_dropin"
    restore_file "$xrdp_wanted"
    restore_file "$sesman_wanted"
    restore_file /usr/local/sbin/xrdp
    restore_file /usr/local/sbin/xrdp-sesman
    restore_file /usr/local/sbin/xrdp-chansrv
    systemctl daemon-reload || true
    systemctl start xrdp.service 2>/dev/null || true
    exit 1
fi

if [ -n "$removal_plan" ]; then
    echo "Removing only the distro xrdp package; xorgxrdp is left installed."
    DEBIAN_FRONTEND=noninteractive apt-get remove -y xrdp
fi

systemctl daemon-reload
systemctl enable xrdp-sesman.service xrdp.service
systemctl restart xrdp.service

if ! systemctl is-active --quiet xrdp.service; then
    echo "The own xrdp service failed after package removal" >&2
    systemctl --no-pager --full status xrdp.service >&2 || true
    journalctl -u xrdp.service -n 40 --no-pager >&2 || true
    exit 1
fi

systemctl restart xrdp-console-chansrv.service 2>/dev/null || true
if ! wait_for_port 3389; then
    echo "xrdp is active but no TCP listener appeared on port 3389" >&2
    systemctl --no-pager --full status xrdp.service >&2 || true
    journalctl -u xrdp.service -n 40 --no-pager >&2 || true
    exit 1
fi

echo "Installed the optimized xrdp-x11vnc console service."
echo "  daemon:  $daemon"
echo "  module:  $module"
echo "  config:  /etc/xrdp/xrdp.ini (preserved)"
echo "  x11vnc:  127.0.0.1:5900 (existing service preserved)"
echo "  xrdp:    0.0.0.0:3389"
echo "  xorgxrdp: left installed but not used by the Console autorun profile"
echo "  backup:  $backup_dir"
systemctl --no-pager --full status xrdp.service | sed -n '1,18p'
