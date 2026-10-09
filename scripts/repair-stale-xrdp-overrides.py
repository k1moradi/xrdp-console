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
import shutil
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


def check_candidate(unit: str, base_file: Path, dropin: Path,
                    effective_value: str) -> dict[str, str | bool]:
    name = EXPECTED[unit]
    if base_file.is_symlink() or not base_file.is_file():
        raise RepairError(f'Unexpected base unit file: {base_file}')
    base_exec = exec_from_unit(base_file.read_text(encoding='utf-8'))
    if base_exec != BASE_DIRECTORY / name:
        raise RepairError(f'Base {unit} unexpectedly executes {base_exec}, not {BASE_DIRECTORY / name}')
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
    return {'unit': unit, 'status': 'stale-temporary-exec', 'base': str(base_exec),
            'current': str(current_exec), 'override': str(dropin), 'repair': True}


def inspect(systemctl: Callable[[list[str]], str] = run,
            base_dir: Path = ETC_SYSTEMD) -> list[dict[str, str | bool]]:
    result = []
    for unit in UNITS:
        base_file = base_dir / unit
        dropin = base_dir / (unit + '.d') / 'upstream-local.conf'
        # Only operate on service overrides with no competing *.conf files.
        extras = [p.name for p in dropin.parent.glob('*.conf') if p.name != 'upstream-local.conf']
        if extras:
            raise RepairError(f'Unexpected other {unit} drop-ins: {extras}')
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
        print('Both overrides backed up and disabled. Backup:', backup)
        print('Configuration reloaded and verified. No services were started or restarted.')
        print('After verifying that no users are connected: sudo systemctl reset-failed xrdp-sesman.service xrdp.service; sudo systemctl start xrdp.service')
        return 0
    except (RepairError, OSError) as exc:
        print('REFUSED:', exc, file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
