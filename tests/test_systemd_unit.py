#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression checks for the direct-console systemd deployment contract."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
XRDP_SERVICE = ROOT / "packaging/systemd/xrdp.service"
SESMAN_SERVICE = ROOT / "packaging/systemd/xrdp-sesman.service"
ACTIVATION = ROOT / "scripts/activate-direct-console.sh"
BUILD = ROOT / "scripts/build-direct-console.sh"
ROOT_CMAKE = ROOT / "CMakeLists.txt"


class DirectConsoleServiceTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
