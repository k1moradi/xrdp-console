#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Activate the first-party XCB/XDamage/XShm module on the host xrdp service.

set -eu

usage()
{
    cat <<EOF
Usage:
  sudo $0
  sudo $0 --rollback BACKUP_DIRECTORY

Activation preserves the existing RDP listener on port 3389. It installs the
locally built direct-X11 module and pinned chansrv binary, enables the module's
text clipboard and dynamic-resize channels, and restarts xrdp and the console
chansrv service. A root-only backup is printed for explicit rollback.
EOF
}

fail()
{
    echo "activate-direct-console: $*" >&2
    exit 1
}

wait_for_rdp_listener()
{
    attempts=0
    while [ "$attempts" -lt 50 ]; do
        listener=$(ss -ltnH 'sport = :3389') || listener=
        if [ -n "$listener" ]; then
            return 0
        fi
        sleep 0.2
        attempts=$((attempts + 1))
    done
    return 1
}

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_BUILD_DIR:-$workspace_root/build}
prefix=${XRDP_CONSOLE_XRDP_INSTALL_DIR:-$build_root/_deps/xrdp-install}
daemon=$prefix/sbin/xrdp
chansrv_source=$prefix/sbin/xrdp-chansrv
chansrv_target=/usr/local/sbin/xrdp-chansrv
module_source=$build_root/src/libxrdp_console.so
module_target=$prefix/lib/xrdp/libxrdp_console.so
revision_header=$build_root/generated/build_revision.h
config=/etc/xrdp/xrdp.ini
dropin_directory=/etc/systemd/system/xrdp.service.d
dropin=$dropin_directory/upstream-local.conf
backup_root=/var/backups/xrdp-x11vnc

rollback()
{
    backup_directory=$1
    [ -d "$backup_directory" ] || fail "missing backup directory: $backup_directory"
    [ -f "$backup_directory/xrdp.ini" ] || fail "backup has no xrdp.ini"
    [ -f "$backup_directory/chansrv-existed" ] ||
        fail "backup predates the chansrv snapshot; refusing a partial rollback"
    [ -e "$backup_directory/xrdp-chansrv" ] ||
        fail "backup is missing the previous chansrv binary"

    cp -a -- "$backup_directory/xrdp.ini" "$config"
    if [ -f "$backup_directory/module-existed" ]; then
        [ -e "$backup_directory/libxrdp_console.so" ] ||
            fail "backup is missing the previous module"
        rm -f -- "$module_target"
        cp -a -- "$backup_directory/libxrdp_console.so" "$module_target"
    else
        rm -f -- "$module_target"
    fi

    rm -f -- "$chansrv_target"
    cp -a -- "$backup_directory/xrdp-chansrv" "$chansrv_target"

    if [ -f "$backup_directory/dropin-existed" ]; then
        [ -f "$backup_directory/upstream-local.conf" ] ||
            fail "backup is missing the previous service drop-in"
        install -d -m 0755 "$dropin_directory"
        cp -a -- "$backup_directory/upstream-local.conf" "$dropin"
    else
        rm -f -- "$dropin"
    fi

    systemctl daemon-reload
    systemctl restart xrdp || fail "rollback restored files but xrdp restart failed"
    systemctl is-active --quiet xrdp || fail "xrdp is not active after rollback"
    wait_for_rdp_listener || fail "xrdp did not restore its port 3389 listener"
    systemctl restart xrdp-console-chansrv.service ||
        fail "rollback restored files but chansrv restart failed"
    systemctl is-active --quiet xrdp-console-chansrv.service ||
        fail "console chansrv is not active after rollback"
    echo "Restored xrdp, chansrv, configuration, module, and service drop-in from: $backup_directory"
    echo "The RDP listener remains configured for port 3389."
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

if [ "${1:-}" = "--rollback" ]; then
    [ "$#" -eq 2 ] || { usage >&2; exit 2; }
    [ "$(id -u)" -eq 0 ] || fail "run rollback as root (for example, with sudo)"
    rollback "$2"
    exit 0
fi

[ "$#" -eq 0 ] || { usage >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || fail "run as root (for example, with sudo)"
[ -x "$daemon" ] || fail "missing pinned xrdp daemon: $daemon"
[ -x "$chansrv_source" ] || fail "missing pinned xrdp chansrv: $chansrv_source"
[ -e "$chansrv_target" ] || fail "missing installed xrdp chansrv: $chansrv_target"
[ -f "$module_source" ] || fail "missing direct-X11 module: $module_source"
[ -r "$revision_header" ] ||
    fail "missing generated build identity: $revision_header"
[ -f "$config" ] || fail "missing xrdp configuration: $config"
[ -d "$(dirname -- "$module_target")" ] ||
    fail "missing xrdp module directory: $(dirname -- "$module_target")"
"$daemon" --version 2>/dev/null | grep -q '^xrdp 0\.10\.6\.1' ||
    fail "candidate daemon is not the pinned xrdp 0.10.6.1 build"
if ! grep -aFq -- 'XRDP_CONSOLE_GFX_PLANAR_BATCH_V1' "$daemon"; then
    fail "candidate daemon lacks the Console Planar batching patch marker"
fi
if ldd "$module_source" 2>/dev/null | grep -q 'not found'; then
    fail "direct-X11 module has an unresolved shared-library dependency"
fi
if ldd "$chansrv_source" 2>/dev/null | grep -q 'not found'; then
    fail "pinned xrdp chansrv has an unresolved shared-library dependency"
fi
build_revision=$(sed -n \
    's/^#define XRDP_CONSOLE_BUILD_REVISION "\(.*\)"$/\1/p' \
    "$revision_header")
[ -n "$build_revision" ] ||
    fail "could not read the generated build revision"
if ! grep -aFq -- "$build_revision" "$module_source"; then
    fail "module does not contain expected build revision $build_revision"
fi

systemctl is-active --quiet xrdp || fail "xrdp.service must be active before activation"
systemctl is-active --quiet xrdp-console-chansrv.service ||
    fail "xrdp-console-chansrv.service must be active before activation"
service_environment=$(systemctl show xrdp.service -p Environment --value) ||
    fail "could not inspect the xrdp service environment"
case " $service_environment " in
    *" DISPLAY=:0 "*) ;;
    *) fail "xrdp.service must export DISPLAY=:0 for the physical console" ;;
