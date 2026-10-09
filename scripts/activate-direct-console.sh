#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Activate the first-party XCB/XDamage/XShm module on the host xrdp service.

set -eu

usage()
{
    cat <<EOF
Usage:
  sudo $0
  sudo $0 --backup
  sudo $0 --preflight
  sudo $0 --preflight --backup
  sudo $0 --prepare-test-runtime  # only repair known stale runtime prerequisites
  sudo $0 --rollback BACKUP_DIRECTORY

Activation preserves the RDP listener configuration on port 3389. It can
repair a stopped direct-console installation and can install the pinned
chansrv binary when no previous /usr/local/sbin/xrdp-chansrv exists. It also
migrates legacy xrdp-x11vnc daemon/sesman ExecStart paths to the matching
tested xrdp-console build. The xrdp.service, xrdp-sesman.service, and
xrdp-console-chansrv.service unit definitions must already exist; only the
physical-session chansrv user/environment policy remains host-specific.

Use --preflight for the same read-only artifact, configuration, environment,
and connected-client checks without changing files or service state.

Client scaled-output, scroll-reuse, and verified bitmap caching are
requested by default, subject to negotiated capabilities and per-path safety checks.
Cache observation is diagnostic-only and opt-in. Set an individual
XRDP_CONSOLE_CLIENT_* variable to exactly 0 in the xrdp service environment
to disable that path. A rollback backup is created automatically before production changes,
preserving the previous configuration, binaries, service overrides, and
service state. --backup is retained for backwards-compatible invocation.
EOF
}

fail()
{
    echo "activate-direct-console: $*" >&2
    exit 1
}

service_state()
{
    systemctl is-active "$1" 2>/dev/null || true
}

service_exists()
{
    load_state=$(systemctl show "$1" -p LoadState --value 2>/dev/null) ||
        return 1
    [ "$load_state" = "loaded" ]
}

fail_service()
{
    failed_service=$1
    shift
    echo "activate-direct-console: service failure: $failed_service" >&2
    systemctl --no-pager --full status "$failed_service" >&2 || true
    journalctl -u "$failed_service" -b -n 40 --no-pager >&2 || true
    fail "$*"
}

wait_for_rdp_listener()
{
    attempts=0
    while [ "$attempts" -lt 50 ]; do
        service_pid=$(systemctl show xrdp.service -p MainPID --value 2>/dev/null) ||
            service_pid=
        case "$service_pid" in
            ''|0|*[!0-9]*) ;;
            *)
                listener=$(ss -ltnpH 'sport = :3389') || listener=
                if printf '%s\n' "$listener" |
                    grep -Eq "pid=${service_pid}([,)]|$)"; then
                    return 0
                fi
                ;;
        esac
        sleep 0.2
        attempts=$((attempts + 1))
    done
    return 1
}

workspace_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${XRDP_CONSOLE_BUILD_DIR:-$workspace_root/build-direct-console}
prefix=${XRDP_CONSOLE_XRDP_INSTALL_DIR:-$build_root/_deps/xrdp-install}
daemon=$prefix/sbin/xrdp
sesman=$prefix/sbin/xrdp-sesman
chansrv_source=$prefix/sbin/xrdp-chansrv
chansrv_target=/usr/local/sbin/xrdp-chansrv
module_source=$build_root/src/libxrdp_console.so
module_target=$prefix/lib/xrdp/libxrdp_console.so
revision_header=$build_root/generated/build_revision.h
config=/etc/xrdp/xrdp.ini
sesman_config=/etc/xrdp/sesman.ini
dropin_directory=/etc/systemd/system/xrdp.service.d
dropin=$dropin_directory/upstream-local.conf
sesman_dropin_directory=/etc/systemd/system/xrdp-sesman.service.d
sesman_dropin=$sesman_dropin_directory/upstream-local.conf
backup_root=/var/backups/xrdp-console

