#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression checks for the direct-console systemd deployment contract."""

from __future__ import annotations

import unittest
import subprocess
from pathlib import Path


ROOT = Path(__file__).parents[1]
XRDP_SERVICE = ROOT / "packaging/systemd/xrdp.service"
SESMAN_SERVICE = ROOT / "packaging/systemd/xrdp-sesman.service"
ACTIVATION = ROOT / "scripts/activate-direct-console.sh"
BUILD = ROOT / "scripts/build-direct-console.sh"
ROOT_CMAKE = ROOT / "CMakeLists.txt"


class DirectConsoleServiceTests(unittest.TestCase):
    def test_activation_cli_usage_and_argument_validation(self):
        help_result = subprocess.run(
            [str(ACTIVATION), "--help"],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("--preflight", help_result.stdout)

        for arguments in (
            ("--unknown",),
            ("--preflight", "extra"),
            ("--rollback",),
        ):
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [str(ACTIVATION), *arguments],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 2, result.stderr)

    def test_xrdp_uses_the_stable_runtime_prefix(self):
        text = XRDP_SERVICE.read_text(encoding="utf-8")
        self.assertIn(
            "ExecStart=/opt/xrdp-console/sbin/xrdp --nodaemon "
            "--config /etc/xrdp/xrdp.ini",
            text,
        )
        self.assertIn("Requires=xrdp-sesman.service", text)
        self.assertNotIn("/home/keivan/", text)
        self.assertNotIn("xrdp-x11vnc", text)

    def test_sesman_uses_the_stable_runtime_prefix(self):
        text = SESMAN_SERVICE.read_text(encoding="utf-8")
        self.assertIn(
            "ExecStart=/opt/xrdp-console/sbin/xrdp-sesman --nodaemon "
            "--config /etc/xrdp/sesman.ini",
            text,
        )
        self.assertIn("BindsTo=xrdp.service", text)
        self.assertNotIn("/home/keivan/", text)
        self.assertNotIn("xrdp-x11vnc", text)

    def test_native_builder_and_direct_activator_defaults(self):
        build = BUILD.read_text(encoding="utf-8")
        activation = ACTIVATION.read_text(encoding="utf-8")
        cmake = ROOT_CMAKE.read_text(encoding="utf-8")

        self.assertIn("-DXRDP_CONSOLE_NATIVE=ON", build)
        self.assertIn("-DXRDP_CONSOLE_BUILD_XRDP=ON", build)
        self.assertIn("build-direct-console", build)
        self.assertIn("build-direct-console", activation)
        self.assertIn("libxrdp_console.so", activation)
        self.assertIn("scripts/diagnose-direct-console.sh", cmake)
        self.assertIn("scripts/restart-direct-console.sh", cmake)
        self.assertNotIn("scripts/build-direct-console.sh", cmake)
        self.assertNotIn("scripts/activate-direct-console.sh", cmake)
        self.assertNotIn("scripts/run-network-latency-matrix.sh", cmake)
        matrix = (ROOT / "scripts/run-network-latency-matrix.sh").read_text(
            encoding="utf-8")
        self.assertIn("--backend direct-x11", matrix)
        self.assertIn("--direct-graphics-transport rfx", matrix)
        self.assertIn("--direct-module", matrix)
        benchmark = (ROOT / "tools/benchmark/xrdp_console_bench.py").read_text(
            encoding="utf-8")
        self.assertIn('WORKSPACE / "build-direct-console"', benchmark)
        self.assertNotIn('WORKSPACE / "build" / "src"', benchmark)
        self.assertNotIn("x11vnc-console.service", cmake)
        self.assertNotIn("x11vnc,", cmake)

    def test_activation_keeps_port_and_refuses_connected_clients(self):
        text = ACTIVATION.read_text(encoding="utf-8")
        self.assertIn("port 3389", text)
        self.assertIn("an RDP client is connected", text)
        self.assertIn("ss -ltnpH 'sport = :3389'", text)
        self.assertIn("-p MainPID --value", text)
        self.assertIn("requested by default", text)
        self.assertIn("diagnostic-only", text)
        self.assertIn("No client capabilities are forced", text)

    def test_activation_can_repair_stopped_runtime_transactionally(self):
        activation = ACTIVATION.read_text(encoding="utf-8")
        build = BUILD.read_text(encoding="utf-8")

        self.assertIn("sudo $0 --preflight", activation)
        self.assertIn("service-state-v1", activation)
        self.assertIn("xrdp-was-active", activation)
        self.assertIn("chansrv-was-active", activation)
        self.assertIn("No previous chansrv target exists", activation)
        self.assertIn("fail_service", activation)
        self.assertNotIn("xrdp.service must be active before activation", activation)
        self.assertNotIn(
            '[ -e "$chansrv_target" ] || fail "missing installed xrdp chansrv',
            activation,
        )

        activation_stop = activation.find(
            "systemctl stop xrdp.service ||",
            activation.find("trap 'exit 143' TERM"),
        )
        self.assertLess(
            activation.find('fail "backup is missing the previous module"'),
            activation_stop,
        )
        activation_install = activation.find(
            'install -m 0755 "$chansrv_source" "$chansrv_target"',
            activation_stop,
        )
        self.assertGreaterEqual(activation_stop, 0)
        self.assertGreater(activation_install, activation_stop)

        activation_restart = activation.rfind(
            'systemctl daemon-reload || fail "systemd daemon-reload failed"'
        )
        self.assertGreaterEqual(activation_restart, 0)
        chansrv_restart = activation.find(
            "systemctl restart xrdp-console-chansrv.service",
            activation_restart,
        )
        xrdp_restart = activation.find(
            "systemctl restart xrdp ||",
            activation_restart,
        )
        self.assertGreaterEqual(chansrv_restart, 0)
        self.assertGreaterEqual(xrdp_restart, 0)
        self.assertLess(chansrv_restart, xrdp_restart)

        self.assertIn("Preflight (read-only):", build)
        self.assertIn("--preflight", build)
        self.assertIn("Activate only after reviewing the preflight result", build)


if __name__ == "__main__":
    unittest.main()
