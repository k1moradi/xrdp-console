#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression checks for the x11vnc console service shutdown contract."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
SERVICE = ROOT / "packaging/systemd/x11vnc-console.service.in"
DROPIN = ROOT / "packaging/systemd/x11vnc-console-clean-shutdown.conf"


class X11vncServiceTests(unittest.TestCase):
    def test_template_uses_x11vnc_clean_shutdown_signal(self):
        text = SERVICE.read_text(encoding="utf-8")
        self.assertIn("KillSignal=SIGINT", text)
        self.assertNotIn("SuccessExitStatus=2", text)
        self.assertNotIn("ConditionPathExists=", text)

    def test_canonical_environment_overrides_legacy_environment(self):
        text = SERVICE.read_text(encoding="utf-8")
        legacy = text.index("EnvironmentFile=-/etc/default/xrdp-vnc-bench")
        canonical = text.index("EnvironmentFile=-/etc/default/xrdp-console")
        self.assertLess(legacy, canonical)

    def test_compatibility_dropin_matches_template(self):
        service = SERVICE.read_text(encoding="utf-8")
        dropin = DROPIN.read_text(encoding="utf-8")
        self.assertIn("KillSignal=SIGINT", dropin)
        self.assertNotIn("SuccessExitStatus=2", dropin)
        self.assertEqual(service.count("KillSignal=SIGINT"), 1)


if __name__ == "__main__":
    unittest.main()
