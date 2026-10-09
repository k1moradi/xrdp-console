#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only preflight behavior without production services or live X11."""
from __future__ import annotations
import importlib.util
import os
import socket
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / 'tools' / 'diagnostics' / 'xrdp_clipboard_test_doctor.py'
spec = importlib.util.spec_from_file_location('clipboard_doctor', MODULE)
doctor = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(doctor)


class DoctorTests(unittest.TestCase):
    def test_missing_parent_is_fail_closed(self):
        with tempfile.TemporaryDirectory() as td:
            result = doctor.classify(Path(td) / 'absent', os.getuid())
            self.assertEqual(result['status'], 'missing-parent')
            self.assertFalse(result['ready'])
    def test_missing_user_directory_is_fail_closed(self):
        with tempfile.TemporaryDirectory() as td:
            result = doctor.classify(Path(td), os.getuid())
            self.assertEqual(result['status'], 'missing-user-directory')
            self.assertFalse(result['ready'])
    def test_missing_user_directory_accepted_when_sesman_socket_available(self):
        with tempfile.TemporaryDirectory() as td:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as service:
                service.bind(str(Path(td) / 'sesman.socket'))
                result = doctor.classify(Path(td), os.getuid())
                self.assertEqual(result['status'], 'sesman-socket-available')
                self.assertTrue(result['ready'])
    def test_present_private_user_directory(self):
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / str(os.getuid())).mkdir(mode=0o700)
            result = doctor.classify(Path(td), os.getuid())
            self.assertEqual(result['status'], 'ready')
            self.assertTrue(result['ready'])
    def test_parent_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            (base / 'real').mkdir()
            (base / 'link').symlink_to(base / 'real')
            result = doctor.classify(base / 'link', os.getuid())
            self.assertEqual(result['status'], 'invalid-parent')
    def test_user_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            (base / 'elsewhere').mkdir()
            (base / str(os.getuid())).symlink_to(base / 'elsewhere')
            result = doctor.classify(base, os.getuid())
            self.assertEqual(result['status'], 'invalid-user-directory')
    def test_wrong_uid_owner(self):
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / str(os.getuid()+10000)).mkdir()
            result = doctor.classify(Path(td), os.getuid()+10000)
            self.assertEqual(result['status'], 'wrong-owner')
    def test_negative_uid(self):
        with self.assertRaises(ValueError):
            doctor.classify(Path('/not-used'), -1)
    def test_known_ctest_signature(self):
        text = '\n'.join([
            '67 - xrdp-loader-clipboard-session (Failed)',
            '68 - xrdp-loader-clipboard-format-list-filter (Failed)',
            "[INFO] /run/xrdp/sockdir/1000 doesn't exist - asking sesman to create it",
            "[INFO] /run/xrdp/sockdir/1000 doesn't exist - asking sesman to create it",
            "[ERROR] Can't connect to sesman", "[ERROR] Can't connect to sesman",
        ])
        d = doctor.summarize_ctest_log(text)
        self.assertEqual(d['failed_clipboard_tests'], 2)
        self.assertTrue(d['shared_missing_socketdir_pattern'])
    def test_unrelated_ctest_failures_not_misclassified(self):
        d = doctor.summarize_ctest_log('67 - unrelated-smoke (Failed)\nAssertionError: wrong pixels')
        self.assertFalse(d['shared_missing_socketdir_pattern'])
    def test_no_implicit_repairs(self):
        with tempfile.TemporaryDirectory() as td:
            rc = doctor.main(['--socket-root', str(Path(td) / 'nonexistent')])
            self.assertEqual(rc, 2)
            self.assertFalse((Path(td) / 'nonexistent').exists())


if __name__ == '__main__':
    unittest.main()
