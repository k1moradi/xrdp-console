#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only preflight for the shared xrdp-chansrv clipboard test socketdir.

Never creates a socket directory, changes permissions or starts sesman.
The pinned xrdp 0.10.x chansrv requires sesman to create the per-user directory.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import stat
import subprocess
from typing import Any

SOCKET_ROOT = Path('/run/xrdp/sockdir')
SOCKET_PATTERN = re.compile(r'^(\d+)\s*-\s*(xrdp-loader-clipboard-[^\n]*?|xrdp-loader-retained-dual-image-reconnect-session)\s*\(Failed\)(?:\s|$)')


def classify(socket_root: Path, uid: int) -> dict[str, Any]:
    """Read metadata only; a present directory does not prove sesman is healthy."""
    if uid < 0:
        raise ValueError('uid cannot be negative')
    user_directory = socket_root / str(uid)
    result: dict[str, Any] = {
        'socket_root': str(socket_root), 'uid': uid,
        'user_directory': str(user_directory), 'ready': False,
        'status': 'unknown', 'reason': '',
    }
    try:
        parent = socket_root.lstat()
    except FileNotFoundError:
        result.update(status='missing-parent', reason='Root-managed xrdp socketdir is absent')
        return result
    except OSError as exc:
        result.update(status='parent-inaccessible', reason=f'Cannot stat socketdir: {exc.strerror}')
        return result
    if not stat.S_ISDIR(parent.st_mode):
        result.update(status='invalid-parent', reason='Socketdir parent is not a real directory')
        return result
    try:
        child = user_directory.lstat()
    except FileNotFoundError:
        # A fresh system may legitimately have no per-user directory yet.
        # In that case chansrv asks sesman; allow it only if the expected
        # *same socket namespace* exposes a reachable-looking UNIX socket.
        # This is a preflight, not a service health or connection test.
        sesman_socket = socket_root / 'sesman.socket'
        try:
            endpoint = sesman_socket.lstat()
            fallback_ready = (stat.S_ISSOCK(endpoint.st_mode) and
                              os.access(sesman_socket, os.W_OK))
        except OSError:
            fallback_ready = False
        if fallback_ready:
            result.update(ready=True, status='sesman-socket-available',
                          reason='No user directory yet; chansrv can ask sesman to create it')
        else:
            result.update(status='missing-user-directory',
                          reason='No per-user directory or accessible sesman.socket in pinned runtime namespace')
        return result
    except OSError as exc:
        result.update(status='user-directory-inaccessible',
                      reason=f'Cannot stat per-user directory: {exc.strerror}')
        return result
    if not stat.S_ISDIR(child.st_mode):
        result.update(status='invalid-user-directory',
                      reason='Per-user socketdir is not a real directory')
        return result
    if child.st_uid != uid:
        result.update(status='wrong-owner',
                      reason=f'Per-user directory owner UID {child.st_uid} differs from test UID {uid}')
        return result
    if not os.access(user_directory, os.R_OK | os.W_OK | os.X_OK):
        result.update(status='permission-denied',
                      reason='Test user cannot access its xrdp socketdir')
        return result
    result.update(ready=True, status='ready', reason='Per-user xrdp socketdir exists and is accessible')
    return result


def sesman_hint() -> str:
    try:
        p = subprocess.run(['systemctl', 'is-active', 'xrdp-sesman.service'],
                           capture_output=True, text=True, timeout=2, check=False)
        value = p.stdout.strip() or p.stderr.strip()
        return value[:80] or 'unknown'
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return 'unknown'


def inspect_systemd_exec_value(value: str) -> dict[str, str]:
    """Classify systemctl 'show -p ExecStart --value'; no service actions."""
    match = re.search(r'(?:^|[\s;{])path=(/\S+)', value)
    if not match:
        return {'status': 'unknown', 'path': ''}
    target = Path(match.group(1))
    if not target.is_file():
        return {'status': 'missing-executable', 'path': str(target)}
    if not os.access(target, os.X_OK):
        return {'status': 'not-executable', 'path': str(target)}
    return {'status': 'executable-present', 'path': str(target)}


