#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Restart the shared physical-console stack without restarting the display
# manager by default.  This intentionally drops active RDP/VNC connections.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
display=${XRDP_VNC_DISPLAY:-:0}
display_number=${display#*:}
display_number=${display_number%%.*}
desktop_socket=/tmp/.X11-unix/X$display_number
with_display_manager=0

usage()
{
    cat <<EOF
Usage: $0 [--with-display-manager]

Restart x11vnc and the xrdp console stack while preserving the logged-in
graphical session.  --with-display-manager is destructive: it restarts the
display manager and terminates the local graphical session.
EOF
}

die()
{
    echo "error: $*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --with-display-manager)
            with_display_manager=1
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown option: $1"
            ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    die "run this script as root, for example: sudo $0"
fi

for command in awk cat journalctl ps ss systemctl sleep; do
    command -v "$command" >/dev/null 2>&1 ||
        die "required command not found: $command"
done

unit_exists()
{
    systemctl cat "$1" >/dev/null 2>&1
}

unit_active()
{
    systemctl is-active --quiet "$1"
}

wait_active()
{
    unit=$1
    attempts=${2:-100}
    i=0
    while [ "$i" -lt "$attempts" ]; do
        if unit_active "$unit"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

wait_inactive()
{
    unit=$1
    attempts=${2:-100}
    i=0
    while [ "$i" -lt "$attempts" ]; do
        if ! unit_active "$unit"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

port_ready()
{
    port=$1
    ss -ltnH |
        awk -v wanted=":$port" '$4 ~ wanted "$" { found = 1 }
            END { exit !found }'
}

wait_port()
{
    port=$1
    attempts=${2:-100}
    i=0
    while [ "$i" -lt "$attempts" ]; do
        if port_ready "$port"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

xorg_pid()
{
    ps -eo pid=,args= |
        awk '$0 ~ /\/Xorg([[:space:]]|$)/ { print $1; exit }'
}

xorg_auth()
{
    pid=$1
    ps -p "$pid" -o args= |
        awk '{
            for (i = 1; i < NF; ++i) {
                if ($i == "-auth") {
                    print $(i + 1)
                    exit
                }
            }
        }'
}

xorg_vt()
{
    pid=$1
    ps -p "$pid" -o args= |
        awk '{
            for (i = 1; i < NF; ++i) {
                if ($i == "-vt") {
                    print $(i + 1)
                    exit
                }
            }
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^vt[0-9]+$/) {
                    print $i
                    exit
                }
            }
        }'
}

process_pid()
{
    pattern=$1
    ps -eo pid=,args= |
        awk -v pattern="$pattern" '$0 ~ pattern { print $1; exit }'
}

process_field()
{
    pid=$1
    field=$2
    ps -p "$pid" -o args= |
        awk -v wanted="$field" '
            {
                for (i = 1; i < NF; ++i) {
                    if ($i == wanted) {
                        print $(i + 1)
                        exit
                    }
                }
            }
        '
}

get_active_vt()
{
    cat /sys/class/tty/tty0/active 2>/dev/null || true
}

vt_number()
{
    case $1 in
        vt*) printf '%s' "${1#vt}" ;;
        tty*) printf '%s' "${1#tty}" ;;
        *) printf '%s' "$1" ;;
    esac
}

validate_xorg()
{
    pid=$(xorg_pid || true)
    [ -n "$pid" ] || return 1
    auth=$(xorg_auth "$pid" || true)
    [ -n "$auth" ] && [ -r "$auth" ] || return 1
    [ -S "$desktop_socket" ] || return 1
    printf '%s\t%s\t%s\n' "$pid" "$auth" "$(xorg_vt "$pid" || true)"
}

validate_x11vnc()
{
    expected_auth=$1
    pid=$(process_pid '/x11vnc([[:space:]]|$)' || true)
    [ -n "$pid" ] || return 1
    [ "$(process_field "$pid" -display)" = "$display" ] || return 1
    [ "$(process_field "$pid" -auth)" = "$expected_auth" ] || return 1
    [ "$(process_field "$pid" -rfbport)" = "5900" ] || return 1
}

diagnose()
{
    if [ -x "$script_dir/diagnose-console-stack.sh" ]; then
        "$script_dir/diagnose-console-stack.sh" >&2 || true
    fi
    for unit in x11vnc-console.service xrdp-console-chansrv.service \
                xrdp-sesman.service xrdp.service; do
        if unit_exists "$unit"; then
            systemctl --no-pager --full status "$unit" >&2 || true
            journalctl -u "$unit" -n 40 --no-pager >&2 || true
        fi
    done
}

restore_unit()
{
    unit=$1
    was_active=$2
    if [ "$was_active" = 1 ] && ! unit_active "$unit"; then
        systemctl start "$unit" >/dev/null 2>&1 || true
    fi
}

