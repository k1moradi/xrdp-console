# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("h264_frame_coherence.py")
SPEC = importlib.util.spec_from_file_location(
    "h264_frame_coherence", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
coherence = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coherence)


class H264FrameCoherenceTests(unittest.TestCase):
    def test_parses_complete_frame_sample(self) -> None:
        parsed = coherence.parse_frame_sample(
            "FRAME 123456 254 255 0 1", 4)
        self.assertEqual(parsed, (123456, (254, 255, 0, 1)))

    def test_rejects_malformed_samples(self) -> None:
        self.assertIsNone(coherence.parse_frame_sample("noise", 4))
        self.assertIsNone(
            coherence.parse_frame_sample("FRAME 0 1 2 3 4", 4))
        self.assertIsNone(
            coherence.parse_frame_sample("FRAME 123 1 nope 3 4", 4))
        self.assertIsNone(
            coherence.parse_frame_sample("FRAME 123 1 2 3 256", 4))

    def test_accepts_coherent_grid_and_generation_wrap(self) -> None:
        generations = (254, 254, 255, 255, 0, 0)
        self.assertTrue(coherence.frame_is_coherent(generations, 2, 3))
        self.assertIsNone(coherence.coherence_problem(generations, 2, 3))

    def test_detects_missing_generation_between_rows(self) -> None:
        generations = (10, 10, 12, 12, 13, 13)
        self.assertIn(
            "vertical generation discontinuity",
            coherence.coherence_problem(generations, 2, 3) or "")

    def test_detects_horizontal_partial_update(self) -> None:
        generations = (10, 11, 11, 12)
        self.assertIn(
            "horizontal tile mismatch",
            coherence.coherence_problem(generations, 2, 2) or "")

    def test_rejects_undecodable_marker_and_bad_grid(self) -> None:
        self.assertIn(
            "could not be decoded",
            coherence.coherence_problem((0, -1), 2, 1) or "")
        self.assertIn(
            "count does not match",
            coherence.coherence_problem((0,), 2, 1) or "")
        self.assertIn(
            "invalid tile-grid",
            coherence.coherence_problem((0,), 0, 1) or "")


if __name__ == "__main__":
    unittest.main()