esac
xauthority=$(printf '%s\n' "$service_environment" |
    tr ' ' '\n' | sed -n 's/^XAUTHORITY=//p')
[ -n "$xauthority" ] && [ -r "$xauthority" ] ||
    fail "the xrdp service XAUTHORITY file is missing or unreadable"
listener=$(ss -ltnH 'sport = :3389') ||
    fail "could not inspect the xrdp listener"
[ -n "$listener" ] ||
    fail "xrdp is not listening on port 3389; refusing to alter the listener"
established=$(ss -tnH state established 'sport = :3389') ||
    fail "could not inspect established RDP connections"
[ -z "$established" ] ||
    fail "an RDP client is connected; disconnect clients before activation"

# Validate the current listener before making a backup or changing files.
python3 - "$config" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
section = ""
ports = []
for line in path.read_text(encoding="utf-8").splitlines():
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped[1:-1].strip().lower()
    elif section == "globals" and "=" in line and not stripped.startswith(("#", ";")):
        key, value = line.split("=", 1)
        if key.strip().lower() == "port":
            ports.append(value.strip())
if ports != ["3389"]:
    raise SystemExit(f"expected exactly one [Globals] port=3389, found {ports!r}")
PY

stamp=$(date +%Y%m%d-%H%M%S)
install -d -m 0700 "$backup_root"
backup_directory=$backup_root/direct-console-$stamp
mkdir -m 0700 "$backup_directory"
cp -a -- "$config" "$backup_directory/xrdp.ini"
if [ -e "$module_target" ] || [ -L "$module_target" ]; then
    cp -a -- "$module_target" "$backup_directory/libxrdp_console.so"
    : >"$backup_directory/module-existed"
fi
cp -a -- "$chansrv_target" "$backup_directory/xrdp-chansrv"
: >"$backup_directory/chansrv-existed"
if [ -e "$dropin" ]; then
    cp -a -- "$dropin" "$backup_directory/upstream-local.conf"
    : >"$backup_directory/dropin-existed"
fi
echo "Rollback backup: $backup_directory"

activation_finalized=0
rollback_failed_activation()
{
    result=$?
    if [ "$result" -ne 0 ] && [ "$activation_finalized" -eq 0 ]; then
        trap - EXIT HUP INT TERM
        echo "Activation failed; restoring the previous xrdp state." >&2
        rollback "$backup_directory" ||
            echo "Automatic rollback failed; backup remains at $backup_directory" >&2
    fi
}
trap rollback_failed_activation EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

install -m 0755 "$chansrv_source" "$chansrv_target"
install -m 0755 "$module_source" "$module_target"

if ! python3 - "$config" <<'PY'
import os
import stat
import sys
import tempfile
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
lines = text.splitlines(keepends=True)
updates = {
    "globals": {
        "allow_channels": "true",
        "max_bpp": "32",
        "use_fastpath": "both",
    },
    "channels": {
        "cliprdr": "true",
        "drdynvc": "true",
    },
    "console": {
        "name": "Physical desktop (direct X11)",
        "lib": "libxrdp_console.so",
        "code": "21",
        "display": ":0",
        "channel.rdpdr": "false",
        "channel.rdpsnd": "false",
        "channel.cliprdr": "true",
        "channel.rail": "false",
        "channel.xrdpvr": "false",
        "channel.drdynvc": "true",
        "enable_dynamic_resizing": "true",
    },
}

