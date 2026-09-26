#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioral tests for direct-X11 benchmark presentation geometry."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest


BENCHMARK = Path(__file__).parents[1] / "tools/benchmark/xrdp_console_bench.py"
spec = importlib.util.spec_from_file_location("xrdp_console_bench", BENCHMARK)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


class DirectPresentationGeometryTests(unittest.TestCase):
    def test_benchmark_defaults_to_direct_x11(self):
        result = subprocess.run(
            [sys.executable, str(BENCHMARK), "--help"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("default: direct-x11", result.stdout)
        self.assertIn("--backend {direct-x11,vnc}", result.stdout)

    def test_fixed_mode_preserves_physical_geometry_policy(self):
        self.assertEqual(
            module.resolve_direct_presentation_geometry(
                1512, 949, 1366, 768, False, False),
            (1366, 768),
        )

    def test_scaled_presentation_does_not_enable_dynamic_resize(self):
        self.assertEqual(
            module.resolve_direct_presentation_geometry(
                1512, 949, 1366, 768, False, True),
            (1512, 949),
        )

    def test_dynamic_resize_keeps_requested_initial_geometry(self):
        self.assertEqual(
            module.resolve_direct_presentation_geometry(
                1512, 949, 1366, 768, True, False),
            (1512, 949),
        )

    def test_matching_fixed_geometry_is_unchanged(self):
        self.assertEqual(
            module.resolve_direct_presentation_geometry(
                1366, 768, 1366, 768, False, False),
            (1366, 768),
        )

    def test_gfx_planar_is_reported_as_required(self):
        self.assertEqual(
            module.effective_gfx_state("direct-x11", "gfx-planar", False),
            "required-planar",
        )

    def test_non_gfx_direct_transport_is_reported_disabled(self):
        self.assertEqual(
            module.effective_gfx_state("direct-x11", "rfx", False),
            "disabled",
        )

if __name__ == "__main__":
    unittest.main()
