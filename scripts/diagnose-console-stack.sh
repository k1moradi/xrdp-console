#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Emit a read-only, machine-readable-ish snapshot of the physical-console
# sharing stack.  This intentionally does not restart services or touch X11.

set -eu

display=${XRDP_VNC_DISPLAY:-:0}
display_number=${display#*:}
display_number=${display_number%%.*}
desktop_socket=/tmp/.X11-unix/X$display_number

command_value()
{
    pid=$1
    if [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ]; then
        tr '\0' ' ' <"/proc/$pid/cmdline" | sed 's/[[:space:]]*$//'
    else
        printf '%s' unknown
    fi
}

process_field()
{
    pid=$1
    field=$2
    if [ -z "$pid" ]; then
        printf '%s' unknown
        return 0
    fi

    ps -p "$pid" -o args= 2>/dev/null |
        awk -v wanted="$field" '
            {
                for (i = 1; i < NF; ++i) {
                    if ($i == wanted) {
                        print $(i + 1)
                        exit
                    }
                }
            }
        ' || true
}

unit_state()
{
    systemctl is-active "$1" 2>/dev/null || true
}

unit_main_pid()
{
    systemctl show "$1" -p MainPID --value 2>/dev/null || true
}

port_state()
{
    port=$1
    if ss -ltnH 2>/dev/null |
        awk -v wanted=":$port" '$4 ~ wanted "$" { found = 1 }
            END { exit !found }'
    then
        printf '%s' ready
    else
        printf '%s' missing
    fi
}

port_owner()
{
    port=$1
    owner=$(ss -ltnp 2>/dev/null |
        awk -v wanted=":$port" '$4 ~ wanted "$" && $NF ~ /^users:/ {
            print $NF
            exit
        }' || true)
    if [ -n "$owner" ]; then
        printf '%s' "$owner"
    else
        printf '%s' unknown
    fi
}

auth_metadata()
{
    prefix=$1
    path=$2
    if [ -n "$path" ] && [ -e "$path" ]; then
        metadata=$(stat -c '%i %s %Y %a' -- "$path" 2>/dev/null || true)
        if [ -n "$metadata" ]; then
            set -- $metadata
            printf '%s.inode=%s\n' "$prefix" "$1"
            printf '%s.size=%s\n' "$prefix" "$2"
            printf '%s.mtime=%s\n' "$prefix" "$3"
            printf '%s.mode=%s\n' "$prefix" "$4"
            return 0
        fi
    fi
    printf '%s.inode=unknown\n' "$prefix"
    printf '%s.size=unknown\n' "$prefix"
    printf '%s.mtime=unknown\n' "$prefix"
    printf '%s.mode=unknown\n' "$prefix"
}

xorg_pid=$(ps -eo pid=,args= 2>/dev/null |
    awk '$0 ~ /\/Xorg([[:space:]]|$)/ { print $1; exit }' || true)
xorg_auth=$(process_field "$xorg_pid" -auth)
xorg_vt=$(ps -p "$xorg_pid" -o args= 2>/dev/null |
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
    }' || true)

x11vnc_pid=$(ps -eo pid=,args= 2>/dev/null |
    awk '$0 ~ /\/x11vnc([[:space:]]|$)/ { print $1; exit }' || true)
x11vnc_display=$(process_field "$x11vnc_pid" -display)
x11vnc_auth=$(process_field "$x11vnc_pid" -auth)
x11vnc_port=$(process_field "$x11vnc_pid" -rfbport)

active_vt=$(cat /sys/class/tty/tty0/active 2>/dev/null || true)
if [ -z "$active_vt" ]; then
    active_vt=$(fgconsole 2>/dev/null || true)
fi

if [ -S "$desktop_socket" ]; then
    socket_state=ready
else
    socket_state=missing
fi

sessions=$(loginctl list-sessions --no-legend 2>/dev/null |
    awk 'BEGIN { first = 1 }
         {
             if (!first) printf ";"
             printf "%s:%s:%s", $1, $2, $3
             first = 0
         }' || true)

printf '%s\n' "xorg.pid=${xorg_pid:-unknown}"
printf '%s\n' "xorg.display=$display"
printf '%s\n' "xorg.vt=${xorg_vt:-unknown}"
printf '%s\n' "xorg.command=$(command_value "$xorg_pid")"
printf '%s\n' "xorg.auth=${xorg_auth:-unknown}"
auth_metadata xorg.auth "${xorg_auth:-}"
printf '%s\n' "x11vnc.pid=${x11vnc_pid:-unknown}"
printf '%s\n' "x11vnc.display=$x11vnc_display"
printf '%s\n' "x11vnc.auth=$x11vnc_auth"
printf '%s\n' "x11vnc.port=$x11vnc_port"
printf '%s\n' "x11vnc.command=$(command_value "$x11vnc_pid")"
auth_metadata x11vnc.auth "${x11vnc_auth:-}"
printf '%s\n' "port5900=$(port_state 5900)"
printf '%s\n' "port5900.owner=$(port_owner 5900)"
printf '%s\n' "port3389=$(port_state 3389)"
printf '%s\n' "port3389.owner=$(port_owner 3389)"
printf '%s\n' "active_vt=${active_vt:-unknown}"
printf '%s\n' "display_manager=$(unit_state display-manager.service)"
printf '%s\n' "x11vnc_console=$(unit_state x11vnc-console.service)"
printf '%s\n' "xrdp=$(unit_state xrdp.service)"
printf '%s\n' "xrdp_sesman=$(unit_state xrdp-sesman.service)"
printf '%s\n' "xrdp_console_chansrv=$(unit_state xrdp-console-chansrv.service)"
printf '%s\n' "x11vnc.main_pid=$(unit_main_pid x11vnc-console.service)"
printf '%s\n' "xrdp.main_pid=$(unit_main_pid xrdp.service)"
printf '%s\n' "xrdp_sesman.main_pid=$(unit_main_pid xrdp-sesman.service)"
printf '%s\n' "xrdp_console_chansrv.main_pid=$(unit_main_pid xrdp-console-chansrv.service)"
printf '%s\n' "desktop_socket=$desktop_socket"
printf '%s\n' "desktop_socket_state=$socket_state"
printf '%s\n' "sessions=${sessions:-unknown}"