on_exit()
{
    status=$1
    if [ "$status" -eq 0 ] || [ "${recovery_started:-0}" -eq 0 ]; then
        return
    fi

    echo "error: recovery failed; attempting to restore services that were active before the run" >&2
    set +e
    restore_unit x11vnc-console.service "$x11vnc_was_active"
    restore_unit xrdp-sesman.service "$sesman_was_active"
    restore_unit xrdp.service "$xrdp_was_active"
    if [ "$chansrv_present" -eq 1 ]; then
        restore_unit xrdp-console-chansrv.service "$chansrv_was_active"
    fi
    diagnose
}

unit_exists x11vnc-console.service || die "x11vnc-console.service is not installed"
unit_exists xrdp-sesman.service || die "xrdp-sesman.service is not installed"
unit_exists xrdp.service || die "xrdp.service is not installed"

chansrv_present=0
if unit_exists xrdp-console-chansrv.service; then
    chansrv_present=1
fi

x11vnc_was_active=0
sesman_was_active=0
xrdp_was_active=0
chansrv_was_active=0
unit_active x11vnc-console.service && x11vnc_was_active=1 || true
unit_active xrdp-sesman.service && sesman_was_active=1 || true
unit_active xrdp.service && xrdp_was_active=1 || true
if [ "$chansrv_present" -eq 1 ]; then
    unit_active xrdp-console-chansrv.service && chansrv_was_active=1 || true
fi

if [ "$with_display_manager" -eq 1 ]; then
    echo "warning: restarting display-manager.service terminates the graphical session" >&2
    systemctl restart display-manager.service
    wait_active display-manager.service 200 || die "display manager did not become active"
fi

if ! xorg_state=$(validate_xorg); then
    die "could not resolve a live Xorg $display, Xauthority, and X socket"
fi
old_ifs=$IFS
IFS='	'
set -- $xorg_state
IFS=$old_ifs
xorg_pid_before=$1
auth_before=$2
xorg_vt_before=${3:-}

active_vt=$(get_active_vt)
if [ -n "$xorg_vt_before" ] && [ -n "$active_vt" ] &&
   [ "$(vt_number "$xorg_vt_before")" != "$(vt_number "$active_vt")" ]; then
    die "Xorg is on $xorg_vt_before but the active VT is $active_vt"
fi

trap 'on_exit "$?"' 0
recovery_started=1

echo "Using Xorg pid=$xorg_pid_before display=$display auth=$auth_before vt=${xorg_vt_before:-unknown}"

systemctl restart x11vnc-console.service
wait_active x11vnc-console.service || { diagnose; die "x11vnc-console.service did not become active"; }
wait_port 5900 || { diagnose; die "x11vnc did not open TCP port 5900"; }
validate_x11vnc "$auth_before" || {
    diagnose
    die "x11vnc is not using the live display, port, or Xauthority"
}

if [ "$chansrv_present" -eq 1 ] && [ "$chansrv_was_active" -eq 1 ]; then
    systemctl stop xrdp-console-chansrv.service
    wait_inactive xrdp-console-chansrv.service || {
        diagnose
        die "xrdp-console-chansrv.service did not stop"
    }
fi

systemctl stop xrdp.service
wait_inactive xrdp.service || { diagnose; die "xrdp.service did not stop"; }
systemctl stop xrdp-sesman.service
wait_inactive xrdp-sesman.service || {
    diagnose
    die "xrdp-sesman.service did not stop"
}

systemctl start xrdp-sesman.service
wait_active xrdp-sesman.service || {
    diagnose
    die "xrdp-sesman.service did not become active"
}
systemctl start xrdp.service
wait_active xrdp.service || { diagnose; die "xrdp.service did not become active"; }
wait_port 3389 || { diagnose; die "xrdp did not open TCP port 3389"; }

if [ "$chansrv_present" -eq 1 ] && [ "$chansrv_was_active" -eq 1 ]; then
    systemctl start xrdp-console-chansrv.service
    wait_active xrdp-console-chansrv.service || {
        diagnose
        die "xrdp-console-chansrv.service did not become active"
    }
fi

if ! xorg_state=$(validate_xorg); then
    diagnose
    die "Xorg changed or its Xauthority/socket disappeared during recovery"
fi
old_ifs=$IFS
IFS='	'
set -- $xorg_state
IFS=$old_ifs
[ "$1" = "$xorg_pid_before" ] || {
    diagnose
    die "Xorg PID changed without an explicit display-manager restart"
}
[ "${2:-}" = "$auth_before" ] || {
    diagnose
    die "Xorg Xauthority changed during recovery"
}
active_vt=$(get_active_vt)
if [ -n "${3:-}" ] && [ -n "$active_vt" ] &&
   [ "$(vt_number "${3:-}")" != "$(vt_number "$active_vt")" ]; then
    diagnose
    die "active VT changed from ${3:-unknown} to $active_vt"
fi
validate_x11vnc "$auth_before" || {
    diagnose
    die "x11vnc no longer points at the validated Xorg state"
}

recovery_started=0
trap - 0
echo "Console stack restarted successfully: x11vnc :0/$auth_before on 5900; xrdp on 3389"
