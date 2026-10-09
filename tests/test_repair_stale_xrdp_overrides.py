#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline tests, no host systemctl, no host service mutation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

FILE = Path(__file__).resolve().parents[1] / 'scripts' / 'repair-stale-xrdp-overrides.py'
spec = importlib.util.spec_from_file_location('repair', FILE)
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


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
            dropin.write_text('[Service]\nExecStart=\nExecStart=' + str(stale) + ' --nodaemon\n')
            self.override[unit] = dropin
            self.stale[unit] = stale
        self.base_patch = mock.patch.object(repair, 'BASE_DIRECTORY', self.sbin)
        self.base_patch.start()
        self.addCleanup(self.base_patch.stop)
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

    def test_reject_competing_dropins(self):
        (self.override['xrdp.service'].parent / 'other.conf').write_text('[Service]\n')
        with self.assertRaisesRegex(repair.RepairError, 'Unexpected other'):
            self.plan()

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
