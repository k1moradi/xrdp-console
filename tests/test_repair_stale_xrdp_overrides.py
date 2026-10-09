#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests, no host systemctl, no host service mutation."""
import re
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

# The recovery code resides INSIDE the activator. Extract only its marked
# Python here-doc, rather than testing an obsolete standalone repair script.
FILE = Path(__file__).resolve().parents[1] / 'scripts' / 'activate-direct-console.sh'
activator = FILE.read_text(encoding='utf-8')
match = re.search(
    r"python3 - <<'PY_RUNTIME'\n(.*?)\nPY_RUNTIME\n",
    activator, re.DOTALL)
assert match, 'Embedded recovery code not found in activator'
repair = types.ModuleType('xrdp_embedded_recovery')
exec(compile(match.group(1), str(FILE) + ':PY_RUNTIME', 'exec'),
     repair.__dict__)


class RepairTest(unittest.TestCase):
    def setUp(self):
        self.ctx = tempfile.TemporaryDirectory()
        self.addCleanup(self.ctx.cleanup)
        self.root = Path(self.ctx.name)
        self.systemd = self.root / 'etc'
        self.systemd.mkdir()
        self.sbin = self.root / 'opt' / 'sbin'
        self.sbin.mkdir(parents=True)
        self.override = {}
        self.stale = {}
        for unit in repair.UNITS:
            name = repair.EXPECTED[unit]
            binary = self.sbin / name
            binary.write_text('#!/bin/sh\nexit 0\n')
            binary.chmod(0o755)
            (self.systemd / unit).write_text(
                '[Unit]\nAfter=network.target\n[Service]\nExecStart=' + str(binary) + ' --nodaemon\n')
            override_dir = self.systemd / (unit + '.d')
            override_dir.mkdir()
            dropin = override_dir / 'upstream-local.conf'
            stale = Path('/var/tmp/xrdp-test-unit-nonexistent-932471') / name
            if unit == 'xrdp.service':
                dropin.write_text('[Unit]\nRequires=xrdp-sesman.service\nAfter=xrdp-sesman.service\n'
                                  '[Service]\nExecStart=\nExecStart=' + str(stale) +
                                  ' --nodaemon --config /etc/xrdp/xrdp.ini\n')
            else:
                dropin.write_text('[Service]\nExecStart=\nExecStart=' + str(stale) +
                                  ' --nodaemon --config /etc/xrdp/sesman.ini\n')
            self.override[unit] = dropin
            self.stale[unit] = stale
        self.base_patch = mock.patch.object(repair, 'BASE_DIRECTORY', self.sbin)
        self.base_patch.start()
        self.addCleanup(self.base_patch.stop)
        original_path_in_temp = repair.path_in_temp
        self.path_patch = mock.patch.object(
            repair, 'path_in_temp',
            side_effect=lambda path: False if path.parent == self.sbin
            else original_path_in_temp(path))
        self.path_patch.start()
        self.addCleanup(self.path_patch.stop)
        self.calls = []
        self.fail_at = None

    def fake_systemctl(self, argv):
        self.calls.append(argv)
        if argv[1] == 'daemon-reload':
            if self.fail_at == 'reload':
                raise repair.RepairError('mock daemon reload failed')
            return ''
        if argv[1] == 'show':
            unit = argv[-1]
            if self.override[unit].exists():
                target = self.stale[unit]
            else:
                target = self.sbin / repair.EXPECTED[unit]
            if self.fail_at == 'verify' and not self.override[unit].exists():
                target = self.stale[unit]
            return '{ path=' + str(target) + ' ; argv[]=' + str(target) + ' --nodaemon ; }'
        raise AssertionError(argv)

    def plan(self):
        return repair.inspect(self.fake_systemctl, self.systemd)

    def test_embedded_script_has_no_independent_python_dependency(self):
        self.assertIn('--prepare-test-runtime', activator)
        self.assertIn('def prepare_test_runtime()', activator)
        self.assertIn('def prepare_test_runtime() -> int:', match.group(1))

    def test_prepare_rejects_connected_rdp_clients(self):
        with mock.patch.object(repair.os, 'geteuid', return_value=0), \
             mock.patch.object(repair, 'inspect', return_value=self.plan()), \
             mock.patch.object(repair, 'run', return_value='ESTABLISHED'):
            self.assertEqual(repair.prepare_test_runtime(), 2)
        self.assertTrue(all(path.exists() for path in self.override.values()))

    def test_prepare_starts_stopped_services_once(self):
        statuses = {unit: False for unit in repair.UNITS}
        def live_run(args):
            if args[0] == 'ss':
                return ''
            if args[1] == 'reset-failed':
                return ''
            if args[1] == 'start':
                statuses[args[-1]] = True
                return ''
            return self.fake_systemctl(args)
        def fake_check(args, **kwargs):
            return types.SimpleNamespace(returncode=0 if statuses[args[-1]] else 3)
        original_apply = repair.apply
        def apply_private(plan, *, systemctl):
            return original_apply(
                plan, systemctl=systemctl,
                backup_root=self.root / 'backups')
        with mock.patch.object(repair.os, 'geteuid', return_value=0), \
             mock.patch.object(repair, 'inspect', return_value=self.plan()), \
             mock.patch.object(repair, 'apply', side_effect=apply_private), \
             mock.patch.object(repair, 'run', side_effect=live_run), \
             mock.patch.object(repair.subprocess, 'run', side_effect=fake_check):
            self.assertEqual(repair.prepare_test_runtime(), 0)
        self.assertTrue(all(statuses.values()))

    def test_prepare_healthy_runtime_does_not_restart(self):
        for path in self.override.values():
            path.unlink()
        plan = self.plan()
        self.assertTrue(all(not item['repair'] for item in plan))
        calls = []
        def live_run(args):
            calls.append(args)
            if args[0] == 'ss':
                return ''
            return self.fake_systemctl(args)
        def healthy_status(args, **kwargs):
            return types.SimpleNamespace(returncode=0)
        with mock.patch.object(repair.os, 'geteuid', return_value=0), \
             mock.patch.object(repair, 'inspect', return_value=plan), \
             mock.patch.object(repair, 'run', side_effect=live_run), \
             mock.patch.object(repair.subprocess, 'run', side_effect=healthy_status):
            self.assertEqual(repair.prepare_test_runtime(), 0)
        self.assertFalse(any(args[1] in ('start', 'restart', 'stop', 'daemon-reload')
                             for args in calls if args[0] == 'systemctl'))

    def test_prepare_refuses_start_when_stale_but_no_clients_not_proven(self):
        with mock.patch.object(repair.os, 'geteuid', return_value=0), \
             mock.patch.object(repair, 'run', side_effect=repair.RepairError('ss unavailable')):
            self.assertEqual(repair.prepare_test_runtime(), 2)
        self.assertTrue(all(path.exists() for path in self.override.values()))

    def test_exact_stale_pair(self):
        p = self.plan()
        self.assertEqual([r['status'] for r in p], ['stale-temporary-exec'] * 2)
        self.assertEqual([r['repair'] for r in p], [True, True])

    def test_paths_under_temp(self):
        self.assertTrue(repair.path_in_temp(Path('/tmp/source')))
        self.assertTrue(repair.path_in_temp(Path('/var/tmp/source')))
        self.assertFalse(repair.path_in_temp(Path('/opt/xrdp-console/sbin/xrdp')))

    def test_systemd_exec_value(self):
        self.assertEqual(repair.exec_from_show('{ path=/opt/xrdp-console/sbin/xrdp ; argv[]=/a ; }'),
                         Path('/opt/xrdp-console/sbin/xrdp'))
        with self.assertRaises(repair.RepairError):
            repair.exec_from_show('foobar')

    def test_reject_base_missing(self):
        (self.sbin / 'xrdp').unlink()
        with self.assertRaisesRegex(repair.RepairError, 'Persistent base executable missing'):
            self.plan()

    def test_reject_unrelated_live_binary(self):
        self.override['xrdp.service'].write_text(
            '[Service]\nExecStart=\nExecStart=' + str(self.sbin / 'xrdp') + ' --nodaemon\n')
        with self.assertRaisesRegex(repair.RepairError, 'does not match'):
            self.plan()

    def test_refuses_extra_directives_in_stale_override(self):
        override = self.override['xrdp.service']
        override.write_text(override.read_text() + 'Environment=MY_TOKEN=present\n')
        with self.assertRaisesRegex(repair.RepairError, 'Unexpected content'):
            self.plan()

    def test_unrelated_environment_dropin_is_preserved(self):
        extra = self.override['xrdp.service'].parent / 'console-environment.conf'
        extra.write_text('[Service]\nEnvironment=DISPLAY=:0\n')
        plan = self.plan()
        repair.apply(plan, systemctl=self.fake_systemctl, backup_root=self.root / 'backups')
        self.assertTrue(extra.is_file())
        self.assertIn('Environment=DISPLAY=:0', extra.read_text())

    def test_reject_unexpected_base_path(self):
        (self.systemd / 'xrdp.service').write_text('[Service]\nExecStart=/usr/bin/xrdp\n')
        with self.assertRaisesRegex(repair.RepairError, 'unexpectedly executes'):
            self.plan()

    def test_second_inspection_after_repair_is_idempotent(self):
        repair.apply(self.plan(), systemctl=self.fake_systemctl,
                     backup_root=self.root / 'backups')
        self.assertEqual([x['status'] for x in self.plan()], ['healthy-base', 'healthy-base'])

    def test_only_one_stale_override_is_repairable(self):
        self.override['xrdp.service'].unlink()
        plan = self.plan()
        self.assertEqual(plan[0]['status'], 'healthy-base')
        self.assertEqual(plan[1]['status'], 'stale-temporary-exec')
        repair.apply(plan, systemctl=self.fake_systemctl, backup_root=self.root / 'backups')
        self.assertEqual([x['status'] for x in self.plan()], ['healthy-base', 'healthy-base'])

    def test_apply_two_units_and_backup(self):
        plan = self.plan()
        backup = repair.apply(plan, systemctl=self.fake_systemctl, backup_root=self.root / 'backups')
        self.assertTrue((backup / 'manifest.json').is_file())
        self.assertEqual(len(list(backup.glob('*.conf'))), 2)
        self.assertFalse(any(f.exists() for f in self.override.values()))
        self.assertEqual(len(list(self.systemd.glob('*.d/*.disabled-*'))), 2)
        self.assertEqual([self.fake_systemctl(['systemctl','show','-p','ExecStart','--value',unit])
                          for unit in repair.UNITS],
                         ['{ path=' + str(self.sbin / repair.EXPECTED[unit]) + ' ; argv[]=' +
                          str(self.sbin / repair.EXPECTED[unit]) + ' --nodaemon ; }'
                          for unit in repair.UNITS])

    def test_failed_reload_restores_both(self):
        plan = self.plan()
        self.fail_at = 'reload'
        with self.assertRaises(repair.RepairError):
            repair.apply(plan, systemctl=self.fake_systemctl, backup_root=self.root / 'backups')
        self.assertTrue(all(f.exists() for f in self.override.values()))

    def test_failed_verification_restores_both(self):
        plan = self.plan()
        self.fail_at = 'verify'
        with self.assertRaisesRegex(repair.RepairError, 'After reload'):
            repair.apply(plan, systemctl=self.fake_systemctl, backup_root=self.root / 'backups')
        self.assertTrue(all(f.exists() for f in self.override.values()))

    def test_reject_non_temp_stale_override(self):
        new = Path('/some/other/deleted/path/xrdp')
        self.override['xrdp.service'].write_text('[Service]\nExecStart=\nExecStart=' + str(new))
        with self.assertRaisesRegex(repair.RepairError, 'does not match'):
            self.plan()

    def test_reject_base_symlink(self):
        original = self.systemd / 'xrdp.service'
        original.rename(self.systemd / 'xrdp-original.service')
        original.symlink_to(self.systemd / 'xrdp-original.service')
        with self.assertRaisesRegex(repair.RepairError, 'Unexpected base unit file'):
            self.plan()


if __name__ == '__main__':
    unittest.main()