prepare_test_runtime()
{
    # BEGIN TEST-RUNTIME-RECOVERY-PYTHON: test-only bootstrap, no separate script.
    python3 - <<'PY_RUNTIME'
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reversibly disable two stale xrdp systemd ExecStart overrides.

Default is read-only. --apply requires root and verified persistent base unit
executables. Does not install, replace, start, restart, or stop any service.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Callable

UNITS = ("xrdp.service", "xrdp-sesman.service")
EXPECTED = {"xrdp.service": "xrdp", "xrdp-sesman.service": "xrdp-sesman"}
ETC_SYSTEMD = Path("/etc/systemd/system")
BACKUP_ROOT = Path("/var/backups/xrdp-console/service-repair")
BASE_DIRECTORY = Path("/opt/xrdp-console/sbin")
EXEC_RE = re.compile(r"(?:^|[\s;{])path=(/[^\s;}]*)")


class RepairError(RuntimeError):
    pass


def run(args: list[str]) -> str:
    process = subprocess.run(args, capture_output=True, text=True, check=False, timeout=10)
    if process.returncode != 0:
        raise RepairError(f"{' '.join(args)} failed: {process.stderr.strip()[:300]}")
    return process.stdout.strip()


def path_in_temp(path: Path) -> bool:
    try:
        resolved = path.resolve(strict=False)
    except (OSError, RuntimeError):
        return False
    return any(resolved == d or d in resolved.parents for d in (Path('/tmp'), Path('/var/tmp')))


def exec_from_show(value: str) -> Path:
    match = EXEC_RE.search(value)
    if not match:
        raise RepairError(f"Cannot identify systemd ExecStart binary: {value[:200]!r}")
    return Path(match.group(1))


def exec_from_unit(content: str) -> Path:
    section = None
    values: list[str] = []
    for line in content.splitlines():
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            section = s.lower()
        elif section == '[service]' and s.startswith('ExecStart='):
            values.append(s.split('=', 1)[1].strip())
    values = [value for value in values if value]
    if len(values) != 1:
        raise RepairError('Expected exactly one nonempty base ExecStart')
    path = values[0].split()[0]
    if not path.startswith('/'):
        raise RepairError('Base ExecStart is not an absolute path')
    return Path(path)


def require_generated_override(unit: str, content: str, binary: Path) -> None:
    """Only disable the exact drop-in structure generated by our activator."""
    if unit == 'xrdp.service':
        expected = ['[Unit]', 'Requires=xrdp-sesman.service',
                    'After=xrdp-sesman.service', '[Service]', 'ExecStart=',
                    f'ExecStart={binary} --nodaemon --config /etc/xrdp/xrdp.ini']
    else:
        expected = ['[Service]', 'ExecStart=',
                    f'ExecStart={binary} --nodaemon --config /etc/xrdp/sesman.ini']
    actual = [line.strip() for line in content.splitlines()
              if line.strip() and not line.lstrip().startswith(('#', ';'))]
    if actual != expected:
        raise RepairError(f'Unexpected content in {unit} upstream-local.conf; manual review required')


def check_candidate(unit: str, base_file: Path, dropin: Path,
                    effective_value: str) -> dict[str, str | bool]:
    name = EXPECTED[unit]
    if base_file.is_symlink() or not base_file.is_file():
        raise RepairError(f'Unexpected base unit file: {base_file}')
    base_exec = exec_from_unit(base_file.read_text(encoding='utf-8'))
    if base_exec != BASE_DIRECTORY / name:
        raise RepairError(f'Base {unit} unexpectedly executes {base_exec}, not {BASE_DIRECTORY / name}')
    if path_in_temp(base_exec):
        raise RepairError(f'Persistent base executable resolves into an unsafe temp path: {base_exec}')
    if not base_exec.is_file() or not os.access(base_exec, os.X_OK):
        raise RepairError(f'Persistent base executable missing or non-executable: {base_exec}')
    current_exec = exec_from_show(effective_value)
    if not dropin.exists():
        if current_exec != base_exec:
            raise RepairError(f'Without upstream-local.conf, {unit} has an unexpected ExecStart: {current_exec}')
        return {'unit': unit, 'status': 'healthy-base', 'base': str(base_exec),
                'current': str(current_exec), 'override': str(dropin), 'repair': False}
    if not dropin.is_file() or dropin.is_symlink():
        raise RepairError(f'Expected a normal upstream-local.conf override: {dropin}')
    current_in_dropin = exec_from_unit(dropin.read_text(encoding='utf-8'))
    if current_in_dropin != current_exec:
        raise RepairError(f'Effective ExecStart does not match expected override for {unit}')
    if current_exec == base_exec:
        return {'unit': unit, 'status': 'already-stable', 'base': str(base_exec),
                'current': str(current_exec), 'override': str(dropin), 'repair': False}
    if not path_in_temp(current_exec) or current_exec.exists():
        raise RepairError(f'Refusing unknown/still-existing ExecStart for {unit}: {current_exec}')
    require_generated_override(unit, dropin.read_text(encoding='utf-8'), current_exec)
    return {'unit': unit, 'status': 'stale-temporary-exec', 'base': str(base_exec),
            'current': str(current_exec), 'override': str(dropin), 'repair': True}


def inspect(systemctl: Callable[[list[str]], str] = run,
            base_dir: Path = ETC_SYSTEMD) -> list[dict[str, str | bool]]:
    result = []
    for unit in UNITS:
        base_file = base_dir / unit
        dropin = base_dir / (unit + '.d') / 'upstream-local.conf'
        # Other drop-ins may set DISPLAY, XAUTHORITY, or hardening policy.
        # Check the effective ExecStart before and after the targeted edit.
        effective = systemctl(['systemctl', 'show', '-p', 'ExecStart', '--value', unit])
        result.append(check_candidate(unit, base_file, dropin, effective))
    return result


def apply(plan: list[dict[str, str | bool]], *,
          systemctl: Callable[[list[str]], str] = run,
          backup_root: Path = BACKUP_ROOT) -> Path:
    changes = [p for p in plan if p['repair']]
    if not changes:
        raise RepairError('No stale overrides in plan; no repair needed')
    backup_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(backup_root, 0o700)
    backup = Path(tempfile.mkdtemp(prefix='service-path-repair-', dir=backup_root))
    renamed: list[tuple[Path, Path]] = []
    try:
        manifest = {'schema': 1, 'units': []}
        for entry in changes:
            source = Path(str(entry['override']))
            data = source.read_bytes()
            target = backup / (str(entry['unit']) + '.upstream-local.conf')
            target.write_bytes(data)
            target.chmod(0o600)
            manifest['units'].append({'unit': entry['unit'], 'source': str(source),
                'sha256': hashlib.sha256(data).hexdigest(), 'backup': str(target)})
        (backup / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        (backup / 'manifest.json').chmod(0o600)
        # All backups are durable before changing unit configuration.
        for entry in changes:
            source = Path(str(entry['override']))
            disabled = source.with_name('upstream-local.conf.disabled-' + backup.name)
            if disabled.exists():
                raise RepairError(f'Disabled file collision: {disabled}')
            source.rename(disabled)
            renamed.append((source, disabled))
        systemctl(['systemctl', 'daemon-reload'])
        for entry in changes:
            actual = exec_from_show(systemctl(
                ['systemctl', 'show', '-p', 'ExecStart', '--value', str(entry['unit'])]))
            if actual != Path(str(entry['base'])):
                raise RepairError(f'After reload, {entry["unit"]} points to {actual}, not {entry["base"]}')
        return backup
    except Exception:
        for source, disabled in reversed(renamed):
            if disabled.exists():
                disabled.rename(source)
        try:
            systemctl(['systemctl', 'daemon-reload'])
        except Exception:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Back up and disable only confirmed stale temporary ExecStart overrides')
    options = parser.parse_args(argv)
    try:
        plan = inspect()
        for item in plan:
            print(f"{item['unit']}: {item['status']}; effective={item['current']}; base={item['base']}")
        if not options.apply:
            print('Read-only mode. To repair after review, run with sudo --apply.')
            return 0
        if os.geteuid() != 0:
            raise RepairError('sudo/root required for --apply')
        if not any(entry['repair'] for entry in plan):
            print('Nothing to repair; both service paths are already stable.')
            return 0
        # No active RDP client may be disrupted by a later service change.
        connected = run(['ss', '-tnH', 'state', 'established', 'sport = :3389'])
        if connected.strip():
            raise RepairError('Active RDP clients present; refusing to change effective service definitions')
        backup = apply(plan)
        print('Confirmed stale overrides backed up and disabled. Backup:', backup)
        print('Configuration reloaded and verified. No services were started or restarted.')
        print('After verifying that no users are connected: sudo systemctl reset-failed xrdp-sesman.service xrdp.service; sudo systemctl start xrdp.service')
        return 0
    except (RepairError, OSError) as exc:
        print('REFUSED:', exc, file=sys.stderr)
        return 2



def prepare_test_runtime() -> int:
    """Run only under the explicitly selected activator recovery mode.

    Recover known stale temporary service paths; otherwise do not modify
    service definitions. Start stopped xrdp+sesman only when neither has
    established RDP clients and both use verified persistent base binaries.
    """
    try:
        if os.geteuid() != 0:
            raise RepairError('root privileges required for test runtime preparation')
        plan = inspect(systemctl=run)
        for item in plan:
            print(f"test-runtime {item['unit']}: {item['status']}; ExecStart={item['current']}", flush=True)
        connections = run(['ss', '-tnH', 'state', 'established', 'sport = :3389'])
        if connections.strip():
            raise RepairError('RDP clients are connected; refusing automatic recovery')
        changes = [item for item in plan if item['repair']]
        if changes:
            backup = apply(plan, systemctl=run)
            print('Backed up stale service overrides:', backup, flush=True)
        for unit in UNITS:
            effective = exec_from_show(run(
                ['systemctl', 'show', '-p', 'ExecStart', '--value', unit]))
            expected = BASE_DIRECTORY / EXPECTED[unit]
            if effective != expected:
                raise RepairError(f'{unit} must execute persistent {expected}, not {effective}')
        active = all(subprocess.run(
            ['systemctl', 'is-active', '--quiet', unit],
            capture_output=True, check=False, timeout=5).returncode == 0
            for unit in UNITS)
        if active:
            print('xrdp and sesman are already active; no service action needed.', flush=True)
            return 0
        # Only a start is permitted; never restart or stop an active service.
        run(['systemctl', 'reset-failed', 'xrdp.service', 'xrdp-sesman.service'])
        # Start inactive dependencies explicitly, without restarting healthy units.
        for unit in ('xrdp-sesman.service', 'xrdp.service'):
            if subprocess.run(['systemctl', 'is-active', '--quiet', unit],
                              capture_output=True, check=False, timeout=5).returncode:
                run(['systemctl', 'start', unit])
        for unit in UNITS:
            if subprocess.run(['systemctl', 'is-active', '--quiet', unit],
                              capture_output=True, check=False, timeout=5).returncode:
                raise RepairError(f'{unit} is not active after attempted start')
        print('xrdp and sesman started; clipboard CTests may proceed.', flush=True)
        return 0
    except (RepairError, OSError, subprocess.TimeoutExpired) as exc:
        print('test-runtime recovery refused: ' + str(exc), file=sys.stderr)
        print('No clipboard tests or new candidate activation should proceed.', file=sys.stderr)
        return 2

if __name__ == '__main__':
    raise SystemExit(prepare_test_runtime())

PY_RUNTIME
    # END TEST-RUNTIME-RECOVERY-PYTHON
}

rollback()
{
    backup_directory=$1
    [ -d "$backup_directory" ] || fail "missing backup directory: $backup_directory"
    [ -f "$backup_directory/xrdp.ini" ] || fail "backup has no xrdp.ini"

    stateful_backup=0
    sesman_stateful_backup=0
    if [ -f "$backup_directory/service-state-v2" ]; then
        stateful_backup=1
        sesman_stateful_backup=1
    elif [ -f "$backup_directory/service-state-v1" ]; then
        stateful_backup=1
    else
        # Backward compatibility with backups made by the previous activator.
        [ -f "$backup_directory/chansrv-existed" ] ||
            fail "backup predates the chansrv snapshot; refusing a partial rollback"
        [ -e "$backup_directory/xrdp-chansrv" ] ||
            fail "backup is missing the previous chansrv binary"
    fi

    if [ -f "$backup_directory/module-existed" ] &&
       [ ! -e "$backup_directory/libxrdp_console.so" ] &&
       [ ! -L "$backup_directory/libxrdp_console.so" ]; then
        fail "backup is missing the previous module"
    fi
    if [ -f "$backup_directory/chansrv-existed" ] &&
       [ ! -e "$backup_directory/xrdp-chansrv" ] &&
       [ ! -L "$backup_directory/xrdp-chansrv" ]; then
        fail "backup is missing the previous chansrv binary"
    fi
    if [ -f "$backup_directory/dropin-existed" ] &&
       [ ! -e "$backup_directory/upstream-local.conf" ] &&
       [ ! -L "$backup_directory/upstream-local.conf" ]; then
        fail "backup is missing the previous service drop-in"
    fi
    if [ "$sesman_stateful_backup" -eq 1 ] &&
       [ -f "$backup_directory/sesman-dropin-existed" ] &&
       [ ! -e "$backup_directory/sesman-upstream-local.conf" ] &&
       [ ! -L "$backup_directory/sesman-upstream-local.conf" ]; then
        fail "backup is missing the previous sesman service drop-in"
    fi

    systemctl stop xrdp-console-chansrv.service ||
        fail_service xrdp-console-chansrv.service \
            "could not stop console chansrv before rollback"
    systemctl stop xrdp.service ||
        fail_service xrdp.service "could not stop xrdp before rollback"
    if [ "$sesman_stateful_backup" -eq 1 ]; then
        systemctl stop xrdp-sesman.service ||
            fail_service xrdp-sesman.service \
                "could not stop xrdp-sesman before rollback"
    fi

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
    if [ -f "$backup_directory/chansrv-existed" ]; then
        [ -e "$backup_directory/xrdp-chansrv" ] ||
            fail "backup is missing the previous chansrv binary"
        cp -a -- "$backup_directory/xrdp-chansrv" "$chansrv_target"
    fi

    if [ -f "$backup_directory/dropin-existed" ]; then
        [ -f "$backup_directory/upstream-local.conf" ] ||
            fail "backup is missing the previous service drop-in"
        install -d -m 0755 "$dropin_directory"
        cp -a -- "$backup_directory/upstream-local.conf" "$dropin"
    else
        rm -f -- "$dropin"
    fi

    if [ "$sesman_stateful_backup" -eq 1 ]; then
        if [ -f "$backup_directory/sesman-dropin-existed" ]; then
            [ -f "$backup_directory/sesman-upstream-local.conf" ] ||
                fail "backup is missing the previous sesman service drop-in"
            install -d -m 0755 "$sesman_dropin_directory"
            cp -a -- "$backup_directory/sesman-upstream-local.conf" "$sesman_dropin"
        else
            rm -f -- "$sesman_dropin"
        fi
    fi

    systemctl daemon-reload
    if [ "$sesman_stateful_backup" -eq 1 ]; then
        if [ -f "$backup_directory/xrdp-was-active" ]; then
            systemctl restart xrdp.service ||
                fail "rollback restored files but xrdp restart failed"
            systemctl is-active --quiet xrdp.service ||
                fail "xrdp is not active after rollback"
            if [ -f "$backup_directory/sesman-was-active" ]; then
                systemctl is-active --quiet xrdp-sesman.service ||
                    fail "xrdp-sesman is not active after rollback"
            fi
            wait_for_rdp_listener ||
                fail "xrdp did not restore its port 3389 listener"
        else
            systemctl stop xrdp.service >/dev/null 2>&1 || true
            if [ -f "$backup_directory/sesman-was-active" ]; then
                systemctl restart xrdp-sesman.service ||
                    fail "rollback restored files but xrdp-sesman restart failed"
                systemctl is-active --quiet xrdp-sesman.service ||
                    fail "xrdp-sesman is not active after rollback"
            else
                systemctl stop xrdp-sesman.service >/dev/null 2>&1 || true
            fi
        fi

        if [ -f "$backup_directory/chansrv-was-active" ]; then
            systemctl restart xrdp-console-chansrv.service ||
                fail "rollback restored files but chansrv restart failed"
            systemctl is-active --quiet xrdp-console-chansrv.service ||
                fail "console chansrv is not active after rollback"
        else
            systemctl stop xrdp-console-chansrv.service >/dev/null 2>&1 || true
        fi
    elif [ "$stateful_backup" -eq 1 ]; then
        if [ -f "$backup_directory/chansrv-was-active" ]; then
            systemctl restart xrdp-console-chansrv.service ||
                fail "rollback restored files but chansrv restart failed"
            systemctl is-active --quiet xrdp-console-chansrv.service ||
                fail "console chansrv is not active after rollback"
        else
            systemctl stop xrdp-console-chansrv.service >/dev/null 2>&1 || true
        fi

        if [ -f "$backup_directory/xrdp-was-active" ]; then
            systemctl restart xrdp ||
                fail "rollback restored files but xrdp restart failed"
            systemctl is-active --quiet xrdp ||
                fail "xrdp is not active after rollback"
            wait_for_rdp_listener ||
                fail "xrdp did not restore its port 3389 listener"
        else
            systemctl stop xrdp.service >/dev/null 2>&1 || true
        fi
    else
        systemctl restart xrdp ||
            fail "rollback restored files but xrdp restart failed"
        systemctl is-active --quiet xrdp ||
            fail "xrdp is not active after rollback"
        wait_for_rdp_listener ||
            fail "xrdp did not restore its port 3389 listener"
        systemctl restart xrdp-console-chansrv.service ||
            fail "rollback restored files but chansrv restart failed"
        systemctl is-active --quiet xrdp-console-chansrv.service ||
            fail "console chansrv is not active after rollback"
    fi
    if [ "$sesman_stateful_backup" -eq 1 ]; then
        echo "Restored xrdp, sesman, chansrv, configuration, module, and service drop-ins from: $backup_directory"
    else
        echo "Restored xrdp, chansrv, configuration, module, and service drop-in from: $backup_directory"
    fi
    echo "The RDP listener remains configured for port 3389."
}

preflight_only=0
# A rollback snapshot is mandatory for every production activation.
# --backup remains an explicit compatible spelling.
backup_enabled=1
backup_flag_seen=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h)
            [ "$#" -eq 1 ] || { usage >&2; exit 2; }
            usage
            exit 0
            ;;
        --preflight)
            [ "$preflight_only" -eq 0 ] || { usage >&2; exit 2; }
            preflight_only=1
            ;;
        --prepare-test-runtime)
            [ "$#" -eq 1 ] &&
            [ "$preflight_only" -eq 0 ] &&
            [ "$backup_flag_seen" -eq 0 ] ||
                { usage >&2; exit 2; }
            prepare_test_runtime
            exit $?
            ;;
        --backup)
            [ "$backup_flag_seen" -eq 0 ] || { usage >&2; exit 2; }
            backup_flag_seen=1
            ;;
        --rollback)
            [ "$#" -eq 2 ] &&
                [ "$preflight_only" -eq 0 ] &&
                [ "$backup_flag_seen" -eq 0 ] || { usage >&2; exit 2; }
            [ "$(id -u)" -eq 0 ] ||
                fail "run rollback as root (for example, with sudo)"
            rollback "$2"
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
    shift