sections = []
section = ""
for line in lines:
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped[1:-1].strip().lower()
        sections.append(section)
for name in updates:
    if sections.count(name) != 1:
        raise SystemExit(f"expected exactly one [{name}] section")

def port_values(source_lines):
    current = ""
    values = []
    for source_line in source_lines:
        stripped = source_line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1].strip().lower()
        elif current == "globals" and "=" in source_line and not stripped.startswith(("#", ";")):
            key, value = source_line.split("=", 1)
            if key.strip().lower() == "port":
                values.append(value.strip())
    return values

if port_values(lines) != ["3389"]:
    raise SystemExit("refusing to change an xrdp listener other than port 3389")

result = []
current_section = ""
seen = set()
for line in lines:
    stripped = line.strip()
    is_header = stripped.startswith("[") and stripped.endswith("]")
    if is_header:
        if current_section in updates:
            for key, value in updates[current_section].items():
                if key not in seen:
                    result.append(f"{key}={value}\n")
        current_section = stripped[1:-1].strip().lower()
        seen = set()
        result.append(line)
        continue

    if current_section in updates and "=" in line and not stripped.startswith(("#", ";")):
        key = line.split("=", 1)[0].strip().lower()
        if key in updates[current_section]:
            if key not in seen:
                ending = "\n" if line.endswith("\n") else ""
                result.append(f"{key}={updates[current_section][key]}{ending}")
                seen.add(key)
            continue
    result.append(line)

if current_section in updates:
    for key, value in updates[current_section].items():
        if key not in seen:
            result.append(f"{key}={value}\n")

new_text = "".join(result)
new_lines = new_text.splitlines()
section = ""
observed = {name: {} for name in updates}
for line in new_lines:
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped[1:-1].strip().lower()
    elif section in updates and "=" in line and not stripped.startswith(("#", ";")):
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if key in updates[section]:
            observed[section].setdefault(key, []).append(value.strip())
for name, expected in updates.items():
    for key, value in expected.items():
        if observed[name].get(key) != [value]:
            raise SystemExit(f"configuration verification failed for [{name}] {key}")
if port_values(new_text.splitlines()) != ["3389"]:
    raise SystemExit("configuration verification failed: listener port changed")

metadata = path.stat()
descriptor, temporary = tempfile.mkstemp(prefix=".xrdp.ini.", dir=str(path.parent))
try:
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
        stream.write(new_text)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.chown(temporary, metadata.st_uid, metadata.st_gid)
    os.replace(temporary, path)
    directory_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)
except Exception:
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass
    raise
PY
then
    fail "could not safely update xrdp.ini"
fi

install -d -m 0755 "$dropin_directory"
temporary_dropin=$(mktemp "$dropin_directory/.upstream-local.XXXXXX")
if ! printf '[Service]\nExecStart=\nExecStart=%s --nodaemon --config /etc/xrdp/xrdp.ini\n' \
    "$daemon" >"$temporary_dropin" ||
   ! chmod 0644 "$temporary_dropin" ||
   ! mv -f -- "$temporary_dropin" "$dropin"; then
    rm -f -- "$temporary_dropin"
    fail "could not write the pinned xrdp service override"
fi

systemctl daemon-reload || fail "systemd daemon-reload failed"
systemctl restart xrdp || fail "the pinned xrdp daemon failed to start"
systemctl is-active --quiet xrdp || fail "the pinned xrdp service is not active"
active_exec=$(systemctl show xrdp.service -p ExecStart --value) ||
    fail "could not inspect the active xrdp command"
case "$active_exec" in
    *"$daemon"*) ;;
    *) fail "systemd is not running the pinned candidate daemon" ;;
esac
wait_for_rdp_listener || fail "xrdp did not return to port 3389 within 10 seconds"
systemctl restart xrdp-console-chansrv.service ||
    fail "the pinned xrdp chansrv failed to restart"
systemctl is-active --quiet xrdp-console-chansrv.service ||
    fail "the pinned xrdp chansrv service is not active"

if ! cmp -s -- "$module_source" "$module_target"; then
    fail "installed module differs from the tested build artifact"
fi
if ! cmp -s -- "$chansrv_source" "$chansrv_target"; then
    fail "installed chansrv differs from the tested build artifact"
fi

activation_finalized=1
echo "Activated the first-party direct-X11 module with pinned xrdp 0.10.6.1."
echo "Installed the matching pinned chansrv binary for text clipboard support."
echo "RDP listener preserved on port 3389."
echo "Enabled Console text clipboard, dynamic virtual channels, and presentation resizing."
echo "RemoteFX is negotiated only when the client supports the module's standard RFX path; otherwise classic bitmap remains available."
echo "Rollback: sudo $0 --rollback $backup_directory"
systemctl --no-pager --full status xrdp | sed -n '1,18p'
