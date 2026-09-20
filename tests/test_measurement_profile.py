#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioral tests for xrdp VNC profile parsing and correlation."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


BENCHMARK = Path(__file__).parents[1] / "src/python/xrdp_vnc_bench.py"
spec = importlib.util.spec_from_file_location("xrdp_vnc_bench", BENCHMARK)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MeasurementProfileTests(unittest.TestCase):
    def test_profile_lines_parse_required_numeric_fields(self):
        text = (
            "VNC_SCHED seq=7 wait_us=11 process_us=22 flush_us=33 "
            "next_request_gap_us=44 next_request_lead_us=0 "
            "logical_update_us=55 rects=2 raw_bytes=66\n"
            "VNC_POINT seq=3 marker_state=1 paint_ns=1000 "
            "flush_begin_ns=2000 send_end_ns=4000 paint_to_send_us=3 "
            "result=0\n"
        )
        sched = module.parse_vnc_sched_lines(text)
        points = module.parse_vnc_point_lines(text)
        self.assertEqual(sched[0]["logical_update_us"], 55)
        self.assertEqual(sched[0]["next_request_lead_us"], 0)
        self.assertEqual(sched[0]["raw_bytes"], 66)
        self.assertEqual(points[0]["marker_state"], 1)
        self.assertEqual(points[0]["send_end_ns"], 4000)

    def test_malformed_profile_records_are_ignored(self):
        text = "VNC_POINT not-a-number\nVNC_SCHED missing=value\n"
        self.assertEqual(module.parse_vnc_point_lines(text), [])
        self.assertEqual(module.parse_vnc_sched_lines(text), [])

    def test_correlation_skips_wrong_state_and_failed_flush(self):
        points = [
            {"seq": 1, "marker_state": 0, "paint_ns": 900,
             "send_end_ns": 950, "result": 0},
            {"seq": 2, "marker_state": 0, "paint_ns": 1100,
             "send_end_ns": 1200, "result": 0},
            {"seq": 3, "marker_state": 1, "paint_ns": 1300,
             "send_end_ns": 1500, "result": 0},
            {"seq": 4, "marker_state": 1, "paint_ns": 2300,
             "send_end_ns": 2400, "result": 1},
            {"seq": 5, "marker_state": 0, "paint_ns": 2500,
             "send_end_ns": 2700, "result": 0},
        ]
        pairs = module.correlate_vnc_points(
            list(reversed(points)), [1000, 2000], [1800, 2800], [1, 0])
        self.assertEqual([pair[2]["seq"] for pair in pairs], [3, 5])

    def test_correlation_rejects_points_outside_sample_window(self):
        points = [
            {"marker_state": 1, "paint_ns": 500,
             "send_end_ns": 700, "result": 0},
            {"marker_state": 1, "paint_ns": 2500,
             "send_end_ns": 2700, "result": 0},
        ]
        pairs = module.correlate_vnc_points(
            points, [1000], [2000], [1])
        self.assertEqual(pairs, [])

    def test_correlation_rejects_mismatched_timestamp_lengths(self):
        with self.assertRaises(ValueError):
            module.correlate_vnc_points([], [1], [], [1])


if __name__ == "__main__":
    unittest.main()
