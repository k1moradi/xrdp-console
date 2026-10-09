#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise activator bootstrap against fake commands; never use real systemd."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ACTIVATOR = ROOT / "scripts" / "activate-direct-console.sh"
BUILDER = ROOT / "scripts" / "build-direct-console.sh"


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "build" / "unit-one-command-bootstrap"
        scratch.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="xrdp-bootstrap-unit-",
                                               dir=scratch)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / "fake-bin"
        self.bin.mkdir()
        self.log = self.root / "calls.log"
        self.state = self.root / "running"
        commands = {
            "id": """#!/bin/sh
if [ "$1" = "-u" ]; then printf '0\\n'; else exit 97; fi
""",
            "ss": """#!/bin/sh
if [ "$MOCK_RDP_CLIENT" = 1 ]; then echo 'ESTAB 127.0.0.1:3389'; fi
""",
            "python3": """#!/bin/sh
echo "python3 $*" >>"$MOCK_CALLS"
case "$1" in
  */repair-stale-xrdp-overrides.py)
    [ "$MOCK_REPAIR_ERROR" = 1 ] && exit 93
    exit 0 ;;
  */xrdp_clipboard_test_doctor.py)
    [ -f "$MOCK_RUNNING" ] && exit 0
    exit 94 ;;
  *) exit 95 ;;
esac
""",
            "systemctl": """#!/bin/sh
echo "systemctl $*" >>"$MOCK_CALLS"
case "$1" in
  is-active)
    [ -f "$MOCK_RUNNING" ] && exit 0
    exit 3 ;;
  reset-failed) exit 0 ;;
  start)
    [ "$MOCK_START_ERROR" = 1 ] && exit 96
    : >"$MOCK_RUNNING"
    exit 0 ;;
  status) exit 0 ;;
  *) exit 98 ;;
esac
""",
            "journalctl": """#!/bin/sh
echo "journalctl $*" >>"$MOCK_CALLS"
exit 0
""",
        }
        for name, shell in commands.items():
            path = self.bin / name
            path.write_text(shell, encoding="utf-8")
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep +
                        os.environ.get("PATH", ""),
                        MOCK_CALLS=str(self.log), MOCK_RUNNING=str(self.state),
                        MOCK_RDP_CLIENT="0", MOCK_REPAIR_ERROR="0", MOCK_START_ERROR="0")

    def run_bootstrap(self, *args, **extra):
        env = dict(self.env, **{name: str(value) for name, value in extra.items()})
        return subprocess.run(["/bin/sh", str(ACTIVATOR), *args],
                              env=env, capture_output=True, text=True,
                              check=False, timeout=6)

    def calls(self):
        return self.log.read_text() if self.log.exists() else ""

    def test_successful_bootstrap(self):
        response = self.run_bootstrap("--repair-test-runtime")
        self.assertEqual(response.returncode, 0, response.stderr)
        calls = self.calls()
        self.assertIn("repair-stale-xrdp-overrides.py --apply", calls)
        self.assertIn("systemctl reset-failed xrdp.service xrdp-sesman.service", calls)
        self.assertIn("systemctl start xrdp.service", calls)
        self.assertIn("xrdp_clipboard_test_doctor.py", calls)
        self.assertLess(calls.index("repair-stale"), calls.index("systemctl start"))
        self.assertLess(calls.index("systemctl start"), calls.index("xrdp_clipboard_test_doctor.py"))
        self.assertNotIn("systemctl restart", calls)
        self.assertTrue(self.state.exists())

    def test_healthy_services_not_restarted(self):
        self.state.touch()
        response = self.run_bootstrap("--repair-test-runtime")
        self.assertEqual(response.returncode, 0, response.stderr)
        self.assertNotIn("systemctl start", self.calls())
        self.assertNotIn("systemctl reset-failed", self.calls())

    def test_connected_client_prevents_change(self):
        response = self.run_bootstrap("--repair-test-runtime", MOCK_RDP_CLIENT="1")
        self.assertNotEqual(response.returncode, 0)
        self.assertNotIn("repair-stale-xrdp-overrides.py", self.calls())
        self.assertNotIn("systemctl start", self.calls())

    def test_repair_failure_prevents_service_start(self):
        response = self.run_bootstrap("--repair-test-runtime", MOCK_REPAIR_ERROR="1")
        self.assertNotEqual(response.returncode, 0)
        self.assertNotIn("systemctl start", self.calls())

    def test_start_failure_does_not_continue(self):
        response = self.run_bootstrap("--repair-test-runtime", MOCK_START_ERROR="1")
        self.assertNotEqual(response.returncode, 0)
        self.assertIn("cannot start existing persistent xrdp runtime", response.stderr)
        self.assertFalse(self.state.exists())

    def test_reject_mixed_modes_in_either_order(self):
        for args in [
            ("--repair-test-runtime", "--preflight"),
            ("--preflight", "--repair-test-runtime"),
            ("--repair-test-runtime", "--backup"),
            ("--backup", "--repair-test-runtime"),
            ("--repair-test-runtime", "--rollback", "irrelevant"),
        ]:
            with self.subTest(args=args):
                self.log.unlink(missing_ok=True)
                response = self.run_bootstrap(*args)
                self.assertEqual(response.returncode, 2)
                self.assertEqual(self.calls(), "")

    def test_build_orchestrates_only_after_prerequisite_failure(self):
        code = BUILDER.read_text(encoding="utf-8")
        first = 'if ! python3 "$workspace_root/tools/diagnostics/xrdp_clipboard_test_doctor.py"; then'
        repair = 'sudo "$workspace_root/scripts/activate-direct-console.sh" --repair-test-runtime'
        second = '    python3 "$workspace_root/tools/diagnostics/xrdp_clipboard_test_doctor.py"\nfi'
        ctest = 'ctest --test-dir "$build_root" --output-on-failure --parallel 1'
        for fragment in (first, repair, second, ctest):
            self.assertIn(fragment, code)
        self.assertLess(code.index(first), code.index(repair))
        self.assertLess(code.index(repair), code.index(second))
        self.assertLess(code.index(second), code.index(ctest))
        self.assertIn('XRDP_CONSOLE_AUTO_REPAIR_RUNTIME', code)

    def test_normal_activation_snapshots_rollback(self):
        code = ACTIVATOR.read_text(encoding="utf-8")
        marker = '# All normal deployments take a rollback snapshot automatically'
        self.assertIn(marker, code)
        self.assertLess(code.index('backup_enabled=1\nfi', code.index(marker)),
                        code.index('if [ "$backup_enabled" -eq 1 ]; then\n    stamp='))
        self.assertIn('rollback_failed_activation()', code)
        self.assertIn('rollback "$backup_directory"', code)

    def test_posix_shell_syntax(self):
        for script in (ACTIVATOR, BUILDER):
            with self.subTest(script=script):
                result = subprocess.run(["/bin/sh", "-n", str(script)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