done


# Deployment ExecStart outlives the build shell. Never pin systemd units to
# scratch locations, even when those directories currently contain binaries.
for activation_path in "$build_root" "$prefix"; do
    resolved_path=$(readlink -m -- "$activation_path") ||
        fail "cannot resolve candidate installation path: $activation_path"
    case "$resolved_path" in
        /tmp|/tmp/*|/var/tmp|/var/tmp/*)
            fail "refusing temporary build path in persistent systemd ExecStart: $resolved_path"
            ;;
    esac
done

[ "$(id -u)" -eq 0 ] || fail "run as root (for example, with sudo)"
[ -x "$daemon" ] || fail "missing pinned xrdp daemon: $daemon"
[ -x "$sesman" ] || fail "missing pinned xrdp-sesman: $sesman"
[ -x "$chansrv_source" ] || fail "missing pinned xrdp chansrv: $chansrv_source"
[ -f "$module_source" ] || fail "missing direct-X11 module: $module_source"
[ -r "$revision_header" ] ||
    fail "missing generated build identity: $revision_header"
[ -f "$config" ] || fail "missing xrdp configuration: $config"
[ -f "$sesman_config" ] || fail "missing xrdp-sesman configuration: $sesman_config"
[ -d "$(dirname -- "$module_target")" ] ||
    fail "missing xrdp module directory: $(dirname -- "$module_target")"
"$daemon" --version 2>/dev/null | grep -q '^xrdp 0\.10\.6\.1' ||
    fail "candidate daemon is not the pinned xrdp 0.10.6.1 build"
"$sesman" --version 2>/dev/null | grep -q '^xrdp-sesman 0\.10\.6\.1' ||
    fail "candidate xrdp-sesman is not the pinned xrdp 0.10.6.1 build"
if ! grep -aFq -- 'XRDP_CONSOLE_GFX_PLANAR_BATCH_V1' "$daemon"; then
    fail "candidate daemon lacks the Console Planar batching patch marker"
fi
if ldd "$module_source" 2>/dev/null | grep -q 'not found'; then
    fail "direct-X11 module has an unresolved shared-library dependency"
fi
if ldd "$sesman" 2>/dev/null | grep -q 'not found'; then
    fail "pinned xrdp-sesman has an unresolved shared-library dependency"
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

service_exists xrdp.service ||
    fail "xrdp.service definition is missing, masked, or not loadable"
service_exists xrdp-sesman.service ||
    fail "xrdp-sesman.service definition is missing, masked, or not loadable"
service_exists xrdp-console-chansrv.service ||
    fail "xrdp-console-chansrv.service definition is missing, masked, or not loadable; the physical-session launch policy must be provisioned before activation"

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

xrdp_state=$(service_state xrdp.service)
sesman_state=$(service_state xrdp-sesman.service)
chansrv_state=$(service_state xrdp-console-chansrv.service)
current_xrdp_exec=$(systemctl show xrdp.service -p ExecStart --value) ||
    fail "could not inspect the xrdp command"
current_sesman_exec=$(systemctl show xrdp-sesman.service -p ExecStart --value) ||
    fail "could not inspect the xrdp-sesman command"
if [ "$preflight_only" -eq 1 ]; then
    echo "Activation preflight passed."
    echo "xrdp.service: ${xrdp_state:-unknown}"
    echo "xrdp-sesman.service: ${sesman_state:-unknown}"
    echo "xrdp-console-chansrv.service: ${chansrv_state:-unknown}"
    case "$current_xrdp_exec" in
        *"$daemon"*)
            echo "xrdp already uses the tested pinned runtime."
            ;;
        *xrdp-x11vnc*)
            echo "Legacy xrdp-x11vnc daemon path detected; activation will migrate it to: $daemon"
            ;;
        *)
            echo "xrdp ExecStart will be overridden with the tested runtime: $daemon"
            ;;
    esac
    case "$current_sesman_exec" in
        *"$sesman"*)
            echo "xrdp-sesman already uses the tested pinned runtime."
            ;;
        *xrdp-x11vnc*)
            echo "Legacy xrdp-x11vnc sesman path detected; activation will migrate it to: $sesman"
            ;;
        *)
            echo "xrdp-sesman ExecStart will be overridden with the tested runtime: $sesman"
            ;;
    esac
    if [ -e "$chansrv_target" ] || [ -L "$chansrv_target" ]; then
        echo "Existing chansrv target will be replaced: $chansrv_target"
    else
        echo "No previous chansrv target exists; activation will install: $chansrv_target"
    fi
    if [ "$backup_enabled" -eq 1 ]; then
        echo "Rollback backup: requested; it will be created only if activation proceeds."
    else
        echo "Rollback backup: automatically enabled for activation."
    fi
    exit 0
fi

backup_directory=
if [ "$backup_enabled" -eq 1 ]; then
    stamp=$(date +%Y%m%d-%H%M%S)
    install -d -m 0700 "$backup_root"
    backup_directory=$(mktemp -d "$backup_root/direct-console-$stamp.XXXXXX") ||
        fail "could not create a unique rollback directory under $backup_root"
    cp -a -- "$config" "$backup_directory/xrdp.ini"
    : >"$backup_directory/service-state-v2"
    if systemctl is-active --quiet xrdp.service; then
        : >"$backup_directory/xrdp-was-active"
    fi
    if systemctl is-active --quiet xrdp-sesman.service; then
        : >"$backup_directory/sesman-was-active"
    fi
    if systemctl is-active --quiet xrdp-console-chansrv.service; then
        : >"$backup_directory/chansrv-was-active"
    fi
    if [ -e "$module_target" ] || [ -L "$module_target" ]; then
        cp -a -- "$module_target" "$backup_directory/libxrdp_console.so"
        : >"$backup_directory/module-existed"
    fi
    if [ -e "$chansrv_target" ] || [ -L "$chansrv_target" ]; then
        cp -a -- "$chansrv_target" "$backup_directory/xrdp-chansrv"
        : >"$backup_directory/chansrv-existed"
    fi
    if [ -e "$dropin" ]; then
        cp -a -- "$dropin" "$backup_directory/upstream-local.conf"
        : >"$backup_directory/dropin-existed"
    fi
    if [ -e "$sesman_dropin" ]; then
        cp -a -- "$sesman_dropin" "$backup_directory/sesman-upstream-local.conf"
        : >"$backup_directory/sesman-dropin-existed"
    fi
    echo "Rollback backup: $backup_directory"
else
    echo "Rollback backup is required for this activation."
fi

activation_finalized=0
rollback_failed_activation()
{
    result=$?
    if [ "$result" -ne 0 ] && [ "$activation_finalized" -eq 0 ]; then
        trap - EXIT HUP INT TERM
        if [ "$backup_enabled" -eq 1 ]; then
            echo "Activation failed; restoring the previous xrdp state." >&2
            rollback "$backup_directory" ||
                echo "Automatic rollback failed; backup remains at $backup_directory" >&2
        else
            echo "Activation failed without a rollback backup; manual recovery may be required." >&2
        fi
    fi
}
trap rollback_failed_activation EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Stop consumers before replacing runtime files. Stop chansrv first to prevent
# its restart loop from racing installation, then stop xrdp and any legacy
# xrdp-x11vnc sesman process before migrating their ExecStart paths.
systemctl stop xrdp-console-chansrv.service ||
    fail_service xrdp-console-chansrv.service \
        "could not stop console chansrv before activation"
systemctl stop xrdp.service ||
    fail_service xrdp.service "could not stop xrdp before activation"
systemctl stop xrdp-sesman.service ||
    fail_service xrdp-sesman.service "could not stop xrdp-sesman before activation"

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

install -d -m 0755 "$sesman_dropin_directory"
temporary_sesman_dropin=$(mktemp "$sesman_dropin_directory/.upstream-local.XXXXXX")
if ! printf '[Service]\nExecStart=\nExecStart=%s --nodaemon --config /etc/xrdp/sesman.ini\n' \
    "$sesman" >"$temporary_sesman_dropin" ||
   ! chmod 0644 "$temporary_sesman_dropin" ||
   ! mv -f -- "$temporary_sesman_dropin" "$sesman_dropin"; then
    rm -f -- "$temporary_sesman_dropin"
    fail "could not write the pinned xrdp-sesman service override"
fi

install -d -m 0755 "$dropin_directory"
temporary_dropin=$(mktemp "$dropin_directory/.upstream-local.XXXXXX")
if ! printf '[Unit]\nRequires=xrdp-sesman.service\nAfter=xrdp-sesman.service\n\n[Service]\nExecStart=\nExecStart=%s --nodaemon --config /etc/xrdp/xrdp.ini\n' \
    "$daemon" >"$temporary_dropin" ||
   ! chmod 0644 "$temporary_dropin" ||
   ! mv -f -- "$temporary_dropin" "$dropin"; then
    rm -f -- "$temporary_dropin"
    fail "could not write the pinned xrdp service override"
fi

systemctl daemon-reload || fail "systemd daemon-reload failed"

# Starting xrdp lets systemd start the required, migrated sesman first.
# Chansrv starts only after sesman is healthy because it asks sesman to create
# the per-user socket directory.
if ! systemctl restart xrdp.service; then
    if ! systemctl is-active --quiet xrdp-sesman.service; then
        fail_service xrdp-sesman.service \
            "xrdp could not start because its pinned sesman dependency failed"
    fi
    fail_service xrdp.service "the pinned xrdp daemon failed to start"
fi
systemctl is-active --quiet xrdp.service ||
    fail_service xrdp.service "the pinned xrdp service is not active"
systemctl is-active --quiet xrdp-sesman.service ||
    fail_service xrdp-sesman.service "the pinned xrdp-sesman service is not active"

active_exec=$(systemctl show xrdp.service -p ExecStart --value) ||
    fail "could not inspect the active xrdp command"
case "$active_exec" in
    *"$daemon"*) ;;
    *) fail "systemd is not running the pinned candidate daemon" ;;
esac
active_sesman_exec=$(systemctl show xrdp-sesman.service -p ExecStart --value) ||
    fail "could not inspect the active xrdp-sesman command"
case "$active_sesman_exec" in
    *"$sesman"*) ;;
    *) fail "systemd is not running the pinned candidate xrdp-sesman" ;;
esac
wait_for_rdp_listener ||
    fail_service xrdp.service "xrdp did not return to port 3389 within 10 seconds"

systemctl restart xrdp-console-chansrv.service ||
    fail_service xrdp-console-chansrv.service \
        "the pinned xrdp chansrv failed to start"
systemctl is-active --quiet xrdp-console-chansrv.service ||
    fail_service xrdp-console-chansrv.service \
        "the pinned xrdp chansrv service is not active"

if ! cmp -s -- "$module_source" "$module_target"; then
    fail "installed module differs from the tested build artifact"
fi
if ! cmp -s -- "$chansrv_source" "$chansrv_target"; then
    fail "installed chansrv differs from the tested build artifact"
fi

activation_finalized=1
echo "Activated the first-party direct-X11 module with pinned xrdp 0.10.6.1."
echo "Activated the matching pinned xrdp-sesman runtime."
echo "Installed the matching pinned chansrv binary for text clipboard support."
echo "RDP listener preserved on port 3389."
echo "Enabled Console text clipboard, dynamic virtual channels, and presentation resizing."
echo "RemoteFX is negotiated only when the client supports the module's standard RFX path; otherwise classic bitmap remains available."
echo "Client scaled-output, scroll-reuse, and verified-cache optimizations default to enabled when negotiated capabilities permit."
echo "Bitmap-cache observation is diagnostic-only and remains disabled unless XRDP_CONSOLE_CLIENT_CACHE_OBSERVE=1 is set."
echo "No client capabilities are forced or advertised by this setting; set an individual XRDP_CONSOLE_CLIENT_* variable to exactly 0 to disable that path."
if [ "$backup_enabled" -eq 1 ]; then
    echo "Rollback: sudo $0 --rollback $backup_directory"
else
    echo "No rollback backup was created; activation must not have proceeded."
fi
systemctl --no-pager --full status xrdp | sed -n '1,18p'