def sesman_exec_hint() -> dict[str, str]:
    """Inspect configured executable without contacting or restarting sesman."""
    try:
        process = subprocess.run(
            ['systemctl', 'show', '-p', 'ExecStart', '--value',
             'xrdp-sesman.service'],
            capture_output=True, text=True, timeout=2, check=False)
        if process.returncode != 0:
            return {'status': 'unavailable', 'path': ''}
        return inspect_systemd_exec_value(process.stdout)
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return {'status': 'unavailable', 'path': ''}


def summarize_ctest_log(text: str) -> dict[str, Any]:
    failed = []
    for line in text.splitlines():
        m = SOCKET_PATTERN.search(line.strip())
        if m:
            failed.append(m.group(2))
    return {
        'failed_clipboard_tests': len(set(failed)),
        'missing_socketdir_occurrences': text.count("doesn't exist - asking sesman to create it"),
        'sesman_connection_failures': text.count("Can't connect to sesman"),
        'shared_missing_socketdir_pattern': bool(failed) and \
            text.count("Can't connect to sesman") > 0 and \
            text.count("doesn't exist - asking sesman to create it") > 0,
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--socket-root', type=Path, default=SOCKET_ROOT)
    p.add_argument('--uid', type=int, default=os.getuid())
    p.add_argument('--log', type=Path, help='Existing CTest log to classify without accessing live clipboard')
    p.add_argument('--json', action='store_true', help='Machine-readable read-only diagnostics')
    args = p.parse_args(argv)
    result = classify(args.socket_root, args.uid)
    if args.log:
        result['log'] = summarize_ctest_log(args.log.read_text(encoding='utf-8', errors='replace'))
    result['sesman_service'] = sesman_hint() if args.socket_root == SOCKET_ROOT else 'not-inspected'
    if args.socket_root == SOCKET_ROOT:
        result['sesman_exec'] = sesman_exec_hint()
        if result['sesman_exec']['status'] == 'missing-executable':
            result.update(ready=False, status='stale-sesman-exec',
                reason='systemd ExecStart targets a missing binary: ' +
                       result['sesman_exec']['path'])
    else:
        result['sesman_exec'] = {'status': 'not-inspected', 'path': ''}
    if args.json:
        print(json.dumps(result, sort_keys=True))
    elif result['ready']:
        print(f"Clipboard test runtime preflight: ready ({result['user_directory']})")
        print('Note: this checks directory readiness only; it cannot guarantee all clipboard tests pass.')
    else:
        print(f"Clipboard test runtime preflight BLOCKED ({result['status']}): {result['reason']}")
        print(f"Expected per-user socketdir: {result['user_directory']}")
        print(f"xrdp-sesman.service: {result['sesman_service']}")
        if result['sesman_exec']['status'] == 'missing-executable':
            print('Stale systemd ExecStart: ' + result['sesman_exec']['path'])
            print('A persistent override may point to a deleted test build.')
            print('  systemctl cat xrdp-sesman.service')
            print('  systemctl show -p ExecStart --value xrdp.service')
        print('Read-only host diagnostics:')
        print('  systemctl status xrdp-sesman.service --no-pager')
        print('  ls -ld /run/xrdp /run/xrdp/sockdir /run/xrdp/sockdir/$(id -u)')
        print('  journalctl -u xrdp-sesman.service -n 50 --no-pager')
        print('Do NOT chmod/mkdir under /run/xrdp manually or auto-restart production services.')
        print('Restore sesman runtime ownership through the host service administrator before retesting.')
    if 'log' in result and not args.json:
        print('Prior clipboard CTest failure signature:', result['log'])
    return 0 if result['ready'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
